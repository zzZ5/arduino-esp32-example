#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <vector>

struct AppConfig {
	String wifiSSID;
	String wifiPass;

	String mqttServer;
	uint16_t mqttPort;
	String mqttUser;
	String mqttPass;
	String mqttClientId;

	std::vector<String> ntpServers;

	uint32_t pumpRunTime;
	uint32_t readInterval;

	// 设备key
	String equipmentKey;

	// 自动生成的 MQTT topics
	String mqttPostTopic() const {
		return "compostlab/v2/" + equipmentKey + "/telemetry";
	}
	String mqttResponseTopic() const {
		return "compostlab/v2/" + equipmentKey + "/response";
	}
};

extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
