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
	appConfig.wifi.ssid = doc["wifi"]["ssid"] | "";
	appConfig.wifi.password = doc["wifi"]["password"] | "";

	// MQTT 配置
	appConfig.mqtt.server = doc["mqtt"]["server"] | "";
	appConfig.mqtt.port = doc["mqtt"]["port"] | 1883;
	appConfig.mqtt.user = doc["mqtt"]["user"] | "";
	appConfig.mqtt.pass = doc["mqtt"]["pass"] | "";
	appConfig.mqtt.clientId = doc["mqtt"]["clientId"] | "";
	appConfig.mqtt.postTopic = doc["mqtt"]["post_topic"] | "";
	appConfig.mqtt.responseTopic = doc["mqtt"]["response_topic"] | "";

	// NTP 配置
	appConfig.ntpServers.clear();
	JsonArray ntpArr = doc["ntp_host"].as<JsonArray>();
	if (!ntpArr.isNull()) {
		for (JsonVariant v : ntpArr) {
			appConfig.ntpServers.push_back(v.as<String>());
		}
	}

	// 数据键配置
	JsonArray dsArr = doc["keys"]["ds"].as<JsonArray>();
	for (int i = 0; i < 6 && i < dsArr.size(); ++i) {
		appConfig.ds[i] = dsArr[i].as<String>();
	}

	JsonArray rankArr = doc["keys"]["rank"].as<JsonArray>();
	for (int i = 0; i < 6 && i < rankArr.size(); ++i) {
		appConfig.rank[i] = rankArr[i].as<int>();
	}

	// 其它参数
	appConfig.postInterval = doc["post_interval"] | 60000;
	appConfig.tempMaxDif = doc["temp_maxdif"] | 5;
	appConfig.equipmentKey = doc["equipment_key"] | "";
	appConfig.sgp30 = doc["sgp30"] | "";

	return true;
}

/**
 * @brief 将 appConfig 中的内容写入到 JSON 配置文件
 */
bool saveConfigToSPIFFS(const char* path) {
	StaticJsonDocument<4096> doc;

	// WiFi 配置
	doc["wifi"]["ssid"] = appConfig.wifi.ssid;
	doc["wifi"]["password"] = appConfig.wifi.password;

	// MQTT 配置
	doc["mqtt"]["server"] = appConfig.mqtt.server;
	doc["mqtt"]["port"] = appConfig.mqtt.port;
	doc["mqtt"]["user"] = appConfig.mqtt.user;
	doc["mqtt"]["pass"] = appConfig.mqtt.pass;
	doc["mqtt"]["clientId"] = appConfig.mqtt.clientId;
	doc["mqtt"]["post_topic"] = appConfig.mqtt.postTopic;
	doc["mqtt"]["response_topic"] = appConfig.mqtt.responseTopic;

	// NTP 配置
	JsonArray ntpArr = doc.createNestedArray("ntp_host");
	for (const auto& server : appConfig.ntpServers) {
		ntpArr.add(server);
	}

	// 数据键配置
	JsonObject keysObj = doc.createNestedObject("keys");

	JsonArray dsArr = keysObj.createNestedArray("ds");
	for (int i = 0; i < 6; i++) {
		dsArr.add(appConfig.ds[i]);
	}

	JsonArray rankArr = keysObj.createNestedArray("rank");
	for (int i = 0; i < 6; i++) {
		rankArr.add(appConfig.rank[i]);
	}


	// 其他参数
	doc["post_interval"] = appConfig.postInterval;
	doc["temp_maxdif"] = appConfig.tempMaxDif;
	doc["equipment_key"] = appConfig.equipmentKey;
	doc["sgp30"] = appConfig.sgp30;

	// 写入文件
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

	Serial.print("WiFi SSID: "); Serial.println(cfg.wifi.ssid);
	Serial.print("WiFi Pass: "); Serial.println(cfg.wifi.password);

	Serial.print("MQTT Server: "); Serial.println(cfg.mqtt.server);
	Serial.print("MQTT Port: "); Serial.println(cfg.mqtt.port);
	Serial.print("MQTT User: "); Serial.println(cfg.mqtt.user);
	Serial.print("MQTT Pass: "); Serial.println(cfg.mqtt.pass);
	Serial.print("MQTT ClientId: "); Serial.println(cfg.mqtt.clientId);
	Serial.print("Post Topic: "); Serial.println(cfg.mqtt.postTopic);
	Serial.print("Response Topic: "); Serial.println(cfg.mqtt.responseTopic);

	Serial.println("NTP Servers:");
	for (size_t i = 0; i < cfg.ntpServers.size(); ++i) {
		Serial.printf("  [%d] %s\n", (int)i, cfg.ntpServers[i].c_str());
	}

	Serial.println("Equipment Key: " + cfg.equipmentKey);

	Serial.println("Keys:");
	Serial.print("  ds = ");
	for (int i = 0; i < 6; i++) {
		Serial.print(cfg.ds[i] + " ");
	}
	Serial.println();

	Serial.print("  rank = ");
	for (int i = 0; i < 6; i++) {
		Serial.print(cfg.rank[i]); Serial.print(" ");
	}
	Serial.println();

	Serial.print("Post Interval (ms): ");
	Serial.println(cfg.postInterval);

	Serial.print("Temp Max Diff: ");
	Serial.println(cfg.tempMaxDif);

	Serial.print("SGP30 Key: ");
	Serial.println(cfg.sgp30);

	Serial.println("---------------------");
}
