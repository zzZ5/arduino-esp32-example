#include <Arduino.h>
#include "config_manager.h"     // initSPIFFS(), loadConfigFromSPIFFS(), printConfig(...), appConfig
#include "wifi_ntp_mqtt.h"      // connectToWiFiWithTimeout(...), multiNTPSetup(), connectToMQTT(...), publishData(...), getTimeString()
#include "sensor.h"             // initSensorAndPump(...), pumpOn(), pumpOff(), readFourInOneSensor(...)

void setup() {
  Serial.begin(115200);
  Serial.println("===== Program Start =====");

  // 1) 初始化 SPIFFS
  if (!initSPIFFS()) {
    Serial.println("[SPIFFS] init fail, proceed anyway...");
  }

  // 2) 加载配置
  if (!loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[Config] no /config.json, using defaults (or config_manager fallback)...");
  }
  printConfig(appConfig);

  // 3) Wi-Fi 连接 (示例: 15秒超时)
  if (!connectToWiFi(15000UL)) {
    Serial.println("[Main] WiFi connect fail => rebooting...");
    ESP.restart();
  }

  // 4) NTP (多服务器无限重试,不返回false; 若要超时可自行改造)
  multiNTPSetup();
  // 若想对NTP也设超时,可在 multiNTPSetup 内返回true/false,这里判断

  // 5) MQTT 连接 (示例: 10秒超时)
  if (!connectToMQTT(10000UL)) {
    Serial.println("[Main] MQTT connect fail => rebooting...");
    ESP.restart();
  }

  // 6) 初始化 传感器 & 氣泵 
  //   (pumpPin=4, HardwareSerial=Serial1=RX16,TX17)
  initSensorAndPump(4, Serial1, 16, 17);

  Serial.println("===== Setup done, start loop() =====");
}

void loop() {
  // A) 若需要维持心跳
  //    1) 先看 mqttClient.connected() 是否仍在线
  //    2) 如果想自动短循环重连 -> connectToMQTT(...), or just maintainMQTT()

  // B) 每 appConfig.readInterval 毫秒执行一次采集
  static unsigned long prevTime = 0;
  unsigned long now = millis();
  if (now - prevTime >= appConfig.readInterval) {
    prevTime = now;

    // 1) 打开气泵, 延时 appConfig.pumpRunTime (阻塞式)
    pumpOn();
    delay(appConfig.pumpRunTime);
    pumpOff();

    // 2) 读取传感器
    uint16_t coVal, h2sVal, ch4Val;
    float o2Val;
    if (readFourInOneSensor(coVal, h2sVal, o2Val, ch4Val)) {
      Serial.printf("[Main] Sensor read ok: CO=%u, H2S=%u, O2=%.1f, CH4=%u\n",
        coVal, h2sVal, o2Val, ch4Val);

      // 3) 拼装 JSON
      String measuredTime = getTimeString(); // 取当前时间
      String payload = "{";
      payload += "\"data\":[";
      // CO
      payload += "{";
      payload += "\"value\":" + String(coVal);
      payload += ",\"key\":\"CO\"";
      payload += ",\"measured_time\":\"" + measuredTime + "\"";
      payload += "},";
      // H2S
      payload += "{";
      payload += "\"value\":" + String(h2sVal);
      payload += ",\"key\":\"H2S\"";
      payload += ",\"measured_time\":\"" + measuredTime + "\"";
      payload += "},";
      // O2
      payload += "{";
      payload += "\"value\":" + String(o2Val, 1);
      payload += ",\"key\":\"O2\"";
      payload += ",\"measured_time\":\"" + measuredTime + "\"";
      payload += "},";
      // CH4
      payload += "{";
      payload += "\"value\":" + String(ch4Val);
      payload += ",\"key\":\"CH4\"";
      payload += ",\"measured_time\":\"" + measuredTime + "\"";
      payload += "}";
      payload += "]}";

      // 4) 发布(示例: 10秒超时)
      if (!publishData(payload, 10000UL)) {
        Serial.println("[Main] publishData fail => rebooting...");
        ESP.restart();
      }
    }
    else {
      Serial.println("[Main] Sensor read fail!");
      ESP.restart();
    }
  }

  // 其它逻辑(维持心跳 or maintainMQTT()):
  // maintainMQTT(); // 如果在 .cpp 里写了
  delay(50);
}
