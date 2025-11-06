/*
 * Project: Bath Heater/Pump Controller (ESP32)
 * File   : main.cpp
 *
 * 功能概要
 * - 通过多探头获取外浴温度（中位去噪）、内核温度和水箱温度
 * - 两种温控模式：
 *   1) n-curve（单阈值，无预测/无回差）—— 依据 t_in 与 t_out_med 的 diff 自适应补热
 *   2) 外浴层定置控温（Setpoint）—— 依据 t_out_med 与 {target, hyst} 定点控温，优先仅泵助热
 * - 互斥控制：加热器与水泵绝不同时运行
 * - “needHeat”用于给水箱预热，保证 (tank - t_out_med) ≥ Δ_on（自适应），随时可“仅泵助热”
 * - “仅泵助热”条件：水箱→外浴热差足够（带回差、自适应）；此时停止加热，只开泵
 * - **曝气两种控制模式（由 aeration_timer 决定）**：
 *   1) 自动定时曝气（aeration_timer.enabled = true）
 *      - 周期：aeration_timer.interval（ms）
 *      - 单次时长：aeration_timer.duration（ms）
 *      - 设备按本地节拍自动开启/关闭曝气，并将最近一次曝气时间写入 NVS 恢复相位
 *   2) 远程/手动控制（aeration_timer.enabled = false）
 *      - 不触发任何自动定时曝气
 *      - 仅响应 MQTT 命令队列中的 “aeration on/off（可带 duration）”，支持手动软锁到期自动关停
 * - 支持 MQTT 命令队列与定时执行；支持远程配置（含 setpoint 与阈值曲线）
 * - 关键事件/状态上报到 MQTT（info 中包含 mode、setpoint/hyst、Δ_on/Δ_off、boost 等）
 *
 * 依赖/环境
 * - 硬件：ESP32
 * - Arduino Core for ESP32 2.x
 * - 库：ArduinoJson、Preferences、FreeRTOS（随 ESP32 Core）、以及你的本地模块：
 *       config_manager.h / wifi_ntp_mqtt.h / sensor.h / log_manager.h
 *
 * 引脚/传感器
 * - 见 initSensors(4, 5, 25, 26, 27)（你自定义的初始化函数）
 *
 * 编译/上传（Arduino IDE）
 * - 开发板：ESP32 Dev Module（或你的具体型号）
 * - 串口波特率：115200
 *
 * MQTT 上线信息（boot message）
 * {
 *   "device": "<equipmentKey>",
 *   "status": "online",
 *   "timestamp": "<YYYY-MM-DD HH:MM:SS>",
 *   "last_measure_time": "<YYYY-MM-DD HH:MM:SS|unknown>"
 * }
 *
 * 数据上报（每轮测控后 post_topic）
 * {
 *   "data":[
 *     {"key":"<keyTempIn>","value":<t_in>,"measured_time":"<ts>"},
 *     {"key":"<keyTempOut[i]>","value":<t_out[i]>,"measured_time":"<ts>"},
 *     ...
 *   ],
 *   "info":{
 *     "mode": "setpoint|ncurve",
 *     "setpoint": <number>,        // 仅 setpoint 模式时附带
 *     "set_hyst": <number>,        // 仅 setpoint 模式时附带
 *     "tank_temp": <number|null>,
 *     "tank_over": <bool>,
 *     "tank_in_delta": <number|null>,
 *     "tank_out_delta": <number|null>,
 *     "msg": "<简要决策信息>",
 *     "heat": <bool>,
 *     "pump": <bool>,
 *     "aeration": <bool>
 *   }
 * }
 */


#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"
#include "log_manager.h"
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

 // ========================= 持久化存储（NVS）键与测控相位 =========================
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";
static const char* NVS_KEY_LAST_MEAS = "lastMeas";     // 上次测量时间（秒）
static const char* NVS_KEY_LAST_AERATION = "lastAer";  // 上次曝气时间（秒）
static unsigned long prevMeasureMs = 0;                // 周期测量基准（ms）
static unsigned long preAerationMs = 0;                // 曝气相位时间基准（ms）

// ========================= 命令队列（非阻塞排程） =========================
struct PendingCommand {
  String cmd;               // "aeration" / "heater" / "pump" / "config_update"
  String action;            // "on" / "off"
  unsigned long duration;   // 持续时间（ms），0 表示不自动关闭
  time_t targetTime;        // 预定执行时间（Unix 秒）
};
std::vector<PendingCommand> pendingCommands;

// ===== 命令队列互斥量句柄 =====
SemaphoreHandle_t gCmdMutex = nullptr;

// ========================= 防抖/软锁 =========================
static unsigned long heaterToggleMs = 0;               // 最近一次加热器切换时刻（ms）
static unsigned long aerationManualUntilMs = 0;        // 手动曝气软锁截止（ms）
static unsigned long pumpManualUntilMs = 0;            // 手动泵软锁截止（ms）
static unsigned long heaterManualUntilMs = 0;          // 手动加热软锁截止（ms）

// ========================= 全局状态 =========================
bool heaterIsOn = false;   // 加热器状态
bool pumpIsOn = false;     // 循环泵状态
bool aerationIsOn = false; // 曝气状态

