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
static const char* NVS_NAMESPACE = "my-nvs";   // NVS 命名空间
static const char* NVS_KEY_LAST_MEAS = "lastMeas"; // 上次测量时间（秒）
static const char* NVS_KEY_LAST_AERATION = "lastAer";  // 上次曝气时间（秒）
static unsigned long prevMeasureMs = 0;               // 周期测量的时间基准（ms）
static unsigned long preAerationMs = 0;               // 曝气相位参考时间（ms）

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

// ========================= 防抖/软锁/泵保护 =========================
static const unsigned long HEATER_MIN_ON_MS = 30000;  // 加热器最短开机 30s
static const unsigned long HEATER_MIN_OFF_MS = 30000;  // 加热器最短关机 30s
static unsigned long heaterToggleMs = 0;               // 最近一次加热器切换时刻（ms）

// 泵：连续运行保护 & 冷却期（休息期）
static const unsigned long PUMP_COOLDOWN_MS = 60000;   // 泵冷却期（60s）
static unsigned long pumpOnStartMs = 0;                // 最近一次“泵开启”的时间
static unsigned long pumpCooldownUntilMs = 0;          // 泵冷却期截止时刻（ms），0 表示无冷却期

// 手动软锁：在手动时段内，自动逻辑不抢夺控制
static unsigned long aerationManualUntilMs = 0;        // 手动曝气软锁截止（ms）
static unsigned long pumpManualUntilMs = 0;        // 手动泵软锁截止（ms）
static unsigned long heaterManualUntilMs = 0;        // 手动加热软锁截止（ms）

// ========================= 延迟补偿：前瞻预测 + 回差 =========================
static const float PRED_LOOKAHEAD_MIN = 3.0f;  // 前瞻预测 3 分钟
static const float DIFF_HYST = 0.5f;  // 回差（关断阈值比开机阈值低 0.5℃）
static const float SLOPE_LIMIT = 1.5f;  // 外浴温度斜率限幅（℃/min）
static float        lastMedOut = NAN;        // 上次外浴中位温
static unsigned long lastMedOutMs = 0;         // 上次外浴中位温时间戳（ms）

// ========================= 关热后尾流（确保水浴连续性） =========================
static const unsigned long PUMP_TAIL_MS = 60000;       // 关热后泵延时继续运行时长（ms）
static unsigned long pumpTailUntilMs = 0;              // 尾流截止时间（ms），0 表示无尾流窗口

// ========================= 水箱温度安全&参与控制 =========================
static const float TANK_TEMP_MAX_C = 80.0f;        // 水箱温度上限 80℃
static const float TANK_PUMP_DELTA_ON = 8.0f;         // ★ 水箱-外浴热差 ≥8.0℃：可仅泵助热
static const float TANK_PUMP_DELTA_OFF = 4.0f;         // ★ 回差：<4.0℃ 停止仅泵助热
static bool  gPumpAssistWanted = false;                // ★ 本周期是否需要“仅泵助热”

