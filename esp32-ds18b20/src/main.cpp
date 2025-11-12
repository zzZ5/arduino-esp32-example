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

static String getTimeString() {
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
  return String(buffer);
}

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
  std::vector<float> t4 = readTemps4();
  std::vector<float> t5 = readTemps5();
  if (t4.empty() && t5.empty()) {
    Serial.println("[Measure] no temps on both buses");
    return false;
  }

  String ts = getTimeString();
  StaticJsonDocument<4096> doc;

  // ✅ 顶层添加设备标识
  doc["device"] = appConfig.equipmentKey;
  doc["timestamp"] = ts;

  JsonArray data = doc.createNestedArray("data");

  // GPIO4
  for (size_t i = 0; i < t4.size(); ++i) {
    JsonObject obj = data.createNestedObject();
    obj["key"] = "temp";
    obj["id"] = (i < appConfig.keyTemp4.size()) ? appConfig.keyTemp4[i] : String("temp4_") + String(i);
    obj["bus"] = 4;
    obj["value"] = t4[i];
    obj["measured_time"] = ts;
  }

  // GPIO5
  for (size_t i = 0; i < t5.size(); ++i) {
    JsonObject obj = data.createNestedObject();
    obj["key"] = "temp";
    obj["id"] = (i < appConfig.keyTemp5.size()) ? appConfig.keyTemp5[i] : String("temp5_") + String(i);
    obj["bus"] = 5;
    obj["value"] = t5[i];
    obj["measured_time"] = ts;
  }

  JsonObject info = doc.createNestedObject("info");
  info["count4"] = t4.size();
  info["count5"] = t5.size();
  info["device"] = appConfig.equipmentKey;
  info["timestamp"] = ts;

  String payload;
  serializeJson(doc, payload);
  Serial.println("[MQTT] publish: " + payload);

  bool ok = publishData(appConfig.mqttPostTopic, payload, 10000);
  if (ok) {
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_MEAS, time(nullptr));
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
