#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

// 全局配置对象
AppConfig appConfig;

/**
 * @brief 初始化 SPIFFS 文件系统
 */
bool initSPIFFS() {
	if (!SPIFFS.begin(true)) {
		Serial.println("[Config] SPIFFS mount fail!");
		return false;
	}
	Serial.println("[Config] SPIFFS mount OK");
	return true;
}

/**
 * @brief 从指定路径读取 JSON 配置文件，填充到 appConfig
 */
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

	// MQTT 配置
	appConfig.mqttServer = doc["mqtt"]["server"] | "";
	appConfig.mqttPort = doc["mqtt"]["port"] | 1883;
	appConfig.mqttUser = doc["mqtt"]["user"] | "";
	appConfig.mqttPass = doc["mqtt"]["pass"] | "";
	appConfig.mqttClientId = doc["mqtt"]["clientId"] | "esp32";
	appConfig.mqttPostTopic = doc["mqtt"]["post_topic"] | "";
	appConfig.mqttResponseTopic = doc["mqtt"]["response_topic"] | "";

	// NTP servers（支持任意个数）
	appConfig.ntpServers.clear();
	JsonArray ntpArr = doc["ntp_servers"].as<JsonArray>();
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
	appConfig.pumpRunTime = doc["pump_run_time"] | 60000;
	appConfig.readInterval = doc["read_interval"] | 600000;

	// 数据 key 映射
	appConfig.equipmentKey = doc["equipment_key"] | "";
	JsonObject keysObj = doc["keys"].as<JsonObject>();
	if (!keysObj.isNull()) {
		appConfig.keyCO2 = keysObj["CO2"] | "";
		appConfig.keyO2 = keysObj["O2"] | "";
		appConfig.keyRoomTemp = keysObj["RoomTemp"] | "";
		appConfig.keyMois = keysObj["Mois"] | "";
	}

	return true;
}

/**
 * @brief 将 appConfig 中的内容写入到 JSON 配置文件
 */
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
	for (const auto& server : appConfig.ntpServers) {
		ntpArr.add(server);
	}

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

	File file = SPIFFS.open(path, FILE_WRITE);
	if (!file) {
		Serial.printf("[Config] open %s fail for write!\n", path);
		return false;
	}

	if (serializeJson(doc, file) == 0) {
		Serial.println("[Config] serializeJson fail or file write fail");
		file.close();
		return false;
	}

	file.close();
	Serial.printf("[Config] Saved config to %s\n", path);
	return true;
}

/**
 * @brief 打印 appConfig 当前内容
 */
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

	Serial.printf("PumpRunTime = %lu ms, ReadInterval = %lu ms\n",
		cfg.pumpRunTime, cfg.readInterval);

	Serial.println("Equipment Key: " + cfg.equipmentKey);

	Serial.println("Keys:");
	Serial.print("  CO2="); Serial.println(cfg.keyCO2);
	Serial.print("  O2="); Serial.println(cfg.keyO2);
	Serial.print("  RoomTemp="); Serial.println(cfg.keyRoomTemp);
	Serial.print("  Mois="); Serial.println(cfg.keyMois);

	Serial.println("---------------------");
}
