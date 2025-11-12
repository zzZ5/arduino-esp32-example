#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <vector>

struct AppConfig {
	// 网络 / MQTT / NTP
	String wifiSSID, wifiPass;
	String mqttServer, mqttUser, mqttPass, mqttClientId, mqttPostTopic, mqttResponseTopic;
	uint16_t mqttPort;
	std::vector<String> ntpServers;

	// 基础
	uint32_t postInterval = 60000;
	String equipmentKey;

	// IDs：支持 keys.temp4 / keys.temp5；兼容旧 keys.temp -> 作为 temp4 使用
	std::vector<String> keyTemp4;
	std::vector<String> keyTemp5;
};

extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
