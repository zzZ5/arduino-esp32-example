#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

AppConfig appConfig;

// --------- 默认值（用于配置缺省或升级兼容）---------
static void fillDefaultsIfNeeded(AppConfig& c) {
	// 基础
	if (c.postInterval == 0)   c.postInterval = 60000;
	if (c.tempMaxDiff == 0)    c.tempMaxDiff = 5;

	// 温度上下限
	if (c.tempLimitOutMax == 0) c.tempLimitOutMax = 75;
	if (c.tempLimitInMax == 0) c.tempLimitInMax = 70;
	if (c.tempLimitOutMin == 0) c.tempLimitOutMin = 25;
	if (c.tempLimitInMin == 0) c.tempLimitInMin = 25;

	// 曝气
	if (c.aerationInterval == 0) c.aerationInterval = 600000; // 10min
	if (c.aerationDuration == 0) c.aerationDuration = 300000; // 5min

	// —— 分组参数默认（与你 main.cpp 的常量逻辑一致）——
	if (c.tankTempMax <= 0) c.tankTempMax = 90.0f;

	if (c.heaterMinOnMs == 0) c.heaterMinOnMs = 30000;
	if (c.heaterMinOffMs == 0) c.heaterMinOffMs = 30000;

	if (c.pumpDeltaOnMin <= 0) c.pumpDeltaOnMin = 6.0f;
	if (c.pumpDeltaOnMax <= 0) c.pumpDeltaOnMax = 25.0f;
	if (c.pumpHystNom <= 0) c.pumpHystNom = 3.0f;
	if (c.pumpNCurveGamma <= 0) c.pumpNCurveGamma = 1.3f;

	if (c.pumpLearnStepUp <= 0) c.pumpLearnStepUp = 0.5f;
	if (c.pumpLearnStepDown <= 0) c.pumpLearnStepDown = 0.2f;
	if (c.pumpLearnMax <= 0) c.pumpLearnMax = 8.0f;
	if (c.pumpProgressMin <= 0) c.pumpProgressMin = 0.05f;

	if (c.inDiffNCurveGamma <= 0) c.inDiffNCurveGamma = 2.0f;
}

bool initSPIFFS() {
	if (!SPIFFS.begin(true)) {
		Serial.println("[Config] SPIFFS mount fail!");
		return false;
	}
	Serial.println("[Config] SPIFFS mount OK");
	return true;
}

static float readF(JsonObject o, const char* k, float dv) {
	return (!o.isNull() && o.containsKey(k)) ? o[k].as<float>() : dv;
}
static uint32_t readU(JsonObject o, const char* k, uint32_t dv) {
	return (!o.isNull() && o.containsKey(k)) ? o[k].as<uint32_t>() : dv;
}
static bool readB(JsonObject o, const char* k, bool dv) {
	return (!o.isNull() && o.containsKey(k)) ? o[k].as<bool>() : dv;
}

