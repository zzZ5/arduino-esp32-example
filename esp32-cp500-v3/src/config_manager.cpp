#include "config_manager.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

AppConfig appConfig;

// --------- 默认值（用于配置缺省或升级兼容）---------
static void fillDefaultsIfNeeded(AppConfig& c) {
	// MQTT device code
	if (c.mqttDeviceCode.length() == 0) c.mqttDeviceCode = "unknown";

	// 基础
	if (c.postInterval == 0)   c.postInterval = 60000;
	if (c.tempMaxDiff == 0)    c.tempMaxDiff = 5;

	// 温度上下限
	if (c.tempLimitOutMax == 0) c.tempLimitOutMax = 75;
	if (c.tempLimitInMax == 0)  c.tempLimitInMax = 70;
	if (c.tempLimitOutMin == 0) c.tempLimitOutMin = 25;
	if (c.tempLimitInMin == 0)  c.tempLimitInMin = 25;

	// 曝气
	if (c.aerationInterval == 0) c.aerationInterval = 600000; // 10min
	if (c.aerationDuration == 0) c.aerationDuration = 300000; // 5min

	// —— 分组参数默认（与你 main.cpp 的常量逻辑一致）——
	if (c.tankTempMax <= 0) c.tankTempMax = 90.0f;

	if (c.heaterMinOnMs == 0)  c.heaterMinOnMs = 30000;
	if (c.heaterMinOffMs == 0) c.heaterMinOffMs = 30000;

	if (c.pumpDeltaOnMin <= 0)   c.pumpDeltaOnMin = 6.0f;
	if (c.pumpDeltaOnMax <= 0)   c.pumpDeltaOnMax = 25.0f;
	if (c.pumpHystNom <= 0)    c.pumpHystNom = 3.0f;
	if (c.pumpNCurveGamma <= 0)  c.pumpNCurveGamma = 1.3f;

	if (c.pumpLearnStepUp <= 0) c.pumpLearnStepUp = 0.5f;
	if (c.pumpLearnStepDown <= 0) c.pumpLearnStepDown = 0.2f;
	if (c.pumpLearnMax <= 0) c.pumpLearnMax = 8.0f;
	if (c.pumpProgressMin <= 0) c.pumpProgressMin = 0.05f;

	if (c.inDiffNCurveGamma <= 0) c.inDiffNCurveGamma = 2.0f;

	// —— 新增：bath_setpoint 默认值 ——
	// 若用户未配置该分组，保持关闭，但给出合理默认参数，便于后续远程启用
	// target 默认 45℃、回差 0.8℃
	// 注意：main.cpp 会在运行时将 target 夹紧到 < tempLimitOutMax
	c.bathSetEnabled = c.bathSetEnabled; // 若未写入，保持 false（全局静态默认是随机的，这里不依赖）
	if (c.bathSetTarget <= 0) c.bathSetTarget = 45.0f;
	if (c.bathSetHyst <= 0) c.bathSetHyst = 0.8f;
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

	JsonDocument doc;
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
	appConfig.mqttDeviceCode = doc["mqtt"]["device_code"] | "";

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

	// Keys 字段已移除，不再读取

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
	JsonObject bathSet = doc["bath_setpoint"];

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

	// ===== 新增：bath_setpoint =====
	appConfig.bathSetEnabled = readB(bathSet, "enabled", appConfig.bathSetEnabled);
	appConfig.bathSetTarget = readF(bathSet, "target", appConfig.bathSetTarget);
	appConfig.bathSetHyst = readF(bathSet, "hyst", appConfig.bathSetHyst);

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

	Serial.println("MQTT Device Code: " + cfg.mqttDeviceCode);

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

	// ===== 新增：bath_setpoint 打印 =====
	Serial.println("Bath Setpoint:");
	Serial.printf("  enabled              : %s\n", cfg.bathSetEnabled ? "true" : "false");
	Serial.printf("  target               : %.2f °C\n", cfg.bathSetTarget);
	Serial.printf("  hyst                 : %.2f °C\n", cfg.bathSetHyst);

	Serial.println("MQTT Topics:");
	Serial.printf("  telemetry            : %s\n", getTelemetryTopic().c_str());
	Serial.printf("  response             : %s\n", getResponseTopic().c_str());

	Serial.println("---------------------");
}

// ========================= MQTT Topic 构建函数 =========================
String getTelemetryTopic() {
	return String("compostlab/v2/") + appConfig.mqttDeviceCode + "/telemetry";
}

String getResponseTopic() {
	return String("compostlab/v2/") + appConfig.mqttDeviceCode + "/response";
}

String getRegisterTopic() {
	return String("compostlab/v2/") + appConfig.mqttDeviceCode + "/register";
}

bool saveConfigToSPIFFS(const char* path) {
	File file = SPIFFS.open(path, "w");
	if (!file) {
		Serial.println("[Config] Failed to open config file for writing!");
		return false;
	}

	JsonDocument doc;

	// —— WiFi ——
	doc["wifi"]["ssid"] = appConfig.wifiSSID;
	doc["wifi"]["password"] = appConfig.wifiPass;

	// —— MQTT ——
	doc["mqtt"]["server"] = appConfig.mqttServer;
	doc["mqtt"]["port"] = appConfig.mqttPort;
	doc["mqtt"]["user"] = appConfig.mqttUser;
	doc["mqtt"]["pass"] = appConfig.mqttPass;
	doc["mqtt"]["device_code"] = appConfig.mqttDeviceCode;

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

	// Keys 字段已移除，不再保存

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
	JsonObject bathSet = doc["bath_setpoint"].to<JsonObject>();

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

	// ===== 新增：bath_setpoint 保存 =====
	bathSet["enabled"] = appConfig.bathSetEnabled;
	bathSet["target"] = appConfig.bathSetTarget;
	bathSet["hyst"] = appConfig.bathSetHyst;

	bool ok = serializeJsonPretty(doc, file) > 0;
	file.close();

	if (ok) Serial.println("[Config] Configuration saved.");
	else    Serial.println("[Config] Failed to serialize config.");

	return ok;
}
