#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <vector>

struct AppConfig {
	// —— 网络/MQTT/NTP ——
	String wifiSSID;
	String wifiPass;

	String mqttServer;
	uint16_t mqttPort;
	String mqttUser;
	String mqttPass;
	String mqttClientId;
	String mqttPostTopic;
	String mqttResponseTopic;

	std::vector<String> ntpServers;

	// —— 基础控制参数 ——
	uint32_t postInterval;   // 上报/测控周期(ms)
	uint32_t tempMaxDiff;    // n-curve用的最大差阈(°C)

	// —— 温度上下限（单位：°C）——
	uint32_t tempLimitOutMax;
	uint32_t tempLimitInMax;
	uint32_t tempLimitOutMin;
	uint32_t tempLimitInMin;

	// —— 业务键值 ——
	String equipmentKey;
	String keyTempIn;
	std::vector<String> keyTempOut;

	// —— 曝气定时策略（单位：毫秒）——
	bool     aerationTimerEnabled;
	uint32_t aerationInterval;
	uint32_t aerationDuration;

	// ======================== 可远程调参（分组来源） ========================
	// safety
	float    tankTempMax;      // 水箱温度上限(°C)

	// heater_guard
	uint32_t heaterMinOnMs;    // 加热器最短开机(ms)
	uint32_t heaterMinOffMs;   // 加热器最短关机(ms)

	// pump_adaptive
	float pumpDeltaOnMin;      // Δ_on 低温下限(°C)
	float pumpDeltaOnMax;      // Δ_on 高温上限(°C)
	float pumpHystNom;         // 名义回差(°C)：用于推导 Δ_off
	float pumpNCurveGamma;     // Δ_on 的 n-curve 指数

	// pump_learning
	float pumpLearnStepUp;     // 仅泵无效→阈值上调步长(°C/次)
	float pumpLearnStepDown;   // 仅泵有效/未仅泵→缓慢回落(°C/次)
	float pumpLearnMax;        // 学习补偿上限(°C)
	float pumpProgressMin;     // 判断仅泵“有效”的最小升温阈(°C)

	// curves
	float inDiffNCurveGamma;   // (t_in→差阈) 的 n-curve 指数

	// ===== 新增：外浴层定置控温（bath_setpoint） =====
	bool  bathSetEnabled;      // 开/关
	float bathSetTarget;       // 目标外浴中位温(°C)
	float bathSetHyst;         // 回差(°C)
};

extern AppConfig appConfig;

bool initSPIFFS();
bool loadConfigFromSPIFFS(const char* path);
bool saveConfigToSPIFFS(const char* path);
void printConfig(const AppConfig& cfg);

#endif