// ========================= 工具：鲁棒中位数（含离群剔除） =========================
float median(std::vector<float> values,
  float minValid = -20.0f,
  float maxValid = 85.0f,
  float outlierThreshold = -1.0f) {
  values.erase(std::remove_if(values.begin(), values.end(), [&](float v) {
    return isnan(v) || v < minValid || v > maxValid;
    }), values.end());
  if (values.empty()) return NAN;

  std::sort(values.begin(), values.end());
  size_t mid = values.size() / 2;
  float med0 = (values.size() % 2 == 0) ? (values[mid - 1] + values[mid]) / 2.0f : values[mid];

  if (outlierThreshold > 0) {
    values.erase(std::remove_if(values.begin(), values.end(), [&](float v) {
      return fabsf(v - med0) > outlierThreshold;
      }), values.end());
    if (values.empty()) return NAN;
    std::sort(values.begin(), values.end());
  }
  mid = values.size() / 2;
  return (values.size() % 2 == 0) ? (values[mid - 1] + values[mid]) / 2.0f : values[mid];
}

// ========================= [ADAPTIVE_TOUT] 仅泵助热阈值：自适应 + 学习补偿 =========================
static float gPumpDeltaBoost = 0.0f;  // 学习补偿（0..appConfig.pumpLearnMax）
static float gLastToutMed = NAN;      // 上一轮 t_out 的中位温（用于判断仅泵是否带来升温）

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// 根据 t_in 在 [in_min, in_max] 的相对位置计算自适应 Δ_on / Δ_off（Δ_off 随 Δ_on 比例回差）
static void computePumpDeltas(float t_in, float in_min, float in_max,
  float& delta_on, float& delta_off) {
  auto clamp = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
  const float MAX_ALLOWED = appConfig.pumpDeltaOnMax + appConfig.pumpLearnMax;

  // 把“名义回差（℃）”换算成“比例”：在 Δ_on≈中值时，回差≈appConfig.pumpHystNom
  const float mid_on = 0.5f * (appConfig.pumpDeltaOnMin + appConfig.pumpDeltaOnMax);
  const float hyst_rat = (mid_on > 0.1f) ? (appConfig.pumpHystNom / mid_on) : 0.2f;

  auto dyn_off = [&](float on) {           // 根据 Δ_on 计算自适应 Δ_off
    float hyst = hyst_rat * on;            // 回差 = 比例 × Δ_on
    return fmaxf(0.5f, on - hyst);         // 保证 Δ_off 不小于 0.5℃
    };

  // 上下限无效时退化
  if (!isfinite(in_min) || !isfinite(in_max) || in_max <= in_min) {
    delta_on = clamp(appConfig.pumpDeltaOnMin + gPumpDeltaBoost, appConfig.pumpDeltaOnMin, MAX_ALLOWED);
    delta_off = dyn_off(delta_on);
    return;
  }

  // 区外早返回（仍叠加学习补偿）
  if (t_in < in_min) {
    delta_on = clamp(appConfig.pumpDeltaOnMin + gPumpDeltaBoost, appConfig.pumpDeltaOnMin, MAX_ALLOWED);
    delta_off = dyn_off(delta_on);
    return;
  }
  if (t_in > in_max) {
    delta_on = clamp(appConfig.pumpDeltaOnMax + gPumpDeltaBoost, appConfig.pumpDeltaOnMin, MAX_ALLOWED);
    delta_off = dyn_off(delta_on);
    return;
  }

  // 区间内：n-curve 平滑 + 学习补偿
  float u = (t_in - in_min) / (in_max - in_min);   // 0..1
  float base_on = lerp(appConfig.pumpDeltaOnMin, appConfig.pumpDeltaOnMax, powf(u, appConfig.pumpNCurveGamma));

  delta_on = clamp(base_on + gPumpDeltaBoost, appConfig.pumpDeltaOnMin, MAX_ALLOWED);
  delta_off = dyn_off(delta_on);
}

