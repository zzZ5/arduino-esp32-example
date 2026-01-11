// main.cpp  (ESP32 + Arduino + PlatformIO)
//
// 目标：
// 1) 与 config_manager.cpp 的字段/含义保持一致：pump_run_time、read_interval、mqtt(post_topic/response_topic)、ntp_servers、keys( CO2/O2/RoomTemp/Mois/AirTemp/AirHumidity )
// 2) MQTT 指令统一入口：restart / aeration / exhaust / config_update
// 3) 第一次连接 MQTT 后发布上线消息，同时把“完整当前配置”一并上传
// 4) config_update 只更新指令里出现的字段，其它保持原状；保存到 /config.json 后重启生效
//
// 依赖：
// - config_manager.h/.cpp（你给的版本）
// - wifi_ntp_mqtt.h/.cpp（提供 connectToWiFi/multiNTPSetup/connectToMQTT/publishData/maintainMQTT/getMQTTClient 等）
// - sensor.h/.cpp（提供 initSensorAndPump/readMHZ16/readEOxygen/readDS18B20/readFDS100/readDHT22Temp/readDHT22Hum/aerationOn/aerationOff/exhaustPumpOn/exhaustPumpOff）
// - log_manager.h/.cpp（可选）
//
// 注意：
// - 你之前遇到的 ArduinoJson v7 报错（doc["device"] != String）已修复：改为先 as<String>() 再比较
// - 兼容 config_update / update_config 两种命令名
// - 兼容 read_interval / post_interval 两种字段名（最终统一存 read_interval）
// - keys 支持两种写法：
//      A) 与 config_manager.cpp 一致：keys: { "CO2": "...", "O2": "...", "RoomTemp": "...", ... }
//      B) 兼容你之前习惯的小写：keys: { "co2": "...", "o2": "...", "temp": "...", ... }

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include <ArduinoJson.h>

#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"
#include "log_manager.h"

// ======================= 持久化 =======================
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";
static const char* NVS_KEY_LAST_MEAS = "lastMeas";
static unsigned long prevMeasureMs = 0;

// ======================= 命令队列 =======================
struct PendingCommand {
  String cmd;
  String action;
  unsigned long duration;
  time_t targetTime;
};
std::vector<PendingCommand> pendingCommands;
static const int MAX_PENDING_COMMANDS = 50;  // 最大命令队列长度

