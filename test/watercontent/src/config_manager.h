#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>

struct AppConfig {
	// Wi-Fi
	String wifiSSID;
	String wifiPass;

	// MQTT
	String mqttServer;
	int    mqttPort;
	String mqttUser;
	String mqttPass;
	String mqttClientId;
	String mqttTopic;

	// NTP servers
	String ntpServers[3];

	// 测量周期
	unsigned long readInterval;

	// 设备标识
	String equipmentKey;

	// 三个土壤水分键值
	String keyWater1;
	String keyWater2;
	String keyWater3;
};

extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