// ========================= 配置更新函数（含分组字段） =========================
bool updateAppConfigFromJson(JsonObject obj) {
  if (obj.containsKey("wifi")) {
    JsonObject wifi = obj["wifi"];
    if (wifi.containsKey("ssid"))     appConfig.wifiSSID = wifi["ssid"].as<String>();
    if (wifi.containsKey("password")) appConfig.wifiPass = wifi["password"].as<String>();
  }
  if (obj.containsKey("mqtt")) {
    JsonObject mqtt = obj["mqtt"];
    if (mqtt.containsKey("server"))         appConfig.mqttServer = mqtt["server"].as<String>();
    if (mqtt.containsKey("port"))           appConfig.mqttPort = mqtt["port"].as<uint16_t>();
    if (mqtt.containsKey("user"))           appConfig.mqttUser = mqtt["user"].as<String>();
    if (mqtt.containsKey("pass"))           appConfig.mqttPass = mqtt["pass"].as<String>();
    if (mqtt.containsKey("clientId"))       appConfig.mqttClientId = mqtt["clientId"].as<String>();
    if (mqtt.containsKey("post_topic"))     appConfig.mqttPostTopic = mqtt["post_topic"].as<String>();
    if (mqtt.containsKey("response_topic")) appConfig.mqttResponseTopic = mqtt["response_topic"].as<String>();
  }
  if (obj.containsKey("ntp_host") && obj["ntp_host"].is<JsonArray>()) {
    JsonArray ntpArr = obj["ntp_host"].as<JsonArray>();
    appConfig.ntpServers.clear();
    for (JsonVariant v : ntpArr) appConfig.ntpServers.push_back(v.as<String>());
  }
  if (obj.containsKey("post_interval")) appConfig.postInterval = obj["post_interval"].as<uint32_t>();
  if (obj.containsKey("temp_maxdif"))   appConfig.tempMaxDiff = obj["temp_maxdif"].as<uint32_t>();

  // 仅外浴上限硬保护；in_max/in_min 参与归一化（控制仍以 t_out 为对象）
  if (obj.containsKey("temp_limitout_max")) appConfig.tempLimitOutMax = obj["temp_limitout_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitout_min")) appConfig.tempLimitOutMin = obj["temp_limitout_min"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_max"))  appConfig.tempLimitInMax = obj["temp_limitin_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_min"))  appConfig.tempLimitInMin = obj["temp_limitin_min"].as<uint32_t>();

  if (obj.containsKey("equipment_key")) appConfig.equipmentKey = obj["equipment_key"].as<String>();
  if (obj.containsKey("keys")) {
    JsonObject keys = obj["keys"];
    if (keys.containsKey("temp_in"))   appConfig.keyTempIn = keys["temp_in"].as<String>();
    if (keys.containsKey("temp_out") && keys["temp_out"].is<JsonArray>()) {
      appConfig.keyTempOut.clear();
      for (JsonVariant v : keys["temp_out"].as<JsonArray>()) appConfig.keyTempOut.push_back(v.as<String>());
    }
  }
  if (obj.containsKey("aeration_timer")) {
    JsonObject aer = obj["aeration_timer"];
    if (aer.containsKey("enabled"))  appConfig.aerationTimerEnabled = aer["enabled"].as<bool>();
    if (aer.containsKey("interval")) appConfig.aerationInterval = aer["interval"].as<uint32_t>();
    if (aer.containsKey("duration")) appConfig.aerationDuration = aer["duration"].as<uint32_t>();
  }

  // ====== 分组调参（与你的 config.json 对齐）======
  if (obj.containsKey("safety")) {
    JsonObject s = obj["safety"];
    if (s.containsKey("tank_temp_max")) appConfig.tankTempMax = s["tank_temp_max"].as<float>();
  }
  if (obj.containsKey("heater_guard")) {
    JsonObject hg = obj["heater_guard"];
    if (hg.containsKey("min_on_ms"))  appConfig.heaterMinOnMs = hg["min_on_ms"].as<uint32_t>();
    if (hg.containsKey("min_off_ms")) appConfig.heaterMinOffMs = hg["min_off_ms"].as<uint32_t>();
  }
  if (obj.containsKey("pump_adaptive")) {
    JsonObject pa = obj["pump_adaptive"];
    if (pa.containsKey("delta_on_min")) appConfig.pumpDeltaOnMin = pa["delta_on_min"].as<float>();
    if (pa.containsKey("delta_on_max")) appConfig.pumpDeltaOnMax = pa["delta_on_max"].as<float>();
    if (pa.containsKey("hyst_nom"))     appConfig.pumpHystNom = pa["hyst_nom"].as<float>();
    if (pa.containsKey("ncurve_gamma")) appConfig.pumpNCurveGamma = pa["ncurve_gamma"].as<float>();
  }
  if (obj.containsKey("pump_learning")) {
    JsonObject pl = obj["pump_learning"];
    if (pl.containsKey("step_up"))      appConfig.pumpLearnStepUp = pl["step_up"].as<float>();
    if (pl.containsKey("step_down"))    appConfig.pumpLearnStepDown = pl["step_down"].as<float>();
    if (pl.containsKey("max"))          appConfig.pumpLearnMax = pl["max"].as<float>();
    if (pl.containsKey("progress_min")) appConfig.pumpProgressMin = pl["progress_min"].as<float>();
  }
  if (obj.containsKey("curves")) {
    JsonObject cv = obj["curves"];
    if (cv.containsKey("in_diff_ncurve_gamma"))
      appConfig.inDiffNCurveGamma = cv["in_diff_ncurve_gamma"].as<float>();
  }
  // === 新增：外浴定置控温分组 ===
  if (obj.containsKey("bath_setpoint")) {
    JsonObject bs = obj["bath_setpoint"];
    if (bs.containsKey("enabled")) appConfig.bathSetEnabled = bs["enabled"].as<bool>();
    if (bs.containsKey("target"))  appConfig.bathSetTarget = bs["target"].as<float>();
    if (bs.containsKey("hyst"))    appConfig.bathSetHyst = bs["hyst"].as<float>();
  }

  return true;
}

// ========================= MQTT 消息回调 =========================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println(String("[MQTT] JSON 解析错误：") + err.c_str());
    return;
  }
  String device = doc["device"] | "";
  if (device != appConfig.equipmentKey) return;
  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (cmds.isNull()) return;

  for (JsonVariant v : cmds) {
    JsonObject obj = v.as<JsonObject>();
    String cmd = obj["command"] | "";
    String action = obj["action"] | "";
    unsigned long duration = obj["duration"] | 0UL;
    String schedule = obj["schedule"] | "";

    if (cmd == "config_update") {
      JsonObject cfg = obj["config"].as<JsonObject>();
      if (!cfg.isNull()) {
        if (updateAppConfigFromJson(cfg)) {
          if (saveConfigToSPIFFS("/config.json")) {
            Serial.println("[CMD] ✅ 配置已远程更新并保存，设备重启以生效");
            ESP.restart();
          }
          else {
            Serial.println("[CMD] ❌ 配置保存失败");
          }
        }
        else {
          Serial.println("[CMD] ❌ 配置更新失败");
        }
      }
      continue;
    }

    time_t target = time(nullptr);
    if (schedule.length() > 0) {
      struct tm schedTime = {};
      if (strptime(schedule.c_str(), "%Y-%m-%d %H:%M:%S", &schedTime)) {
        target = mktime(&schedTime);
      }
      else {
        Serial.println("[MQTT] 错误的时间格式（期望 YYYY-MM-DD HH:MM:SS）");
        continue;
      }
    }

    if (gCmdMutex && xSemaphoreTake(gCmdMutex, pdMS_TO_TICKS(200))) {
      pendingCommands.push_back({ cmd, action, duration, target });
      xSemaphoreGive(gCmdMutex);
    }
    else {
      Serial.println("[CMDQ] 队列上锁失败，丢弃一条命令");
    }
  }
}

