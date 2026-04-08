/*
 * Project: Bath Heater/Pump Controller (ESP32)
 * File   : main.cpp
 *
 * High-level behavior:
 * - Read bath outlet, internal loop, and tank temperatures.
 * - Support n-curve and setpoint control modes.
 * - Coordinate heater, pump, and aeration with tank safety guards.
 * - Accept MQTT commands, config updates, and emergency-stop input.
 * - Prefer online operation, while keeping local control as the fallback path.
 * - Retry boot registration and telemetry after connectivity recovers.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"
#include "emergency_stop.h"
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

 // ========================= Constants =========================
static const size_t JSON_DOC_SIZE = 2048;      // Shared JSON document size
static const float TEMP_VALID_MIN = -20.0f;    // Lower bound for valid temperatures
static const float TEMP_VALID_MAX = 100.0f;    // Upper bound for valid temperatures
static const size_t MAX_OUT_SENSORS = 3;       // Maximum number of bath outlet probes

// ========================= NVS keys and timing state =========================
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";
static const char* NVS_KEY_LAST_MEAS = "lastMeas";      // Last successful telemetry time (seconds)
static const char* NVS_KEY_LAST_AERATION = "lastAer";   // Last aeration event time (seconds)
static unsigned long prevMeasureMs = 0;                  // Measurement scheduler baseline (ms)
static unsigned long preAerationMs = 0;                  // Aeration scheduler baseline (ms)

// ========================= Command and publish queues =========================
struct PendingCommand {
  String cmd;                 // "aeration" / "heater" / "pump" / "config_update"
  String action;              // "on" / "off" / "auto"
  unsigned long duration;     // Requested duration in ms, 0 means no auto-off
  unsigned long targetTimeMs; // Scheduled execution time in millis()
};
std::vector<PendingCommand> pendingCommands;

struct PendingPublish {
  String topic;
  String payload;
  time_t sampleEpoch;
};
static std::vector<PendingPublish> pendingTelemetryPublishes;
static const size_t MAX_PENDING_TELEMETRY = 12;

// ===== Queue mutexes =====
SemaphoreHandle_t gCmdMutex = nullptr;
SemaphoreHandle_t gPublishMutex = nullptr;
static bool gBootPayloadPending = false;
static String gPendingBootPayload;

// ========================= Debounce and manual locks =========================
static unsigned long heaterToggleMs = 0;        // Last heater state change time
static unsigned long aerationManualUntilMs = 0; // Manual aeration lock deadline
static unsigned long pumpManualUntilMs = 0;     // Manual pump lock deadline
static unsigned long heaterManualUntilMs = 0;   // Manual heater lock deadline
static const unsigned long MANUAL_LOCK_FOREVER = 0xFFFFFFFFUL;

// ========================= Runtime device state =========================
bool heaterIsOn = false;
bool pumpIsOn = false;
bool aerationIsOn = false;

// ========================= Cached tank safety state =========================
static bool gLastTankValid = false;
static bool gLastTankOver = false;

// ========================= Utility: median with filtering =========================
// The input vector is passed by value so filtering and sorting do not affect callers.
float median(const std::vector<float>& values,
  float minValid = TEMP_VALID_MIN,
  float maxValid = TEMP_VALID_MAX,
  float outlierThreshold = -1.0f) {
  // Step 1: discard invalid samples.
  std::vector<float> filtered = values;
  filtered.erase(std::remove_if(filtered.begin(), filtered.end(), [&](float v) {
    return isnan(v) || v < minValid || v > maxValid;
    }), filtered.end());

  if (filtered.empty()) return NAN;

  // Step 2: optionally remove outliers around the first median estimate.
  if (outlierThreshold > 0) {
    std::sort(filtered.begin(), filtered.end());
    size_t mid = filtered.size() / 2;
    float med0 = (filtered.size() % 2 == 0)
      ? (filtered[mid - 1] + filtered[mid]) / 2.0f
      : filtered[mid];

    filtered.erase(std::remove_if(filtered.begin(), filtered.end(), [&](float v) {
      return fabsf(v - med0) > outlierThreshold;
      }), filtered.end());

    if (filtered.empty()) return NAN;
  }

  // Step 3: compute the final median after filtering.
  std::sort(filtered.begin(), filtered.end());
  size_t mid = filtered.size() / 2;
  return (filtered.size() % 2 == 0)
    ? (filtered[mid - 1] + filtered[mid]) / 2.0f
    : filtered[mid];
}

// ========================= Adaptive pump-only threshold state =========================
static float gPumpDeltaBoost = 0.0f;  // Learned compensation, clamped by appConfig.pumpLearnMax
static float gLastToutMed = NAN;      // Previous bath median used by adaptive learning

inline float lerp_f(float a, float b, float t) { return a + (b - a) * t; }

// Returns true while the manual lock is still active.
// Subtraction is used instead of addition so millis() rollover stays safe.
static inline bool isManualLockActive(unsigned long lockUntilMs) {
  if (lockUntilMs == 0) return false;
  if (lockUntilMs == MANUAL_LOCK_FOREVER) return true;
  unsigned long nowMs = millis();
  return (lockUntilMs - nowMs) < 0x80000000UL;
}

static void removePendingCommandsForDeviceLocked(const String& what) {
  pendingCommands.erase(
    std::remove_if(pendingCommands.begin(), pendingCommands.end(),
      [&](const PendingCommand& cmd) { return cmd.cmd == what; }),
    pendingCommands.end());
}

static void clearPendingCommandsForDevice(const String& what) {
  if (gCmdMutex && xSemaphoreTake(gCmdMutex, pdMS_TO_TICKS(200))) {
    removePendingCommandsForDeviceLocked(what);
    xSemaphoreGive(gCmdMutex);
  }
}

static unsigned long computeManualLockUntil(unsigned long duration) {
  return duration > 0 ? millis() + duration : MANUAL_LOCK_FOREVER;
}

static bool isSupportedActionForDevice(const String& action) {
  return action == "on" || action == "off" || action == "auto";
}

static void publishPendingBootPayloadIfNeeded() {
  if (!gBootPayloadPending || gPendingBootPayload.length() == 0) {
    return;
  }

  PubSubClient& mqtt = getMQTTClient();
  if (!mqtt.connected()) {
    return;
  }

  bool ok = publishData(getRegisterTopic(), gPendingBootPayload, 5000);
  if (ok) {
    Serial.println("[MQTT] Deferred boot payload published");
    gBootPayloadPending = false;
    gPendingBootPayload = "";
  }
  else {
    Serial.println("[MQTT] Deferred boot payload publish failed, will retry");
  }
}

static bool enqueueTelemetryPublish(const String& topic, const String& payload, time_t sampleEpoch) {
  if (!gPublishMutex || !xSemaphoreTake(gPublishMutex, pdMS_TO_TICKS(200))) {
    Serial.println("[MQTT] Telemetry queue busy, dropping sample");
    return false;
  }

  if (pendingTelemetryPublishes.size() >= MAX_PENDING_TELEMETRY) {
    pendingTelemetryPublishes.erase(pendingTelemetryPublishes.begin());
    Serial.println("[MQTT] Telemetry queue full, dropped oldest sample");
  }

  pendingTelemetryPublishes.push_back({ topic, payload, sampleEpoch });
  size_t queuedCount = pendingTelemetryPublishes.size();
  xSemaphoreGive(gPublishMutex);

  Serial.printf("[MQTT] Telemetry queued for retry (%u pending)\n", (unsigned)queuedCount);
  return true;
}

static void flushPendingTelemetryIfNeeded() {
  PubSubClient& mqtt = getMQTTClient();
  if (!mqtt.connected()) {
    return;
  }

  PendingPublish next;
  bool hasPending = false;
  if (gPublishMutex && xSemaphoreTake(gPublishMutex, pdMS_TO_TICKS(200))) {
    if (!pendingTelemetryPublishes.empty()) {
      next = pendingTelemetryPublishes.front();
      hasPending = true;
    }
    xSemaphoreGive(gPublishMutex);
  }

  if (!hasPending) {
    return;
  }

  bool ok = publishData(next.topic, next.payload, 3000);
  if (!ok) {
    Serial.println("[MQTT] Pending telemetry publish failed, will retry later");
    return;
  }

  if (gPublishMutex && xSemaphoreTake(gPublishMutex, pdMS_TO_TICKS(200))) {
    if (!pendingTelemetryPublishes.empty() &&
      pendingTelemetryPublishes.front().topic == next.topic &&
      pendingTelemetryPublishes.front().payload == next.payload) {
      pendingTelemetryPublishes.erase(pendingTelemetryPublishes.begin());
    }
    xSemaphoreGive(gPublishMutex);
  }

  if (next.sampleEpoch > 0 && preferences.begin(NVS_NAMESPACE, false)) {
    preferences.putULong(NVS_KEY_LAST_MEAS, next.sampleEpoch);
    preferences.end();
  }

  Serial.println("[MQTT] Pending telemetry published");
}

// 公共函数：Tank 温度安全检查
// 如果 Tank 温度无效或过高，强制关闭加热器并阻止自动开启
// 返回值：true 表示加热被阻止
// Tank safety guard for invalid or over-limit readings.
// If tank temperature is invalid or too high, block heating and force the heater off.
static bool applyTankSafetyCheck(bool tankValid, bool tankOver, bool& targetHeat, String& reason) {
  bool heatBlocked = false;

  if (!tankValid || tankOver) {
    if (targetHeat) {
      reason += " | tank invalid/over-limit: force heater off";
    }
    targetHeat = false;
    heatBlocked = true;

    if (heaterIsOn) {
      heaterOff();
      heaterIsOn = false;
      heaterToggleMs = millis();
      Serial.println("[SAFETY] Tank temperature invalid or over limit, forcing heater off");
    }
  }

  return heatBlocked;
}

// Tank-to-bath delta safety guard.
// If the delta is too large, stop heating and force circulation.
static bool applyTankBathDeltaSafety(bool tankValid, float delta_tank_out,
  float delta_limit, bool& targetHeat, bool& targetPump, String& reason) {
  if (!tankValid) return false;
  if (delta_tank_out < delta_limit) return false;

  if (targetHeat) {
    reason += " | tank-bath delta too large: stop heating and force pump";
  }
  else {
    reason += " | tank-bath delta too large: force pump";
  }
  targetHeat = false;
  targetPump = true;
  heaterManualUntilMs = 0;

  if (heaterIsOn) {
    heaterOff();
    heaterIsOn = false;
    heaterToggleMs = millis();
    Serial.printf("[SAFETY] Tank-bath delta too large (%.1f >= %.1f), forcing heater off\n",
      delta_tank_out, delta_limit);
  }
  return true;
}

// Compute adaptive pump on/off deltas from t_in within the configured range.
static void computePumpDeltas(float t_in, float in_min, float in_max,
  float& delta_on, float& delta_off) {
  auto clamp = [](float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
    };
  const float MAX_ALLOWED = appConfig.pumpDeltaOnMax + appConfig.pumpLearnMax;

  // Convert the nominal hysteresis in C into a ratio relative to delta_on.
  const float mid_on = 0.5f * (appConfig.pumpDeltaOnMin + appConfig.pumpDeltaOnMax);
  const float hyst_rat = (mid_on > 0.1f) ? (appConfig.pumpHystNom / mid_on) : 0.2f;

  auto dyn_off = [&](float on) {   // Compute adaptive delta_off from delta_on
    float hyst = hyst_rat * on;    // hysteresis = ratio * delta_on
    return fmaxf(0.5f, on - hyst); // Keep delta_off above 0.5 C
    };

  // Fall back when the normalization range is invalid.
  if (!isfinite(in_min) || !isfinite(in_max) || in_max <= in_min) {
    delta_on = clamp(appConfig.pumpDeltaOnMin + gPumpDeltaBoost,
      appConfig.pumpDeltaOnMin, MAX_ALLOWED);
    delta_off = dyn_off(delta_on);
    return;
  }

  // Clamp to the low/high edge outside the configured range.
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

  // Inside range: smooth n-curve interpolation plus learned compensation.
  float u = (t_in - in_min) / (in_max - in_min); // 0..1
  float base_on = lerp_f(appConfig.pumpDeltaOnMin,
    appConfig.pumpDeltaOnMax,
    powf(u, appConfig.pumpNCurveGamma));

  delta_on = clamp(base_on + gPumpDeltaBoost,
    appConfig.pumpDeltaOnMin, MAX_ALLOWED);
  delta_off = dyn_off(delta_on);
}

// ========================= Apply heater and pump targets =========================
// Shared actuator application path used by both setpoint and n-curve modes.
void applyHeaterPumpTargets(bool targetHeat,
  bool targetPump,
  bool hardCool,
  const String& msgSafety,
  String& reason) {
  unsigned long nowMs2 = millis();
  unsigned long elapsed = nowMs2 - heaterToggleMs;

  if (hardCool) {
    // Bath temperature above hard limit: force everything off and clear locks.
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
    reason = msgSafety;
    return;
  }

  // ===== Heater with minimum on/off guard times =====
  if (targetHeat) {
    if (!heaterIsOn) {
      if (elapsed >= appConfig.heaterMinOffMs) {
        heaterOn();
        heaterIsOn = true;
        heaterToggleMs = nowMs2;
      }
      else {
        reason += " | heater start suppressed: min off time not reached";
      }
    }
    // If the heater is already on, keep it running unless another safety rule turns it off.
  }
  else {
    if (heaterIsOn) {
      if (elapsed >= appConfig.heaterMinOnMs) {
        heaterOff();
        heaterIsOn = false;
        heaterToggleMs = nowMs2;
      }
      else {
        reason += " | heater stop suppressed: min on time not reached";
      }
    }
  }

  // ===== Pump target application =====
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

// ========================= Config update helper =========================
bool updateAppConfigFromJson(JsonObject obj) {
  if (obj["wifi"].is<JsonObject>()) {
    JsonObject wifi = obj["wifi"];
    if (wifi["ssid"].is<String>())     appConfig.wifiSSID = wifi["ssid"].as<String>();
    if (wifi["password"].is<String>()) appConfig.wifiPass = wifi["password"].as<String>();
  }
  if (obj["mqtt"].is<JsonObject>()) {
    JsonObject mqtt = obj["mqtt"];
    if (mqtt["server"].is<String>())      appConfig.mqttServer = mqtt["server"].as<String>();
    if (mqtt["port"].is<uint16_t>())      appConfig.mqttPort = mqtt["port"].as<uint16_t>();
    if (mqtt["user"].is<String>())        appConfig.mqttUser = mqtt["user"].as<String>();
    if (mqtt["pass"].is<String>())        appConfig.mqttPass = mqtt["pass"].as<String>();
    if (mqtt["device_code"].is<String>()) appConfig.mqttDeviceCode = mqtt["device_code"].as<String>();
  }
  if (obj["ntp_host"].is<JsonArray>()) {
    JsonArray ntpArr = obj["ntp_host"].as<JsonArray>();
    appConfig.ntpServers.clear();
    for (JsonVariant v : ntpArr) appConfig.ntpServers.push_back(v.as<String>());
  }
  if (obj["post_interval"].is<uint32_t>()) appConfig.postInterval = obj["post_interval"].as<uint32_t>();
  if (obj["temp_maxdif"].is<uint32_t>())   appConfig.tempMaxDiff = obj["temp_maxdif"].as<uint32_t>();

  // Bath and internal limits used by normalization and hard safety checks.
  if (obj["temp_limitout_max"].is<uint32_t>()) appConfig.tempLimitOutMax = obj["temp_limitout_max"].as<uint32_t>();
  if (obj["temp_limitout_min"].is<uint32_t>()) appConfig.tempLimitOutMin = obj["temp_limitout_min"].as<uint32_t>();
  if (obj["temp_limitin_max"].is<uint32_t>())  appConfig.tempLimitInMax = obj["temp_limitin_max"].as<uint32_t>();
  if (obj["temp_limitin_min"].is<uint32_t>())  appConfig.tempLimitInMin = obj["temp_limitin_min"].as<uint32_t>();
  if (obj["aeration_timer"].is<JsonObject>()) {
    JsonObject aer = obj["aeration_timer"];
    if (aer["enabled"].is<bool>())      appConfig.aerationTimerEnabled = aer["enabled"].as<bool>();
    if (aer["interval"].is<uint32_t>()) appConfig.aerationInterval = aer["interval"].as<uint32_t>();
    if (aer["duration"].is<uint32_t>()) appConfig.aerationDuration = aer["duration"].as<uint32_t>();
  }

  // Grouped parameters aligned with config.json.
  if (obj["safety"].is<JsonObject>()) {
    JsonObject s = obj["safety"];
    if (s["tank_temp_max"].is<float>()) appConfig.tankTempMax = s["tank_temp_max"].as<float>();
  }
  if (obj["heater_guard"].is<JsonObject>()) {
    JsonObject hg = obj["heater_guard"];
    if (hg["min_on_ms"].is<uint32_t>())  appConfig.heaterMinOnMs = hg["min_on_ms"].as<uint32_t>();
    if (hg["min_off_ms"].is<uint32_t>()) appConfig.heaterMinOffMs = hg["min_off_ms"].as<uint32_t>();
  }
  if (obj["pump_adaptive"].is<JsonObject>()) {
    JsonObject pa = obj["pump_adaptive"];
    if (pa["delta_on_min"].is<float>()) appConfig.pumpDeltaOnMin = pa["delta_on_min"].as<float>();
    if (pa["delta_on_max"].is<float>()) appConfig.pumpDeltaOnMax = pa["delta_on_max"].as<float>();
    if (pa["hyst_nom"].is<float>())     appConfig.pumpHystNom = pa["hyst_nom"].as<float>();
    if (pa["ncurve_gamma"].is<float>()) appConfig.pumpNCurveGamma = pa["ncurve_gamma"].as<float>();
  }
  if (obj["pump_learning"].is<JsonObject>()) {
    JsonObject pl = obj["pump_learning"];
    if (pl["step_up"].is<float>())      appConfig.pumpLearnStepUp = pl["step_up"].as<float>();
    if (pl["step_down"].is<float>())    appConfig.pumpLearnStepDown = pl["step_down"].as<float>();
    if (pl["max"].is<float>())          appConfig.pumpLearnMax = pl["max"].as<float>();
    if (pl["progress_min"].is<float>()) appConfig.pumpProgressMin = pl["progress_min"].as<float>();
  }
  if (obj["curves"].is<JsonObject>()) {
    JsonObject cv = obj["curves"];
    if (cv["in_diff_ncurve_gamma"].is<float>())
      appConfig.inDiffNCurveGamma = cv["in_diff_ncurve_gamma"].as<float>();
  }
  if (obj["bath_setpoint"].is<JsonObject>()) {
    JsonObject bs = obj["bath_setpoint"];
    if (bs["enabled"].is<bool>()) appConfig.bathSetEnabled = bs["enabled"].as<bool>();
    if (bs["target"].is<float>()) appConfig.bathSetTarget = bs["target"].as<float>();
    if (bs["hyst"].is<float>())   appConfig.bathSetHyst = bs["hyst"].as<float>();
  }
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println(String("[MQTT] JSON 解析错误：") + err.c_str());
    return;
  }
  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (cmds.isNull()) return;

  for (JsonVariant v : cmds) {
    JsonObject obj = v.as<JsonObject>();
    String cmd = obj["command"] | "";
    String action = obj["action"] | "";
    unsigned long duration = obj["duration"] | 0UL;

    // 兼容 fan 和 aeration 命令(实际控制同一设备)
    if (cmd == "fan") cmd = "aeration";

    // === 紧急停止命令（最高优先级，无需 device 字段检查）===
    if (cmd == "emergency") {
      if (action == "on") {
        Serial.println("[CMD] 收到急停命令");
        activateEmergencyStop();
      }
      else if (action == "off") {
        Serial.println("[CMD] 收到恢复命令");
        resumeFromEmergencyStop();
      }
      continue;
    }

    // 其他命令：急停状态下拒绝执行
    if (isEmergencyStopped()) {
      Serial.println("[CMD] ⚠️ 急停状态生效中，拒绝执行命令: " + cmd);
      continue;
    }

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

    if ((cmd == "heater" || cmd == "pump" || cmd == "aeration") &&
      !isSupportedActionForDevice(action)) {
      Serial.println("[CMD] Unsupported action for device command, ignored: " + cmd + "/" + action);
      continue;
    }

    unsigned long target = millis();

    if (gCmdMutex && xSemaphoreTake(gCmdMutex, pdMS_TO_TICKS(200))) {
      if (cmd == "heater" || cmd == "pump" || cmd == "aeration") {
        removePendingCommandsForDeviceLocked(cmd);
      }
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
  // 急停状态下拒绝执行所有手动命令
  if (isEmergencyStopped()) {
    Serial.println("[CMD] ⚠️ 急停状态生效中，拒绝执行: " + pcmd.cmd);
    return;
  }

  Serial.printf("[CMD] 执行：%s %s 持续 %lu ms\n",
    pcmd.cmd.c_str(), pcmd.action.c_str(), pcmd.duration);

  auto scheduleOff = [&](const String& what, unsigned long ms) {
    if (ms == 0) return;
    PendingCommand off;
    off.cmd = what;
    off.action = "off";
    off.duration = 0;
    off.targetTimeMs = millis() + ms;
    if (gCmdMutex && xSemaphoreTake(gCmdMutex, pdMS_TO_TICKS(200))) {
      removePendingCommandsForDeviceLocked(what);
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
      aerationManualUntilMs = computeManualLockUntil(pcmd.duration);
      scheduleOff("aeration", pcmd.duration);
    }
    else if (pcmd.action == "auto") {
      aerationManualUntilMs = 0;
      clearPendingCommandsForDevice("aeration");
    }
    else if (pcmd.action == "off") {
      aerationOff();
      aerationIsOn = false;
      aerationManualUntilMs = 0;
      clearPendingCommandsForDevice("aeration");
    }
    else {
      Serial.println("[CMD] Unsupported aeration action ignored: " + pcmd.action);
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
      heaterManualUntilMs = computeManualLockUntil(pcmd.duration);
      scheduleOff("heater", pcmd.duration);
    }
    else if (pcmd.action == "auto") {
      heaterManualUntilMs = 0;
      clearPendingCommandsForDevice("heater");
    }
    else if (pcmd.action == "off") {
      heaterOff();
      heaterIsOn = false;
      heaterToggleMs = millis();
      heaterManualUntilMs = 0;
      clearPendingCommandsForDevice("heater");
    }
    else {
      Serial.println("[CMD] Unsupported heater action ignored: " + pcmd.action);
    }
  }
  else if (pcmd.cmd == "pump") {
    if (pcmd.action == "on") {
      pumpOn();
      pumpIsOn = true;
      pumpManualUntilMs = computeManualLockUntil(pcmd.duration);
      scheduleOff("pump", pcmd.duration);
    }
    else if (pcmd.action == "auto") {
      pumpManualUntilMs = 0;
      clearPendingCommandsForDevice("pump");
    }
    else if (pcmd.action == "off") {
      pumpOff();
      pumpIsOn = false;
      pumpManualUntilMs = 0;
      clearPendingCommandsForDevice("pump");
    }
    else {
      Serial.println("[CMD] Unsupported pump action ignored: " + pcmd.action);
    }
  }
  else {
    Serial.println("[CMD] 未知命令：" + pcmd.cmd);
  }
}

// ========================= 定时曝气控制 =========================
// ========================= Timed aeration control =========================
void checkAndControlAerationByTimer() {
  if (!appConfig.aerationTimerEnabled) return;
  if (isManualLockActive(aerationManualUntilMs)) return;

  unsigned long nowMs = millis();
  time_t nowEpoch = time(nullptr);

  if (!aerationIsOn && (nowMs - preAerationMs >= appConfig.aerationInterval)) {
    Serial.printf("[Aeration] Aeration window started for %lu ms\n", appConfig.aerationDuration);
    aerationOn();
    aerationIsOn = true;
    preAerationMs = nowMs;
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_AERATION, nowEpoch);
      preferences.end();
    }
  }

  if (aerationIsOn && (nowMs - preAerationMs >= appConfig.aerationDuration)) {
    Serial.println("[Aeration] Aeration window completed, stopping aeration");
    aerationOff();
    aerationIsOn = false;
    preAerationMs = nowMs;
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_AERATION, nowEpoch);
      preferences.end();
    }
  }
}

// ========================= Telemetry quality helper =========================
static String getQualityString(float value) {
  if (isnan(value)) return "NaN";
  if (value < TEMP_VALID_MIN || value > TEMP_VALID_MAX) return "ERR";
  return "ok";
}

// ========================= Build telemetry channels and publish =========================
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

  JsonArray channels = doc["channels"].to<JsonArray>();

  // TempIn channel
  JsonObject ch_in = channels.add<JsonObject>();
  ch_in["code"] = "TempIn";
  ch_in["value"] = t_in;
  ch_in["unit"] = "℃";
  ch_in["quality"] = getQualityString(t_in);

  // TempOut channels, up to MAX_OUT_SENSORS
  for (size_t i = 0; i < t_outs.size() && i < MAX_OUT_SENSORS; ++i) {
    char codeBuf[16];
    snprintf(codeBuf, sizeof(codeBuf), "TempOut%d", (int)(i + 1));
    JsonObject ch = channels.add<JsonObject>();
    ch["code"] = codeBuf;
    ch["value"] = t_outs[i];
    ch["unit"] = "℃";
    ch["quality"] = getQualityString(t_outs[i]);
  }

  // TankTemp channel
  JsonObject ch_tank = channels.add<JsonObject>();
  ch_tank["code"] = "TankTemp";
  ch_tank["value"] = tankValid ? t_tank : (float)NAN;
  ch_tank["unit"] = "℃";
  ch_tank["quality"] = tankValid ? "ok" : "ERR";

  // Heater channel
  JsonObject ch_heat = channels.add<JsonObject>();
  ch_heat["code"] = "Heater";
  ch_heat["value"] = heaterIsOn ? 1 : 0;
  ch_heat["unit"] = "";
  ch_heat["quality"] = "ok";

  // Pump channel
  JsonObject ch_pump = channels.add<JsonObject>();
  ch_pump["code"] = "Pump";
  ch_pump["value"] = pumpIsOn ? 1 : 0;
  ch_pump["unit"] = "";
  ch_pump["quality"] = "ok";

  // Aeration channel
  JsonObject ch_aer = channels.add<JsonObject>();
  ch_aer["code"] = "Aeration";
  ch_aer["value"] = aerationIsOn ? 1 : 0;
  ch_aer["unit"] = "";
  ch_aer["quality"] = "ok";

  // EmergencyState channel
  JsonObject ch_emergency = channels.add<JsonObject>();
  ch_emergency["code"] = "EmergencyState";
  ch_emergency["value"] = (int)getEmergencyState();
  ch_emergency["unit"] = "";
  ch_emergency["quality"] = "ok";

  String payload;
  serializeJson(doc, payload);
  bool ok = publishData(getTelemetryTopic(), payload, 10000);
  if (ok) {
    Serial.printf("[MQTT] Data published (%s mode)\n", modeTag.c_str());
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_MEAS, nowEpoch);
      preferences.end();
    }
    return true;
  }

  bool queued = enqueueTelemetryPublish(getTelemetryTopic(), payload, nowEpoch);
  if (queued) {
    Serial.printf("[MQTT] Data buffered for retry (%s mode)\n", modeTag.c_str());
  }
  return queued;
}

// ========================= Measurement, control, and reporting =========================
bool doMeasurementAndSave() {
  Serial.println("[Measure] Sampling temperatures");

  // Emergency-stop mode still reports telemetry, but skips automatic control.
  if (shouldBlockControl()) {
    Serial.println("[Emergency] Emergency stop active, automatic control paused");

    heaterIsOn = false;
    pumpIsOn = false;
    aerationIsOn = false;

    float t_in = NAN;
    std::vector<float> t_outs = readTempOut();
    float t_tank = NAN;
    readInternalTemps(t_in, t_tank);

    if (t_outs.empty()) {
      Serial.println("[Measure] No external temperature samples, skipping report");
      return false;
    }

    float med_out = median(t_outs, -20.0f, 100.0f, 5.0f);
    if (isnan(med_out)) {
      Serial.println("[Measure] External samples invalid after filtering, skipping report");
      return false;
    }

    String ts = getTimeString();
    time_t nowEpoch = time(nullptr);
    bool tankValid = !isnan(t_tank) && (t_tank > -10.0f) && (t_tank < 120.0f);
    return buildChannelsAndPublish(t_in, t_outs, t_tank, tankValid, ts, nowEpoch, "Emergency");
  }

  float t_in = NAN;                 // Internal loop temperature
  std::vector<float> t_outs = readTempOut();   // Multiple bath outlet probes
  float t_tank = NAN;               // Tank temperature used for control and reporting
  readInternalTemps(t_in, t_tank);

  if (t_outs.empty()) {
    Serial.println("[Measure] No external temperature samples, skipping control cycle");
    // Safety fallback: stop heater and pump if bath probes fail.
    if (heaterIsOn) {
      heaterOff();
      heaterIsOn = false;
      heaterToggleMs = millis();
    }
    if (pumpIsOn) {
      pumpOff();
      pumpIsOn = false;
    }
    heaterManualUntilMs = 0;
    pumpManualUntilMs = 0;
    return false;
  }

  float med_out = median(t_outs, -20.0f, 100.0f, 5.0f);
  if (isnan(med_out)) {
    Serial.println("[Measure] External samples invalid after filtering, skipping control cycle");
    // Safety fallback: stop heater and pump if filtered bath data is invalid.
    if (heaterIsOn) {
      heaterOff();
      heaterIsOn = false;
      heaterToggleMs = millis();
    }
    if (pumpIsOn) {
      pumpOff();
      pumpIsOn = false;
    }
    heaterManualUntilMs = 0;
    pumpManualUntilMs = 0;
    return false;
  }

  // Capture previous heater/pump state for adaptive learning.
  bool prevHeaterOn = heaterIsOn;
  bool prevPumpOn = pumpIsOn;

  // Tank validity and high-limit checks.
  bool tankValid = !isnan(t_tank) && (t_tank > -10.0f) && (t_tank < 120.0f);
  bool tankOver = tankValid && (t_tank >= appConfig.tankTempMax);
  float delta_tank_in = tankValid ? (t_tank - t_in) : 0.0f;    // tank minus internal temperature delta
  float delta_tank_out = tankValid ? (t_tank - med_out) : 0.0f; // tank minus bath temperature delta

  // Update cached tank safety state for both automatic and manual paths.
  gLastTankValid = tankValid;
  gLastTankOver = tankOver;

  String ts = getTimeString();
  time_t nowEpoch = time(nullptr);

  // Cached configuration values used in this cycle.
  const float out_max = (float)appConfig.tempLimitOutMax;
  const float in_max = (float)appConfig.tempLimitInMax;   // Used by normalization on the internal temperature axis
  const float in_min = (float)appConfig.tempLimitInMin;
  float diff_now = t_in - med_out;

  // ---- Hard bath over-temperature protection ----
  bool hardCool = false;
  String msgSafety;
  if (med_out >= out_max) {
    hardCool = true;
    msgSafety = String("[SAFETY] Bath temperature ") + String(med_out, 2) +
      " >= " + String(out_max, 2) +
      "; forcing shutdown of heater and pump";
  }

  // Adaptive learning: update boost only when the previous cycle was pump-only.
  // Otherwise decay the learned boost slowly back toward zero.
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

  float DELTA_ON = 0.0f;
  float DELTA_OFF = 0.0f;
  computePumpDeltas(t_in, in_min, in_max, DELTA_ON, DELTA_OFF);

  // =====================================================================
  //                          Setpoint mode
  // =====================================================================
  if (!hardCool && appConfig.bathSetEnabled) {
    bool targetHeat = false;
    bool targetPump = false;
    String reason;

    float tgt = appConfig.bathSetTarget;
    float hyst = fmaxf(0.1f, appConfig.bathSetHyst);
    if (isfinite(out_max)) {
      tgt = fminf(tgt, out_max - 0.2f);
    }

    bool bathLow = (med_out < tgt - hyst);
    bool bathHigh = (med_out > tgt + hyst);
    bool bathOk = (!bathLow && !bathHigh);

    if (bathLow) {
      if (!tankValid) {
        targetHeat = false;
        targetPump = false;
        reason = "[SAFETY] Tank reading unavailable; automatic heating blocked until inspected";
      }
      else {
        if (t_tank < tgt + DELTA_ON) {
          targetHeat = true;
          if (delta_tank_out > 0.5f) {
            targetPump = true;
            reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
              " < (" + String(tgt, 1) + "-" + String(hyst, 1) +
              ") -> heat tank and circulate";
          }
          else {
            targetPump = false;
            reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
              " < (" + String(tgt, 1) + "-" + String(hyst, 1) +
              ") -> tank cold, heater only";
          }
        }
        else {
          if (delta_tank_out > DELTA_ON) {
            targetHeat = true;
            targetPump = true;
            reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
              " < (" + String(tgt, 1) + "-" + String(hyst, 1) +
              ") -> surplus tank heat available, heater + pump";
          }
          else {
            targetHeat = true;
            targetPump = false;
            reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
              " < (" + String(tgt, 1) + "-" + String(hyst, 1) +
              ") -> prioritize heater";
          }
        }
      }
    }
    else if (bathHigh) {
      targetHeat = false;
      targetPump = false;
      reason = String("[Setpoint] t_out_med=") + String(med_out, 1) +
        " > (" + String(tgt, 1) + "+" + String(hyst, 1) +
        ") -> cooling down";
    }
    else if (bathOk) {
      targetHeat = false;
      if (tankValid && (delta_tank_out > DELTA_ON)) {
        targetPump = true;
        reason = String("[Setpoint] |t_out_med-") + String(tgt, 1) +
          "| <= " + String(hyst, 1) +
          " and tank is warmer -> gentle pump assist";
      }
      else {
        targetPump = false;
        reason = String("[Setpoint] |t_out_med-") + String(tgt, 1) +
          "| <= " + String(hyst, 1) + " -> hold temperature";
      }
    }

    bool heaterManualActive = isManualLockActive(heaterManualUntilMs);
    bool pumpManualActive = isManualLockActive(pumpManualUntilMs);
    if (heaterManualActive) {
      targetHeat = heaterIsOn;
      reason += " | heater manual lock active";
    }
    if (pumpManualActive) {
      targetPump = pumpIsOn;
      reason += " | pump manual lock active";
    }

    applyTankSafetyCheck(tankValid, tankOver, targetHeat, reason);
    const float deltaSafetyLimit = fmaxf(5.0f, DELTA_ON * 1.6f + appConfig.pumpHystNom);
    applyTankBathDeltaSafety(tankValid, delta_tank_out, deltaSafetyLimit,
      targetHeat, targetPump, reason);

    applyHeaterPumpTargets(targetHeat, targetPump, hardCool, msgSafety, reason);
    checkAndControlAerationByTimer();

    bool ok = buildChannelsAndPublish(t_in, t_outs, t_tank, tankValid, ts, nowEpoch, "Setpoint");
    gLastToutMed = med_out;
    return ok;
  }

  // =====================================================================
  //                          n-curve mode
  // =====================================================================
  bool targetHeat = false;
  bool targetPump = false;
  String reason;

  if (!hardCool) {
    bool bathWantHeat = false;

    if (t_in < in_min) {
      bathWantHeat = true;
      reason = String("t_in ") + String(t_in, 2) +
        " < " + String(in_min, 2) + " -> heat demand";
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
        (bathWantHeat ? " > " : " <= ") +
        "thr " + String(DIFF_THR, 2);
    }

    targetHeat = bathWantHeat;
    applyTankSafetyCheck(tankValid, tankOver, targetHeat, reason);

    if (tankValid && !targetHeat && !tankOver && (delta_tank_out < DELTA_ON)) {
      targetHeat = true;
      reason += " | tank delta=" + String(delta_tank_out, 1) +
        " < delta_on=" + String(DELTA_ON, 1) +
        " -> preheat tank";
    }

    bool heaterManualActive = isManualLockActive(heaterManualUntilMs);
    bool pumpManualActive = isManualLockActive(pumpManualUntilMs);
    if (heaterManualActive) {
      targetHeat = heaterIsOn;
      reason += " | heater manual lock active";
    }

    if (!pumpManualActive) {
      targetPump = false;
    }
    else {
      targetPump = pumpIsOn;
      reason += " | pump manual lock active";
    }

    if (!pumpManualActive && tankValid && bathWantHeat && !tankOver) {
      if (delta_tank_out > DELTA_ON) {
        targetPump = true;
        targetHeat = true;
        reason += " | tank delta=" + String(delta_tank_out, 1) +
          " > delta_on=" + String(DELTA_ON, 1) +
          " -> heater + pump";
      }
      else if (delta_tank_out > DELTA_OFF) {
        targetPump = pumpIsOn;
        reason += " | tank delta=" + String(delta_tank_out, 1) +
          " within delta_off..delta_on -> keep pump state";
      }
      else {
        targetPump = false;
        reason += " | tank delta=" + String(delta_tank_out, 1) +
          " < delta_off=" + String(DELTA_OFF, 1) +
          " -> heater only";
      }
    }
  }

  const float deltaSafetyLimit = fmaxf(5.0f, DELTA_ON * 1.6f + appConfig.pumpHystNom);
  applyTankBathDeltaSafety(tankValid, delta_tank_out, deltaSafetyLimit,
    targetHeat, targetPump, reason);

  applyHeaterPumpTargets(targetHeat, targetPump, hardCool, msgSafety, reason);
  checkAndControlAerationByTimer();

  bool ok = buildChannelsAndPublish(t_in, t_outs, t_tank, tankValid, ts, nowEpoch, "n-curve");
  gLastToutMed = med_out;

  return ok;
}

// ========================= Measurement task =========================
void measurementTask(void* pv) {
  while (true) {
    if (millis() - prevMeasureMs >= appConfig.postInterval) {
      prevMeasureMs = millis();
      doMeasurementAndSave();
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ========================= Command scheduler task =========================
void commandTask(void* pv) {
  while (true) {
    unsigned long now = millis();

    // Collect due commands first.
    std::vector<PendingCommand> readyToExecute;
    if (gCmdMutex && xSemaphoreTake(gCmdMutex, pdMS_TO_TICKS(200))) {
      // Iterate backward so erasing entries does not invalidate later indexes.
      for (int i = (int)pendingCommands.size() - 1; i >= 0; --i) {
        if ((now - pendingCommands[i].targetTimeMs) < 0x80000000UL) {
          readyToExecute.push_back(pendingCommands[i]);
          pendingCommands.erase(pendingCommands.begin() + i);
        }
      }
      xSemaphoreGive(gCmdMutex);
    }

    // Execute commands outside the mutex to avoid blocking other tasks.
    std::reverse(readyToExecute.begin(), readyToExecute.end());
    for (const auto& cmd : readyToExecute) {
      executeCommand(cmd);
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ========================= Startup =========================
void setup() {
  Serial.begin(115200);
  Serial.println("[System] Starting...");

  initEmergencyStop();

  if (!initSPIFFS()) {
    Serial.println("[System] SPIFFS init failed, restarting");
    delay(1000);
    ESP.restart();
  }
  if (!loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[System] Config unavailable, starting with fallback defaults");
  }
  printConfig(appConfig);

  bool wifiReady = connectToWiFi(20000);
  bool ntpReady = false;
  if (wifiReady) {
    ntpReady = multiNTPSetup(30000);
    if (!ntpReady) {
      Serial.println("[System] NTP failed, continue with local control and degraded timestamps");
    }
  }
  else {
    Serial.println("[System] WiFi failed, continue with local control mode");
  }

  if (wifiReady) {
    getMQTTClient().setCallback(mqttCallback);
    if (!connectToMQTT(20000)) {
      Serial.println("[System] MQTT failed, continue with local control mode");
    }
  }

  if (!initSensors(4, 5, 25, 26, 27)) {
    Serial.println("[System] Sensor init failed, restarting");
    ESP.restart();
  }

  gCmdMutex = xSemaphoreCreateMutex();
  gPublishMutex = xSemaphoreCreateMutex();

  String nowStr = ntpReady ? getTimeString() : String("1970-01-01 00:00:00");
  String ipAddress = getPublicIP();

  JsonDocument bootDoc;
  bootDoc["schema_version"] = 2;
  bootDoc["timestamp"] = nowStr;
  bootDoc["ip_address"] = ipAddress;

  JsonObject config = bootDoc["config"].to<JsonObject>();

  JsonObject wifi = config["wifi"].to<JsonObject>();
  wifi["ssid"] = appConfig.wifiSSID;
  wifi["password"] = "********";

  JsonObject mqtt = config["mqtt"].to<JsonObject>();
  mqtt["server"] = appConfig.mqttServer;
  mqtt["port"] = appConfig.mqttPort;
  mqtt["user"] = appConfig.mqttUser;
  mqtt["pass"] = "********";
  mqtt["device_code"] = appConfig.mqttDeviceCode;

  JsonArray ntpServers = config["ntp_servers"].to<JsonArray>();
  for (const auto& server : appConfig.ntpServers) {
    ntpServers.add(server);
  }

  config["read_interval"] = appConfig.postInterval;
  config["temp_limitout_max"] = appConfig.tempLimitOutMax;
  config["temp_limitin_max"] = appConfig.tempLimitInMax;
  config["temp_limitout_min"] = appConfig.tempLimitOutMin;
  config["temp_limitin_min"] = appConfig.tempLimitInMin;
  config["temp_maxdif"] = appConfig.tempMaxDiff;

  JsonObject aerationTimer = config["aeration_timer"].to<JsonObject>();
  aerationTimer["enabled"] = appConfig.aerationTimerEnabled;
  aerationTimer["interval"] = appConfig.aerationInterval;
  aerationTimer["duration"] = appConfig.aerationDuration;

  JsonObject safety = config["safety"].to<JsonObject>();
  safety["tank_temp_max"] = appConfig.tankTempMax;

  JsonObject heaterGuard = config["heater_guard"].to<JsonObject>();
  heaterGuard["min_on_ms"] = appConfig.heaterMinOnMs;
  heaterGuard["min_off_ms"] = appConfig.heaterMinOffMs;

  JsonObject pumpAdaptive = config["pump_adaptive"].to<JsonObject>();
  pumpAdaptive["delta_on_min"] = appConfig.pumpDeltaOnMin;
  pumpAdaptive["delta_on_max"] = appConfig.pumpDeltaOnMax;
  pumpAdaptive["hyst_nom"] = appConfig.pumpHystNom;
  pumpAdaptive["ncurve_gamma"] = appConfig.pumpNCurveGamma;

  JsonObject pumpLearning = config["pump_learning"].to<JsonObject>();
  pumpLearning["step_up"] = appConfig.pumpLearnStepUp;
  pumpLearning["step_down"] = appConfig.pumpLearnStepDown;
  pumpLearning["max"] = appConfig.pumpLearnMax;
  pumpLearning["progress_min"] = appConfig.pumpProgressMin;

  JsonObject curves = config["curves"].to<JsonObject>();
  curves["in_diff_ncurve_gamma"] = appConfig.inDiffNCurveGamma;

  JsonObject bathSetpoint = config["bath_setpoint"].to<JsonObject>();
  bathSetpoint["enabled"] = appConfig.bathSetEnabled;
  bathSetpoint["target"] = appConfig.bathSetTarget;
  bathSetpoint["hyst"] = appConfig.bathSetHyst;

  String bootMsg;
  serializeJson(bootDoc, bootMsg);
  gPendingBootPayload = bootMsg;
  gBootPayloadPending = bootMsg.length() > 0;

  if (wifiReady) {
    bool ok = publishData(getRegisterTopic(), bootMsg, 10000);
    Serial.println(ok ? "[MQTT] Boot payload published" : "[MQTT] Boot payload publish failed, queued for retry");
    if (ok) {
      gBootPayloadPending = false;
      gPendingBootPayload = "";
    }
    Serial.println("[MQTT] Payload: " + bootMsg);
    delay(500);
  }
  else {
    Serial.println("[MQTT] Boot payload queued until connectivity is restored");
  }

  if (preferences.begin(NVS_NAMESPACE, true)) {
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

  xTaskCreatePinnedToCore(measurementTask, "MeasureTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandTask, "CommandTask", 4096, NULL, 1, NULL, 1);

  Serial.println("[System] Startup complete");
}

// ========================= 主循环 =========================
void loop() {
  // 保持MQTT连接并处理心跳（高频调用）
  maintainMQTT(5000);
  publishPendingBootPayloadIfNeeded();
  flushPendingTelemetryIfNeeded();
  delay(100);
}
