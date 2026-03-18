// main.cpp
// 负责设备启动、配置同步、MQTT 指令处理、定时采样与数据上报。
// 当前传感器组合:
// - MH-Z16: CO2
// - ZCE04B: CO / H2S / O2 / CH4
// - DHT21: AirTemp / AirHumidity

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"
#include "data_buffer.h"

// ======================= 持久化 =======================
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";
static const char* NVS_KEY_LAST_MEAS = "lastMeas";
static unsigned long prevMeasureMs = 0;

static constexpr int PIN_EXHAUST = 25;
static constexpr int PIN_AERATION = 26;
static constexpr int PIN_MHZ16_RX = 18;
static constexpr int PIN_MHZ16_TX = 19;
static constexpr int PIN_ZCE04B_RX = 16;
static constexpr int PIN_ZCE04B_TX = 17;
static constexpr uint8_t PIN_DHT21 = 4;
static constexpr int MAX_PENDING_COMMANDS = 50;
static constexpr int CO2_SAMPLE_COUNT = 3;
static constexpr unsigned long COMMAND_SLICE_MS = 5000;
static constexpr unsigned long CACHE_UPLOAD_INTERVAL_MS = 30000;

// ======================= 命令队列 =======================
struct PendingCommand {
  String cmd;
  String action;
  unsigned long duration;
  time_t targetTime;
};
std::vector<PendingCommand> pendingCommands;
static SemaphoreHandle_t g_cmdMutex = nullptr;
static volatile bool g_pendingRestart = false;
static unsigned long g_restartAtMs = 0;

// =====================================================
// 工具：从 JsonVariant 读 String（空则返回 defaultVal）
// =====================================================
static String readStr(JsonVariant v, const char* defaultVal = "") {
  if (v.is<const char*>()) return String(v.as<const char*>());
  if (v.is<String>())      return v.as<String>();
  return String(defaultVal);
}

static const char* qualityOf(float value) {
  return value < 0 ? "ERR" : "OK";
}

static void appendChannel(
  String& payload,
  bool& firstChannel,
  const char* code,
  float value,
  int decimals,
  const char* unit) {
  if (!firstChannel) {
    payload += ",";
  }
  firstChannel = false;

  payload += "{";
  payload += "\"code\":\"" + String(code) + "\",";
  payload += "\"value\":" + String(value, decimals) + ",";
  payload += "\"unit\":\"" + String(unit) + "\",";
  payload += "\"quality\":\"" + String(qualityOf(value)) + "\"";
  payload += "}";
}

static void runPumpForDuration(void (*pumpOn)(), void (*pumpOff)(), unsigned long durationMs) {
  pumpOn();
  if (durationMs == 0) {
    return;
  }

  unsigned long remaining = durationMs;
  while (remaining > 0) {
    unsigned long chunk = min(remaining, COMMAND_SLICE_MS);
    delay(chunk);
    remaining -= chunk;
    yield();
  }

  pumpOff();
}

// =====================================================
// 工具：解析 schedule（"YYYY-MM-DD HH:MM:SS"），失败则返回 now
// =====================================================
static time_t parseScheduleOrNow(const String& schedule) {
  time_t target = time(nullptr);
  if (schedule.length() == 0) return target;

  struct tm tm_s {};
  if (strptime(schedule.c_str(), "%Y-%m-%d %H:%M:%S", &tm_s)) {
    target = mktime(&tm_s);
  }
  return target;
}