// ========================= 非阻塞命令执行 =========================
void executeCommand(const PendingCommand& pcmd) {
  Serial.printf("[CMD] 执行：%s %s 持续 %lu ms\n",
    pcmd.cmd.c_str(), pcmd.action.c_str(), pcmd.duration);

  auto scheduleOff = [&](const String& what, unsigned long ms) {
    if (ms == 0) return;
    PendingCommand off;
    off.cmd = what;
    off.action = "off";
    off.duration = 0;
    off.targetTime = time(nullptr) + (time_t)(ms / 1000UL);
    if (gCmdMutex && xSemaphoreTake(gCmdMutex, pdMS_TO_TICKS(200))) {
      pendingCommands.push_back(off);
      xSemaphoreGive(gCmdMutex);
    }
    else {
      Serial.println("[CMDQ] 无法加入定时关闭命令");
    }
    };

  if (pcmd.cmd == "aeration") {
    if (pcmd.action == "on") {
      aerationOn(); aerationIsOn = true;
      if (pcmd.duration > 0) aerationManualUntilMs = millis() + pcmd.duration;
      scheduleOff("aeration", pcmd.duration);
    }
    else {
      aerationOff(); aerationIsOn = false;
      aerationManualUntilMs = 0;
    }
  }
  else if (pcmd.cmd == "heater") {
    if (pcmd.action == "on") {
      // 互斥：开热前先关泵
      if (pumpIsOn) { pumpOff(); pumpIsOn = false; }
      pumpManualUntilMs = 0;          // 清除泵软锁，避免和自动互斥冲突
      heaterOn(); heaterIsOn = true; heaterToggleMs = millis();
      if (pcmd.duration > 0) heaterManualUntilMs = millis() + pcmd.duration;
      scheduleOff("heater", pcmd.duration);
    }
    else {
      heaterOff(); heaterIsOn = false; heaterToggleMs = millis();
      heaterManualUntilMs = 0;
    }
  }
  else if (pcmd.cmd == "pump") {
    if (pcmd.action == "on") {
      // 互斥：开泵前先关加热
      if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = millis(); }
      heaterManualUntilMs = 0;        // 清除加热软锁
      pumpOn(); pumpIsOn = true;
      if (pcmd.duration > 0) pumpManualUntilMs = millis() + pcmd.duration;
      scheduleOff("pump", pcmd.duration);
    }
    else {
      pumpOff(); pumpIsOn = false;
      pumpManualUntilMs = 0;
    }
  }
  else {
    Serial.println("[CMD] 未知命令：" + pcmd.cmd);
  }
}

// ========================= 定时曝气控制 =========================
void checkAndControlAerationByTimer() {
  if (!appConfig.aerationTimerEnabled) return;
  if (aerationManualUntilMs != 0 && (long)(millis() - aerationManualUntilMs) < 0) return;

  unsigned long nowMs = millis();
  time_t nowEpoch = time(nullptr);

  if (!aerationIsOn && (nowMs - preAerationMs >= appConfig.aerationInterval)) {
    Serial.printf("[Aeration] 到达曝气时间，开始曝气 %lu ms\n", appConfig.aerationDuration);
    aerationOn();
    aerationIsOn = true;
    preAerationMs = nowMs;
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_AERATION, nowEpoch);
      preferences.end();
    }
  }

  if (aerationIsOn && (nowMs - preAerationMs >= appConfig.aerationDuration)) {
    Serial.println("[Aeration] 曝气时间到，停止曝气");
    aerationOff();
    aerationIsOn = false;
    preAerationMs = nowMs;
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_AERATION, nowEpoch);
      preferences.end();
    }
  }
}