bool loadConfigFromSPIFFS(const char* path) {
	File file = SPIFFS.open(path, "r");
	if (!file) {
		Serial.println("[Config] no config file");
		fillDefaultsIfNeeded(appConfig);
		return false;
	}

	StaticJsonDocument<4096> doc;
	DeserializationError err = deserializeJson(doc, file);
	file.close();
	if (err) {
		Serial.print("[Config] parse error: ");
		Serial.println(err.c_str());
		fillDefaultsIfNeeded(appConfig);
		return false;
	}

	// —— WiFi ——
	appConfig.wifiSSID = doc["wifi"]["ssid"] | "";
	appConfig.wifiPass = doc["wifi"]["password"] | "";

	// —— MQTT ——
	appConfig.mqttServer = doc["mqtt"]["server"] | "";
	appConfig.mqttPort = doc["mqtt"]["port"] | 1883;
	appConfig.mqttUser = doc["mqtt"]["user"] | "";
	appConfig.mqttPass = doc["mqtt"]["pass"] | "";
	appConfig.mqttClientId = doc["mqtt"]["clientId"] | "cp500";
	appConfig.mqttPostTopic = doc["mqtt"]["post_topic"] | "";
	appConfig.mqttResponseTopic = doc["mqtt"]["response_topic"] | "";

	// —— NTP ——
	appConfig.ntpServers.clear();
	JsonArray ntpArr = doc["ntp_host"].as<JsonArray>();
	if (!ntpArr.isNull()) {
		for (JsonVariant v : ntpArr) appConfig.ntpServers.push_back(v.as<String>());
	}
	if (appConfig.ntpServers.empty()) {
		appConfig.ntpServers = {
		  "ntp.aliyun.com",
		  "cn.ntp.org.cn",
		  "ntp.tuna.tsinghua.edu.cn"
		};
	}

	// —— 基础控制参数 ——
	appConfig.postInterval = doc["post_interval"] | 60000;
	appConfig.tempMaxDiff = doc["temp_maxdif"] | 5;

	// —— 温度上下限 ——
	appConfig.tempLimitOutMax = doc["temp_limitout_max"] | 75;
	appConfig.tempLimitInMax = doc["temp_limitin_max"] | 70;
	appConfig.tempLimitOutMin = doc["temp_limitout_min"] | 25;
	appConfig.tempLimitInMin = doc["temp_limitin_min"] | 25;

	// —— Keys ——
	appConfig.equipmentKey = doc["equipment_key"] | "";
	{
		JsonObject keysObj = doc["keys"].as<JsonObject>();
		if (!keysObj.isNull()) {
			appConfig.keyTempIn = keysObj["temp_in"] | "";
			appConfig.keyTempOut.clear();
			JsonArray tempOutArr = keysObj["temp_out"].as<JsonArray>();
			if (!tempOutArr.isNull()) {
				for (JsonVariant v : tempOutArr) appConfig.keyTempOut.push_back(v.as<String>());
			}
		}
	}

	// —— 曝气策略 ——
	{
		JsonObject aero = doc["aeration_timer"];
		appConfig.aerationTimerEnabled = readB(aero, "enabled", false);
		appConfig.aerationInterval = readU(aero, "interval", 600000);
		appConfig.aerationDuration = readU(aero, "duration", 300000);
	}

	// ====================== 分组调参（新的 JSON 结构） ======================
	JsonObject safety = doc["safety"];
	JsonObject heaterGuard = doc["heater_guard"];
	JsonObject pumpAdaptive = doc["pump_adaptive"];
	JsonObject pumpLearning = doc["pump_learning"];
	JsonObject curves = doc["curves"];

	// safety
	appConfig.tankTempMax = readF(safety, "tank_temp_max", appConfig.tankTempMax);

	// heater_guard
	appConfig.heaterMinOnMs = readU(heaterGuard, "min_on_ms", appConfig.heaterMinOnMs);
	appConfig.heaterMinOffMs = readU(heaterGuard, "min_off_ms", appConfig.heaterMinOffMs);

	// pump_adaptive
	appConfig.pumpDeltaOnMin = readF(pumpAdaptive, "delta_on_min", appConfig.pumpDeltaOnMin);
	appConfig.pumpDeltaOnMax = readF(pumpAdaptive, "delta_on_max", appConfig.pumpDeltaOnMax);
	appConfig.pumpHystNom = readF(pumpAdaptive, "hyst_nom", appConfig.pumpHystNom);
	appConfig.pumpNCurveGamma = readF(pumpAdaptive, "ncurve_gamma", appConfig.pumpNCurveGamma);

	// pump_learning
	appConfig.pumpLearnStepUp = readF(pumpLearning, "step_up", appConfig.pumpLearnStepUp);
	appConfig.pumpLearnStepDown = readF(pumpLearning, "step_down", appConfig.pumpLearnStepDown);
	appConfig.pumpLearnMax = readF(pumpLearning, "max", appConfig.pumpLearnMax);
	appConfig.pumpProgressMin = readF(pumpLearning, "progress_min", appConfig.pumpProgressMin);

	// curves
	appConfig.inDiffNCurveGamma = readF(curves, "in_diff_ncurve_gamma", appConfig.inDiffNCurveGamma);

	// —— 兜底默认，避免无配置时出现0或负值 ——
	fillDefaultsIfNeeded(appConfig);

	return true;
}