// =====================================================
// 生成"完整当前配置"的 JSON（用于上线/回执）
// 格式：{ "wifi": {...}, "mqtt": {...}, "ntp_servers": [...], "pump_run_time": ..., "read_interval": ... }
// =====================================================
static void fillConfigJson(JsonObject cfg) {
  // WiFi
  JsonObject wifi = cfg["wifi"].to<JsonObject>();
  wifi["ssid"] = appConfig.wifiSSID;
  wifi["password"] = appConfig.wifiPass;

  // MQTT
  JsonObject mqtt = cfg["mqtt"].to<JsonObject>();
  mqtt["server"] = appConfig.mqttServer;
  mqtt["port"] = appConfig.mqttPort;
  mqtt["user"] = appConfig.mqttUser;
  mqtt["pass"] = appConfig.mqttPass;
  mqtt["device_code"] = appConfig.deviceCode;

  // NTP servers
  JsonArray ntps = cfg["ntp_servers"].to<JsonArray>();
  for (auto& s : appConfig.ntpServers) ntps.add(s);

  // 控制参数
  cfg["pump_run_time"] = appConfig.pumpRunTime;
  cfg["read_interval"] = appConfig.readInterval;
}

// =====================================================
// 发布上线消息（带完整配置）
// topic: compostlab/v2/{device_code}/register
// =====================================================
static void publishOnlineWithConfig() {
  Serial.println("[Register] 准备发布上线消息...");

  JsonDocument doc;
  doc["schema_version"] = 2;

  // 使用公网 IP 地址
  String ipAddress = getPublicIP();
  doc["ip_address"] = ipAddress;

  String timestamp = getTimeString();
  doc["timestamp"] = timestamp;
  Serial.printf("[Register] Timestamp: %s\n", timestamp.c_str());
  Serial.printf("[Register] IP Address: %s\n", ipAddress.c_str());

  JsonObject cfg = doc["config"].to<JsonObject>();
  fillConfigJson(cfg);

  String out;
  serializeJson(doc, out);
  Serial.printf("[Register] Payload size: %d bytes\n", out.length());

  // 使用注册 topic: compostlab/v2/{device_code}/register
  String registerTopic = "compostlab/v2/" + appConfig.deviceCode + "/register";
  Serial.printf("[Register] Topic: %s\n", registerTopic.c_str());

  bool result = publishData(registerTopic, out, 10000);
  if (result) {
    Serial.println("[Register] 上线消息发布成功！");
  }
  else {
    Serial.println("[Register] 上线消息发布失败！");
  }
}

// =====================================================
// 远程配置更新：只更新指令里出现的字段，其它保持原状
// 与 config_manager.cpp 存储结构保持一致（/config.json）
// =====================================================
static bool updateAppConfigFromJson(JsonObject cfg) {

  // -------- pump_run_time / read_interval --------
  // 支持：pump_run_time, read_interval
  // 兼容：post_interval(旧习惯) -> read_interval
  if (cfg["pump_run_time"].is<uint32_t>()) {
    appConfig.pumpRunTime = cfg["pump_run_time"].as<uint32_t>();
    Serial.printf("[CFG] pump_run_time = %u\n", (unsigned)appConfig.pumpRunTime);
  }

  if (cfg["read_interval"].is<uint32_t>()) {
    appConfig.readInterval = cfg["read_interval"].as<uint32_t>();
    Serial.printf("[CFG] read_interval = %u\n", (unsigned)appConfig.readInterval);
  }
  else if (cfg["post_interval"].is<uint32_t>()) { // 兼容旧字段名
    appConfig.readInterval = cfg["post_interval"].as<uint32_t>();
    Serial.printf("[CFG] read_interval(post_interval) = %u\n", (unsigned)appConfig.readInterval);
  }

  // -------- WiFi --------
  if (cfg["wifi"].is<JsonObject>()) {
    JsonObject wifi = cfg["wifi"].as<JsonObject>();
    if (wifi["ssid"].is<String>() || wifi["ssid"].is<const char*>())
      appConfig.wifiSSID = readStr(wifi["ssid"]);
    if (wifi["password"].is<String>() || wifi["password"].is<const char*>())
      appConfig.wifiPass = readStr(wifi["password"]);
  }

  // -------- MQTT --------
  if (cfg["mqtt"].is<JsonObject>()) {
    JsonObject mqtt = cfg["mqtt"].as<JsonObject>();
    if (mqtt["server"].is<String>() || mqtt["server"].is<const char*>())
      appConfig.mqttServer = readStr(mqtt["server"]);
    if (mqtt["port"].is<uint16_t>() || mqtt["port"].is<uint32_t>())
      appConfig.mqttPort = (uint16_t)mqtt["port"].as<uint32_t>();

    if (mqtt["user"].is<String>() || mqtt["user"].is<const char*>())
      appConfig.mqttUser = readStr(mqtt["user"]);
    if (mqtt["pass"].is<String>() || mqtt["pass"].is<const char*>())
      appConfig.mqttPass = readStr(mqtt["pass"]);

    if (mqtt["clientId"].is<String>() || mqtt["clientId"].is<const char*>())
      appConfig.mqttClientId = readStr(mqtt["clientId"]);

    if (mqtt["device_code"].is<String>() || mqtt["device_code"].is<const char*>())
      appConfig.deviceCode = readStr(mqtt["device_code"]);

    // post_topic 和 response_topic 现在自动根据 device_code 生成，忽略配置中的字段
  }

  // -------- NTP servers --------
  // 支持：ntp_servers: ["a","b","c"]
  if (cfg["ntp_servers"].is<JsonArray>()) {
    JsonArray arr = cfg["ntp_servers"].as<JsonArray>();
    appConfig.ntpServers.clear();
    for (JsonVariant v : arr) {
      String s = readStr(v, "");
      if (s.length() > 0) appConfig.ntpServers.push_back(s);
    }
    Serial.printf("[CFG] ntp_servers size = %u\n", (unsigned)appConfig.ntpServers.size());
  }

  // -------- device_code 兼容旧配置格式 --------
  if (cfg["equipment_key"].is<const char*>() || cfg["equipment_key"].is<String>()) {
    appConfig.deviceCode = readStr(cfg["equipment_key"]);
  }

  return true;
}

