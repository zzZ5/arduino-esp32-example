/*
 * Project: Bath Heater/Pump Controller (ESP32)
 * File   : main.cpp
 *
 * 功能概要
 * - 通过多探头获取外浴温度（中位去噪）、内核温度和水箱温度
 * - 使用 n-curve + 前瞻斜率 + 回差，判定是否需要“补热”（bathWantHeat）
 * - 互斥控制：加热器与水泵绝不同时运行
 * - “needHeat”用于给水箱预热，保证 delta_tank_in ≥ TANK_PUMP_DELTA_ON，随时可“仅泵助热”
 * - “仅泵助热”条件：水箱足够热（带回差）；此时停止加热，只开泵
 * - 支持 MQTT 命令队列与定时执行；支持定时曝气
 * - 关键事件/状态上报到 MQTT
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
 *     "tank_temp": <number|null>,
 *     "tank_over": <bool>,
 *     "tank_in_delta": <number|null>,
 *     "msg": "<简要决策信息>",
 *     "heat": <bool>,          // 加热器当前状态
 *     "pump": <bool>,          // 水泵当前状态
 *     "aeration": <bool>       // 曝气当前状态
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
// （最小开/停机抑制：如用机械继电器，建议保留≥3~5s；用 SSR 可设为 0）
static const unsigned long HEATER_MIN_ON_MS = 30000;  // 加热器最短开机 30s
static const unsigned long HEATER_MIN_OFF_MS = 30000;  // 加热器最短关机 30s
static unsigned long heaterToggleMs = 0;               // 最近一次加热器切换时刻（ms）

// 手动软锁：在手动时段内，自动逻辑不抢夺控制
static unsigned long aerationManualUntilMs = 0;        // 手动曝气软锁截止（ms）
static unsigned long pumpManualUntilMs = 0;            // 手动泵软锁截止（ms）
static unsigned long heaterManualUntilMs = 0;          // 手动加热软锁截止（ms）

// ========================= 延迟补偿：前瞻预测 + 回差 =========================
static const float PRED_LOOKAHEAD_MIN = 3.0f;  // 前瞻预测 3 分钟
static const float DIFF_HYST = 0.5f;  // 回差（关断阈值比开机阈值低 0.5℃）
static const float SLOPE_LIMIT = 1.5f;  // 外浴温度斜率限幅（℃/min）
static float        lastMedOut = NAN;         // 上次外浴中位温
static unsigned long lastMedOutMs = 0;         // 上次外浴中位温时间戳（ms）

// ========================= 水箱温度安全&参与控制 =========================
static const float TANK_TEMP_MAX_C = 80.0f; // 水箱温度上限 80℃
static const float TANK_PUMP_DELTA_ON = 6.0f;  // 水箱-内温热差 ≥6.0℃ 可仅泵助热
static const float TANK_PUMP_DELTA_OFF = 4.0f;  // 回差：<4.0℃ 退出仅泵助热

// ========================= 全局状态 =========================
bool heaterIsOn = false;  // 加热器状态
bool pumpIsOn = false;  // 循环泵状态
bool aerationIsOn = false;  // 曝气状态

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

// ========================= 配置更新函数 =========================
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

  // 仅外浴上限硬保护；in_max 只用于归一化（注意 out_min/in_min 未在本文件使用）
  if (obj.containsKey("temp_limitout_max")) appConfig.tempLimitOutMax = obj["temp_limitout_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitout_min")) appConfig.tempLimitOutMin = obj["temp_limitout_min"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_max"))  appConfig.tempLimitInMax = obj["temp_limitin_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_min"))  appConfig.tempLimitInMin = obj["temp_limitin_min"].as<uint32_t>();

  if (obj.containsKey("equipment_key")) appConfig.equipmentKey = obj["equipment_key"].as<String>();
  if (obj.containsKey("keys")) {
    JsonObject keys = obj["keys"];
    if (keys.containsKey("temp_in")) appConfig.keyTempIn = keys["temp_in"].as<String>();
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
      heaterManualUntilMs = 0;         // 清除加热软锁
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

  float t_in = readTempIn();     // 核心温度（内部）
  std::vector<float> t_outs = readTempOut(); // 外浴多个探头
  float t_tank = readTempTank();   // 水箱温度（用于控制与上报 info）

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
  bool  tankOver = tankValid && (t_tank >= TANK_TEMP_MAX_C);
  float delta_tank_in = tankValid ? (t_tank - t_in) : 0.0f; // 水箱-内温热差

  String ts = getTimeString();
  time_t nowEpoch = time(nullptr);

  // 配置快捷变量
  const float out_max = (float)appConfig.tempLimitOutMax;
  const float out_min = (float)appConfig.tempLimitOutMin;   // 未在直接使用
  const float in_max = (float)appConfig.tempLimitInMax;    // 仅用于归一化，不做硬切断
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

  // ---- n-curve + 前瞻 + 回差（不触发外浴硬切时）-----

  bool bathWantHeat = false; // 是否希望补热（原有算法核心判据，保持不改）
  bool needHeat = false; // 为保证“随时可泵助热”而主动给水箱补热
  bool needPump = false; // 满足仅泵助热条件时启动水泵（互斥：此时不加热）

  String reason;
  if (!hardCool) {
    if (t_in <= in_min) {
      bathWantHeat = true;
      reason = String("t_in ") + String(t_in, 2) + " ≤ " + String(in_min, 2) + " → 补热";
    }
    else {
      float u = 0.0f;
      if (in_max > in_min) {
        float t_ref = min(max(t_in, in_min), in_max);
        u = (t_ref - in_min) / (in_max - in_min);
      }
      const float diff_max = (float)appConfig.tempMaxDiff;
      const float diff_min = max(0.1f, diff_max * 0.02f);
      const float n_curve = 2.0f;
      float DIFF_ON_MAX = diff_min + (diff_max - diff_min) * powf(u, n_curve);
      float DIFF_OFF_MAX = DIFF_ON_MAX - DIFF_HYST;

      unsigned long nowMs_pred = millis();
      float slope_cpm = 0.0f;
      if (!isnan(lastMedOut) && lastMedOutMs != 0) {
        float dt_min = (nowMs_pred - lastMedOutMs) / 60000.0f;
        if (dt_min > 0.001f) {
          slope_cpm = (med_out - lastMedOut) / dt_min;
          if (slope_cpm > SLOPE_LIMIT) slope_cpm = SLOPE_LIMIT;
          if (slope_cpm < -SLOPE_LIMIT) slope_cpm = -SLOPE_LIMIT;
        }
      }
      float med_out_pred = med_out + slope_cpm * PRED_LOOKAHEAD_MIN;
      float diff_pred = t_in - med_out_pred;

      if (!heaterIsOn) {
        if (diff_pred > DIFF_ON_MAX) {
          bathWantHeat = true;
          reason = String("pred_diff=") + String(diff_pred, 2) +
            " > on_thr " + String(DIFF_ON_MAX, 2) +
            " | slope=" + String(slope_cpm, 2);
        }
        else {
          bathWantHeat = false;
          reason = String("pred_diff=") + String(diff_pred, 2) +
            " ≤ on_thr " + String(DIFF_ON_MAX, 2) +
            " | slope=" + String(slope_cpm, 2);
        }
      }
      else {
        if (diff_pred <= DIFF_OFF_MAX) {
          bathWantHeat = false;
          reason = String("pred_diff=") + String(diff_pred, 2) +
            " ≤ off_thr " + String(DIFF_OFF_MAX, 2) +
            " | slope=" + String(slope_cpm, 2);
        }
        else {
          bathWantHeat = true;
          reason = String("pred_diff=") + String(diff_pred, 2) +
            " > off_thr " + String(DIFF_OFF_MAX, 2) +
            " | slope=" + String(slope_cpm, 2);
        }
      }
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
    if (tankValid && t_tank >= TANK_TEMP_MAX_C) {
      bathWantHeat = false;
      reason += " | Tank≥80℃：强制停热";
    }

    // 若当前不泵或尚未满足仅泵阈值，则优先把水箱加热到 Δ≥ON（保证随时可泵助热）
    if (tankValid && !heaterManualActive && (t_tank < TANK_TEMP_MAX_C) &&
      (delta_tank_in < TANK_PUMP_DELTA_ON)) {
      needHeat = true; // “needHeat”=优先给水箱加热（非仅泵时）
    }

    // 若需要补热，且水箱足够热（带回差），则进入“仅泵助热”，并与加热器互斥
    static bool lastAssist = false; // 仅泵助热回差记忆
    if (!heaterManualActive && !pumpManualActive && !tankOver && bathWantHeat && tankValid) {
      bool canAssistOn = (delta_tank_in >= TANK_PUMP_DELTA_ON);
      bool keepAssist = (lastAssist && delta_tank_in >= TANK_PUMP_DELTA_OFF);
      if ((canAssistOn || keepAssist) && !needHeat) {
        needPump = true;
        bathWantHeat = false; // 仅泵与加热互斥
        lastAssist = true;
        reason += String(" | tankΔ=") + String(delta_tank_in, 1) +
          "℃≥" + String(TANK_PUMP_DELTA_ON, 1) + "℃ → 仅泵助热";
      }
      else {
        needPump = false;
        lastAssist = false;
      }
    }
    else {
      needPump = false;
      lastAssist = false;
    }

    // 最小开/停机时间抑制（水箱过温或仅泵助热时跳过抑制）
    bool skipMinTime = tankOver || needPump;
    if (!skipMinTime) {
      if (bathWantHeat && !heaterIsOn && (nowMs - heaterToggleMs) < HEATER_MIN_OFF_MS) {
        needHeat = false;  // 抑制新开热
        reason += " | 抑制(needHeat)：未到最小关断间隔";
      }
      if (!bathWantHeat && heaterIsOn && (nowMs - heaterToggleMs) < HEATER_MIN_ON_MS) {
        bathWantHeat = true;  // 维持已开热
        reason += " | 维持(needHeat)：未到最小开机间隔";
      }
    }
  } // end !hardCool

  // ---- 执行动作（互斥：泵与加热绝不同时运行）----
  unsigned long nowMs2 = millis();
  if (hardCool) {
    if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2; }
    if (pumpIsOn) { pumpOff();   pumpIsOn = false; }
    pumpManualUntilMs = 0; // 清软锁
  }
  else {
    if (needPump) {
      // 仅泵：确保加热关闭
      if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2; }
      if (!pumpIsOn) { pumpOn();   pumpIsOn = true; }
    }
    else if (needHeat || bathWantHeat) {
      // 只加热：确保泵关闭
      if (pumpIsOn) { pumpOff(); pumpIsOn = false; }
      if (!heaterIsOn) { heaterOn(); heaterIsOn = true; heaterToggleMs = nowMs2; }
    }
    else {
      // 全停
      if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2; }
      if (pumpIsOn) { pumpOff();   pumpIsOn = false; }
    }
  }

  // ===== 定时曝气 =====
  checkAndControlAerationByTimer();

  // ===== 上报 =====
  StaticJsonDocument<1024> doc;
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
  }
  else {
    doc["info"]["tank_temp"] = nullptr;
    doc["info"]["tank_in_delta"] = nullptr;
  }
  doc["info"]["tank_over"] = tankOver;

  doc["info"]["msg"] = (hardCool ?
    msg :
    (String("[Heat-nCurve] ") + reason +
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

  // 记录本次外浴中位数与时间，用于下次计算斜率
  lastMedOut = med_out;
  lastMedOutMs = millis();

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
      else prevMeasureMs = millis() - (appConfig.postInterval - elapsedSec * 1000UL);
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
      struct tm* tm_info = localtime((time_t*)&lastMeasSec);
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
