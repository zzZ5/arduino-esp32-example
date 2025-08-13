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

// ========================= 持久化存储设置 =========================
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";              // NVS 命名空间
static const char* NVS_KEY_LAST_MEAS = "lastMeas";        // 上次测量时间的 key（秒级）
static const char* NVS_KEY_LAST_AERATION = "lastAer";     // 上次曝气时间的 key（秒级）
static unsigned long prevMeasureMs = 0;                   // 上次测量的毫秒时间戳
static unsigned long preAerationMs = 0;                   // 上次曝气的毫秒时间戳（定时器相位参考）

// ========================= 命令队列结构体 =========================
struct PendingCommand {
  String cmd;              // "aeration"、"heater"、"pump"、或 "config_update"
  String action;           // "on" / "off"
  unsigned long duration;  // 持续时间（毫秒），0 表示无自动 off
  time_t targetTime;       // 预定执行时间（Unix 时间戳，秒）
};
std::vector<PendingCommand> pendingCommands;


// ========================= 泵/加热防抖 & 软锁控制变量 =========================
unsigned long pumpStartMs = 0;                 // 最近一次泵开启的时间（用于余温循环）
static const unsigned long HEATER_MIN_ON_MS = 30000; // 加热器最短连续开启 30s
static const unsigned long HEATER_MIN_OFF_MS = 30000; // 加热器最短连续关闭 30s
static unsigned long heaterToggleMs = 0;               // 最近一次加热器状态切换时间戳
static unsigned long heaterLastOnMs = 0;               // 最近一次进入“加热开启”时刻
static unsigned long aerationManualUntilMs = 0;        // 手动曝气软锁截止时间（ms），到期前定时器不接管


// ========================= 全局控制状态变量 =========================
bool heaterIsOn = false;     // 加热器状态
bool pumpIsOn = false;       // 循环泵状态（用于水浴循环/余温循环）
bool aerationIsOn = false;   // 曝气状态（反应曝气）

// ========================= 工具函数：鲁棒中位数 =========================
// outlierThreshold > 0 时启用以“中位数”为中心的离群剔除（典型 3~5℃）
// minValid/maxValid 用于过滤坏值（如 -127℃、85℃ 等）
float median(std::vector<float> values,
  float minValid = -20.0f,
  float maxValid = 100.0f,
  float outlierThreshold = -1.0f) {
  // 1) 去掉 NaN 与超范围值
  values.erase(std::remove_if(values.begin(), values.end(), [&](float v) {
    return isnan(v) || v < minValid || v > maxValid;
    }), values.end());
  if (values.empty()) return NAN;

  // 2) 可选：按初步中位数做离群剔除
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

  // 3) 最终中位数
  mid = values.size() / 2;
  return (values.size() % 2 == 0) ? (values[mid - 1] + values[mid]) / 2.0f : values[mid];
}

// ========================= 配置更新函数 =========================
bool updateAppConfigFromJson(JsonObject obj) {
  // 1. WiFi 参数
  if (obj.containsKey("wifi")) {
    JsonObject wifi = obj["wifi"];
    if (wifi.containsKey("ssid"))     appConfig.wifiSSID = wifi["ssid"].as<String>();
    if (wifi.containsKey("password")) appConfig.wifiPass = wifi["password"].as<String>();
  }

  // 2. MQTT 参数
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

  // 3. NTP 服务器（向量）
  if (obj.containsKey("ntp_host") && obj["ntp_host"].is<JsonArray>()) {
    JsonArray ntpArr = obj["ntp_host"].as<JsonArray>();
    appConfig.ntpServers.clear();
    for (JsonVariant v : ntpArr) appConfig.ntpServers.push_back(v.as<String>());
  }

  // 4. 控制参数
  if (obj.containsKey("post_interval")) appConfig.postInterval = obj["post_interval"].as<uint32_t>();
  if (obj.containsKey("temp_maxdif"))   appConfig.tempMaxDiff = obj["temp_maxdif"].as<uint32_t>();

  // 5. 温度限制参数（说明：这里只保留 out_max 的硬保护；in_max 只参与归一化，不做硬切断）
  if (obj.containsKey("temp_limitout_max")) appConfig.tempLimitOutMax = obj["temp_limitout_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitout_min")) appConfig.tempLimitOutMin = obj["temp_limitout_min"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_max"))  appConfig.tempLimitInMax = obj["temp_limitin_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_min"))  appConfig.tempLimitInMin = obj["temp_limitin_min"].as<uint32_t>();

  // 6. 设备编号
  if (obj.containsKey("equipment_key")) appConfig.equipmentKey = obj["equipment_key"].as<String>();

  // 7. 传感器 key
  if (obj.containsKey("keys")) {
    JsonObject keys = obj["keys"];
    if (keys.containsKey("temp_in")) appConfig.keyTempIn = keys["temp_in"].as<String>();
    if (keys.containsKey("temp_out") && keys["temp_out"].is<JsonArray>()) {
      appConfig.keyTempOut.clear();
      for (JsonVariant v : keys["temp_out"].as<JsonArray>()) {
        appConfig.keyTempOut.push_back(v.as<String>());
      }
    }
  }

  // 8. 曝气定时器参数
  if (obj.containsKey("aeration_timer")) {
    JsonObject aer = obj["aeration_timer"];
    if (aer.containsKey("enabled"))  appConfig.aerationTimerEnabled = aer["enabled"].as<bool>();
    if (aer.containsKey("interval")) appConfig.aerationInterval = aer["interval"].as<uint32_t>();
    if (aer.containsKey("duration")) appConfig.aerationDuration = aer["duration"].as<uint32_t>();
  }

  // 9. 水泵余温循环最长时长
  if (obj.containsKey("pump_max_duration")) appConfig.pumpMaxDuration = obj["pump_max_duration"].as<uint32_t>();

  return true;
}


