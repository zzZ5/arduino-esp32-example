#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <vector>

struct AppConfig {
	static constexpr size_t kPointCount = 6;

	String wifiSSID;
	String wifiPass;

	String mqttServer;
	uint16_t mqttPort;
	String mqttUser;
	String mqttPass;
	String mqttClientId;

	// 设备代码（用于生成 MQTT topic）
	String deviceCode;
	std::vector<String> pointDeviceCodes;

	// 自动生成的 MQTT topics
	String mqttPostTopic() const {
		return "compostlab/v2/" + deviceCode + "/telemetry";
	}
	String mqttPostTopic(const String& code) const {
		return "compostlab/v2/" + code + "/telemetry";
	}
	String mqttResponseTopic() const {
		return "compostlab/v2/" + deviceCode + "/response";
	}

	std::vector<String> ntpServers;

	// 单个采样点的抽气检测总时长（毫秒）。
	uint32_t sampleTime;
	// 点位之间用于清空气路的吹扫时长（毫秒）。
	uint32_t purgePumpTime;
	// 整轮巡检的启动周期（毫秒）。
	uint32_t readInterval;
};

extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