// =====================================================
// 执行控制命令（普通控制：aeration/exhaust/restart）
// 注意：对于长时间的操作，会阻塞任务执行，建议 duration 不要太大
// =====================================================
static void executeCommand(const PendingCommand& pcmd) {
  Serial.printf("[CMD] 执行：%s %s (持续 %lu ms)\n",
    pcmd.cmd.c_str(), pcmd.action.c_str(), pcmd.duration);

  // ---- 重启 ----
  if (pcmd.cmd == "restart") {
    Serial.println("[CMD] 远程重启设备！");
    delay(300);
    ESP.restart();
    return;
  }

  // ---- 曝气继电器 ----
  if (pcmd.cmd == "aeration") {
    if (pcmd.action == "on") {
      runPumpForDuration(aerationOn, aerationOff, pcmd.duration);
    }
    else {
      aerationOff();
    }
    return;
  }

  // ---- 抽气继电器 ----
  if (pcmd.cmd == "exhaust") {
    if (pcmd.action == "on") {
      runPumpForDuration(exhaustPumpOn, exhaustPumpOff, pcmd.duration);
    }
    else {
      exhaustPumpOff();
    }
    return;
  }

  Serial.println("[CMD] 未知命令：" + pcmd.cmd);
}

// =====================================================
// MQTT 回调：统一解析 commands
//  - config_update/update_config：立即更新->保存->重启
//  - restart/aeration/exhaust：进入队列，支持 schedule/duration
// =====================================================
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println("[MQTT] 收到指令");

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("[MQTT] JSON 解析失败: ");
    Serial.println(err.c_str());
    return;
  }

  String device = readStr(doc["device"], "");
  if (device != appConfig.deviceCode) {
    Serial.println("[MQTT] 设备不匹配，忽略");
    return;
  }

  if (!doc["commands"].is<JsonArray>()) {
    Serial.println("[MQTT] commands 数组为空/不存在");
    return;
  }

  JsonArray cmds = doc["commands"].as<JsonArray>();

  for (JsonVariant v : cmds) {
    if (!v.is<JsonObject>()) continue;

    JsonObject obj = v.as<JsonObject>();
    String cmd = readStr(obj["command"], "");

    // 兼容两种命令名
    bool isCfgUpdate = (cmd == "config_update" || cmd == "update_config");
    if (isCfgUpdate) {
      if (!obj["config"].is<JsonObject>()) {
        Serial.println("[CFG] config 字段为空/不存在");
        continue;
      }

      JsonObject cfg = obj["config"].as<JsonObject>();
      Serial.println("[CFG] 更新配置中...");

      if (!updateAppConfigFromJson(cfg)) {
        Serial.println("[CFG] 配置解析失败");
        continue;
      }

      if (!saveConfigToSPIFFS("/config.json")) {
        Serial.println("[CFG] 配置保存失败");
        continue;
      }

      Serial.println("[CFG] 配置已保存,3 秒后重启生效");

      // 仅设置重启标记，避免在回调中阻塞
      g_pendingRestart = true;
      g_restartAtMs = millis() + 3000;
      return;
    }

    // 普通控制命令：action/duration/schedule
    String action = readStr(obj["action"], "");
    unsigned long dur = 0;
    if (obj["duration"].is<uint32_t>() || obj["duration"].is<uint64_t>())
      dur = obj["duration"].as<uint32_t>();

    String schedule = readStr(obj["schedule"], "");
    time_t target = parseScheduleOrNow(schedule);

    if (g_cmdMutex) {
      xSemaphoreTake(g_cmdMutex, portMAX_DELAY);
    }
    // 检查队列是否已满
    if ((int)pendingCommands.size() >= MAX_PENDING_COMMANDS) {
      Serial.println("[CMD] 命令队列已满,忽略新命令");
      if (g_cmdMutex) {
        xSemaphoreGive(g_cmdMutex);
      }
      continue;
    }
    pendingCommands.push_back({ cmd, action, dur, target });
    if (g_cmdMutex) {
      xSemaphoreGive(g_cmdMutex);
    }
  }
}

