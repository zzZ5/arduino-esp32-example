// wifi_ntp_mqtt.h
#ifndef WIFI_NTP_MQTT_H
#define WIFI_NTP_MQTT_H

#include <Arduino.h>
#include <PubSubClient.h>

// ========== MQTT client access ==========
PubSubClient& getMQTTClient();

// ========== WiFi and time sync ==========
bool connectToWiFi(unsigned long timeoutMs);
bool multiNTPSetup(unsigned long timeoutMs);
String getTimeString();
String getPublicIP();

// ========== MQTT core operations ==========
bool connectToMQTT(unsigned long timeoutMs);
void maintainMQTT(unsigned long timeoutMs);
bool publishData(const String& topic, const String& payload, unsigned long timeoutMs);

#endif
