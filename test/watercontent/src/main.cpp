#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "log_manager.h"
#include "sensor.h"

// NVS 设置
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";
static const char* NVS_KEY_LAST_MEAS = "lastMeas";
static unsigned long prevMeasureMs = 0;

// 睡眠节电
void goToLightSleep() {
  esp_sleep_enable_timer_wakeup(60 * 1000000);  // 1分钟
  esp_light_sleep_start();
}

// 读取三个水分数据并上传 MQTT
bool doMeasurementAndSave() {
  logWrite(LogLevel::INFO, "Start soil moisture measurement...");

  float moisture1, moisture2, moisture3;

  if (!readAnalogCapacitive(moisture1)) return false;
  if (!readFDS100(moisture2)) return false;
  if (!readRS485SoilMoisture(moisture3)) return false;

  String measuredTime = getTimeString();
  time_t nowEpoch = time(nullptr);

  String payload = "{\"data\":[";
  payload += "{\"key\":\"" + appConfig.keyWater1 + "\",\"value\":" + String(moisture1, 1) + ",\"measured_time\":\"" + measuredTime + "\"},";
  payload += "{\"key\":\"" + appConfig.keyWater2 + "\",\"value\":" + String(moisture2, 1) + ",\"measured_time\":\"" + measuredTime + "\"},";
  payload += "{\"key\":\"" + appConfig.keyWater3 + "\",\"value\":" + String(moisture3, 1) + ",\"measured_time\":\"" + measuredTime + "\"}";
  payload += "]}";

  if (!publishData(appConfig.mqttTopic, payload, 20000UL)) {
    logWrite(LogLevel::ERROR, "MQTT publish failed");
    return false;
  }

  preferences.putULong(NVS_KEY_LAST_MEAS, nowEpoch);
  logWrite(LogLevel::INFO, "Measurement success");
  return true;
}

void setup() {
  Serial.begin(115200);
  logWrite(LogLevel::INFO, "Program starting...");

  // 初始化各项模块
  initLogSystem();
  initSPIFFS();
  loadConfigFromSPIFFS("/config.json");
  printConfig(appConfig);

  connectToWiFi(20000UL);
  multiNTPSetup(20000UL);
  connectToMQTT(20000UL);
  initSensors();

  // 读取上次测量时间判断是否需要等待
  if (!preferences.begin(NVS_NAMESPACE, false)) {
    logWrite(LogLevel::ERROR, "NVS init fail");
  }
  else {
    unsigned long lastMeasSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    time_t nowEpoch = time(nullptr);
    unsigned long intervalSec = appConfig.readInterval / 1000UL;
    unsigned long elapsed = (nowEpoch > lastMeasSec) ? (nowEpoch - lastMeasSec) : 0;

    if (lastMeasSec == 0 || elapsed >= intervalSec) {
      logWrite(LogLevel::INFO, "Ready for immediate measurement");
    }
    else {
      unsigned long waitSec = intervalSec - elapsed;
      logWrite(LogLevel::INFO, String("Waiting ") + waitSec + "s for next cycle...");
      delay(waitSec * 1000UL);
    }

    prevMeasureMs = millis();
    if (!doMeasurementAndSave()) {
      logWrite(LogLevel::ERROR, "Initial measurement failed, restarting...");
      ESP.restart();
    }
  }

  goToLightSleep();
}

void loop() {
  if (millis() - prevMeasureMs >= appConfig.readInterval) {
    prevMeasureMs = millis();
    if (!doMeasurementAndSave()) {
      ESP.restart();
    }
  }

  delay(50);
  goToLightSleep();
}
