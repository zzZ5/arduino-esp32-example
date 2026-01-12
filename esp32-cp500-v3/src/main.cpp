/*
 * Project: Bath Heater/Pump Controller (ESP32)
 * File   : main.cpp
 *
 * 功能概要
 * - 通过多探头获取外浴温度（中位去噪）、内核温度和水箱温度
 * - 两种温控模式：
 *   1) n-curve（单阈值，无预测/无回差）—— 依据 t_in 与 t_out_med 的 diff 自适应补热
 *   2) 外浴层定置控温（Setpoint）—— 依据 t_out_med 与 {target, hyst} 定点控温
 * - 允许加热器与水泵同时运行，根据当前温度与水箱热量动态组合：
 *   - 只加热（heater-only）
 *   - 只泵助热（pump-only）
 *   - 加热 + 泵同时运行（heater + pump）
 * - ADAPTIVE_TOUT：通过“上一周期泵-only 是否有效升温”自适应调整仅泵助热阈值 Δ_on
 * - Tank 安全：
 *   - Tank 温度无效或过高 → 自动控制绝不加热，并强制关闭正在加热的加热器
 *   - 手动 heater on 命令在 Tank 无效或过高时亦被硬拦截
 * - 支持 MQTT 命令队列与定时执行；支持远程配置（含 setpoint 与阈值曲线）
 * - 上线信息包含当前模式（setpoint/ncurve）
 * - 关键事件/状态上报到 MQTT（info 中包含 mode、setpoint/hyst、Δ_on/Δ_off、boost 等）
 *
 * MQTT 上线信息（boot message）
 * {
 *   "device": "<device_code>",
 *   "status": "online",
 *   "timestamp": "<YYYY-MM-DD HH:MM:SS>",
 *   "last_measure_time": "<YYYY-MM-DD HH:MM:SS|unknown>",
 *   "mode": "setpoint|ncurve",
 *   "other": {
 *     "setpoint_enabled": <bool>,
 *     "setpoint_target": <number>,
 *     "setpoint_hyst": <number>,
 *     "heater_min_on_ms": <number>,
 *     "heater_min_off_ms": <number>,
 *     "pump_delta_on_min": <number>,
 *     "pump_delta_on_max": <number>,
 *     "pump_hyst_nom": <number>,
 *     "pump_ncurve_gamma": <number>,
 *     "tank_temp_max": <number>,
 *     "temp_limitout_max": <number>,
 *     "temp_limitin_max": <number>,
 *     "temp_limitout_min": <number>,
 *     "temp_limitin_min": <number>
 *   }
 * }
 *
 * 数据上报（每轮测控后 telemetry topic）
 * {
 *   "schema_version": 2,
 *   "timestamp": "<YYYY-MM-DD HH:MM:SS>",
 *   "channels": [
 *     {"code": "TempIn", "value": <t_in>, "unit": "℃", "quality": "<ok|ERR|NaN>"},
 *     {"code": "TempOut1", "value": <t_out[0]>, "unit": "℃", "quality": "<ok|ERR|NaN>"},
 *     {"code": "TempOut2", "value": <t_out[1]>, "unit": "℃", "quality": "<ok|ERR|NaN>"},
 *     {"code": "TempOut3", "value": <t_out[2]>, "unit": "℃", "quality": "<ok|ERR|NaN>"},
 *     {"code": "TankTemp", "value": <tank_temp>, "unit": "℃", "quality": "<ok|ERR|NaN>"},
 *     {"code": "Heater", "value": <0|1>, "unit": "", "quality": "ok"},
 *     {"code": "Pump", "value": <0|1>, "unit": "", "quality": "ok"},
 *     {"code": "Aeration", "value": <0|1>, "unit": "", "quality": "ok"}
 *   ]
 * }
 */

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

 // ========================= 常量定义 =========================
static const size_t JSON_DOC_SIZE = 2048;      // JSON 文档大小
static const float TEMP_VALID_MIN = -20.0f;    // 温度有效下限
static const float TEMP_VALID_MAX = 100.0f;    // 温度有效上限
static const size_t MAX_OUT_SENSORS = 3;      // 最大外浴传感器数量