// ========================= 主测量 + 控制 + 上报 =========================
bool doMeasurementAndSave() {
  Serial.println("[Measure] 采集温度");

  float t_in = readTempIn();                   // 核心温度（内部）
  std::vector<float> t_outs = readTempOut();   // 外浴多个探头
  float t_tank = readTempTank();               // 水箱温度（用于控制与上报 info）

  if (t_outs.empty()) {
    Serial.println("[Measure] 外部温度读数为空，跳过本轮控制");
    return false;
  }

  float med_out = median(t_outs, -20.0f, 100.0f, 5.0f);
  if (isnan(med_out)) {
    Serial.println("[Measure] 外部温度有效值为空，跳过本轮控制");
    return false;
  }

  // 水箱温度有效性与上限
  bool  tankValid = !isnan(t_tank) && (t_tank > -10.0f) && (t_tank < 120.0f);
  bool  tankOver = tankValid && (t_tank >= appConfig.tankTempMax);
  float delta_tank_in = tankValid ? (t_tank - t_in) : 0.0f;   // 水箱-内温热差（上报）
  float delta_tank_out = tankValid ? (t_tank - med_out) : 0.0f;   // 水箱-外浴热差（用于仅泵）

  String ts = getTimeString();
  time_t nowEpoch = time(nullptr);

  // 配置快捷变量
  const float out_max = (float)appConfig.tempLimitOutMax;
  const float out_min = (float)appConfig.tempLimitOutMin;   // 未直接使用
  const float in_max = (float)appConfig.tempLimitInMax;    // 参与归一化（以 t_in 的上下限）
  const float in_min = (float)appConfig.tempLimitInMin;

  float diff_now = t_in - med_out;

  // ---- 外浴硬保护 ----
  bool   hardCool = false;
  String msg;
  if (med_out >= out_max) {
    hardCool = true;
    msg = String("[SAFETY] 外部温度 ") + String(med_out, 2) +
      " ≥ " + String(out_max, 2) + "，强制冷却（关加热+关泵）";
  }

  // [ADAPTIVE_TOUT] 计算“仅泵助热”的自适应阈值（Δ_on / Δ_off），仍用 t_in 上下限归一化
  float DELTA_ON = 0.0f, DELTA_OFF = 0.0f;
  computePumpDeltas(t_in, in_min, in_max, DELTA_ON, DELTA_OFF);

  // ============= 新增：外浴层定置控温（Setpoint）模式（早返回） =============
  if (!hardCool && appConfig.bathSetEnabled) {
    float tgt = appConfig.bathSetTarget;
    float hyst = fmaxf(0.1f, appConfig.bathSetHyst);
    if (isfinite(out_max)) tgt = fminf(tgt, out_max - 0.2f);   // 不顶死上限

    bool needHeat = false;
    bool needPump = false;
    String reason;

    // 死区控制
    if (med_out < tgt - hyst) {
      // 优先仅泵助热（省电）
      if (tankValid && (delta_tank_out > DELTA_ON)) {
        needPump = true;
        reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
          " < (" + String(tgt, 1) + "-" + String(hyst, 1) + ") → 仅泵助热";
      }
      else {
        needHeat = true;
        reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
          " < (" + String(tgt, 1) + "-" + String(hyst, 1) + ") → 加热";
      }
    }
    else if (med_out > tgt + hyst) {
      // 过高：全停，自然冷却
      needHeat = false;
      needPump = false;
      reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
        " > (" + String(tgt, 1) + "+" + String(hyst, 1) + ") → 全停降温";
    }
    else {
      // 死区：保持全停，避免抖动
      needHeat = false;
      needPump = false;
      reason = String("[Setpoint] |t_out_med-") + String(tgt, 1) +
        "| ≤ " + String(hyst, 1) + " → 保持";
    }

    // 手动锁优先
    unsigned long nowMs = millis();
    bool heaterManualActive = (heaterManualUntilMs != 0 && (long)(nowMs - heaterManualUntilMs) < 0);
    bool pumpManualActive = (pumpManualUntilMs != 0 && (long)(nowMs - pumpManualUntilMs) < 0);
    if (heaterManualActive) { needHeat = heaterIsOn; }
    if (pumpManualActive) { needPump = true; needHeat = false; reason += " | 手动泵锁生效"; }

    // Tank 上限：强制停热
    if (!tankValid || tankOver) {
      needHeat = false;
      reason += " | Tank≥上限/无读数：停热";
      if (heaterIsOn) {
        heaterOff(); heaterIsOn = false; heaterToggleMs = millis();
        Serial.println("[SAFETY] Tank 温度无效或过高，强制关闭加热");
      }
    }

    // ---- 执行动作（互斥）----
    unsigned long nowMs2 = millis();
    if (needPump) {
      if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2; }
      if (!pumpIsOn) { pumpOn();   pumpIsOn = true; }
    }
    else if (needHeat) {
      if (pumpIsOn) { pumpOff();  pumpIsOn = false; }
      // 最小开/停机时间抑制
      bool canOpenHeat = true;
      if (!heaterIsOn && (nowMs2 - heaterToggleMs) < appConfig.heaterMinOffMs) canOpenHeat = false;
      if (canOpenHeat && !heaterIsOn) { heaterOn(); heaterIsOn = true; heaterToggleMs = nowMs2; }
      if (!canOpenHeat) reason += " | 抑制开热：未到最小关断间隔";
    }
    else {
      if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2; }
      if (pumpIsOn) { pumpOff();  pumpIsOn = false; }
    }

    // ===== 定时曝气 =====
    checkAndControlAerationByTimer();

    // ===== 上报 =====
    StaticJsonDocument<2048> doc; // 容量可按探头数量上调
    JsonArray data = doc.createNestedArray("data");

    JsonObject obj_in = data.createNestedObject();
    obj_in["key"] = appConfig.keyTempIn;
    obj_in["value"] = t_in;
    obj_in["measured_time"] = ts;

    for (size_t i = 0; i < t_outs.size(); ++i) {
      JsonObject obj = data.createNestedObject();
      if (i < appConfig.keyTempOut.size())      obj["key"] = appConfig.keyTempOut[i];
      else if (!appConfig.keyTempOut.empty())   obj["key"] = appConfig.keyTempOut[0] + String("_X") + String(i);
      else                                      obj["key"] = String("temp_out_") + String(i);
      obj["value"] = t_outs[i];
      obj["measured_time"] = ts;
    }

    if (tankValid) {
      doc["info"]["tank_temp"] = t_tank;
      doc["info"]["tank_in_delta"] = delta_tank_in;
      doc["info"]["tank_out_delta"] = delta_tank_out;
    }
    else {
      doc["info"]["tank_temp"] = nullptr;
      doc["info"]["tank_in_delta"] = nullptr;
      doc["info"]["tank_out_delta"] = nullptr;
    }
    doc["info"]["tank_over"] = tankOver;

    // 模式标识 & 目标
    doc["info"]["mode"] = "setpoint";
    doc["info"]["setpoint"] = tgt;
    doc["info"]["set_hyst"] = hyst;

    // 复用 msg 格式
    String brief = reason +
      String(" | Δ_on=") + String(DELTA_ON, 1) +
      String(", Δ_off=") + String(DELTA_OFF, 1) +
      String(", boost=") + String(gPumpDeltaBoost, 1) +
      String(" | t_in=") + String(t_in, 1) +
      String(", t_out_med=") + String(med_out, 1);
    if (brief.length() > 300) brief = brief.substring(0, 300);
    doc["info"]["msg"] = brief;

    doc["info"]["heat"] = heaterIsOn;
    doc["info"]["pump"] = pumpIsOn;
    doc["info"]["aeration"] = aerationIsOn;

    String payload;
    serializeJson(doc, payload);
    bool ok = publishData(appConfig.mqttPostTopic, payload, 10000);
    if (ok) {
      if (preferences.begin(NVS_NAMESPACE, false)) {
        preferences.putULong(NVS_KEY_LAST_MEAS, nowEpoch);
        preferences.end();
      }
    }

    // [ADAPTIVE_TOUT] 记录本轮 t_out_med
    gLastToutMed = med_out;

    return ok; // ★ 早返回：setpoint 模式下不再执行 n-curve 分支
  }

  // ======================== 原 n-curve 控制逻辑 ========================
  bool bathWantHeat = false; // 是否希望补热（仅按当前 diff_now 与单阈值比较）
  bool needHeat = false;     // 保证“随时可泵助热”的预热
  bool needPump = false;     // 满足仅泵助热条件时启动水泵（与加热互斥）

  String reason;
  if (!hardCool) {
    if (t_in < in_min) {
      // 明显低于 in_min -> 直接补热
      bathWantHeat = true;
      reason = String("t_in ") + String(t_in, 2) + " < " + String(in_min, 2) + " → 补热";
    }
    else {
      // n-curve：依据 t_in 在 [in_min, in_max] 的位置计算单阈值
      float u = 0.0f;
      if (in_max > in_min) {
        float t_ref = min(max(t_in, in_min), in_max);
        u = (t_ref - in_min) / (in_max - in_min);
      }
      const float diff_max = (float)appConfig.tempMaxDiff;
      const float diff_min = max(0.1f, diff_max * 0.02f);
      float DIFF_THR = diff_min + (diff_max - diff_min) * powf(u, appConfig.inDiffNCurveGamma);
      bathWantHeat = (diff_now > DIFF_THR);

      reason = String("diff_now=") + String(diff_now, 2) +
        (bathWantHeat ? " > " : " ≤ ") +
        "thr " + String(DIFF_THR, 2);
    }

    // 手动加热软锁：自动逻辑不改变 heater 状态
    unsigned long nowMs = millis();
    bool heaterManualActive = (heaterManualUntilMs != 0 && (long)(nowMs - heaterManualUntilMs) < 0);
    if (heaterManualActive) {
      bathWantHeat = heaterIsOn;
      reason += " | 手动加热锁生效";
    }
    // 手动泵软锁：强制仅泵
    bool pumpManualActive = (pumpManualUntilMs != 0 && (long)(nowMs - pumpManualUntilMs) < 0);
    if (pumpManualActive) {
      bathWantHeat = false;
      needHeat = false;
      needPump = true;
      reason += " | 手动泵锁生效";
    }

    // 水箱上限：强制停热
    if (!tankValid || tankOver) {
      bathWantHeat = false;
      needHeat = false;
      reason += " | Tank≥上限/无读数：强制停热";
      if (heaterIsOn) {
        heaterOff(); heaterIsOn = false; heaterToggleMs = millis();
        Serial.println("[SAFETY] Tank 温度无效或过高，强制关闭加热");
      }
    }

    // 若当前不泵，则优先把水箱加热到 Δ_out ≥ Δ_on（保证随时可泵助热）
    if (tankValid && !bathWantHeat && !heaterManualActive && !tankOver && (delta_tank_out < DELTA_ON)) {
      needHeat = true;
      needPump = false;
      reason += " | tankΔ=" + String(delta_tank_out, 1) +
        "℃ < Δ_on=" + String(DELTA_ON, 1) + "℃ → 加热";
    }

    // ---- 泵助热 vs 加热（以 Δ_out 的 Δ_on/Δ_off 判据；泵侧回差保留）----
    if (tankValid && bathWantHeat && !heaterManualActive && !pumpManualActive && !tankOver) {
      if (pumpIsOn) {
        if (delta_tank_out < DELTA_OFF) {
          needPump = false;
          needHeat = true;
          reason += String(" | tankΔ=") + String(delta_tank_out, 1) +
            "℃ < Δ_off=" + String(DELTA_OFF, 1) + "℃ → 退出仅泵，加热";
        }
        else {
          needPump = true;
          needHeat = false;
          reason += String(" | tankΔ=") + String(delta_tank_out, 1) +
            "℃ ≥ Δ_off=" + String(DELTA_OFF, 1) + "℃ → 保持仅泵";
        }
      }
      else {
        if (delta_tank_out > DELTA_ON) {
          needPump = true;
          needHeat = false;
          reason += String(" | tankΔ=") + String(delta_tank_out, 1) +
            "℃ > Δ_on=" + String(DELTA_ON, 1) + "℃ → 进入仅泵";
        }
        else {
          needPump = false;
          needHeat = true;
          reason += String(" | tankΔ=") + String(delta_tank_out, 1) +
            "℃ ≤ Δ_on=" + String(DELTA_ON, 1) + "℃ → 加热";
        }
      }
    }
    else {
      needPump = false;
      // needHeat 保持由前面逻辑决定
    }

    // 最小开/停机时间抑制（水箱过温或仅泵助热时跳过抑制）
    bool skipMinTime = tankOver || needPump;
    if (!skipMinTime) {
      if (bathWantHeat && !heaterIsOn && (nowMs - heaterToggleMs) < appConfig.heaterMinOffMs) {
        needHeat = false;  // 抑制新开热
        reason += " | 抑制(needHeat)：未到最小关断间隔";
      }
      if (!bathWantHeat && heaterIsOn && (nowMs - heaterToggleMs) < appConfig.heaterMinOnMs) {
        bathWantHeat = true;  // 维持已开热
        reason += " | 维持(needHeat)：未到最小开机间隔";
      }
    }
  } // end !hardCool

  // [ADAPTIVE_TOUT] —— 学习补偿：如果“仅泵”不带来 t_out 升温，就逐步提高阈值 —— 
  if (needPump || pumpIsOn) {
    if (!isnan(gLastToutMed)) {
      float dT_out = med_out - gLastToutMed;
      if (dT_out < appConfig.pumpProgressMin) {
        gPumpDeltaBoost = fminf(appConfig.pumpLearnMax, gPumpDeltaBoost + appConfig.pumpLearnStepUp);   // 仅泵无效→抬高阈值
      }
      else {
        gPumpDeltaBoost = fmaxf(0.0f, gPumpDeltaBoost - appConfig.pumpLearnStepDown);                   // 仅泵有效→缓慢回落
      }
    }
  }
  else {
    gPumpDeltaBoost = fmaxf(0.0f, gPumpDeltaBoost - appConfig.pumpLearnStepDown);
  }

  // ---- 执行动作（互斥：泵与加热绝不同时运行）----
  unsigned long nowMs2 = millis();
  if (hardCool) {
    if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2; }
    if (pumpIsOn) { pumpOff();   pumpIsOn = false; }
    pumpManualUntilMs = 0; // 清软锁
    heaterManualUntilMs = 0; // 清加热软锁（硬保护优先）
  }
  else {
    if (needPump) {
      if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2; }
      if (!pumpIsOn) { pumpOn();   pumpIsOn = true; }
    }
    else if (needHeat || bathWantHeat) {
      if (pumpIsOn) { pumpOff();  pumpIsOn = false; }
      if (!heaterIsOn) { heaterOn(); heaterIsOn = true; heaterToggleMs = nowMs2; }
    }
    else {
      if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2; }
      if (pumpIsOn) { pumpOff();  pumpIsOn = false; }
    }
  }

  // ===== 定时曝气 =====
  checkAndControlAerationByTimer();

  // ===== 上报（n-curve 模式）=====
  StaticJsonDocument<1536> doc; // 如探头多可上调
  JsonArray data = doc.createNestedArray("data");

  JsonObject obj_in = data.createNestedObject();
  obj_in["key"] = appConfig.keyTempIn;
  obj_in["value"] = t_in;
  obj_in["measured_time"] = ts;

  for (size_t i = 0; i < t_outs.size(); ++i) {
    JsonObject obj = data.createNestedObject();
    if (i < appConfig.keyTempOut.size())      obj["key"] = appConfig.keyTempOut[i];
    else if (!appConfig.keyTempOut.empty())   obj["key"] = appConfig.keyTempOut[0] + String("_X") + String(i);
    else                                      obj["key"] = String("temp_out_") + String(i);
    obj["value"] = t_outs[i];
    obj["measured_time"] = ts;
  }

  if (tankValid) {
    doc["info"]["tank_temp"] = t_tank;
    doc["info"]["tank_in_delta"] = delta_tank_in;
    doc["info"]["tank_out_delta"] = delta_tank_out;
  }
  else {
    doc["info"]["tank_temp"] = nullptr;
    doc["info"]["tank_in_delta"] = nullptr;
    doc["info"]["tank_out_delta"] = nullptr;
  }
  doc["info"]["tank_over"] = tankOver;

  // 模式标识（n-curve）
  doc["info"]["mode"] = "ncurve";

  doc["info"]["msg"] =
    (hardCool ? msg :
      (String("[Heat-nCurve] ") + reason +
        String(" | Δ_on=") + String(DELTA_ON, 1) +
        String(", Δ_off=") + String(DELTA_OFF, 1) +
        String(", boost=") + String(gPumpDeltaBoost, 1) +
        String(" | t_in=") + String(t_in, 1) +
        String(", t_out_med=") + String(med_out, 1) +
        String(", diff=") + String(diff_now, 1)));

  doc["info"]["heat"] = heaterIsOn;
  doc["info"]["pump"] = pumpIsOn;
  doc["info"]["aeration"] = aerationIsOn;

  String payload;
  serializeJson(doc, payload);
  bool ok = publishData(appConfig.mqttPostTopic, payload, 10000);
  if (ok) {
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_MEAS, nowEpoch);
      preferences.end();
    }
  }

  // [ADAPTIVE_TOUT] 记录本轮 t_out_med
  gLastToutMed = med_out;

  return ok;
}

