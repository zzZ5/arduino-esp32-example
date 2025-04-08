#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

AppConfig appConfig;  // 定义全局配置

bool initSPIFFS() {
	if (!SPIFFS.begin(true)) {
		Serial.println("[SPIFFS] mount failed!");
		return false;
	}
	return true;
}

bool loadConfigFromSPIFFS(const char* path) {
	File file = SPIFFS.open(path, "r");
	if (!file) {
		Serial.println("[Config] File not found!");
		return false;
	}

	StaticJsonDocument<1024> doc;
	DeserializationError err = deserializeJson(doc, file);
	file.close();
	if (err) {
		Serial.print("[Config] JSON parse error: ");
		Serial.println(err.c_str());
		return false;
	}

	// 读取字段
	appConfig.wifiSSID = (const char*)doc["wifi"]["ssid"];
	appConfig.wifiPass = (const char*)doc["wifi"]["password"];
	appConfig.mqttServer = (const char*)doc["mqtt"]["server"];
	appConfig.mqttPort = doc["mqtt"]["port"];
	appConfig.mqttUser = (const char*)doc["mqtt"]["user"];
	appConfig.mqttPass = (const char*)doc["mqtt"]["pass"];
	appConfig.mqttClientId = (const char*)doc["mqtt"]["clientId"];
	appConfig.mqttTopic = (const char*)doc["mqtt"]["topic"];

	appConfig.pumpRunTime = doc["pump_run_time"] | 60000UL;
	appConfig.readInterval = doc["read_interval"] | 300000UL;

	JsonArray ntpArr = doc["ntp_servers"].as<JsonArray>();
	if (!ntpArr.isNull()) {
		int i = 0;
		for (JsonVariant v : ntpArr) {
			if (i < 3) {
				appConfig.ntpServers[i] = (const char*)v;
				i++;
			}
		}
	}
	return true;
}

bool saveConfigToSPIFFS(const char* path) {
	// 将 appConfig 写回文件
	StaticJsonDocument<1024> doc;

	JsonObject wifiObj = doc.createNestedObject("wifi");
	wifiObj["ssid"] = appConfig.wifiSSID;
	wifiObj["password"] = appConfig.wifiPass;

	JsonObject mqttObj = doc.createNestedObject("mqtt");
	mqttObj["server"] = appConfig.mqttServer;
	mqttObj["port"] = appConfig.mqttPort;
	mqttObj["user"] = appConfig.mqttUser;
	mqttObj["pass"] = appConfig.mqttPass;
	mqttObj["clientId"] = appConfig.mqttClientId;
	mqttObj["topic"] = appConfig.mqttTopic;

	doc["pump_run_time"] = appConfig.pumpRunTime;
	doc["read_interval"] = appConfig.readInterval;

	JsonArray ntpArr = doc.createNestedArray("ntp_servers");
	for (int i = 0;i < 3;i++) {
		ntpArr.add(appConfig.ntpServers[i]);
	}

	File file = SPIFFS.open(path, "w");
	if (!file) {
		Serial.println("[Config] open for write fail!");
		return false;
	}
	if (serializeJson(doc, file) == 0) {
		Serial.println("[Config] write JSON fail!");
		file.close();
		return false;
	}
	file.close();
	Serial.println("[Config] saved to SPIFFS");
	return true;
}

void printConfig(const AppConfig& cfg) {
	Serial.println("----- AppConfig -----");
	Serial.println("WiFi: " + cfg.wifiSSID + " / " + cfg.wifiPass);
	Serial.printf("MQTT: %s:%d user=%s pass=%s\n",
		cfg.mqttServer.c_str(),
		cfg.mqttPort,
		cfg.mqttUser.c_str(),
		cfg.mqttPass.c_str());
	Serial.println("ClientId=" + cfg.mqttClientId);
	Serial.println("Topic=" + cfg.mqttTopic);

	Serial.printf("PumpRunTime=%lu  readInterval=%lu\n",
		cfg.pumpRunTime,
		cfg.readInterval);

	for (int i = 0;i < 3;i++) {
		Serial.print("NTP[" + String(i) + "]: ");
		Serial.println(cfg.ntpServers[i]);
	}
	Serial.println("---------------------");
}