// ========================= 持久化存储（NVS）键与测控相位 =========================
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";
static const char* NVS_KEY_LAST_MEAS = "lastMeas";   // 上次测量时间（秒）
static const char* NVS_KEY_LAST_AERATION = "lastAer";    // 上次曝气时间（秒）
static unsigned long prevMeasureMs = 0;            // 周期测量基准（ms）
static unsigned long preAerationMs = 0;            // 曝气相位时间基准（ms）

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
static unsigned long heaterToggleMs = 0;  // 最近一次加热器切换时刻（ms）
static unsigned long aerationManualUntilMs = 0; // 手动曝气软锁截止（ms）
static unsigned long pumpManualUntilMs = 0;  // 手动泵软锁截止（ms）
static unsigned long heaterManualUntilMs = 0;  // 手动加热软锁截止（ms）

// ========================= 全局状态 =========================
bool heaterIsOn = false;  // 加热器状态
bool pumpIsOn = false;  // 循环泵状态
bool aerationIsOn = false;  // 曝气状态

// ========================= Tank 状态（给自动 + 手动控制共用的安全信息） =========================
static bool gLastTankValid = false;  // 上一轮测量中 Tank 是否有效
static bool gLastTankOver = false;  // 上一轮测量中 Tank 是否超上限

// ========================= 工具：鲁棒中位数（含离群剔除） =========================
float median(std::vector<float> values,
  float minValid = TEMP_VALID_MIN,
  float maxValid = TEMP_VALID_MAX,
  float outlierThreshold = -1.0f) {
  // 第一步：过滤无效值
  values.erase(std::remove_if(values.begin(), values.end(), [&](float v) {
    return isnan(v) || v < minValid || v > maxValid;
    }), values.end());

  if (values.empty()) return NAN;

  // 第二步：如果有离群值阈值，先计算中位数并过滤
  if (outlierThreshold > 0) {
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2;
    float med0 = (values.size() % 2 == 0)
      ? (values[mid - 1] + values[mid]) / 2.0f
      : values[mid];

    values.erase(std::remove_if(values.begin(), values.end(), [&](float v) {
      return fabsf(v - med0) > outlierThreshold;
      }), values.end());

    if (values.empty()) return NAN;
  }

  // 第三步：最终排序并返回中位数（只排序一次）
  std::sort(values.begin(), values.end());
  size_t mid = values.size() / 2;
  return (values.size() % 2 == 0)
    ? (values[mid - 1] + values[mid]) / 2.0f
    : values[mid];
}

// ========================= [ADAPTIVE_TOUT] 仅泵助热阈值：自适应 + 学习补偿 =========================
static float gPumpDeltaBoost = 0.0f;  // 学习补偿（0..appConfig.pumpLearnMax）
static float gLastToutMed = NAN;   // 上一轮 t_out 的中位温（用于判断仅泵是否带来升温）

inline float lerp_f(float a, float b, float t) { return a + (b - a) * t; }

// 根据 t_in 在 [in_min, in_max] 的相对位置计算自适应 Δ_on / Δ_off（Δ_off 随 Δ_on 比例回差）
static void computePumpDeltas(float t_in, float in_min, float in_max,
  float& delta_on, float& delta_off) {
  auto clamp = [](float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
    };
  const float MAX_ALLOWED = appConfig.pumpDeltaOnMax + appConfig.pumpLearnMax;

  // 把“名义回差（℃）”换算成“比例”：在 Δ_on≈中值时，回差≈appConfig.pumpHystNom
  const float mid_on = 0.5f * (appConfig.pumpDeltaOnMin + appConfig.pumpDeltaOnMax);
  const float hyst_rat = (mid_on > 0.1f) ? (appConfig.pumpHystNom / mid_on) : 0.2f;

  auto dyn_off = [&](float on) {   // 根据 Δ_on 计算自适应 Δ_off
    float hyst = hyst_rat * on;    // 回差 = 比例 × Δ_on
    return fmaxf(0.5f, on - hyst); // 保证 Δ_off 不小于 0.5℃
    };

  // 上下限无效时退化
  if (!isfinite(in_min) || !isfinite(in_max) || in_max <= in_min) {
    delta_on = clamp(appConfig.pumpDeltaOnMin + gPumpDeltaBoost,
      appConfig.pumpDeltaOnMin, MAX_ALLOWED);
    delta_off = dyn_off(delta_on);
    return;
  }

  // 区外早返回（仍叠加学习补偿）
  if (t_in < in_min) {
    delta_on = clamp(appConfig.pumpDeltaOnMin + gPumpDeltaBoost,
      appConfig.pumpDeltaOnMin, MAX_ALLOWED);
    delta_off = dyn_off(delta_on);
    return;
  }
  if (t_in > in_max) {
    delta_on = clamp(appConfig.pumpDeltaOnMax + gPumpDeltaBoost,
      appConfig.pumpDeltaOnMin, MAX_ALLOWED);
    delta_off = dyn_off(delta_on);
    return;
  }

  // 区间内：n-curve 平滑 + 学习补偿
  float u = (t_in - in_min) / (in_max - in_min); // 0..1
  float base_on = lerp_f(appConfig.pumpDeltaOnMin,
    appConfig.pumpDeltaOnMax,
    powf(u, appConfig.pumpNCurveGamma));

  delta_on = clamp(base_on + gPumpDeltaBoost,
    appConfig.pumpDeltaOnMin, MAX_ALLOWED);
  delta_off = dyn_off(delta_on);
}

