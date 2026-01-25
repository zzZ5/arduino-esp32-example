// wifi_ntp_mqtt.h
#ifndef WIFI_NTP_MQTT_H
#define WIFI_NTP_MQTT_H

#include <Arduino.h>
#include <PubSubClient.h>

// ========== MQTT 客户端访问 ==========
PubSubClient& getMQTTClient();  // 获取 MQTT 客户端引用（可用于设置回调）

// ========== WiFi & NTP ==========
bool connectToWiFi(unsigned long timeoutMs);
bool multiNTPSetup(unsigned long timeoutMs);
String getTimeString();
String getPublicIP();  // 获取公网IP地址

// ========== MQTT 核心操作 ==========
bool connectToMQTT(unsigned long timeoutMs);
void maintainMQTT(unsigned long timeoutMs);
bool publishData(const String& topic, const String& payload, unsigned long timeoutMs);

#endif