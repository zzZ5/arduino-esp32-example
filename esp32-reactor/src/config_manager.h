#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <vector>

// WiFi 配置结构体
struct WiFiConfig {
	String ssid;
	String password;
};

// MQTT 配置结构体
struct MqttConfig {
	String server;
	int port;
	String user;
	String pass;
	String clientId;
	String postTopic;
	String responseTopic;
};

// 配置结构体
struct AppConfig {
	WiFiConfig wifi;
	MqttConfig mqtt;
	std::vector<String> ntpServers; // NTP 服务器
	String equipmentKey;            // 设备标识符
	String ds[6];                   // 温度传感器唯一标识符
	int rank[6];                    // 传感器排序
	unsigned long postInterval;     // 上传间隔（ms）
	int tempMaxDif;                 // 温差阈值
	String sgp30;                   // SGP30 的 key
};

extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
