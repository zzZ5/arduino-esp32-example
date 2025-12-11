/*
 * Project: ESP32 Dual-Bus Temperature Monitor
 * Version: Final (with equipment_key)
 * ------------------------------------------
 * - 读取 GPIO4 / GPIO5 两路 DS18B20
 * - 每隔 post_interval 上传温度到 MQTT
 * - JSON 顶层包含设备标识 equipment_key
 * - 支持远程 config_update
 */

#include <Arduino.h>
#include <time.h>
#include <ArduinoJson.h>
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"
#include <Preferences.h>
#include <vector>

Preferences preferences;
static const char* NVS_NAMESPACE = "temps";
static const char* NVS_KEY_LAST_MEAS = "lastMeas";
static unsigned long prevMeasureMs = 0;

// ========== 远程配置更新 ==========
static bool updateAppConfigFromJson(JsonObject obj) {
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
  if (obj.containsKey("equipment_key"))
    appConfig.equipmentKey = obj["equipment_key"].as<String>();

  if (obj.containsKey("ntp_host") && obj["ntp_host"].is<JsonArray>()) {
    appConfig.ntpServers.clear();
    for (JsonVariant v : obj["ntp_host"].as<JsonArray>())
      appConfig.ntpServers.push_back(v.as<String>());
  }
  if (obj.containsKey("post_interval"))
    appConfig.postInterval = obj["post_interval"].as<uint32_t>();

  // keys
  if (obj.containsKey("keys") && obj["keys"].is<JsonObject>()) {
    JsonObject keys = obj["keys"];
    appConfig.keyTemp4.clear();
    appConfig.keyTemp5.clear();
    if (keys.containsKey("temp4") && keys["temp4"].is<JsonArray>())
      for (JsonVariant v : keys["temp4"].as<JsonArray>())
        appConfig.keyTemp4.push_back(v.as<String>());
    if (keys.containsKey("temp5") && keys["temp5"].is<JsonArray>())
      for (JsonVariant v : keys["temp5"].as<JsonArray>())
        appConfig.keyTemp5.push_back(v.as<String>());
    if (keys.containsKey("temp") && appConfig.keyTemp4.empty())
      for (JsonVariant v : keys["temp"].as<JsonArray>())
        appConfig.keyTemp4.push_back(v.as<String>());
  }
  return true;
}

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, payload, length)) return;
  String device = doc["device"] | "";
  if (device != appConfig.equipmentKey) return;

  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (cmds.isNull()) return;
  for (JsonVariant v : cmds) {
    JsonObject obj = v.as<JsonObject>();
    if (obj["command"] != "config_update") continue;
    JsonObject cfg = obj["config"].as<JsonObject>();
    if (cfg.isNull()) continue;
    if (updateAppConfigFromJson(cfg)) {
      if (saveConfigToSPIFFS("/config.json")) {
        Serial.println("[Config] updated & saved, restarting...");
        delay(300);
        ESP.restart();
      }
    }
  }
}

// ========== 采集 + 上报 ==========
static bool doMeasurementAndPost() {
  // 读取两路
  std::vector<float> t4 = readTemps4();
  std::vector<float> t5 = readTemps5();

  if (t4.empty() && t5.empty()) {
    Serial.println("[Measure] no temps on both buses");
    return false;
  }

  // 合并读数，顺序：先 GPIO4 后 GPIO5
  std::vector<float> temps;
  temps.reserve(t4.size() + t5.size());
  temps.insert(temps.end(), t4.begin(), t4.end());
  temps.insert(temps.end(), t5.begin(), t5.end());

  // 生成对应 key 列表：优先 keys.temp；否则 temp4 + temp5 依次拼接
  std::vector<String> keys;
  if (!appConfig.keyTemp4.empty() && appConfig.keyTemp5.empty() && appConfig.keyTemp4.size() != temps.size()) {
    // 兼容旧配置：如果用户只给了 "temp"（在 loadConfig 时我们映射到了 keyTemp4）
    // 这里直接用 keyTemp4 当作整套 key
    keys = appConfig.keyTemp4;
  }
  else {
    keys.reserve(appConfig.keyTemp4.size() + appConfig.keyTemp5.size());
    keys.insert(keys.end(), appConfig.keyTemp4.begin(), appConfig.keyTemp4.end());
    keys.insert(keys.end(), appConfig.keyTemp5.begin(), appConfig.keyTemp5.end());
  }

  // 对齐数量：只发送“有 key 的读数”
  size_t n = min(temps.size(), keys.size());
  if (n == 0) {
    Serial.println("[Measure] no matching keys for temps");
    return false;
  }
  if (temps.size() != keys.size()) {
    Serial.printf("[Measure] WARN: temps=%u keys=%u -> will post %u items\n",
      (unsigned)temps.size(), (unsigned)keys.size(), (unsigned)n);
  }

  String ts = getTimeString();

  // 构建 payload（注意：顶层不放 device；data 只能 {key,value,measured_time}）
  StaticJsonDocument<4096> doc;
  JsonArray data = doc.createNestedArray("data");

  for (size_t i = 0; i < n; ++i) {
    JsonObject obj = data.createNestedObject();
    obj["key"] = keys[i];        // ✅ 必须是服务器那套 key（例如 "OpYeXW..."）
    obj["value"] = temps[i];
    obj["measured_time"] = ts;   // ✅ 字段名固定
  }

  // info 可扩展，放设备编号等
  JsonObject info = doc.createNestedObject("info");
  info["device"] = appConfig.equipmentKey;  // ✅ 放在 info 里
  info["count4"] = (uint32_t)t4.size();
  info["count5"] = (uint32_t)t5.size();
  info["timestamp"] = ts;

  String payload;
  serializeJson(doc, payload);
  Serial.println("[MQTT] publish: " + payload);

  bool ok = publishData(appConfig.mqttPostTopic, payload, 10000);
  if (ok) {
    Preferences preferences;
    if (preferences.begin("temps", false)) {
      preferences.putULong("lastMeas", time(nullptr));
      preferences.end();
    }
  }
  return ok;
}


// ========== 后台测量任务 ==========
static void measurementTask(void* pv) {
  while (true) {
    if (millis() - prevMeasureMs >= appConfig.postInterval) {
      prevMeasureMs = millis();
      doMeasurementAndPost();
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ========== 初始化 ==========
void setup() {
  Serial.begin(115200);
  Serial.println("\n[System] Dual-bus temp monitor starting");

  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[System] config not found, using defaults");
  }
  printConfig(appConfig);

  if (!connectToWiFi(20000) || !multiNTPSetup(20000)) {
    Serial.println("[System] WiFi/NTP failed, restart");
    ESP.restart();
  }
  if (!connectToMQTT(20000)) {
    Serial.println("[System] MQTT failed, restart");
    ESP.restart();
  }

  if (!initSensors(4, 5)) {
    Serial.println("[System] DS18B20 init failed, restart");
    ESP.restart();
  }

  getMQTTClient().setCallback(mqttCallback);
  if (appConfig.mqttResponseTopic.length())
    getMQTTClient().subscribe(appConfig.mqttResponseTopic.c_str());

  // 上线消息
  String ts = getTimeString();
  String boot = String("{\"device\":\"") + appConfig.equipmentKey +
    "\",\"status\":\"online\",\"timestamp\":\"" + ts + "\"}";
  publishData(appConfig.mqttPostTopic, boot, 10000);

  xTaskCreatePinnedToCore(measurementTask, "MeasureTask", 4096, NULL, 1, NULL, 1);
  Serial.println("[System] ready");
}

void loop() {
  maintainMQTT(5000);
  delay(100);
}
