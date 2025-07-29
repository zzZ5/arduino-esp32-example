#include <Arduino.h>
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor_control.h"
#include <ArduinoJson.h>

const int pinHeater = 2;  // 加热继电器连接 GPIO2
bool heaterOn = false;
unsigned long lastPostTime = 0;

float median(std::vector<float> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  size_t mid = values.size() / 2;
  if (values.size() % 2 == 0) {
    return (values[mid - 1] + values[mid]) / 2.0;
  }
  return values[mid];
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // 初始化配置
  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[Main] Failed to load config. Rebooting...");
    delay(1000);
    ESP.restart();
  }

  printConfig(appConfig);

  // 连接 WiFi & NTP
  if (!connectToWiFi(15000)) {
    Serial.println("[Main] WiFi connect failed.");
    ESP.restart();
  }
  if (!multiNTPSetup(20000)) {
    Serial.println("[Main] NTP setup failed.");
    ESP.restart();
  }

  // 初始化传感器
  initSensors();

  // 初始化加热继电器引脚
  pinMode(pinHeater, OUTPUT);
  digitalWrite(pinHeater, LOW);  // 默认关闭
  heaterOn = false;

  // 连接 MQTT
  connectToMQTT(10000);
}

void loop() {
  maintainMQTT(5000);

  unsigned long now = millis();
  if (now - lastPostTime >= appConfig.postInterval) {
    lastPostTime = now;

    // 读取温度（按 rank 排序）
    std::vector<float> temps;
    std::vector<String> keys;
    if (!readTemperatures(temps, keys)) {
      Serial.println("[Main] Failed to read temperatures.");
      return;
    }


    // 控制加热：前3个为内部，后3个为外部
    std::vector<float> inside(temps.begin(), temps.begin() + 3);
    std::vector<float> outside(temps.begin() + 3, temps.end());

    float med_in = median(inside);
    float med_out = median(outside);
    float diff = med_in - med_out;
    float max_allowed_diff = pow((med_in - appConfig.tempMaxDif) / 40.0f, 2);

    if (diff >= max_allowed_diff) {
      digitalWrite(pinHeater, HIGH);
      heaterOn = true;
    }
    else {
      digitalWrite(pinHeater, LOW);
      heaterOn = false;
    }

    // 读取 CO₂
    uint16_t co2;
    String co2Key;
    if (!readCO2(co2, co2Key)) {
      Serial.println("[Main] Failed to read CO2.");
      return;
    }

    // 构建 JSON 数据
    StaticJsonDocument<1024> doc;
    JsonArray dataArr = doc.createNestedArray("data");
    String nowTime = getTimeString();
    for (size_t i = 0; i < temps.size(); ++i) {

      JsonObject obj = dataArr.createNestedObject();
      obj["value"] = temps[i];
      obj["key"] = keys[i];
      obj["measured_time"] = nowTime;
    }

    JsonObject co2Obj = dataArr.createNestedObject();
    co2Obj["value"] = co2;
    co2Obj["key"] = co2Key;
    co2Obj["measured_time"] = nowTime;

    // 系统状态字段（如加热状态）
    doc["info"]["heat"] = heaterOn;

    // 发布到 MQTT
    String payload;
    serializeJson(doc, payload);
    publishData(appConfig.mqtt.postTopic, payload, 5000);
  }
}
