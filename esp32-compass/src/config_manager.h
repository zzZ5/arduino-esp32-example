#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>

// 定义一个结构体存储配置信息
struct AppConfig {
	String wifiSSID;
	String wifiPass;
	String mqttServer;
	int    mqttPort;
	String mqttUser;
	String mqttPass;
	String mqttClientId;
	String mqttTopic;
	unsigned long pumpRunTime;
	unsigned long readInterval;
	String ntpServers[3];
};

// 声明全局的 AppConfig 对象 (或在cpp里做static,由 getter返回)
extern AppConfig appConfig;

// 声明函数
bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
