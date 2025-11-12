#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

AppConfig appConfig;

static void fillDefaultsIfNeeded(AppConfig& c) {
	if (c.postInterval == 0) c.postInterval = 60000;
	if (c.ntpServers.empty()) {
		c.ntpServers = { "ntp.aliyun.com","cn.ntp.org.cn","ntp.tuna.tsinghua.edu.cn" };
	}
}

bool initSPIFFS() {
	if (!SPIFFS.begin(true)) { Serial.println("[Config] SPIFFS mount fail!"); return false; }
	Serial.println("[Config] SPIFFS mount OK"); return true;
}

bool loadConfigFromSPIFFS(const char* path) {
	File f = SPIFFS.open(path, "r");
	if (!f) { Serial.println("[Config] no config file, use defaults"); fillDefaultsIfNeeded(appConfig); return false; }

	StaticJsonDocument<4096> doc;
	DeserializationError err = deserializeJson(doc, f); f.close();
	if (err) { Serial.print("[Config] parse error: "); Serial.println(err.c_str()); fillDefaultsIfNeeded(appConfig); return false; }

	appConfig.wifiSSID = doc["wifi"]["ssid"] | "";
	appConfig.wifiPass = doc["wifi"]["password"] | "";

	appConfig.mqttServer = doc["mqtt"]["server"] | "";
	appConfig.mqttPort = doc["mqtt"]["port"] | 1883;
	appConfig.mqttUser = doc["mqtt"]["user"] | "";
	appConfig.mqttPass = doc["mqtt"]["pass"] | "";
	appConfig.mqttClientId = doc["mqtt"]["clientId"] | "esp32-dualbus";
	appConfig.mqttPostTopic = doc["mqtt"]["post_topic"] | "";
	appConfig.mqttResponseTopic = doc["mqtt"]["response_topic"] | "";

	appConfig.ntpServers.clear();
	if (doc.containsKey("ntp_host") && doc["ntp_host"].is<JsonArray>()) {
		for (JsonVariant v : doc["ntp_host"].as<JsonArray>()) appConfig.ntpServers.push_back(v.as<String>());
	}

	appConfig.postInterval = doc["post_interval"] | 60000;
	appConfig.equipmentKey = doc["equipment_key"] | "";

	appConfig.keyTemp4.clear();
	appConfig.keyTemp5.clear();
	if (doc.containsKey("keys") && doc["keys"].is<JsonObject>()) {
		JsonObject keys = doc["keys"].as<JsonObject>();
		if (keys.containsKey("temp4") && keys["temp4"].is<JsonArray>())
			for (JsonVariant v : keys["temp4"].as<JsonArray>()) appConfig.keyTemp4.push_back(v.as<String>());
		if (keys.containsKey("temp5") && keys["temp5"].is<JsonArray>())
			for (JsonVariant v : keys["temp5"].as<JsonArray>()) appConfig.keyTemp5.push_back(v.as<String>());
		// legacy: keys.temp -> 用作 pin4
		if (keys.containsKey("temp") && keys["temp"].is<JsonArray>() && appConfig.keyTemp4.empty())
			for (JsonVariant v : keys["temp"].as<JsonArray>()) appConfig.keyTemp4.push_back(v.as<String>());
	}

	fillDefaultsIfNeeded(appConfig);
	return true;
}

bool saveConfigToSPIFFS(const char* path) {
	File f = SPIFFS.open(path, "w"); if (!f) { Serial.println("[Config] open for write fail"); return false; }

	StaticJsonDocument<4096> doc;
	doc["wifi"]["ssid"] = appConfig.wifiSSID;
	doc["wifi"]["password"] = appConfig.wifiPass;

	doc["mqtt"]["server"] = appConfig.mqttServer;
	doc["mqtt"]["port"] = appConfig.mqttPort;
	doc["mqtt"]["user"] = appConfig.mqttUser;
	doc["mqtt"]["pass"] = appConfig.mqttPass;
	doc["mqtt"]["clientId"] = appConfig.mqttClientId;
	doc["mqtt"]["post_topic"] = appConfig.mqttPostTopic;
	doc["mqtt"]["response_topic"] = appConfig.mqttResponseTopic;

	{
		JsonArray ntpArr = doc["ntp_host"].to<JsonArray>();
		for (const auto& s : appConfig.ntpServers) ntpArr.add(s);
	}

	doc["post_interval"] = appConfig.postInterval;
	doc["equipment_key"] = appConfig.equipmentKey;

	// 同时写出 temp4 / temp5（如需要也可再写 legacy temp）
	{
		JsonObject keys = doc["keys"].to<JsonObject>();
		JsonArray a4 = keys["temp4"].to<JsonArray>();
		for (const auto& k : appConfig.keyTemp4) a4.add(k);
		JsonArray a5 = keys["temp5"].to<JsonArray>();
		for (const auto& k : appConfig.keyTemp5) a5.add(k);
	}

	bool ok = serializeJsonPretty(doc, f) > 0; f.close();
	Serial.println(ok ? "[Config] saved" : "[Config] save failed");
	return ok;
}

void printConfig(const AppConfig& c) {
	Serial.println("----- AppConfig (dual-bus) -----");
	Serial.print("WiFi SSID: "); Serial.println(c.wifiSSID);
	Serial.print("MQTT Server: "); Serial.println(c.mqttServer);
	Serial.print("MQTT Port  : "); Serial.println(c.mqttPort);
	Serial.print("ClientId   : "); Serial.println(c.mqttClientId);
	Serial.print("Post Topic : "); Serial.println(c.mqttPostTopic);
	Serial.print("Resp Topic : "); Serial.println(c.mqttResponseTopic);
	Serial.println("NTP Servers:");
	for (size_t i = 0;i < c.ntpServers.size();++i) Serial.printf("  [%d] %s\n", (int)i, c.ntpServers[i].c_str());
	Serial.printf("PostInterval = %lu ms\n", c.postInterval);
	Serial.println("Equipment Key: " + c.equipmentKey);
	Serial.println("keys.temp4:");
	for (size_t i = 0;i < c.keyTemp4.size();++i) Serial.printf("  [%d] %s\n", (int)i, c.keyTemp4[i].c_str());
	Serial.println("keys.temp5:");
	for (size_t i = 0;i < c.keyTemp5.size();++i) Serial.printf("  [%d] %s\n", (int)i, c.keyTemp5[i].c_str());
	Serial.println("---------------------------------");
}