// ========================= MQTT 消息回调 =========================
// 支持命令数组：[{command, action, duration(ms), schedule("YYYY-mm-dd HH:MM:SS"), ...}]
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

    // 配置更新命令：实时应用并保存，然后重启
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
      continue; // 跳过后续排程
    }

    // 解析 schedule（如果有），否则用当前时间
    time_t target = time(nullptr);
    if (schedule.length() > 0) {
      struct tm schedTime = {};
      // 注意：某些工具链不支持 strptime
      if (strptime(schedule.c_str(), "%Y-%m-%d %H:%M:%S", &schedTime)) {
        target = mktime(&schedTime);
      }
      else {
        Serial.println("[MQTT] 错误的时间格式（期望 YYYY-MM-DD HH:MM:SS）");
        continue;
      }
    }

    pendingCommands.push_back({ cmd, action, duration, target });
  }
}

// ========================= 非阻塞命令执行 =========================
// “打开 + 安排定时关闭”为一对儿；不使用 delay()，避免阻塞任务
void executeCommand(const PendingCommand& pcmd) {
  Serial.printf("[CMD] 执行：%s %s 持续 %lu ms\n",
    pcmd.cmd.c_str(), pcmd.action.c_str(), pcmd.duration);

  auto scheduleOff = [&](const String& what, unsigned long ms) {
    if (ms == 0) return; // 0 表示不自动关闭
    PendingCommand offCmd;
    offCmd.cmd = what;
    offCmd.action = "off";
    offCmd.duration = 0;
    offCmd.targetTime = time(nullptr) + (time_t)(ms / 1000UL);
    pendingCommands.push_back(offCmd);
    Serial.printf("[CMD] 已计划 %s 定时关闭：%lu ms 后\n", what.c_str(), ms);
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
      heaterOn(); heaterIsOn = true;
      heaterToggleMs = millis();
      heaterLastOnMs = heaterToggleMs;
      // 加热时泵常开以提高换热效率
      if (!pumpIsOn) { pumpOn(); pumpIsOn = true; }
      scheduleOff("heater", pcmd.duration);
    }
    else {
      heaterOff(); heaterIsOn = false;
      heaterToggleMs = millis();
      // 进入余温循环窗口
      if (!pumpIsOn) { pumpOn(); pumpIsOn = true; }
      pumpStartMs = millis();
    }
  }
  else if (pcmd.cmd == "pump") {
    if (pcmd.action == "on") {
      pumpOn(); pumpIsOn = true; pumpStartMs = millis();
      scheduleOff("pump", pcmd.duration);
    }
    else {
      pumpOff(); pumpIsOn = false;
    }
  }
  else {
    Serial.println("[CMD] 未知命令：" + pcmd.cmd);
  }
}

// ========================= 定时曝气控制 =========================
void checkAndControlAerationByTimer() {
  if (!appConfig.aerationTimerEnabled) return;

  // 手动软锁未过期 → 暂停定时器接管，避免打架
  if (aerationManualUntilMs != 0 && (long)(millis() - aerationManualUntilMs) < 0) return;

  unsigned long nowMs = millis();
  time_t nowEpoch = time(nullptr);

  // 状态1：未在曝气中，检查是否应启动曝气
  if (!aerationIsOn && (nowMs - preAerationMs >= appConfig.aerationInterval)) {
    Serial.printf("[Aeration] 到达曝气时间，开始曝气 %lu ms\n", appConfig.aerationDuration);
    aerationOn();
    aerationIsOn = true;
    preAerationMs = nowMs;  // 作为本轮开始
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_AERATION, nowEpoch);
      preferences.end();
    }
  }

  // 状态2：正在曝气中，检查是否应停止
  if (aerationIsOn && (nowMs - preAerationMs >= appConfig.aerationDuration)) {
    Serial.println("[Aeration] 曝气时间到，停止曝气");
    aerationOff();
    aerationIsOn = false;
    preAerationMs = nowMs;  // 作为关相位起点
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_AERATION, nowEpoch);
      preferences.end();
    }
  }
}