// =====================================================
// 采样与上传（包含 DHT）
// =====================================================
static bool doMeasurementAndSave() {
  Serial.println("[Measure] 开始采样");

  // 抽气
  exhaustPumpOn();
  delay(appConfig.pumpRunTime);
  exhaustPumpOff();

  // CO2 平均
  std::vector<int> co2Samples;
  co2Samples.reserve(CO2_SAMPLE_COUNT);

  for (int i = 0; i < CO2_SAMPLE_COUNT; i++) {
    int v = readMHZ16();
    if (v > 0) co2Samples.push_back(v);
    delay(200);
  }

  float co2ppm = -1;
  if (!co2Samples.empty()) {
    float sum = 0;
    for (int v : co2Samples) sum += v;
    co2ppm = sum / co2Samples.size();
  }
  float co2pct = (co2ppm > 0) ? (co2ppm / 10000.0f) : -1;

  // 其他传感器
  ZCE04BGasData gasData{};
  bool gasOk = readZCE04B(gasData);
  float co = gasOk ? gasData.co : -1.0f;
  float h2s = gasOk ? gasData.h2s : -1.0f;
  float o2 = gasOk ? gasData.o2 : -1.0f;
  float ch4 = gasOk ? gasData.ch4 : -1.0f;

  DHT21Data dhtData{};
  bool dhtOk = readDHT21(dhtData);
  float t_air = dhtOk ? dhtData.temperature : -1.0f;
  float h_air = dhtOk ? dhtData.humidity : -1.0f;

  String ts = getTimeString();
  time_t nowEpoch = time(nullptr);

  // 新格式：{ "schema_version": 2, "ts": "...", "channels": [ ... ] }
  String payload = "{";
  payload += "\"schema_version\":2,";
  payload += "\"ts\":\"" + ts + "\",";
  payload += "\"channels\":[";
  bool firstChannel = true;
  appendChannel(payload, firstChannel, "CO2", co2pct, 2, "%VOL");
  appendChannel(payload, firstChannel, "CO", co, 1, "ppm");
  appendChannel(payload, firstChannel, "H2S", h2s, 1, "ppm");
  appendChannel(payload, firstChannel, "O2", o2, 2, "%VOL");
  appendChannel(payload, firstChannel, "CH4", ch4, 1, "%LEL");
  appendChannel(payload, firstChannel, "AirTemp", t_air, 1, "C");
  appendChannel(payload, firstChannel, "AirHumidity", h_air, 1, "%RH");

  payload += "]}";

  // 使用带缓存功能的发布
  if (publishDataOrCache(appConfig.mqttPostTopic(), payload, ts, 10000)) {
    // 上传成功，更新NVS中的上次测量时间
    preferences.begin(NVS_NAMESPACE, false);
    preferences.putULong(NVS_KEY_LAST_MEAS, (unsigned long)nowEpoch);
    preferences.end();

    // 上传成功后，尝试上传缓存的数据（最多10条）
    int uploaded = uploadCachedData(10);
    if (uploaded > 0) {
      Serial.printf("[Measure] Uploaded %d cached data items\n", uploaded);
    }

    Serial.println("[Measure] 上传成功");
    return true;
  }

  Serial.println("[Measure] 上传失败，数据已缓存");
  return false;
}