// ========================= 全局状态 =========================
bool heaterIsOn = false;  // 加热器状态
bool pumpIsOn = false;  // 热水循环泵（把热水箱热水打入水浴）
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
  // 仅外浴上限硬保护；in_max 只用于归一化
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
  // 9. 泵连续运行上限（毫秒，用于寿命保护）
  if (obj.containsKey("pump_max_duration")) appConfig.pumpMaxDuration = obj["pump_max_duration"].as<uint32_t>();
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
      heaterOn(); heaterIsOn = true; heaterToggleMs = millis();
      pumpTailUntilMs = 0; // 开热时清理尾流窗口
      if (pcmd.duration > 0) heaterManualUntilMs = millis() + pcmd.duration;
      unsigned long nowMs = millis();
      bool allowAutoPump = !(pumpManualUntilMs != 0 && (long)(nowMs - pumpManualUntilMs) < 0);
      if (allowAutoPump && nowMs >= pumpCooldownUntilMs && !pumpIsOn) {
        pumpOn(); pumpIsOn = true; pumpOnStartMs = nowMs;
      }
      scheduleOff("heater", pcmd.duration);
    }
    else {
      heaterOff(); heaterIsOn = false; heaterToggleMs = millis();
      heaterManualUntilMs = 0;
      // 开启尾流窗口（由 pumpProtectionTick 统一处理）
      pumpTailUntilMs = millis() + PUMP_TAIL_MS;
      unsigned long nowMs = millis();
      bool allowAutoPump = !(pumpManualUntilMs != 0 && (long)(nowMs - pumpManualUntilMs) < 0);
      if (allowAutoPump && nowMs >= pumpCooldownUntilMs && !pumpIsOn) {
        pumpOn(); pumpIsOn = true; pumpOnStartMs = nowMs;
        Serial.println("[PumpTail] 加热关闭，进入尾流阶段：泵已开启");
      }
    }
  }
  else if (pcmd.cmd == "pump") {
    if (pcmd.action == "on") {
      pumpOn(); pumpIsOn = true; pumpOnStartMs = millis();
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

// ========================= 泵连续运行保护（每轮测控都检查） =========================
void pumpProtectionTick() {
  unsigned long nowMs = millis();

  // (a) 连续运行超时 → 进入冷却期
  if (pumpIsOn && appConfig.pumpMaxDuration > 0 &&
    (nowMs - pumpOnStartMs >= appConfig.pumpMaxDuration)) {
    pumpOff(); pumpIsOn = false;
    pumpCooldownUntilMs = nowMs + PUMP_COOLDOWN_MS;
    pumpManualUntilMs = 0;
    Serial.printf("[PumpProtect] 连续运行超时（≥ %lu ms），进入冷却期 %lu ms\n",
      (unsigned long)appConfig.pumpMaxDuration, (unsigned long)PUMP_COOLDOWN_MS);
  }

  // (b) 冷却后、若需要加热 → 自动恢复泵
  bool allowAutoPump = !(pumpManualUntilMs != 0 && (long)(nowMs - pumpManualUntilMs) < 0);
  if (heaterIsOn && allowAutoPump && !pumpIsOn && nowMs >= pumpCooldownUntilMs) {
    pumpOn(); pumpIsOn = true; pumpOnStartMs = nowMs;
    Serial.println("[PumpProtect] 冷却期结束且需要加热，自动恢复泵");
  }

  // (c) 加热关闭时的尾流/水箱助热控制（与冷却期&软锁并行约束）
  if (!heaterIsOn && allowAutoPump) {
    bool inTail = (pumpTailUntilMs != 0 && (long)(nowMs - pumpTailUntilMs) < 0);
    if ((inTail || gPumpAssistWanted)) {
      if (!pumpIsOn && nowMs >= pumpCooldownUntilMs) {
        pumpOn(); pumpIsOn = true; pumpOnStartMs = nowMs;
        Serial.println("[PumpAssist] 尾流或水箱热差驱动：自动开启泵");
      }
    }
    else {
      if (pumpIsOn) {
        pumpOff(); pumpIsOn = false;
        Serial.println("[PumpAssist] 不在尾流且无水箱热差驱动：自动关闭泵");
      }
      if (!inTail) pumpTailUntilMs = 0; // 清理尾流窗口
    }
  }
}

// ========================= 主测量 + 控制 + 上报 =========================
bool doMeasurementAndSave() {
  Serial.println("[Measure] 采集温度");

  float t_in = readTempIn();                 // 核心温度（内部）
  std::vector<float> t_outs = readTempOut();   // 外浴多个探头
  float t_tank = readTempTank();               // 水箱温度（用于控制与上报info）

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
  float delta_tank_out = tankValid ? (t_tank - med_out) : 0.0f; // 水箱-外浴热差

  String ts = getTimeString();
  time_t nowEpoch = time(nullptr);

  // 配置快捷变量
  const float out_max = (float)appConfig.tempLimitOutMax;
  const float out_min = (float)appConfig.tempLimitOutMin;
  const float in_max = (float)appConfig.tempLimitInMax;  // 仅用于归一化，不做硬切断
  const float in_min = (float)appConfig.tempLimitInMin;

  float diff_now = t_in - med_out;

  // ---- 外浴硬保护 ----
  bool hardCool = false;
  String msg;
  if (med_out >= out_max) {
    hardCool = true;
    msg = String("[SAFETY] 外部温度 ") + String(med_out, 2) +
      " ≥ " + String(out_max, 2) + "，强制冷却（关加热+关泵）";
  }

  // ---- n-curve + 前瞻 + 回差（不触发外浴硬切时）----
  bool wantHeat = false; // 是否希望开启加热
  String reason;
  if (!hardCool) {
    if (t_in <= in_min) {
      wantHeat = true;
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
          wantHeat = true;
          reason = String("pred_diff=") + String(diff_pred, 2) +
            " > on_thr " + String(DIFF_ON_MAX, 2) +
            " | slope=" + String(slope_cpm, 2);
        }
        else {
          wantHeat = false;
          reason = String("pred_diff=") + String(diff_pred, 2) +
            " ≤ on_thr " + String(DIFF_ON_MAX, 2) +
            " | slope=" + String(slope_cpm, 2);
        }
      }
      else {
        if (diff_pred <= DIFF_OFF_MAX) {
          wantHeat = false;
          reason = String("pred_diff=") + String(diff_pred, 2) +
            " ≤ off_thr " + String(DIFF_OFF_MAX, 2) +
            " | slope=" + String(slope_cpm, 2);
        }
        else {
          wantHeat = true;
          reason = String("pred_diff=") + String(diff_pred, 2) +
            " > off_thr " + String(DIFF_OFF_MAX, 2) +
            " | slope=" + String(slope_cpm, 2);
        }
      }
    }

    // 水箱上限优先：强制停热（不受最小开机时间抑制）
    if (tankOver) {
      wantHeat = false;
      reason += " | Tank≥80℃：强制停热";
    }

    // 尊重手动加热软锁
    unsigned long nowMs = millis();
    bool heaterManualActive = (heaterManualUntilMs != 0 && (long)(nowMs - heaterManualUntilMs) < 0);
    if (heaterManualActive) {
      reason += " | 手动加热锁生效";
      wantHeat = heaterIsOn; // 手动期：自动逻辑不改变 heater 状态
    }

    // ★★★ 水箱足够热 → 不加热，只泵水（仅在非手动加热期、且原本需要补热时）★★★
    // 回差：上一周期若已在“仅泵助热”，则使用更低的 OFF 阈值退出
    static bool lastAssist = false; // 仅泵助热的回差状态
    bool assistNow = false;
    gPumpAssistWanted = false;      // 默认不助热

    if (!heaterManualActive && !tankOver && wantHeat && tankValid) {
      float onThr = TANK_PUMP_DELTA_ON;
      float offThr = TANK_PUMP_DELTA_OFF;
      if (!lastAssist) assistNow = (delta_tank_out >= onThr);
      else             assistNow = (delta_tank_out >= offThr);

      if (assistNow) {
        // 覆盖为“停热，仅泵”模式，并且跳过最小开/停机抑制
        wantHeat = false;
        reason += String(" | tankΔ=") + String(delta_tank_out, 1) +
          "℃≥" + String(onThr, 1) + "℃ → 仅泵助热";
      }
      gPumpAssistWanted = assistNow;
      lastAssist = assistNow;
    }
    else {
      // 不满足条件则根据回差退出仅泵（若上周期为真，这里置 false）
      gPumpAssistWanted = false;
      static bool initOnce = false; // 防止编译器告警
      (void)initOnce;
    }

    // 最小开/停机时间抑制（水箱过温或仅泵助热时跳过抑制，保证策略立即生效）
    bool skipMinTime = tankOver || gPumpAssistWanted;
    if (!skipMinTime) {
      if (wantHeat && !heaterIsOn && (nowMs - heaterToggleMs) < HEATER_MIN_OFF_MS) {
        wantHeat = false;
        reason += " | 抑制：未到最小关断间隔";
      }
      if (!wantHeat && heaterIsOn && (nowMs - heaterToggleMs) < HEATER_MIN_ON_MS) {
        wantHeat = true;
        reason += " | 抑制：未到最小开机间隔";
      }
    }
  }

  // ---- 执行动作（泵的保护/尾流/助热由 pumpProtectionTick 统一收口）----
  unsigned long nowMs2 = millis();
  if (hardCool) {
    if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2; }
    if (pumpIsOn) { pumpOff();   pumpIsOn = false; }
    pumpManualUntilMs = 0;
    pumpCooldownUntilMs = 0;
    pumpTailUntilMs = 0; // 硬保护触发：尾流无条件取消
    gPumpAssistWanted = false;
  }
  else {
    if (wantHeat && !heaterIsOn) {
      heaterOn(); heaterIsOn = true; heaterToggleMs = nowMs2;
      pumpTailUntilMs = 0; // 开热时清尾流
      bool allowAutoPump = !(pumpManualUntilMs != 0 && (long)(nowMs2 - pumpManualUntilMs) < 0);
      if (allowAutoPump && nowMs2 >= pumpCooldownUntilMs && !pumpIsOn) {
        pumpOn(); pumpIsOn = true; pumpOnStartMs = nowMs2;
      }
    }
    else if (!wantHeat && heaterIsOn) {
      heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs2;
      pumpTailUntilMs = nowMs2 + PUMP_TAIL_MS; // 停热→尾流
      bool allowAutoPump = !(pumpManualUntilMs != 0 && (long)(nowMs2 - pumpManualUntilMs) < 0);
      if (allowAutoPump && nowMs2 >= pumpCooldownUntilMs && !pumpIsOn) {
        pumpOn(); pumpIsOn = true; pumpOnStartMs = nowMs2;
        Serial.println("[PumpTail] 加热关闭，进入尾流阶段：泵已开启");
      }
    }
    // 状态未变：由 pumpProtectionTick 维持
  }

  // ===== 泵保护和曝气定时 =====
  pumpProtectionTick();
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
    if (i < appConfig.keyTempOut.size()) obj["key"] = appConfig.keyTempOut[i];
    else if (!appConfig.keyTempOut.empty()) obj["key"] = appConfig.keyTempOut[0] + "_X" + String(i);
    else obj["key"] = String("temp_out_") + String(i);
    obj["value"] = t_outs[i];
    obj["measured_time"] = ts;
  }

  // 水箱温度不上报 key；放入 info 字段
  doc["info"]["tank_temp"] = tankValid ? t_tank : NAN;
  doc["info"]["tank_over"] = tankOver;
  doc["info"]["tank_out_delta"] = tankValid ? delta_tank_out : NAN;
  doc["info"]["pump_assist"] = gPumpAssistWanted; // 是否处于“仅泵助热”

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

  xTaskCreatePinnedToCore(measurementTask, "MeasureTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandTask, "CommandTask", 4096, NULL, 1, NULL, 1);

  Serial.println("[System] 启动完成");
}

// ========================= 主循环 =========================
void loop() {
  maintainMQTT(5000);
  // 如需更“实时”地处理尾流/冷却/助热，可在此额外调用：
  // pumpProtectionTick();
  delay(100);
}