// ========================= 测量任务 =========================
void measurementTask(void* pv) {
  while (true) {
    if (millis() - prevMeasureMs >= appConfig.postInterval) {
      prevMeasureMs = millis();
      doMeasurementAndSave();
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ========================= 命令调度任务（加锁遍历/删除） =========================
void commandTask(void* pv) {
  while (true) {
    time_t now = time(nullptr);
    if (gCmdMutex && xSemaphoreTake(gCmdMutex, pdMS_TO_TICKS(200))) {
      for (int i = 0; i < (int)pendingCommands.size(); ++i) {
        if (now >= pendingCommands[i].targetTime) {
          PendingCommand pc = pendingCommands[i];
          pendingCommands.erase(pendingCommands.begin() + i);
          --i;
          xSemaphoreGive(gCmdMutex);
          executeCommand(pc);
          if (!xSemaphoreTake(gCmdMutex, pdMS_TO_TICKS(200))) break;
        }
      }
      xSemaphoreGive(gCmdMutex);
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ========================= 初始化 =========================
void setup() {
  Serial.begin(115200);
  Serial.println("[System] 启动中");

  initLogSystem();
  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[System] 配置加载失败，重启");
    ESP.restart();
  }
  printConfig(appConfig);

  if (!connectToWiFi(20000) || !multiNTPSetup(20000)) {
    Serial.println("[System] 网络/NTP失败，重启");
    ESP.restart();
  }
  if (!connectToMQTT(20000)) {
    Serial.println("[System] MQTT失败，重启");
    ESP.restart();
  }

  getMQTTClient().setCallback(mqttCallback);
  getMQTTClient().subscribe(appConfig.mqttResponseTopic.c_str());

  if (!initSensors(4, 5, 25, 26, 27)) {
    Serial.println("[System] 传感器初始化失败，重启");
    ESP.restart();
  }

  gCmdMutex = xSemaphoreCreateMutex();

  // 恢复测量/曝气节拍（用 NVS 里的 “上次事件时间” 推算相位）
  if (preferences.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
    unsigned long lastSecMea = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    unsigned long lastSecAera = preferences.getULong(NVS_KEY_LAST_AERATION, 0);
    time_t nowSec = time(nullptr);

    if (nowSec > 0 && lastSecAera > 0) {
      unsigned long long elapsedAeraMs64 = (unsigned long long)(nowSec - lastSecAera) * 1000ULL;
      preAerationMs = millis() - (unsigned long)elapsedAeraMs64;
    }
    else {
      preAerationMs = millis() - appConfig.aerationInterval;
    }

    if (nowSec > 0 && lastSecMea > 0) {
      unsigned long intervalSec = appConfig.postInterval / 1000UL;
      unsigned long elapsedSec = (nowSec > lastSecMea) ? (nowSec - lastSecMea) : 0UL;
      if (elapsedSec >= intervalSec) prevMeasureMs = millis() - appConfig.postInterval;
      else                           prevMeasureMs = millis() - (appConfig.postInterval - elapsedSec * 1000UL);
    }
    else {
      prevMeasureMs = millis() - appConfig.postInterval;
    }
    preferences.end();
  }
  else {
    prevMeasureMs = millis() - appConfig.postInterval;
    preAerationMs = millis() - appConfig.aerationInterval;
  }

  // 上线消息
  String nowStr = getTimeString();
  String lastMeasStr = "unknown";
  if (preferences.begin(NVS_NAMESPACE, true)) {
    unsigned long lastMeasSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    if (lastMeasSec > 0) {
      time_t t_last = (time_t)lastMeasSec;
      struct tm* tm_info = localtime(&t_last);
      char buffer[32];
      strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
      lastMeasStr = String(buffer);
    }
    preferences.end();
  }
  String bootMsg = "{";
  bootMsg += "\"device\":\"" + appConfig.equipmentKey + "\",";
  bootMsg += "\"status\":\"online\",";
  bootMsg += "\"timestamp\":\"" + nowStr + "\",";
  bootMsg += "\"last_measure_time\":\"" + lastMeasStr + "\"";
  bootMsg += "}";

  bool ok = publishData(appConfig.mqttPostTopic, bootMsg, 10000);
  Serial.println(ok ? "[MQTT] 上线消息发送成功" : "[MQTT] 上线消息发送失败");
  Serial.println("[MQTT] Payload: " + bootMsg);

  // 后台任务
  xTaskCreatePinnedToCore(measurementTask, "MeasureTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandTask, "CommandTask", 4096, NULL, 1, NULL, 1);

  Serial.println("[System] 启动完成");
}

// ========================= 主循环 =========================
void loop() {
  maintainMQTT(5000);
  delay(100);
}
