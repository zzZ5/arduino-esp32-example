#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

AppConfig appConfig;

static void fillDefaultsIfNeeded(AppConfig& c) {
	if (c.postInterval == 0) c.postInterval = 60000;
	if (c.ntpServers.empty())
		c.ntpServers = { "ntp.aliyun.com","cn.ntp.org.cn","ntp.tuna.tsinghua.edu.cn" };
}

bool initSPIFFS() {
	if (!SPIFFS.begin(true)) { Serial.println("[Config] SPIFFS mount fail!"); return false; }
	Serial.println("[Config] SPIFFS mount OK"); return true;
}

bool loadConfigFromSPIFFS(const char* path) {
	File f = SPIFFS.open(path, "r");
	if (!f) { fillDefaultsIfNeeded(appConfig); return false; }

	StaticJsonDocument<4096> doc;
	if (deserializeJson(doc, f)) { f.close(); fillDefaultsIfNeeded(appConfig); return false; }
	f.close();

	appConfig.wifiSSID = doc["wifi"]["ssid"] | "";
	appConfig.wifiPass = doc["wifi"]["password"] | "";
	appConfig.mqttServer = doc["mqtt"]["server"] | "";
	appConfig.mqttPort = doc["mqtt"]["port"] | 1883;
	appConfig.mqttUser = doc["mqtt"]["user"] | "";
	appConfig.mqttPass = doc["mqtt"]["pass"] | "";
	appConfig.mqttClientId = doc["mqtt"]["clientId"] | "esp32-dualbus";
	appConfig.mqttPostTopic = doc["mqtt"]["post_topic"] | "";
	appConfig.mqttResponseTopic = doc["mqtt"]["response_topic"] | "";
	appConfig.equipmentKey = doc["equipment_key"] | "esp32_dualbus";

	appConfig.ntpServers.clear();
	if (doc.containsKey("ntp_host") && doc["ntp_host"].is<JsonArray>())
		for (JsonVariant v : doc["ntp_host"].as<JsonArray>())
			appConfig.ntpServers.push_back(v.as<String>());

	appConfig.postInterval = doc["post_interval"] | 60000;

	appConfig.keyTemp4.clear();
	appConfig.keyTemp5.clear();
	if (doc.containsKey("keys") && doc["keys"].is<JsonObject>()) {
		JsonObject keys = doc["keys"].as<JsonObject>();
		if (keys.containsKey("temp4"))
			for (JsonVariant v : keys["temp4"].as<JsonArray>())
				appConfig.keyTemp4.push_back(v.as<String>());
		if (keys.containsKey("temp5"))
			for (JsonVariant v : keys["temp5"].as<JsonArray>())
				appConfig.keyTemp5.push_back(v.as<String>());
		if (keys.containsKey("temp") && appConfig.keyTemp4.empty())
			for (JsonVariant v : keys["temp"].as<JsonArray>())
				appConfig.keyTemp4.push_back(v.as<String>());
	}

	fillDefaultsIfNeeded(appConfig);
	return true;
}

bool saveConfigToSPIFFS(const char* path) {
	File f = SPIFFS.open(path, "w"); if (!f) return false;

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
	doc["equipment_key"] = appConfig.equipmentKey;
	doc["post_interval"] = appConfig.postInterval;

	JsonArray ntpArr = doc["ntp_host"].to<JsonArray>();
	for (auto& s : appConfig.ntpServers) ntpArr.add(s);

	JsonObject keys = doc["keys"].to<JsonObject>();
	JsonArray k4 = keys["temp4"].to<JsonArray>();
	for (auto& k : appConfig.keyTemp4) k4.add(k);
	JsonArray k5 = keys["temp5"].to<JsonArray>();
	for (auto& k : appConfig.keyTemp5) k5.add(k);

	bool ok = serializeJsonPretty(doc, f) > 0; f.close();
	Serial.println(ok ? "[Config] saved" : "[Config] save failed");
	return ok;
}

void printConfig(const AppConfig& c) {
	Serial.println("------ AppConfig (dual-bus) ------");
	Serial.println("WiFi: " + c.wifiSSID);
	Serial.println("MQTT: " + c.mqttServer + ":" + String(c.mqttPort));
	Serial.println("Post: " + c.mqttPostTopic);
	Serial.println("DeviceKey: " + c.equipmentKey);
	Serial.printf("Interval: %lu ms\n", c.postInterval);
	Serial.println("keys.temp4:");
	for (size_t i = 0;i < c.keyTemp4.size();++i) Serial.printf(" [%d] %s\n", (int)i, c.keyTemp4[i].c_str());
	Serial.println("keys.temp5:");
	for (size_t i = 0;i < c.keyTemp5.size();++i) Serial.printf(" [%d] %s\n", (int)i, c.keyTemp5[i].c_str());
	Serial.println("----------------------------------");
}