// ========================= 公共执行函数：应用加热 / 水泵目标 =========================
// 不改变原有逻辑，仅把 Setpoint & n-curve 里重复的执行部分抽出来
void applyHeaterPumpTargets(bool targetHeat,
  bool targetPump,
  bool hardCool,
  const String& msgSafety,
  String& reason) {
  unsigned long nowMs2 = millis();
  unsigned long elapsed = nowMs2 - heaterToggleMs;

  if (hardCool) {
    // 外浴超上限：强制全停、清软锁
    if (heaterIsOn) {
      heaterOff();
      heaterIsOn = false;
      heaterToggleMs = nowMs2;
    }
    if (pumpIsOn) {
      pumpOff();
      pumpIsOn = false;
    }
    heaterManualUntilMs = 0;
    pumpManualUntilMs = 0;
    // 覆盖 reason 为安全提示
    reason = msgSafety;
    return;
  }

  // ===== 加热器：考虑最小开/停机时间 =====
  if (targetHeat) {
    if (!heaterIsOn) {
      if (elapsed >= appConfig.heaterMinOffMs) {
        heaterOn();
        heaterIsOn = true;
        heaterToggleMs = nowMs2;
      }
      else {
        reason += " | 抑制开热：未到最小关断间隔";
      }
    }
    // 已经是开机状态则保持，由其它安全逻辑控制关机
  }
  else {
    if (heaterIsOn) {
      if (elapsed >= appConfig.heaterMinOnMs) {
        heaterOff();
        heaterIsOn = false;
        heaterToggleMs = nowMs2;
      }
      else {
        reason += " | 抑制关热：未到最小开机时间";
      }
    }
  }

  // ===== 水泵：不与加热器互斥 =====
  if (targetPump) {
    if (!pumpIsOn) {
      pumpOn();
      pumpIsOn = true;
    }
  }
  else {
    if (pumpIsOn) {
      pumpOff();
      pumpIsOn = false;
    }
  }
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
    if (mqtt.containsKey("device_code"))    appConfig.mqttDeviceCode = mqtt["device_code"].as<String>();
  }
  if (obj.containsKey("ntp_host") && obj["ntp_host"].is<JsonArray>()) {
    JsonArray ntpArr = obj["ntp_host"].as<JsonArray>();
    appConfig.ntpServers.clear();
    for (JsonVariant v : ntpArr) appConfig.ntpServers.push_back(v.as<String>());
  }
  if (obj.containsKey("post_interval")) appConfig.postInterval = obj["post_interval"].as<uint32_t>();
  if (obj.containsKey("temp_maxdif"))   appConfig.tempMaxDiff = obj["temp_maxdif"].as<uint32_t>();

  // 仅外浴上限硬保护；in_max/in_min 参与归一化（控制仍以 t_in 为对象）
  if (obj.containsKey("temp_limitout_max")) appConfig.tempLimitOutMax = obj["temp_limitout_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitout_min")) appConfig.tempLimitOutMin = obj["temp_limitout_min"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_max"))  appConfig.tempLimitInMax = obj["temp_limitin_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_min"))  appConfig.tempLimitInMin = obj["temp_limitin_min"].as<uint32_t>();
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
  // === 外浴定置控温分组 ===
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
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println(String("[MQTT] JSON 解析错误：") + err.c_str());
    return;
  }
  String device = doc["device"] | "";
  if (device != appConfig.mqttDeviceCode) return;
  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (cmds.isNull()) return;

  for (JsonVariant v : cmds) {
    JsonObject obj = v.as<JsonObject>();
    String cmd = obj["command"] | "";
    String action = obj["action"] | "";
    unsigned long duration = obj["duration"] | 0UL;

    // 兼容 fan 和 aeration 命令(实际控制同一设备)
    if (cmd == "fan") cmd = "aeration";

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
      aerationOn();
      aerationIsOn = true;
      if (pcmd.duration > 0) {
        aerationManualUntilMs = millis() + pcmd.duration;
      } else {
        aerationManualUntilMs = 0;
      }
      scheduleOff("aeration", pcmd.duration);
    }
    else {
      aerationOff();
      aerationIsOn = false;
      aerationManualUntilMs = 0;
    }
  }
  else if (pcmd.cmd == "heater") {
    // 手动 heater on 也遵守 Tank 安全：Tank 无效或过温时一律拒绝
    if (pcmd.action == "on") {
      if (!gLastTankValid || gLastTankOver) {
        Serial.println("[SAFETY] 手动加热命令被拦截：Tank 无效或过温");
        return;
      }
      heaterOn();
      heaterIsOn = true;
      heaterToggleMs = millis();
      if (pcmd.duration > 0) {
        heaterManualUntilMs = millis() + pcmd.duration;
      } else {
        heaterManualUntilMs = 0;
      }
      scheduleOff("heater", pcmd.duration);
    }
    else {
      heaterOff();
      heaterIsOn = false;
      heaterToggleMs = millis();
      heaterManualUntilMs = 0;
    }
  }
  else if (pcmd.cmd == "pump") {
    if (pcmd.action == "on") {
      pumpOn();
      pumpIsOn = true;
      if (pcmd.duration > 0) {
        pumpManualUntilMs = millis() + pcmd.duration;
      } else {
        pumpManualUntilMs = 0;
      }
      scheduleOff("pump", pcmd.duration);
    }
    else {
      pumpOff();
      pumpIsOn = false;
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

// ========================= 工具：质量判定 =========================
static String getQualityString(float value) {
  if (isnan(value)) return "NaN";
  if (value < TEMP_VALID_MIN || value > TEMP_VALID_MAX) return "ERR";
  return "ok";
}

// ========================= 公共函数：构建 channels 数组并上报 =========================
static bool buildChannelsAndPublish(
  float t_in,
  const std::vector<float>& t_outs,
  float t_tank,
  bool tankValid,
  const String& ts,
  time_t nowEpoch,
  const String& modeTag) {

  JsonDocument doc;
  doc["schema_version"] = 2;
  doc["ts"] = ts;

  JsonArray channels = doc.createNestedArray("channels");

  // TempIn 通道
  JsonObject ch_in = channels.createNestedObject();
  ch_in["code"] = "TempIn";
  ch_in["value"] = t_in;
  ch_in["unit"] = "℃";
  ch_in["quality"] = getQualityString(t_in);

  // TempOut 通道（最多 MAX_OUT_SENSORS 个）
  for (size_t i = 0; i < t_outs.size() && i < MAX_OUT_SENSORS; ++i) {
    char codeBuf[16];
    snprintf(codeBuf, sizeof(codeBuf), "TempOut%d", (int)(i + 1));
    JsonObject ch = channels.createNestedObject();
    ch["code"] = codeBuf;
    ch["value"] = t_outs[i];
    ch["unit"] = "℃";
    ch["quality"] = getQualityString(t_outs[i]);
  }

  // TankTemp 通道
  JsonObject ch_tank = channels.createNestedObject();
  ch_tank["code"] = "TankTemp";
  ch_tank["value"] = tankValid ? t_tank : (float)NAN;
  ch_tank["unit"] = "℃";
  ch_tank["quality"] = tankValid ? "ok" : "ERR";

  // Heater 通道
  JsonObject ch_heat = channels.createNestedObject();
  ch_heat["code"] = "Heater";
  ch_heat["value"] = heaterIsOn ? 1 : 0;
  ch_heat["unit"] = "";
  ch_heat["quality"] = "ok";

  // Pump 通道
  JsonObject ch_pump = channels.createNestedObject();
  ch_pump["code"] = "Pump";
  ch_pump["value"] = pumpIsOn ? 1 : 0;
  ch_pump["unit"] = "";
  ch_pump["quality"] = "ok";

  // Aeration 通道
  JsonObject ch_aer = channels.createNestedObject();
  ch_aer["code"] = "Aeration";
  ch_aer["value"] = aerationIsOn ? 1 : 0;
  ch_aer["unit"] = "";
  ch_aer["quality"] = "ok";

  String payload;
  serializeJson(doc, payload);
  bool ok = publishData(getTelemetryTopic(), payload, 10000);
  if (ok) {
    Serial.printf("[MQTT] Data published (%s mode)\n", modeTag.c_str());
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_MEAS, nowEpoch);
      preferences.end();
    }
  }
  return ok;
}

