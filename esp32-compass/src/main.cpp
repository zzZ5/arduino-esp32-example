#include <Arduino.h>
#include "log_manager.h"
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"

//=
// 常量定义
//=

// Wi-Fi 超时(示例20秒)
static const unsigned long WIFI_TIMEOUT = 20000UL;
// NTP 超时(示例: 20秒)
static const unsigned long NTP_TIMEOUT = 20000UL;
// MQTT 超时(示例20秒)
static const unsigned long MQTT_TIMEOUT = 20000UL;
// 初始化超时(示例5秒)
static const unsigned long INIT_TIMEOUT = 5000UL;

void setup() {
  Serial.begin(115200);
  Serial.println("[Setup] Program start...");

  // 1) 初始化日志系统(若尚未mount SPIFFS,可在 initLogSystem中做)
  if (!initLogSystem()) {
    Serial.println("[Setup] initLogSystem fail, proceed anyway...");
  }
  else {
    // 设置最小日志等级(示例：只写INFO以上)
    setMinLogLevel(LogLevel::INFO);
    // 设置日志文件最大50KB
    setMaxLogSize(50 * 1024);
    // 记录启动
    logWrite(LogLevel::INFO, "Device booting, log system ready.");
  }

  // 2) 挂载 SPIFFS 并加载配置
  if (!initSPIFFS()) {
    Serial.println("[Setup] SPIFFS init fail => reboot");
    logWrite(LogLevel::ERROR, "SPIFFS init fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "SPIFFS init OK");

  if (!loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[Setup] no /config.json => use defaults");
    logWrite(LogLevel::WARN, "No config => use defaults");
  }
  else {
    logWrite(LogLevel::INFO, "Config loaded from /config.json");
  }
  printConfig(appConfig);


  // 3) Wi-Fi 连接
  logWrite(LogLevel::INFO, "Connecting WiFi...");
  unsigned long startTime = millis();
  if (!connectToWiFi(WIFI_TIMEOUT)) {
    logWrite(LogLevel::ERROR, "WiFi connect fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "WiFi connected.");
  // 4) NTP
  logWrite(LogLevel::INFO, "multiNTPSetup with 20s totalTimeout");
  startTime = millis();
  if (!multiNTPSetup(NTP_TIMEOUT)) {
    logWrite(LogLevel::ERROR, "NTP fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "NTP done.");

  // 5) MQTT连接
  logWrite(LogLevel::INFO, "Connect MQTT...");
  startTime = millis();
  if (!connectToMQTT(MQTT_TIMEOUT)) {
    logWrite(LogLevel::ERROR, "MQTT connect fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "MQTT connected OK");

  // 6) 初始化传感器 & 气泵
  logWrite(LogLevel::INFO, "Init sensor & pump with 5s timeout...");
  startTime = millis();
  if (!initSensorAndPump(4, Serial1, 16, 17, INIT_TIMEOUT)) {
    logWrite(LogLevel::ERROR, "initSensorAndPump fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "Sensor & pump inited.");

  Serial.println("[Setup] All done, enter loop");
  logWrite(LogLevel::INFO, "Setup done, entering loop");
}

void loop() {
  static unsigned long prevTime = 0;
  unsigned long now = millis();
  // 每5分钟采集 & 发布
  if (now - prevTime >= appConfig.readInterval) {
    prevTime = now;

    // 1) 打开气泵, 延时 appConfig.pumpRunTime (阻塞式)
    pumpOn();
    logWrite(LogLevel::INFO, "Pump ON, wait 60s...");
    delay(appConfig.pumpRunTime);
    pumpOff();
    logWrite(LogLevel::INFO, "Pump OFF, reading sensor...");

    // 2) 读取传感器
    uint16_t coVal, h2sVal, ch4Val;
    float o2Val;
    if (readFourInOneSensor(coVal, h2sVal, o2Val, ch4Val)) {
      Serial.printf("[Main] Sensor read ok: CO=%u, H2S=%u, O2=%.1f, CH4=%u\n",
        coVal, h2sVal, o2Val, ch4Val);
      // 写日志
      String sensorLog = "Sensor => CO=" + String(coVal)
        + ",H2S=" + String(h2sVal)
        + ",O2=" + String(o2Val, 1)
        + ",CH4=" + String(ch4Val);
      logWrite(LogLevel::INFO, sensorLog);

      // (3) 拼装 JSON
      String measuredTime = getTimeString(); // 取当前时间
      String payload = "{";
      payload += "\"data\":[";
      // CO
      payload += "{";
      payload += "\"value\":" + String(coVal);
      payload += ",\"key\":\"" + appConfig.keyCO + "\"";        // 从 config 中读取 CO 的键
      payload += ",\"measured_time\":\"" + measuredTime + "\"";
      payload += "},";

      // H2S
      payload += "{";
      payload += "\"value\":" + String(h2sVal);
      payload += ",\"key\":\"" + appConfig.keyH2S + "\"";       // 从 config 中读取 H2S 的键
      payload += ",\"measured_time\":\"" + measuredTime + "\"";
      payload += "},";

      // O2
      payload += "{";
      payload += "\"value\":" + String(o2Val, 1);
      payload += ",\"key\":\"" + appConfig.keyO2 + "\"";        // 从 config 中读取 O2 的键
      payload += ",\"measured_time\":\"" + measuredTime + "\"";
      payload += "},";

      // CH4
      payload += "{";
      payload += "\"value\":" + String(ch4Val);
      payload += ",\"key\":\"" + appConfig.keyCH4 + "\"";       // 从 config 中读取 CH4 的键
      payload += ",\"measured_time\":\"" + measuredTime + "\"";
      payload += "}";

      payload += "]}";

      // 4) 发布(示例: 10秒超时)
      logWrite(LogLevel::INFO, "Publishing data to MQTT...");
      if (!publishData(appConfig.mqttTopic, payload, MQTT_TIMEOUT)) {
        logWrite(LogLevel::ERROR, "Publish fail => Reboot");
        ESP.restart();
      }
      logWrite(LogLevel::WARN, "Sensor read fail, skip publish");
    }
    else {
      Serial.println("[Main] Sensor read fail!");
      ESP.restart();
    }
    logWrite(LogLevel::INFO, "Sensor measure process done.");
  }
  // 其它逻辑(维持心跳 or maintainMQTT()):
  // maintainMQTT(); // 如果在 .cpp 里写了
  delay(50);
}