// ========================= 主测量 + 控制 + 上报 =========================
bool doMeasurementAndSave() {
  Serial.println("[Measure] 采集温度");

  float t_in = readTempIn();                 // 核心温度（内部）
  std::vector<float> t_outs = readTempOut(); // 外浴多个探头

  // 外浴空读保护：若全部失败，跳过本轮控制（避免错误动作）
  if (t_outs.empty()) {
    Serial.println("[Measure] 外部温度读数为空，跳过本轮控制");
    return false;
  }

  // 鲁棒中位数：-20~100℃范围，-1.0不开启离群剔除（可根据现场噪声改为 3~5）
  float med_out = median(t_outs, -20.0f, 100.0f, -1.0f);
  if (isnan(med_out)) {
    Serial.println("[Measure] 外部温度有效值为空，跳过本轮控制");
    return false;
  }

  String ts = getTimeString();               // 时间字符串
  time_t nowEpoch = time(nullptr);           // Unix 时间戳

  // 配置快捷变量
  const float out_max = (float)appConfig.tempLimitOutMax;
  const float out_min = (float)appConfig.tempLimitOutMin;
  const float in_max = (float)appConfig.tempLimitInMax;  // 仅用于归一化，不做硬切断
  const float in_min = (float)appConfig.tempLimitInMin;

  // 当前差值：核心 - 外浴（正数表示外浴更冷，有保温空间）
  float diff_now = t_in - med_out;

  // ---- 仅外浴上限的硬保护 ----
  bool hardCool = false;
  String msg;

  if (med_out >= out_max) {
    hardCool = true;
    msg = String("[SAFETY] 外部温度 ") + String(med_out, 2) +
      " ≥ " + String(out_max, 2) + "，强制冷却（关加热，开泵）";
  }

  // ---- n-curve 目标差值阈值（不触发硬切时才计算）----
  bool wantHeat = false; // “希望开启加热”的判定
  String reason;

  if (!hardCool) {
    if (t_in <= in_min) {
      // 低于最小内部温度 → 强制补热，确保堆体不被水浴过度带走热量
      wantHeat = true;
      reason = String("内部温度 ") + String(t_in, 2) + " ≤ " + String(in_min, 2) + "（低于最小值，强制补热）";
    }
    else {
      // 归一化 u ∈ [0,1]：t_in 越高，允许的差值越大
      float u = 0.0f;
      if (in_max > in_min) {
        float t_ref = min(max(t_in, in_min), in_max);
        u = (t_ref - in_min) / (in_max - in_min);
      }
      const float diff_max = (float)appConfig.tempMaxDiff;       // 高温端最大允许差
      const float diff_min = max(0.1f, diff_max * 0.02f);        // 低温端最小允许差
      const float n_curve = 2.0f;                               // n>1 高温放宽更明显
      float DIFF_ON_MAX = diff_min + (diff_max - diff_min) * powf(u, n_curve);

      if (diff_now > DIFF_ON_MAX) {
        wantHeat = true;  // 外浴偏冷，需补热
        reason = String("diff=") + String(diff_now, 2) + " > 阈值 " + String(DIFF_ON_MAX, 2) +
          "（外浴偏冷，需要补热）";
      }
      else {
        wantHeat = false; // 外浴够暖
        reason = String("diff=") + String(diff_now, 2) + " ≤ 阈值 " + String(DIFF_ON_MAX, 2) +
          "（外浴已足够热）";
      }
    }

    // ---- 最小开停机时间抑制抖动 ----
    unsigned long nowMs = millis();
    if (wantHeat && !heaterIsOn && (nowMs - heaterToggleMs) < HEATER_MIN_OFF_MS) {
      wantHeat = false; // 刚关不久，不允许立刻再开
      reason += " | 抑制：未到最小关断间隔";
    }
    if (!wantHeat && heaterIsOn && (nowMs - heaterToggleMs) < HEATER_MIN_ON_MS) {
      wantHeat = true;  // 刚开不久，不允许立刻再关
      reason += " | 抑制：未到最小开机间隔";
    }
  }

  // ---- 执行动作 ----
  unsigned long nowMs = millis();
  if (hardCool) {
    // 外浴过热：直接进入强制冷却（关加热，开泵）
    if (heaterIsOn) { heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs; }
    if (!pumpIsOn) { pumpOn();  pumpIsOn = true; }
    pumpStartMs = nowMs; // 从此刻开始计余温/降温
  }
  else {
    // 按 wantHeat 控制
    if (wantHeat && !heaterIsOn) {
      heaterOn(); heaterIsOn = true; heaterToggleMs = nowMs; heaterLastOnMs = nowMs;
      // 加热时泵常开提高换热效率
      if (!pumpIsOn) { pumpOn(); pumpIsOn = true; }
    }
    else if (!wantHeat && heaterIsOn) {
      heaterOff(); heaterIsOn = false; heaterToggleMs = nowMs;
      // 进入余温循环窗口（限时开泵）
      if (!pumpIsOn) { pumpOn(); pumpIsOn = true; }
      pumpStartMs = nowMs;
    }

    // 泵：加热时常开；非加热时仅在余温循环窗口内运行，超时即停
    if (heaterIsOn && !pumpIsOn) {
      pumpOn(); pumpIsOn = true;
    }
    if (!heaterIsOn && pumpIsOn && (nowMs - pumpStartMs >= appConfig.pumpMaxDuration)) {
      pumpOff(); pumpIsOn = false;
    }
  }

  // ===== 曝气定时判断 =====
  checkAndControlAerationByTimer();

  // ===== 构建 JSON 并上传 =====
  StaticJsonDocument<1024> doc;
  JsonArray data = doc.createNestedArray("data");

  JsonObject obj_in = data.createNestedObject();
  obj_in["key"] = appConfig.keyTempIn;
  obj_in["value"] = t_in;
  obj_in["measured_time"] = ts;

  for (size_t i = 0; i < t_outs.size(); ++i) {
    JsonObject obj = data.createNestedObject();
    if (i < appConfig.keyTempOut.size()) obj["key"] = appConfig.keyTempOut[i];
    else obj["key"] = appConfig.keyTempOut[0] + "_X" + String(i);
    obj["value"] = t_outs[i];
    obj["measured_time"] = ts;
  }

  // info 字段：用于可视化“判定→动作”链路
  doc["info"]["msg"] = (hardCool ? msg : (String("[Heat-nCurve] ") + reason +
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

// ========================= 命令调度任务 =========================
void commandTask(void* pv) {
  while (true) {
    time_t now = time(nullptr);
    for (int i = 0; i < (int)pendingCommands.size(); ++i) {
      if (now >= pendingCommands[i].targetTime) {
        executeCommand(pendingCommands[i]);
        pendingCommands.erase(pendingCommands.begin() + i);
        --i;
      }
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

  // 订阅下行
  getMQTTClient().setCallback(mqttCallback);
  getMQTTClient().subscribe(appConfig.mqttResponseTopic.c_str());

  // 传感器初始化（按你的引脚定义）
  if (!initSensors(4, 5, 25, 26, 27)) {
    Serial.println("[System] 传感器初始化失败，重启");
    ESP.restart();
  }

  // ==== 恢复测量/曝气相位 ====
  if (preferences.begin(NVS_NAMESPACE, /*readOnly=*/true)) {
    unsigned long lastSecMea = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    unsigned long lastSecAera = preferences.getULong(NVS_KEY_LAST_AERATION, 0);
    time_t nowSec = time(nullptr);

    // 恢复曝气相位起点（仅用时间戳，不管相位 on/off）
    if (nowSec > 0 && lastSecAera > 0) {
      unsigned long long elapsedAeraMs64 = (unsigned long long)(nowSec - lastSecAera) * 1000ULL;
      preAerationMs = millis() - (unsigned long)elapsedAeraMs64;
    }
    else {
      // 第一次运行或 NTP 未就绪 → 让“关相位”算已过完，好尽快进入首轮
      preAerationMs = millis() - appConfig.aerationInterval;
    }

    // 恢复测量起点
    if (nowSec > 0 && lastSecMea > 0) {
      unsigned long intervalSec = appConfig.postInterval / 1000UL;
      unsigned long elapsedSec = (nowSec > lastSecMea) ? (nowSec - lastSecMea) : 0UL;
      if (elapsedSec >= intervalSec) prevMeasureMs = millis() - appConfig.postInterval;  // 立刻测
      else prevMeasureMs = millis() - (appConfig.postInterval - elapsedSec * 1000UL);    // 等剩余
    }
    else {
      prevMeasureMs = millis() - appConfig.postInterval; // 上电立刻测一轮
    }
    preferences.end();
  }
  else {
    prevMeasureMs = millis() - appConfig.postInterval;
    preAerationMs = millis() - appConfig.aerationInterval;
  }

  // ==== 上线消息 ====
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

  // ==== 启动任务 ====
  xTaskCreatePinnedToCore(measurementTask, "MeasureTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandTask, "CommandTask", 4096, NULL, 1, NULL, 1);

  Serial.println("[System] 启动完成");
}

// ========================= 主循环 =========================
void loop() {
  maintainMQTT(5000);
  delay(100);
}