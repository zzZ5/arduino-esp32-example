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
	String mqttPostTopic;
	String mqttResponseTopic;

	std::vector<String> ntpServers;

	uint32_t pumpRunTime;
	uint32_t readInterval;

	String equipmentKey;
	String keyCO2;
	String keyO2;
	String keyRoomTemp;
	String keyMois;
};

extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
