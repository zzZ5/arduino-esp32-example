#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>

// AppConfig 结构定义（包含 wifi / mqtt / equipment_key / keys等）
struct AppConfig {
	// wifi
	String wifiSSID;
	String wifiPass;

	// mqtt
	String mqttServer;
	int    mqttPort;
	String mqttUser;
	String mqttPass;
	String mqttClientId;
	String mqttTopic;

	// ntp
	String ntpServers[3];

	// 采集周期 & 泵时长
	unsigned long pumpRunTime;
	unsigned long readInterval;

	// equipment_key
	String equipmentKey;

	// "CO", "O2", "CH4", "H2S"
	String keyCO;
	String keyO2;
	String keyCH4;
	String keyH2S;
};

// 全局对象
extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
