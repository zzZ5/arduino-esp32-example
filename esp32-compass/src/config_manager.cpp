#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

// 定义全局 appConfig
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
	appConfig.wifiSSID = doc["wifi"]["ssid"] | "LHJD";
	appConfig.wifiPass = doc["wifi"]["password"] | "lhjd8888";

	// mqtt
	appConfig.mqttServer = doc["mqtt"]["server"] | "118.25.108.254";
	appConfig.mqttPort = doc["mqtt"]["port"] | 1883;
	appConfig.mqttUser = doc["mqtt"]["user"] | "equipment";
	appConfig.mqttPass = doc["mqtt"]["pass"] | "ZNXK8888";
	appConfig.mqttClientId = doc["mqtt"]["clientId"] | "linhu";
	appConfig.mqttTopic = doc["mqtt"]["topic"] | "compostlab/test/post";

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

	// pump & interval
	appConfig.pumpRunTime = doc["pump_run_time"] | 60000;
	appConfig.readInterval = doc["read_interval"] | 300000;

	// ===== 新增: equipment_key =====
	appConfig.equipmentKey = doc["equipment_key"] | "";

	// ===== 新增: keys => CO, O2, CH4, H2S
	JsonObject keysObj = doc["keys"].as<JsonObject>();
	if (!keysObj.isNull()) {
		appConfig.keyCO = keysObj["CO"] | "";
		appConfig.keyO2 = keysObj["O2"] | "";
		appConfig.keyCH4 = keysObj["CH4"] | "";
		appConfig.keyH2S = keysObj["H2S"] | "";
	}

	return true;
}

void printConfig(const AppConfig& cfg) {
	Serial.println("----- AppConfig -----");
	// Wi-Fi
	Serial.print("WiFi SSID: "); Serial.println(cfg.wifiSSID);
	Serial.print("WiFi PASS: "); Serial.println(cfg.wifiPass);
	// MQTT
	Serial.print("MQTT Server: "); Serial.println(cfg.mqttServer);
	Serial.print("MQTT Port: ");   Serial.println(cfg.mqttPort);
	Serial.print("MQTT User: ");   Serial.println(cfg.mqttUser);
	Serial.print("MQTT Pass: ");   Serial.println(cfg.mqttPass);
	Serial.print("MQTT ClientId: "); Serial.println(cfg.mqttClientId);
	Serial.print("MQTT Topic: ");    Serial.println(cfg.mqttTopic);
	// NTP
	Serial.println("NTP servers:");
	for (int i = 0;i < 3;i++) {
		Serial.printf("  [%d] %s\n", i, cfg.ntpServers[i].c_str());
	}
	// pump & interval
	Serial.printf("PumpRunTime=%lu, readInterval=%lu\n", cfg.pumpRunTime, cfg.readInterval);

	// 新增
	Serial.print("equipment_key: "); Serial.println(cfg.equipmentKey);

	Serial.println("keys:");
	Serial.print("  CO=");  Serial.println(cfg.keyCO);
	Serial.print("  O2=");  Serial.println(cfg.keyO2);
	Serial.print("  CH4="); Serial.println(cfg.keyCH4);
	Serial.print("  H2S="); Serial.println(cfg.keyH2S);

	Serial.println("---------------------");
}

bool saveConfigToSPIFFS(const char* path) {
	// 构建 JSON 文档
	StaticJsonDocument<2048> doc;

	// wifi
	doc["wifi"]["ssid"] = appConfig.wifiSSID;
	doc["wifi"]["password"] = appConfig.wifiPass;

	// mqtt
	doc["mqtt"]["server"] = appConfig.mqttServer;
	doc["mqtt"]["port"] = appConfig.mqttPort;
	doc["mqtt"]["user"] = appConfig.mqttUser;
	doc["mqtt"]["pass"] = appConfig.mqttPass;
	doc["mqtt"]["clientId"] = appConfig.mqttClientId;
	doc["mqtt"]["topic"] = appConfig.mqttTopic;

	// ntp_servers
	JsonArray ntpArr = doc.createNestedArray("ntp_servers");
	for (int i = 0; i < 3; i++) {
		ntpArr.add(appConfig.ntpServers[i]);
	}

	// pump_run_time & read_interval
	doc["pump_run_time"] = appConfig.pumpRunTime;
	doc["read_interval"] = appConfig.readInterval;

	// equipment_key
	doc["equipment_key"] = appConfig.equipmentKey;

	// keys
	JsonObject keysObj = doc.createNestedObject("keys");
	keysObj["CO"] = appConfig.keyCO;
	keysObj["O2"] = appConfig.keyO2;
	keysObj["CH4"] = appConfig.keyCH4;
	keysObj["H2S"] = appConfig.keyH2S;

	// 打开文件(覆盖写)
	File file = SPIFFS.open(path, FILE_WRITE);
	if (!file) {
		Serial.printf("[Config] open %s fail for write!\n", path);
		return false;
	}

	// 序列化写入 JSON
	if (serializeJson(doc, file) == 0) {
		Serial.println("[Config] serializeJson fail or file write fail");
		file.close();
		return false;
	}
	file.close();

	Serial.printf("[Config] Saved config to %s\n", path);
	return true;
}