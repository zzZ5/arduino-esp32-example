#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

// 全局配置对象
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

	StaticJsonDocument<4096> doc;
	DeserializationError err = deserializeJson(doc, file);
	file.close();
	if (err) {
		Serial.print("[Config] parse error: ");
		Serial.println(err.c_str());
		return false;
	}

	// WiFi 配置
	appConfig.wifiSSID = doc["wifi"]["ssid"] | "compostlab";
	appConfig.wifiPass = doc["wifi"]["password"] | "ZNXK8888";

	// MQTT
	appConfig.mqttServer = doc["mqtt"]["server"] | "";
	appConfig.mqttPort = doc["mqtt"]["port"] | 1883;
	appConfig.mqttUser = doc["mqtt"]["user"] | "";
	appConfig.mqttPass = doc["mqtt"]["pass"] | "";
	appConfig.mqttClientId = doc["mqtt"]["clientId"] | "esp32";
	appConfig.mqttPostTopic = doc["mqtt"]["post_topic"] | "";
	appConfig.mqttResponseTopic = doc["mqtt"]["response_topic"] | "";

	// NTP servers
	appConfig.ntpServers.clear();
	JsonArray ntpArr = doc["ntp_servers"].as<JsonArray>();
	if (!ntpArr.isNull()) {
		for (JsonVariant v : ntpArr)
			appConfig.ntpServers.push_back(v.as<String>());
	}
	if (appConfig.ntpServers.empty()) {
		appConfig.ntpServers = {
			"ntp.aliyun.com",
			"cn.ntp.org.cn",
			"ntp.tuna.tsinghua.edu.cn"
		};
	}

	// 控制参数
	appConfig.pumpRunTime = doc["pump_run_time"] | 60000;
	appConfig.readInterval = doc["read_interval"] | 600000;

	// keys
	appConfig.equipmentKey = doc["equipment_key"] | "";
	JsonObject keysObj = doc["keys"].as<JsonObject>();
	if (!keysObj.isNull()) {
		appConfig.keyCO2 = keysObj["CO2"] | "";
		appConfig.keyO2 = keysObj["O2"] | "";
		appConfig.keyRoomTemp = keysObj["RoomTemp"] | "";
		appConfig.keyMois = keysObj["Mois"] | "";
		appConfig.keyAirTemp = keysObj["AirTemp"] | "";       // ★ 新增
		appConfig.keyAirHum = keysObj["AirHumidity"] | "";   // ★ 新增
	}

	return true;
}

bool saveConfigToSPIFFS(const char* path) {
	StaticJsonDocument<4096> doc;

	// WiFi
	doc["wifi"]["ssid"] = appConfig.wifiSSID;
	doc["wifi"]["password"] = appConfig.wifiPass;

	// MQTT
	doc["mqtt"]["server"] = appConfig.mqttServer;
	doc["mqtt"]["port"] = appConfig.mqttPort;
	doc["mqtt"]["user"] = appConfig.mqttUser;
	doc["mqtt"]["pass"] = appConfig.mqttPass;
	doc["mqtt"]["clientId"] = appConfig.mqttClientId;
	doc["mqtt"]["post_topic"] = appConfig.mqttPostTopic;
	doc["mqtt"]["response_topic"] = appConfig.mqttResponseTopic;

	// NTP
	JsonArray ntpArr = doc.createNestedArray("ntp_servers");
	for (auto& s : appConfig.ntpServers)
		ntpArr.add(s);

	// 控制参数
	doc["pump_run_time"] = appConfig.pumpRunTime;
	doc["read_interval"] = appConfig.readInterval;

	// keys
	doc["equipment_key"] = appConfig.equipmentKey;
	JsonObject keysObj = doc.createNestedObject("keys");

	keysObj["CO2"] = appConfig.keyCO2;
	keysObj["O2"] = appConfig.keyO2;
	keysObj["RoomTemp"] = appConfig.keyRoomTemp;
	keysObj["Mois"] = appConfig.keyMois;
	keysObj["AirTemp"] = appConfig.keyAirTemp;           // ★ 新增
	keysObj["AirHumidity"] = appConfig.keyAirHum;        // ★ 新增

	// 写回文件
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

void printConfig(const AppConfig& cfg) {
	Serial.println("----- AppConfig -----");

	Serial.println("WiFi SSID: " + cfg.wifiSSID);
	Serial.println("MQTT Server: " + cfg.mqttServer);

	Serial.println("Keys:");
	Serial.println("  CO2=" + cfg.keyCO2);
	Serial.println("  O2=" + cfg.keyO2);
	Serial.println("  RoomTemp=" + cfg.keyRoomTemp);
	Serial.println("  Mois=" + cfg.keyMois);
	Serial.println("  AirTemp=" + cfg.keyAirTemp);     // ★ 新增
	Serial.println("  AirHumidity=" + cfg.keyAirHum);  // ★ 新增

	Serial.println("---------------------");
}