// =====================================================
// 任务：定时测量
// =====================================================
static void measurementTask(void*) {
  // 启动后延迟，确保与上线消息之间至少间隔 1000ms
  delay(1000);

  while (true) {
    // 使用无符号差值,避免 millis 溢出问题
    if ((millis() - prevMeasureMs) >= appConfig.readInterval) {
      prevMeasureMs = millis();

      // 重试机制：最多尝试 3 次
      for (int retry = 0; retry < 3; retry++) {
        if (doMeasurementAndSave()) {
          break;  // 成功则退出重试
        }
        Serial.printf("[Measure] Retry %d failed, waiting 3s...\n", retry + 1);
        delay(3000);  // 减少延迟时间
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);  // 增加检查频率
  }
}

// =====================================================
// 任务：执行队列命令
// =====================================================
static void commandTask(void*) {
  while (true) {
    time_t now = time(nullptr);
    std::vector<PendingCommand> dueCommands;
    if (g_cmdMutex) {
      xSemaphoreTake(g_cmdMutex, portMAX_DELAY);
    }
    for (int i = 0; i < (int)pendingCommands.size();) {
      if (now >= pendingCommands[i].targetTime) {
        dueCommands.push_back(pendingCommands[i]);
        pendingCommands.erase(pendingCommands.begin() + i);
        continue;
      }
      i++;
    }
    if (g_cmdMutex) {
      xSemaphoreGive(g_cmdMutex);
    }
    for (auto& cmd : dueCommands) {
      executeCommand(cmd);
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// =====================================================
// setup
// =====================================================
void setup() {
  Serial.begin(115200);
  Serial.println("[System] 启动中...");
  if (!g_cmdMutex) {
    g_cmdMutex = xSemaphoreCreateMutex();
    if (!g_cmdMutex) {
      Serial.println("[CMD] Failed to create command mutex");
    }
  }

  // 1) SPIFFS + 读取 config.json
  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[System] 配置加载失败，重启");
    ESP.restart();
  }

  // 2) 初始化数据缓存模块
  if (!initDataBuffer(200, 7)) {
    Serial.println("[System] 数据缓存模块初始化失败，继续运行...");
  }
  else {
    Serial.println("[System] 数据缓存模块初始化成功");
  }

  // 3) WiFi + NTP
  if (!connectToWiFi(20000) || !multiNTPSetup(20000)) {
    Serial.println("[System] WiFi/NTP 失败，重启");
    ESP.restart();
  }

  // 4) MQTT
  if (!connectToMQTT(20000)) {
    Serial.println("[System] MQTT 连接失败，重启");
    ESP.restart();
  }

  getMQTTClient().setCallback(mqttCallback);

  // 订阅 response topic（根据 device_code 自动生成）
  String respTopic = appConfig.mqttResponseTopic();
  if (respTopic.length() > 0) {
    getMQTTClient().subscribe(respTopic.c_str());
    Serial.println("[MQTT] Subscribed: " + respTopic);
  }
  else {
    Serial.println("[MQTT] response_topic 为空，无法订阅");
  }

  // 5) 传感器初始化
  // 这里默认使用:
  // MH-Z16 -> Serial1 RX=16 TX=17
  // ZCE04B -> Serial2 RX=32 TX=33
  // DHT21  -> GPIO4
  if (!initSensorAndPump(
    PIN_EXHAUST,
    PIN_AERATION,
    Serial1,
    PIN_MHZ16_RX,
    PIN_MHZ16_TX,
    Serial2,
    PIN_ZCE04B_RX,
    PIN_ZCE04B_TX,
    PIN_DHT21,
    5000)) {
    Serial.println("[ERR] 传感器初始化失败，重启");
    ESP.restart();
  }

  // 预热 MH-Z16
  readMHZ16();
  delay(500);

  // 6) 发布上线消息（先告知服务器设备已上线）
  Serial.println("[System] 发布上线消息...");
  publishOnlineWithConfig();
  delay(500);  // 等待上线消息完全发送

  // 7) 上传之前缓存的旧数据（如果有）
  int pendingCount = uploadCachedData(10);
  if (pendingCount > 0) {
    Serial.printf("[System] 上传了 %d 条缓存的历史数据\n", pendingCount);
  }

  // 8) 恢复上次测量时间（确保重启后按照间隔时间测量）
  if (preferences.begin(NVS_NAMESPACE, false)) {
    unsigned long lastSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    time_t nowSec = time(nullptr);

    if (lastSec > 0 && nowSec > (time_t)lastSec) {
      // 计算上次测量到现在经过的时间
      unsigned long elapsedMs = (unsigned long)(nowSec - (time_t)lastSec) * 1000UL;

      if (elapsedMs >= appConfig.readInterval) {
        // 已超过间隔，立即测量
        Serial.println("[Time] Interval exceeded, measuring immediately...");
        doMeasurementAndSave();
        // 将 prevMeasureMs 设置为应该上一次测量的时间点，确保下次测量间隔正确
        prevMeasureMs = millis() - elapsedMs;
        unsigned long nextDelayMs = appConfig.readInterval - (elapsedMs % appConfig.readInterval);
        Serial.printf("[Time] Next measure in %lu ms\n", nextDelayMs);
      }
      else {
        // 还未到间隔，等待剩余时间
        prevMeasureMs = millis() - elapsedMs;
        unsigned long remainingMs = appConfig.readInterval - elapsedMs;
        Serial.printf("[Time] Wait %lu ms until next measure\n", remainingMs);
      }
    }
    else {
      // 没有上次记录或时间异常，从现在开始
      prevMeasureMs = millis();
      Serial.println("[Time] No previous measure record, starting fresh");
    }
    preferences.end();
  }
  else {
    // NVS 失败，从现在开始
    prevMeasureMs = millis();
  }

  // 9) 启动任务
  xTaskCreatePinnedToCore(measurementTask, "Measure", 16384, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandTask, "Command", 8192, NULL, 1, NULL, 1);

  Serial.println("[System] 初始化完成");
}

// =====================================================
// loop
// =====================================================
void loop() {
  maintainMQTT(30000);  // 增加超时时间，给网络更多恢复时间
  if (g_pendingRestart && (int32_t)(millis() - g_restartAtMs) >= 0) {
    ESP.restart();
  }
  static unsigned long lastCacheUploadMs = 0;
  unsigned long now = millis();
  if (now - lastCacheUploadMs >= CACHE_UPLOAD_INTERVAL_MS) {
    lastCacheUploadMs = now;
    int uploaded = uploadCachedData(10);
    if (uploaded > 0) {
      Serial.printf("[Loop] Uploaded %d cached data items\n", uploaded);
    }
  }
  delay(100);
}