// =====================================================
// 工具：从 JsonVariant 读 String（空则返回 defaultVal）
// =====================================================
static String readStr(JsonVariant v, const char* defaultVal = "") {
  if (v.is<const char*>()) return String(v.as<const char*>());
  if (v.is<String>())      return v.as<String>();
  return String(defaultVal);
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
// 生成“完整当前配置”的 JSON（用于上线/回执）
// =====================================================
static void fillConfigJson(JsonObject cfg) {
  // WiFi
  cfg["wifi_ssid"] = appConfig.wifiSSID;

  // MQTT
  cfg["mqtt_server"] = appConfig.mqttServer;
  cfg["mqtt_port"] = appConfig.mqttPort;
  cfg["mqtt_user"] = appConfig.mqttUser;
  cfg["mqtt_clientId"] = appConfig.mqttClientId;
  cfg["post_topic"] = appConfig.mqttPostTopic();
  cfg["response_topic"] = appConfig.mqttResponseTopic();

  // NTP servers
  JsonArray ntps = cfg["ntp_servers"].to<JsonArray>();
  for (auto& s : appConfig.ntpServers) ntps.add(s);

  // 控制参数
  cfg["pump_run_time"] = appConfig.pumpRunTime;
  cfg["read_interval"] = appConfig.readInterval;


  // 设备标识
  cfg["equipment_key"] = appConfig.equipmentKey;
}

// =====================================================
// 发布上线消息（带完整配置）
// =====================================================
static void publishOnlineWithConfig() {
  JsonDocument doc;
  doc["device"] = appConfig.equipmentKey;
  doc["status"] = "online";
  doc["timestamp"] = getTimeString();

  JsonObject cfg = doc["config"].to<JsonObject>();
  fillConfigJson(cfg);

  String out;
  serializeJson(doc, out);

  publishData(appConfig.mqttPostTopic(), out, 10000);
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

    // post_topic 和 response_topic 现在自动根据 equipment_key 生成，忽略配置中的字段
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

  // -------- equipment_key（一般不建议改，但仍支持）--------
  if (cfg["equipment_key"].is<const char*>() || cfg["equipment_key"].is<String>()) {
    appConfig.equipmentKey = readStr(cfg["equipment_key"]);
  }

  return true;
}

// =====================================================
// 执行控制命令（普通控制：aeration/exhaust/restart）
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
      aerationOn();
      if (pcmd.duration > 0) {
        delay(pcmd.duration);
        aerationOff();
      }
    }
    else {
      aerationOff();
    }
    return;
  }

  // ---- 抽气继电器 ----
  if (pcmd.cmd == "exhaust") {
    if (pcmd.action == "on") {
      exhaustPumpOn();
      if (pcmd.duration > 0) {
        delay(pcmd.duration);
        exhaustPumpOff();
      }
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
  if (device != appConfig.equipmentKey) {
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

      // 非阻塞延迟重启
      unsigned long restartStart = millis();
      while (millis() - restartStart < 3000) {
        maintainMQTT(100);  // 保持 MQTT 连接
        delay(100);
      }
      ESP.restart();
      return;
    }

    // 普通控制命令：action/duration/schedule
    String action = readStr(obj["action"], "");
    unsigned long dur = 0;
    if (obj["duration"].is<uint32_t>() || obj["duration"].is<uint64_t>())
      dur = obj["duration"].as<uint32_t>();

    String schedule = readStr(obj["schedule"], "");
    time_t target = parseScheduleOrNow(schedule);

    // 检查队列是否已满
    if ((int)pendingCommands.size() >= MAX_PENDING_COMMANDS) {
      Serial.println("[CMD] 命令队列已满,忽略新命令");
      continue;
    }

    pendingCommands.push_back({ cmd, action, dur, target });
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
  const int sampleCount = 3;
  std::vector<int> co2Samples;
  co2Samples.reserve(sampleCount);

  for (int i = 0; i < sampleCount; i++) {
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
  float o2 = readEOxygen();
  float t_ds = readDS18B20();
  float mois = readFDS100(34);

  float t_air = readDHT22Temp();
  float h_air = readDHT22Hum();
  if (isnan(t_air)) t_air = -1;
  if (isnan(h_air)) h_air = -1;

  String ts = getTimeString();
  time_t nowEpoch = time(nullptr);

  // 数据质量判断
  auto getQuality = [](float val) -> const char* {
    if (val < 0) return "ERR";
    return "OK";
  };

  // 新格式：{ "schema_version": 2, "ts": "...", "channels": [ { "code": "...", "value": ..., "unit": "...", "quality": "..." }, ... ] }
  String payload = "{";
  payload += "\"schema_version\":2,";
  payload += "\"ts\":\"" + ts + "\",";
  payload += "\"channels\":[";

  // CO2 (%VOL)
  payload += "{";
  payload += "\"code\":\"CO2\",";
  payload += "\"value\":" + String(co2pct, 2) + ",";
  payload += "\"unit\":\"%VOL\",";
  payload += "\"quality\":\"" + String(getQuality(co2pct)) + "\"";
  payload += "},";

  // O2 (%VOL)
  payload += "{";
  payload += "\"code\":\"O2\",";
  payload += "\"value\":" + String(o2, 2) + ",";
  payload += "\"unit\":\"%VOL\",";
  payload += "\"quality\":\"" + String(getQuality(o2)) + "\"";
  payload += "},";

  // RoomTemp (℃)
  payload += "{";
  payload += "\"code\":\"RoomTemp\",";
  payload += "\"value\":" + String(t_ds, 1) + ",";
  payload += "\"unit\":\"℃\",";
  payload += "\"quality\":\"" + String(getQuality(t_ds)) + "\"";
  payload += "},";

  // MOIS (%)
  payload += "{";
  payload += "\"code\":\"MOIS\",";
  payload += "\"value\":" + String(mois, 1) + ",";
  payload += "\"unit\":\"%\",";
  payload += "\"quality\":\"" + String(getQuality(mois)) + "\"";
  payload += "},";

  // AirTemp (℃)
  payload += "{";
  payload += "\"code\":\"AirTemp\",";
  payload += "\"value\":" + String(t_air, 1) + ",";
  payload += "\"unit\":\"℃\",";
  payload += "\"quality\":\"" + String(getQuality(t_air)) + "\"";
  payload += "},";

  // AirHumidity (%RH)
  payload += "{";
  payload += "\"code\":\"AirHumidity\",";
  payload += "\"value\":" + String(h_air, 1) + ",";
  payload += "\"unit\":\"%RH\",";
  payload += "\"quality\":\"" + String(getQuality(h_air)) + "\"";
  payload += "}";

  payload += "]}";

  if (publishData(appConfig.mqttPostTopic(), payload, 10000)) {
    preferences.begin(NVS_NAMESPACE, false);
    preferences.putULong(NVS_KEY_LAST_MEAS, (unsigned long)nowEpoch);
    preferences.end();
    Serial.println("[Measure] 上传成功");
    return true;
  }

  Serial.println("[Measure] 上传失败");
  return false;
}

// =====================================================
// 任务：定时测量
// =====================================================
static void measurementTask(void*) {
  while (true) {
    // 使用无符号差值,避免 millis 溢出问题
    if ((millis() - prevMeasureMs) >= appConfig.readInterval) {
      prevMeasureMs = millis();
      doMeasurementAndSave();
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// =====================================================
// 任务：执行队列命令
// =====================================================
static void commandTask(void*) {
  while (true) {
    time_t now = time(nullptr);
    int size = (int)pendingCommands.size();
    for (int i = 0; i < size; i++) {
      if (now >= pendingCommands[i].targetTime) {
        // 先删除命令,再执行(避免重启时内存泄漏)
        PendingCommand cmdCopy = pendingCommands[i];
        pendingCommands.erase(pendingCommands.begin() + i);
        executeCommand(cmdCopy);
        // 删除后索引调整
        i--;
        size--;
      }
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

  initLogSystem();
  setMinLogLevel(LogLevel::INFO);

  // 1) SPIFFS + 读取 config.json
  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[System] 配置加载失败，重启");
    ESP.restart();
  }

  // 2) WiFi + NTP
  if (!connectToWiFi(20000) || !multiNTPSetup(20000)) {
    Serial.println("[System] WiFi/NTP 失败，重启");
    ESP.restart();
  }

  // 3) MQTT
  if (!connectToMQTT(20000)) {
    Serial.println("[System] MQTT 连接失败，重启");
    ESP.restart();
  }

  getMQTTClient().setCallback(mqttCallback);

  // 订阅 response topic（根据 equipment_key 自动生成）
  String respTopic = appConfig.mqttResponseTopic();
  if (respTopic.length() > 0) {
    getMQTTClient().subscribe(respTopic.c_str());
    Serial.println("[MQTT] Subscribed: " + respTopic);
  }
  else {
    Serial.println("[MQTT] response_topic 为空，无法订阅");
  }

  // 4) 传感器初始化（引脚按你当前项目固定：exhaust=25, aeration=26 等）
  if (!initSensorAndPump(25, 26, Serial1, 16, 17, 15, 5000)) {
    Serial.println("[ERR] 传感器初始化失败，重启");
    ESP.restart();
  }

  // 预热 MH-Z16
  readMHZ16();
  delay(500);

  // 5) 恢复上次测量时间（避免重启后立刻测）
  if (preferences.begin(NVS_NAMESPACE, false)) {
    unsigned long lastSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    time_t nowSec = time(nullptr);

    unsigned long intervalSec = appConfig.readInterval / 1000UL;
    if (intervalSec == 0) intervalSec = 1;  // 避免除零

    unsigned long elapsedSec = (nowSec > (time_t)lastSec) ? (unsigned long)(nowSec - (time_t)lastSec) : 0;

    // 无论是否超过间隔,都从当前时间开始计时,避免启动后立即上传
    if (elapsedSec >= intervalSec) {
      // 已超过间隔,等待一个完整周期后再测量
      prevMeasureMs = millis();
    }
    else {
      // 还未超过间隔,继续之前的计时
      unsigned long remainingMs = (intervalSec - elapsedSec) * 1000UL;
      prevMeasureMs = millis() - (appConfig.readInterval - remainingMs);
    }
    preferences.end();
  }

  // 6) 上线消息：带完整配置（关键！）
  publishOnlineWithConfig();

  // 7) 启动任务
  xTaskCreatePinnedToCore(measurementTask, "Measure", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandTask, "Command", 8192, NULL, 1, NULL, 1);

  Serial.println("[System] 初始化完成");
}

// =====================================================
// loop
// =====================================================
void loop() {
  maintainMQTT(5000);
  delay(100);
}
