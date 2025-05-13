#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

AppConfig appConfig;

bool initSPIFFS() {
	if (!SPIFFS.begin(true)) {
		Serial.println("[Config] SPIFFS mount fail!");
		return false;
	}
	Serial.println("[Config] SPIFFS mount OK");
	return true;
}

bool loadConfigFromSPIFFS(const char* path) {
	File file = SPIFFS.open(path, "r");
	if (!file) {
		Serial.println("[Config] no config file");
		return false;
	}

	StaticJsonDocument<2048> doc;
	DeserializationError err = deserializeJson(doc, file);
	file.close();
	if (err) {
		Serial.print("[Config] parse error: ");
		Serial.println(err.c_str());
		return false;
	}

	// wifi
	appConfig.wifiSSID = doc["wifi"]["ssid"] | "";
	appConfig.wifiPass = doc["wifi"]["password"] | "";

	// mqtt
	appConfig.mqttServer = doc["mqtt"]["server"] | "";
	appConfig.mqttPort = doc["mqtt"]["port"] | 1883;
	appConfig.mqttUser = doc["mqtt"]["user"] | "";
	appConfig.mqttPass = doc["mqtt"]["pass"] | "";
	appConfig.mqttClientId = doc["mqtt"]["clientId"] | "esp32";
	appConfig.mqttTopic = doc["mqtt"]["topic"] | "compostlab/test";

	// ntp
	JsonArray ntpArr = doc["ntp_servers"].as<JsonArray>();
	if (!ntpArr.isNull()) {
		int i = 0;
		for (JsonVariant v : ntpArr) {
			if (i < 3) {
				appConfig.ntpServers[i] = v.as<String>();
				i++;
			}
		}
	}
	else {
		// fallback
		appConfig.ntpServers[0] = "ntp.aliyun.com";
		appConfig.ntpServers[1] = "cn.ntp.org.cn";
		appConfig.ntpServers[2] = "ntp.tuna.tsinghua.edu.cn";
	}

	// interval
	appConfig.readInterval = doc["read_interval"] | 60000;

	// equipment key
	appConfig.equipmentKey = doc["equipment_key"] | "";

	// keys for water content
	JsonObject keys = doc["keys"];
	appConfig.keyWater1 = keys["WaterContent1"] | "soil_moisture_1";
	appConfig.keyWater2 = keys["WaterContent2"] | "soil_moisture_2";
	appConfig.keyWater3 = keys["WaterContent3"] | "soil_moisture_3";

	return true;
}

void printConfig(const AppConfig& cfg) {
	Serial.println("----- AppConfig -----");
	Serial.print("WiFi SSID: "); Serial.println(cfg.wifiSSID);
	Serial.print("MQTT Server: "); Serial.println(cfg.mqttServer);
	Serial.print("MQTT Topic: "); Serial.println(cfg.mqttTopic);
	Serial.print("Read Interval: "); Serial.println(cfg.readInterval);
	Serial.print("Equipment Key: "); Serial.println(cfg.equipmentKey);

	Serial.println("NTP Servers:");
	for (int i = 0; i < 3; i++) {
		Serial.printf("  [%d] %s\n", i, cfg.ntpServers[i].c_str());
	}

	Serial.println("Keys:");
	Serial.println("  WaterContent1: " + cfg.keyWater1);
	Serial.println("  WaterContent2: " + cfg.keyWater2);
	Serial.println("  WaterContent3: " + cfg.keyWater3);
	Serial.println("---------------------");
}

bool saveConfigToSPIFFS(const char* path) {
	StaticJsonDocument<2048> doc;

	doc["wifi"]["ssid"] = appConfig.wifiSSID;
	doc["wifi"]["password"] = appConfig.wifiPass;

	doc["mqtt"]["server"] = appConfig.mqttServer;
	doc["mqtt"]["port"] = appConfig.mqttPort;
	doc["mqtt"]["user"] = appConfig.mqttUser;
	doc["mqtt"]["pass"] = appConfig.mqttPass;
	doc["mqtt"]["clientId"] = appConfig.mqttClientId;
	doc["mqtt"]["topic"] = appConfig.mqttTopic;

	JsonArray ntpArr = doc.createNestedArray("ntp_servers");
	for (int i = 0; i < 3; i++) {
		ntpArr.add(appConfig.ntpServers[i]);
	}

	doc["read_interval"] = appConfig.readInterval;
	doc["equipment_key"] = appConfig.equipmentKey;

	JsonObject keys = doc.createNestedObject("keys");
	keys["WaterContent1"] = appConfig.keyWater1;
	keys["WaterContent2"] = appConfig.keyWater2;
	keys["WaterContent3"] = appConfig.keyWater3;

	File file = SPIFFS.open(path, FILE_WRITE);
	if (!file) {
		Serial.printf("[Config] open %s fail for write!\n", path);
		return false;
	}

	if (serializeJson(doc, file) == 0) {
		Serial.println("[Config] serializeJson fail");
		file.close();
		return false;
	}

	file.close();
	Serial.printf("[Config] Saved config to %s\n", path);
	return true;
}