void printConfig(const AppConfig& cfg) {
	Serial.println("----- AppConfig -----");

	Serial.print("WiFi SSID: "); Serial.println(cfg.wifiSSID);
	Serial.print("WiFi PASS: "); Serial.println(cfg.wifiPass);

	Serial.print("MQTT Server: "); Serial.println(cfg.mqttServer);
	Serial.print("MQTT Port: ");   Serial.println(cfg.mqttPort);
	Serial.print("MQTT User: ");   Serial.println(cfg.mqttUser);
	Serial.print("MQTT Pass: ");   Serial.println(cfg.mqttPass);
	Serial.print("MQTT ClientId: ");   Serial.println(cfg.mqttClientId);
	Serial.print("Post Topic: ");      Serial.println(cfg.mqttPostTopic);
	Serial.print("Response Topic: ");  Serial.println(cfg.mqttResponseTopic);

	Serial.println("NTP Servers:");
	for (size_t i = 0; i < cfg.ntpServers.size(); ++i) {
		Serial.printf("  [%d] %s\n", (int)i, cfg.ntpServers[i].c_str());
	}

	Serial.printf("PostInterval = %lu ms, TempMaxDiff = %lu °C\n",
		cfg.postInterval, cfg.tempMaxDiff);

	Serial.printf("Temp Limits (Out): min=%lu °C, max=%lu °C\n",
		cfg.tempLimitOutMin, cfg.tempLimitOutMax);
	Serial.printf("Temp Limits (In) : min=%lu °C, max=%lu °C\n",
		cfg.tempLimitInMin, cfg.tempLimitInMax);

	Serial.println("Equipment Key: " + cfg.equipmentKey);
	Serial.println("Key - TempIn: " + cfg.keyTempIn);
	for (size_t i = 0; i < cfg.keyTempOut.size(); ++i) {
		Serial.printf("Key - TempOut[%d]: %s\n", (int)i, cfg.keyTempOut[i].c_str());
	}

	Serial.println("Aeration Timer:");
	Serial.printf("  Enabled  : %s\n", cfg.aerationTimerEnabled ? "true" : "false");
	Serial.printf("  Interval : %lu ms\n", cfg.aerationInterval);
	Serial.printf("  Duration : %lu ms\n", cfg.aerationDuration);

	Serial.println("Safety:");
	Serial.printf("  tank_temp_max        : %.2f °C\n", cfg.tankTempMax);

	Serial.println("Heater Guard:");
	Serial.printf("  min_on_ms            : %lu ms\n", cfg.heaterMinOnMs);
	Serial.printf("  min_off_ms           : %lu ms\n", cfg.heaterMinOffMs);

	Serial.println("Pump Adaptive:");
	Serial.printf("  delta_on_min         : %.2f °C\n", cfg.pumpDeltaOnMin);
	Serial.printf("  delta_on_max         : %.2f °C\n", cfg.pumpDeltaOnMax);
	Serial.printf("  hyst_nom             : %.2f °C\n", cfg.pumpHystNom);
	Serial.printf("  ncurve_gamma         : %.2f\n", cfg.pumpNCurveGamma);

	Serial.println("Pump Learning:");
	Serial.printf("  step_up              : %.2f °C/step\n", cfg.pumpLearnStepUp);
	Serial.printf("  step_down            : %.2f °C/step\n", cfg.pumpLearnStepDown);
	Serial.printf("  max                  : %.2f °C\n", cfg.pumpLearnMax);
	Serial.printf("  progress_min         : %.3f °C\n", cfg.pumpProgressMin);

	Serial.println("Curves:");
	Serial.printf("  in_diff_ncurve_gamma : %.2f\n", cfg.inDiffNCurveGamma);

	Serial.println("---------------------");
}

