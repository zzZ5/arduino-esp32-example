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

// ========== 获取公网 IP ==========
String getPublicIP();  // 通过外部服务获取公网 IP

// ========== MQTT 核心操作 ==========
bool connectToMQTT(unsigned long timeoutMs);
void maintainMQTT(unsigned long timeoutMs);
bool publishData(const String& topic, const String& payload, unsigned long timeoutMs);
bool publishDataOrCache(const String& topic, const String& payload, const String& timestamp, unsigned long timeoutMs);
int uploadCachedData(int maxUpload = 10);

#endif