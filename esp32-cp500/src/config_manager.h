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

	uint32_t postInterval;
	uint32_t tempMaxDiff;

	// 温度上下限（单位：°C）
	uint32_t tempLimitOutMax;
	uint32_t tempLimitInMax;
	uint32_t tempLimitOutMin;
	uint32_t tempLimitInMin;

	String equipmentKey;
	String keyTempIn;
	std::vector<String> keyTempOut;

	// 曝气定时策略（单位：毫秒）
	bool aerationTimerEnabled;
	uint32_t aerationInterval;
	uint32_t aerationDuration;

	// 水泵最大持续运行时间（单位：毫秒）
	uint32_t pumpMaxDuration;

};

extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