bool saveConfigToSPIFFS(const char* path) {
	File file = SPIFFS.open(path, "w");
	if (!file) {
		Serial.println("[Config] Failed to open config file for writing!");
		return false;
	}

	StaticJsonDocument<4096> doc;

	// —— WiFi ——
	doc["wifi"]["ssid"] = appConfig.wifiSSID;
	doc["wifi"]["password"] = appConfig.wifiPass;

	// —— MQTT ——
	doc["mqtt"]["server"] = appConfig.mqttServer;
	doc["mqtt"]["port"] = appConfig.mqttPort;
	doc["mqtt"]["user"] = appConfig.mqttUser;
	doc["mqtt"]["pass"] = appConfig.mqttPass;
	doc["mqtt"]["clientId"] = appConfig.mqttClientId;
	doc["mqtt"]["post_topic"] = appConfig.mqttPostTopic;
	doc["mqtt"]["response_topic"] = appConfig.mqttResponseTopic;

	// —— NTP ——
	{
		JsonArray ntpArr = doc["ntp_host"].to<JsonArray>();
		for (const auto& s : appConfig.ntpServers) ntpArr.add(s);
	}

	// —— 基础控制参数 ——
	doc["post_interval"] = appConfig.postInterval;
	doc["temp_maxdif"] = appConfig.tempMaxDiff;

	// —— 温度上下限 ——
	doc["temp_limitout_max"] = appConfig.tempLimitOutMax;
	doc["temp_limitin_max"] = appConfig.tempLimitInMax;
	doc["temp_limitout_min"] = appConfig.tempLimitOutMin;
	doc["temp_limitin_min"] = appConfig.tempLimitInMin;

	// —— Keys ——
	doc["equipment_key"] = appConfig.equipmentKey;
	{
		JsonObject keysObj = doc["keys"].to<JsonObject>();
		keysObj["temp_in"] = appConfig.keyTempIn;
		JsonArray outArr = keysObj["temp_out"].to<JsonArray>();
		for (const auto& k : appConfig.keyTempOut) outArr.add(k);
	}

	// —— 曝气策略 ——
	doc["aeration_timer"]["enabled"] = appConfig.aerationTimerEnabled;
	doc["aeration_timer"]["interval"] = appConfig.aerationInterval;
	doc["aeration_timer"]["duration"] = appConfig.aerationDuration;

	// —— 分组调参 —— 
	JsonObject safety = doc["safety"].to<JsonObject>();
	JsonObject heaterGuard = doc["heater_guard"].to<JsonObject>();
	JsonObject pumpAdaptive = doc["pump_adaptive"].to<JsonObject>();
	JsonObject pumpLearning = doc["pump_learning"].to<JsonObject>();
	JsonObject curves = doc["curves"].to<JsonObject>();

	safety["tank_temp_max"] = appConfig.tankTempMax;

	heaterGuard["min_on_ms"] = appConfig.heaterMinOnMs;
	heaterGuard["min_off_ms"] = appConfig.heaterMinOffMs;

	pumpAdaptive["delta_on_min"] = appConfig.pumpDeltaOnMin;
	pumpAdaptive["delta_on_max"] = appConfig.pumpDeltaOnMax;
	pumpAdaptive["hyst_nom"] = appConfig.pumpHystNom;
	pumpAdaptive["ncurve_gamma"] = appConfig.pumpNCurveGamma;

	pumpLearning["step_up"] = appConfig.pumpLearnStepUp;
	pumpLearning["step_down"] = appConfig.pumpLearnStepDown;
	pumpLearning["max"] = appConfig.pumpLearnMax;
	pumpLearning["progress_min"] = appConfig.pumpProgressMin;

	curves["in_diff_ncurve_gamma"] = appConfig.inDiffNCurveGamma;

	bool ok = serializeJsonPretty(doc, file) > 0;
	file.close();

	if (ok) Serial.println("[Config] Configuration saved.");
	else    Serial.println("[Config] Failed to serialize config.");

	return ok;
}
