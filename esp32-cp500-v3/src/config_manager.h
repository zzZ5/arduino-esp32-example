#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <vector>

struct AppConfig {
	// Network / MQTT / NTP
	String wifiSSID;
	String wifiPass;

	String mqttServer;
	uint16_t mqttPort;
	String mqttUser;
	String mqttPass;
	String mqttDeviceCode;  // Used to build MQTT topics

	std::vector<String> ntpServers;

	// Basic control parameters
	uint32_t postInterval;   // Measurement / publish interval (ms)
	uint32_t tempMaxDiff;    // Max n-curve diff threshold (C)

	// Temperature limits (C)
	uint32_t tempLimitOutMax;
	uint32_t tempLimitInMax;
	uint32_t tempLimitOutMin;
	uint32_t tempLimitInMin;

	// Aeration timer (ms)
	bool     aerationTimerEnabled;
	uint32_t aerationInterval;
	uint32_t aerationDuration;

	// Safety
	float tankTempMax;       // Tank over-temperature limit (C)

	// Heater guard
	uint32_t heaterMinOnMs;
	uint32_t heaterMinOffMs;

	// Pump adaptive thresholds
	float pumpDeltaOnMin;
	float pumpDeltaOnMax;
	float pumpHystNom;
	float pumpNCurveGamma;

	// Pump learning
	float pumpLearnStepUp;
	float pumpLearnStepDown;
	float pumpLearnMax;
	float pumpProgressMin;

	// Curves
	float inDiffNCurveGamma;

	// Bath setpoint mode
	bool  bathSetEnabled;
	float bathSetTarget;
	float bathSetHyst;
};

extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

// MQTT topics built from mqtt.device_code
String getTelemetryTopic();   // compostlab/v2/{device_code}/telemetry
String getResponseTopic();    // compostlab/v2/{device_code}/response
String getRegisterTopic();    // compostlab/v2/{device_code}/register

#endif