// ========================= 主测量 + 控制 + 上报 =========================
bool doMeasurementAndSave() {
  Serial.println("[Measure] 采集温度");

  float t_in = readTempIn();                 // 核心温度（内部）
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

  // 记录“上一周期”加热器 / 水泵状态，用于 ADAPTIVE_TOUT 学习
  bool prevHeaterOn = heaterIsOn;
  bool prevPumpOn = pumpIsOn;

  // 水箱温度有效性与上限
  bool  tankValid = !isnan(t_tank) && (t_tank > -10.0f) && (t_tank < 120.0f);
  bool  tankOver = tankValid && (t_tank >= appConfig.tankTempMax);
  float delta_tank_in = tankValid ? (t_tank - t_in) : 0.0f;  // 水箱-内温热差
  float delta_tank_out = tankValid ? (t_tank - med_out) : 0.0f;  // 水箱-外浴热差

  // 更新全局 Tank 状态（给自动控制 & 手动命令共用）
  gLastTankValid = tankValid;
  gLastTankOver = tankOver;

  String ts = getTimeString();
  time_t nowEpoch = time(nullptr);

  // 配置快捷变量
  const float out_max = (float)appConfig.tempLimitOutMax;
  const float in_max = (float)appConfig.tempLimitInMax;   // 参与归一化（以 t_in 的上下限）
  const float in_min = (float)appConfig.tempLimitInMin;
  float diff_now = t_in - med_out;

  // ---- 外浴硬保护 ----
  bool   hardCool = false;
  String msgSafety;
  if (med_out >= out_max) {
    hardCool = true;
    msgSafety = String("[SAFETY] 外部温度 ") + String(med_out, 2) +
      " ≥ " + String(out_max, 2) +
      "，强制冷却（关加热+关泵）";
  }

  // *** [ADAPTIVE_TOUT] 学习：仅在“上一周期为泵-only”时依据 dT_out 调整 boost ***
  if (!isnan(gLastToutMed)) {
    float dT_out = med_out - gLastToutMed;
    bool pumpOnlyPrev = (prevPumpOn && !prevHeaterOn);
    if (pumpOnlyPrev) {
      if (dT_out < appConfig.pumpProgressMin) {
        gPumpDeltaBoost = fminf(appConfig.pumpLearnMax,
          gPumpDeltaBoost + appConfig.pumpLearnStepUp);
      }
      else {
        gPumpDeltaBoost = fmaxf(0.0f,
          gPumpDeltaBoost - appConfig.pumpLearnStepDown);
      }
    }
    else {
      gPumpDeltaBoost = fmaxf(0.0f,
        gPumpDeltaBoost - appConfig.pumpLearnStepDown);
    }
  }

  // [ADAPTIVE_TOUT] 计算“仅泵助热”的自适应阈值 Δ_on / Δ_off（仍用 t_in 上下限归一化）
  float DELTA_ON = 0.0f;
  float DELTA_OFF = 0.0f;
  computePumpDeltas(t_in, in_min, in_max, DELTA_ON, DELTA_OFF);

  // =====================================================================
  //                          Setpoint 模式
  // =====================================================================
  if (!hardCool && appConfig.bathSetEnabled) {
    bool   targetHeat = false;
    bool   targetPump = false;
    String reason;

    float tgt = appConfig.bathSetTarget;                 // 目标温度
    float hyst = fmaxf(0.1f, appConfig.bathSetHyst);      // 回差（避免频繁开启/关闭）
    if (isfinite(out_max)) {
      tgt = fminf(tgt, out_max - 0.2f);                   // 不顶死上限，避免过热
    }

    bool bathLow = (med_out < tgt - hyst);
    bool bathHigh = (med_out > tgt + hyst);
    bool bathOk = (!bathLow && !bathHigh);

    if (bathLow) {
      if (!tankValid) {
        // 无 Tank 温度：外浴偏低但不清楚 Tank 情况 → 禁止自动加热（安全第一）
        targetHeat = false;
        targetPump = false;
        reason = "[SAFETY] Tank 无读数 → 禁止自动加热，等待人工检查";
      }
      else {
        if (t_tank < tgt + DELTA_ON) {
          // 水箱还不够热：必须点火加热水箱
          targetHeat = true;
          if (delta_tank_out > 0.5f) {
            targetPump = true;
            reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
              " < (" + String(tgt, 1) + "-" + String(hyst, 1) +
              ") → 加热水箱 + 泵循环助热";
          }
          else {
            targetPump = false;
            reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
              " < (" + String(tgt, 1) + "-" + String(hyst, 1) +
              ") → 水箱偏冷，仅加热水箱";
          }
        }
        else {
          // 水箱已经足够热：视为“热源充足”
          if (delta_tank_out > DELTA_ON) {
            targetHeat = true;
            targetPump = true;
            reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
              " < (" + String(tgt, 1) + "-" + String(hyst, 1) +
              ") → 水箱富余热量，加热 + 泵同时运行";
          }
          else {
            targetHeat = true;
            targetPump = false;
            reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
              " < (" + String(tgt, 1) + "-" + String(hyst, 1) +
              ") → 以加热为主";
          }
        }
      }
    }
    else if (bathHigh) {
      // 外浴温度明显高于目标 → 全停冷却
      targetHeat = false;
      targetPump = false;
      reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
        " > (" + String(tgt, 1) + "+" + String(hyst, 1) +
        ") → 全停降温";
    }
    else if (bathOk) {
      // t_out 在 deadband 内：一般不再加热，可视水箱热差决定是否用泵微调
      targetHeat = false;
      if (tankValid && (delta_tank_out > DELTA_ON)) {
        targetPump = true;
        reason = String("[Setpoint] |t_out_med-") + String(tgt, 1) +
          "| ≤ " + String(hyst, 1) +
          " 且水箱明显更热 → 仅泵微量助热";
      }
      else {
        targetPump = false;
        reason = String("[Setpoint] |t_out_med-") + String(tgt, 1) +
          "| ≤ " + String(hyst, 1) + " → 保持当前温度";
      }
    }

    // 手动控制软锁优先：自动逻辑不主动改动被锁定设备
    unsigned long nowMs = millis();
    bool heaterManualActive = (heaterManualUntilMs != 0 &&
      (long)(nowMs - heaterManualUntilMs) < 0);
    bool pumpManualActive = (pumpManualUntilMs != 0 &&
      (long)(nowMs - pumpManualUntilMs) < 0);
    if (heaterManualActive) {
      targetHeat = heaterIsOn;
      reason += " | 手动加热锁生效";
    }
    if (pumpManualActive) {
      targetPump = pumpIsOn;
      reason += " | 手动泵锁生效";
    }

    // Tank 安全：温度无效或过高时停止加热
    if (!tankValid || tankOver) {
      if (targetHeat) {
        reason += " | Tank≥上限/无读数：停热";
      }
      targetHeat = false;
      if (heaterIsOn) {
        heaterOff();
        heaterIsOn = false;
        heaterToggleMs = millis();
        Serial.println("[SAFETY] Tank 温度无效或过高，强制关闭加热");
      }
    }

    // 统一执行加热 + 泵控制
    applyHeaterPumpTargets(targetHeat, targetPump, hardCool, msgSafety, reason);

    // ===== 定时曝气 =====
    checkAndControlAerationByTimer();

    // ===== 上报（Setpoint 模式）=====
    bool ok = buildChannelsAndPublish(t_in, t_outs, t_tank, tankValid, ts, nowEpoch, "Setpoint");

    // [ADAPTIVE_TOUT] 记录本轮 t_out_med，用于下一轮学习
    gLastToutMed = med_out;

    return ok; // ★ Setpoint 模式早返回
  }

  // =====================================================================
  //                          n-curve 模式
  // =====================================================================
  bool   targetHeat = false;
  bool   targetPump = false;
  String reason;

  if (!hardCool) {

    bool bathWantHeat = false;  // 是否“希望补热”（由 diff_now 与单阈值判定）

    if (t_in < in_min) {
      bathWantHeat = true;
      reason = String("t_in ") + String(t_in, 2) +
        " < " + String(in_min, 2) + " → 补热";
    }
    else {
      float u = 0.0f;
      if (in_max > in_min) {
        float t_ref = min(max(t_in, in_min), in_max);
        u = (t_ref - in_min) / (in_max - in_min);
      }
      const float diff_max = (float)appConfig.tempMaxDiff;
      const float diff_min = max(0.1f, diff_max * 0.02f);
      float DIFF_THR = diff_min + (diff_max - diff_min) *
        powf(u, appConfig.inDiffNCurveGamma);
      bathWantHeat = (diff_now > DIFF_THR);

      reason = String("diff_now=") + String(diff_now, 2) +
        (bathWantHeat ? " > " : " ≤ ") +
        "thr " + String(DIFF_THR, 2);
    }

    // 初始期望加热：由 bathWantHeat 决定
    targetHeat = bathWantHeat;

    unsigned long nowMs = millis();
    bool heaterManualActive = (heaterManualUntilMs != 0 &&
      (long)(nowMs - heaterManualUntilMs) < 0);
    bool pumpManualActive = (pumpManualUntilMs != 0 &&
      (long)(nowMs - pumpManualUntilMs) < 0);
    if (heaterManualActive) {
      targetHeat = heaterIsOn;
      reason += " | 手动加热锁生效";
    }
    if (pumpManualActive) {
      targetPump = pumpIsOn;
      reason += " | 手动泵锁生效";
    }

    // 水箱上限/无效：强制停热
    if (!tankValid || tankOver) {
      if (targetHeat) {
        reason += " | Tank≥上限/无读数：强制停热";
      }
      targetHeat = false;
      if (heaterIsOn) {
        heaterOff();
        heaterIsOn = false;
        heaterToggleMs = millis();
        Serial.println("[SAFETY] Tank 温度无效或过高，强制关闭加热");
      }
    }

    // 若当前不满足“可随时泵助热”的条件，则优先把水箱加热到 Δ_on
    if (tankValid && !targetHeat && !tankOver && (delta_tank_out < DELTA_ON)) {
      targetHeat = true;
      reason += " | tankΔ=" + String(delta_tank_out, 1) +
        "℃ < Δ_on=" + String(DELTA_ON, 1) +
        "℃ → 预热水箱";
    }

    // 泵助热 vs 加热：以 Δ_out 的 Δ_on/Δ_off 判据，允许“加热+泵”并行
    if (tankValid && bathWantHeat && !tankOver) {
      if (delta_tank_out > DELTA_ON) {
        // 水箱明显更热：优先“加热+泵”一起工作
        targetPump = true;
        targetHeat = true;
        reason += " | tankΔ=" + String(delta_tank_out, 1) +
          "℃ > Δ_on=" + String(DELTA_ON, 1) +
          "℃ → 加热+泵同时运行";
      }
      else if (delta_tank_out > DELTA_OFF) {
        // 处于 hysteresis 区：允许保留当前泵状态
        targetPump = pumpIsOn;
        reason += " | tankΔ=" + String(delta_tank_out, 1) +
          "℃ 在 Δ_off~Δ_on 区间 → 泵状态保持";
      }
      else {
        // 热差不足：主要靠加热器
        targetPump = false;
        reason += " | tankΔ=" + String(delta_tank_out, 1) +
          "℃ < Δ_off=" + String(DELTA_OFF, 1) +
          "℃ → 仅加热";
      }
    }
    else if (!tankValid || !bathWantHeat) {
      if (!pumpManualActive) {
        targetPump = false;  // 无手动锁时自动倾向关闭泵，避免空转
      }
    }

  } // end !hardCool

  // 统一执行加热 + 泵控制（含 hardCool）
  applyHeaterPumpTargets(targetHeat, targetPump, hardCool, msgSafety, reason);

  // ===== 定时曝气 =====
  checkAndControlAerationByTimer();

  // ===== 上报（n-curve 模式）=====
  bool ok = buildChannelsAndPublish(t_in, t_outs, t_tank, tankValid, ts, nowEpoch, "n-curve");

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

  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[System] 配置加载失败，重启");
    ESP.restart();
  }
  printConfig(appConfig);

  if (!connectToWiFi(20000) || !multiNTPSetup(30000)) {
    Serial.println("[System] 网络/NTP失败，重启");
    ESP.restart();
  }
  if (!connectToMQTT(20000)) {
    Serial.println("[System] MQTT失败，重启");
    ESP.restart();
  }

  getMQTTClient().setCallback(mqttCallback);
  getMQTTClient().subscribe(getResponseTopic().c_str());

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
      if (elapsedSec >= intervalSec)
        prevMeasureMs = millis() - appConfig.postInterval;
      else
        prevMeasureMs = millis() - (appConfig.postInterval - elapsedSec * 1000UL);
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

  // ============ 上线消息（带控制配置信息） ============
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
  String currentMode = appConfig.bathSetEnabled ? "setpoint" : "ncurve";

  JsonDocument bootDoc;
  bootDoc["schema_version"] = 2;
  bootDoc["ts"] = nowStr;
  bootDoc["device"] = appConfig.mqttDeviceCode;
  bootDoc["status"] = "online";
  bootDoc["last_measure_time"] = lastMeasStr;

  // ---- 控制配置信息（other 字段）----
  JsonObject other = bootDoc.createNestedObject("other");

  other["mode"] = currentMode;

  // 基础节拍
  other["post_interval"] = appConfig.postInterval;

  // 温度限值
  JsonObject limits = other.createNestedObject("temp_limits");
  limits["out_max"] = appConfig.tempLimitOutMax;
  limits["out_min"] = appConfig.tempLimitOutMin;
  limits["in_max"] = appConfig.tempLimitInMax;
  limits["in_min"] = appConfig.tempLimitInMin;

  // 水箱安全
  JsonObject safety = other.createNestedObject("safety");
  safety["tank_temp_max"] = appConfig.tankTempMax;

  // 加热器防抖
  JsonObject hg = other.createNestedObject("heater_guard");
  hg["min_on_ms"] = appConfig.heaterMinOnMs;
  hg["min_off_ms"] = appConfig.heaterMinOffMs;

  // 泵自适应阈值
  JsonObject pa = other.createNestedObject("pump_adaptive");
  pa["delta_on_min"] = appConfig.pumpDeltaOnMin;
  pa["delta_on_max"] = appConfig.pumpDeltaOnMax;
  pa["hyst_nom"] = appConfig.pumpHystNom;
  pa["ncurve_gamma"] = appConfig.pumpNCurveGamma;

  // 泵学习参数
  JsonObject pl = other.createNestedObject("pump_learning");
  pl["step_up"] = appConfig.pumpLearnStepUp;
  pl["step_down"] = appConfig.pumpLearnStepDown;
  pl["max"] = appConfig.pumpLearnMax;
  pl["progress_min"] = appConfig.pumpProgressMin;

  // n-curve diff 曲线参数
  JsonObject curves = other.createNestedObject("curves");
  curves["in_diff_ncurve_gamma"] = appConfig.inDiffNCurveGamma;

  // Setpoint 模式参数
  JsonObject bs = other.createNestedObject("bath_setpoint");
  bs["enabled"] = appConfig.bathSetEnabled;
  bs["target"] = appConfig.bathSetTarget;
  bs["hyst"] = appConfig.bathSetHyst;

  // 曝气定时
  JsonObject aer = other.createNestedObject("aeration_timer");
  aer["enabled"] = appConfig.aerationTimerEnabled;
  aer["interval"] = appConfig.aerationInterval;
  aer["duration"] = appConfig.aerationDuration;

  String bootMsg;
  serializeJson(bootDoc, bootMsg);

  bool ok = publishData(getTelemetryTopic(), bootMsg, 10000);
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
