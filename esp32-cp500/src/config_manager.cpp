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

	StaticJsonDocument<4096> doc;
	DeserializationError err = deserializeJson(doc, file);
	file.close();
	if (err) {
		Serial.print("[Config] parse error: ");
		Serial.println(err.c_str());
		return false;
	}

	// WiFi 参数
	appConfig.wifiSSID = doc["wifi"]["ssid"] | "";
	appConfig.wifiPass = doc["wifi"]["password"] | "";

	// MQTT 参数
	appConfig.mqttServer = doc["mqtt"]["server"] | "";
	appConfig.mqttPort = doc["mqtt"]["port"] | 1883;
	appConfig.mqttUser = doc["mqtt"]["user"] | "";
	appConfig.mqttPass = doc["mqtt"]["pass"] | "";
	appConfig.mqttClientId = doc["mqtt"]["clientId"] | "cp500";
	appConfig.mqttPostTopic = doc["mqtt"]["post_topic"] | "";
	appConfig.mqttResponseTopic = doc["mqtt"]["response_topic"] | "";

	// NTP 服务器
	appConfig.ntpServers.clear();
	JsonArray ntpArr = doc["ntp_host"].as<JsonArray>();
	if (!ntpArr.isNull()) {
		for (JsonVariant v : ntpArr) {
			appConfig.ntpServers.push_back(v.as<String>());
		}
	}
	if (appConfig.ntpServers.empty()) {
		appConfig.ntpServers = {
			"ntp.aliyun.com",
			"cn.ntp.org.cn",
			"ntp.tuna.tsinghua.edu.cn"
		};
	}

	// 控制参数
	appConfig.postInterval = doc["post_interval"] | 60000;
	appConfig.tempMaxDiff = doc["temp_maxdif"] | 5;

	// 温度限制参数（单位：摄氏度）
	appConfig.tempLimitOutMax = doc["temp_limitout_max"] | 75;
	appConfig.tempLimitInMax = doc["temp_limitin_max"] | 70;
	appConfig.tempLimitOutMin = doc["temp_limitout_min"] | 25;
	appConfig.tempLimitInMin = doc["temp_limitin_min"] | 25;

	// Keys
	appConfig.equipmentKey = doc["equipment_key"] | "";
	JsonObject keysObj = doc["keys"].as<JsonObject>();
	if (!keysObj.isNull()) {
		appConfig.keyTempIn = keysObj["temp_in"] | "";
		appConfig.keyTempOut.clear();
		JsonArray tempOutArr = keysObj["temp_out"].as<JsonArray>();
		if (!tempOutArr.isNull()) {
			for (JsonVariant v : tempOutArr) {
				appConfig.keyTempOut.push_back(v.as<String>());
			}
		}
	}

	return true;
}

void printConfig(const AppConfig& cfg) {
	Serial.println("----- AppConfig -----");

	Serial.print("WiFi SSID: "); Serial.println(cfg.wifiSSID);
	Serial.print("WiFi PASS: "); Serial.println(cfg.wifiPass);

	Serial.print("MQTT Server: "); Serial.println(cfg.mqttServer);
	Serial.print("MQTT Port: "); Serial.println(cfg.mqttPort);
	Serial.print("MQTT User: "); Serial.println(cfg.mqttUser);
	Serial.print("MQTT Pass: "); Serial.println(cfg.mqttPass);
	Serial.print("MQTT ClientId: "); Serial.println(cfg.mqttClientId);
	Serial.print("Post Topic: "); Serial.println(cfg.mqttPostTopic);
	Serial.print("Response Topic: "); Serial.println(cfg.mqttResponseTopic);

	Serial.println("NTP Servers:");
	for (size_t i = 0; i < cfg.ntpServers.size(); ++i) {
		Serial.printf("  [%d] %s\n", (int)i, cfg.ntpServers[i].c_str());
	}

	Serial.printf("PostInterval = %lu ms, TempMaxDiff = %lu °C\n",
		cfg.postInterval, cfg.tempMaxDiff);

	Serial.printf("Temp Limits (Out): min=%lu °C, max=%lu °C\n", cfg.tempLimitOutMin, cfg.tempLimitOutMax);
	Serial.printf("Temp Limits (In) : min=%lu °C, max=%lu °C\n", cfg.tempLimitInMin, cfg.tempLimitInMax);

	Serial.println("Equipment Key: " + cfg.equipmentKey);
	Serial.println("Key - TempIn: " + cfg.keyTempIn);
	for (size_t i = 0; i < cfg.keyTempOut.size(); ++i) {
		Serial.printf("Key - TempOut[%d]: %s\n", (int)i, cfg.keyTempOut[i].c_str());
	}

	Serial.println("---------------------");
}
