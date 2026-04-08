#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

// 全局配置对象
AppConfig appConfig;

static void ensurePointDeviceCodes() {
	if (appConfig.pointDeviceCodes.size() < AppConfig::kPointCount) {
		appConfig.pointDeviceCodes.resize(AppConfig::kPointCount);
	}
	for (size_t i = 0; i < AppConfig::kPointCount; ++i) {
		if (appConfig.pointDeviceCodes[i].length() == 0) {
			appConfig.pointDeviceCodes[i] = appConfig.deviceCode + "-P" + String(i + 1);
		}
	}
}

bool initSPIFFS() {
	if (!SPIFFS.begin(true)) {
		Serial.println("[Config] Failed to mount SPIFFS");
		return false;
	}
	Serial.println("[Config] SPIFFS mounted successfully");
	return true;
}

bool loadConfigFromSPIFFS(const char* path) {
	File file = SPIFFS.open(path, "r");
	if (!file) {
		Serial.println("[Config] Configuration file not found");
		return false;
	}

	// 调试信息：显示文件大小
	size_t fileSize = file.size();
	Serial.printf("[Config] File size: %d bytes\n", fileSize);

	if (fileSize == 0) {
		Serial.println("[Config] Configuration file is empty");
		Serial.println("[Config] Using default configuration values");
		file.close();

		// 使用默认配置
		appConfig.wifiSSID = "compostlab";
		appConfig.wifiPass = "ZNXK8888";
		appConfig.mqttServer = "";
		appConfig.mqttPort = 1883;
		appConfig.mqttUser = "";
		appConfig.mqttPass = "";
		appConfig.mqttClientId = "esp32";
		appConfig.deviceCode = "SmartCompost001";
		appConfig.pointDeviceCodes.clear();
		appConfig.ntpServers = {"ntp.aliyun.com", "cn.ntp.org.cn"};
		appConfig.sampleTime = 10000;
		appConfig.staticMeasureTime = 30000;
		appConfig.purgePumpTime = 15000;
		appConfig.readInterval = 60000;
		ensurePointDeviceCodes();

		return true;
	}

	// 读取文件内容
	String content = file.readString();
	Serial.printf("[Config] Content preview: %s\n", content.substring(0, min(100, (int)content.length())).c_str());
	file.close();

	// 重新打开文件解析
	StaticJsonDocument<4096> doc;
	DeserializationError err = deserializeJson(doc, content);
	if (err) {
		Serial.print("[Config] JSON parse error: ");
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
	appConfig.deviceCode = doc["mqtt"]["device_code"] | "SmartCompost001";
	appConfig.pointDeviceCodes.clear();
	JsonArray pointCodes = doc["mqtt"]["point_device_codes"].as<JsonArray>();
	if (!pointCodes.isNull()) {
		for (JsonVariant v : pointCodes)
			appConfig.pointDeviceCodes.push_back(v.as<String>());
	}
	// post_topic 和 response_topic 现在自动根据 device_code 生成

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
	appConfig.sampleTime = doc["sample_time"] | 10000;
	appConfig.staticMeasureTime = doc["static_measure_time"] | 30000;
	appConfig.purgePumpTime = doc["purge_pump_time"] | 15000;
	appConfig.readInterval = doc["read_interval"] | 600000;
	ensurePointDeviceCodes();

	return true;
}

bool saveConfigToSPIFFS(const char* path) {
	StaticJsonDocument<8192> doc;

	// WiFi
	doc["wifi"]["ssid"] = appConfig.wifiSSID;
	doc["wifi"]["password"] = appConfig.wifiPass;

	// MQTT
	doc["mqtt"]["server"] = appConfig.mqttServer;
	doc["mqtt"]["port"] = appConfig.mqttPort;
	doc["mqtt"]["user"] = appConfig.mqttUser;
	doc["mqtt"]["pass"] = appConfig.mqttPass;
	doc["mqtt"]["clientId"] = appConfig.mqttClientId;
	doc["mqtt"]["device_code"] = appConfig.deviceCode;
	JsonArray pointCodes = doc["mqtt"].createNestedArray("point_device_codes");
	for (auto& code : appConfig.pointDeviceCodes)
		pointCodes.add(code);
	// post_topic 和 response_topic 根据 device_code 自动生成，不需要保存

	// NTP
	JsonArray ntpArr = doc.createNestedArray("ntp_servers");
	for (auto& s : appConfig.ntpServers)
		ntpArr.add(s);

	// 控制参数
	doc["sample_time"] = appConfig.sampleTime;
	doc["static_measure_time"] = appConfig.staticMeasureTime;
	doc["purge_pump_time"] = appConfig.purgePumpTime;
	doc["read_interval"] = appConfig.readInterval;

	// 写回文件
	File file = SPIFFS.open(path, FILE_WRITE);
	if (!file) {
		Serial.printf("[Config] Failed to open %s for writing\n", path);
		return false;
	}
	if (serializeJson(doc, file) == 0) {
		Serial.println("[Config] Failed to serialize configuration JSON");
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
	Serial.println("Device Code: " + cfg.deviceCode);
	for (size_t i = 0; i < cfg.pointDeviceCodes.size(); ++i)
		Serial.printf("Point %u Device Code: %s\n", (unsigned)(i + 1), cfg.pointDeviceCodes[i].c_str());

	Serial.println("---------------------");
}
