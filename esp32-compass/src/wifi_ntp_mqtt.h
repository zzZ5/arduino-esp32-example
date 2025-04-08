#ifndef WIFI_NTP_MQTT_H
#define WIFI_NTP_MQTT_H

#include <PubSubClient.h>

// 对外暴露的全局 mqttClient
extern PubSubClient mqttClient;

// 保持之前的方法名不变，但内部做短循环超时
bool connectToMQTT(unsigned long timeoutMs);
bool publishData(const String& payload, unsigned long timeoutMs);

// 其余函数
bool connectToWiFi(unsigned long timeoutMs);
void multiNTPSetup();
void maintainMQTT(unsigned long timeoutMs);
String getTimeString();

#endif
