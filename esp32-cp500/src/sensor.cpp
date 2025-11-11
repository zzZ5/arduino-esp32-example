#include <Arduino.h>
#include "sensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>

// ===================== DS18B20 总线/句柄 =====================
static OneWire* oneWireIn = nullptr;
static DallasTemperature* sensorIn = nullptr;
static int numInSensors = 0;

static OneWire* oneWireOut = nullptr;
static DallasTemperature* sensorOut = nullptr;
static int numOutSensors = 0;

// ===================== 控制引脚（数字输出 & PWM） =====================
static int heaterPinGlobal = -1;     // 加热继电器/固态模块
static int pumpPinGlobal = -1;       // 循环泵
static int aerationPinGlobal = -1;   // 曝气 MOS 模块 PWM 输入

// ===================== LEDC（ESP32 PWM）参数 =====================
static const int  AERATION_LEDC_FREQ_HZ = 1000;   // 1kHz，适合 D4184 等
static const int  AERATION_LEDC_RES_BITS = 10;    // 10bit: 0..1023
static int aerationChannel = -1;                  // 新版自动分配通道

// ===================== 曝气运行参数 =====================
static int g_aerCurrentDutyPct = 0;   // 当前占空 0~100
static int g_aerMaxDutyPct = 100;     // 上限（防止电源过载）
static int g_softOnMs = 1200;         // 软启动时间
static int g_softOffMs = 800;         // 软停止时间
static int g_kickPct = 0;             // 起动脉冲占空比
static int g_kickMs = 0;              // 起动脉冲持续时间

// ============================================================================
//                                  初始化
// ============================================================================
bool initSensors(int tempInPin, int tempOutPin, int heaterPin, int pumpPin, int aerationPin) {
	// 内总线（例如 GPIO4）
	oneWireIn = new OneWire(tempInPin);
	sensorIn = new DallasTemperature(oneWireIn);
	sensorIn->begin();
	sensorIn->requestTemperatures();
	numInSensors = sensorIn->getDeviceCount();
	Serial.printf("[TempIn] Found %d sensors\n", numInSensors);

	// 外总线（例如 GPIO5）
	oneWireOut = new OneWire(tempOutPin);
	sensorOut = new DallasTemperature(oneWireOut);
	sensorOut->begin();
	sensorOut->requestTemperatures();
	numOutSensors = sensorOut->getDeviceCount();
	Serial.printf("[TempOut] Found %d sensors\n", numOutSensors);

	// 控制引脚初始化
	heaterPinGlobal = heaterPin;
	pumpPinGlobal = pumpPin;
	aerationPinGlobal = aerationPin;

	if (heaterPinGlobal >= 0) { pinMode(heaterPinGlobal, OUTPUT); digitalWrite(heaterPinGlobal, LOW); }
	if (pumpPinGlobal >= 0) { pinMode(pumpPinGlobal, OUTPUT);   digitalWrite(pumpPinGlobal, LOW); }

	// 曝气 PWM 初始化（新版 LEDC 接口）
	if (aerationPinGlobal >= 0) {
		aerationChannel = ledcAttach(aerationPinGlobal, AERATION_LEDC_FREQ_HZ, AERATION_LEDC_RES_BITS);
		ledcWrite(aerationChannel, 0);  // 初始安全态
		g_aerCurrentDutyPct = 0;
		Serial.println("[Aeration] Mode=PWM (soft start/stop embedded)");
	}

	return true;
}

// ============================================================================
//                                温度读取
// ============================================================================
static float readTempInByIndex(int index) {
	if (!sensorIn) return NAN;
	if (index < 0 || index >= numInSensors) return NAN;
	sensorIn->requestTemperatures();
	float t = sensorIn->getTempCByIndex(index);
	Serial.printf("[TempInBus idx=%d] %.1f °C\n", index, t);
	return t;
}

float readTempIn() { return readTempInByIndex(0); }

float readTempTank() {
	if (numInSensors < 2) {
		Serial.println("[Tank] Not found (need 2nd sensor on internal bus).");
		return NAN;
	}
	return readTempInByIndex(1);
}

std::vector<float> readTempOut() {
	std::vector<float> temps;
	if (!sensorOut) return temps;

	sensorOut->requestTemperatures();
	for (int i = 0; i < numOutSensors && i < 3; ++i) {
		float t = sensorOut->getTempCByIndex(i);
		Serial.printf("[TempOut-%d] %.1f °C\n", i, t);
		temps.push_back(t);
	}
	return temps;
}

// ============================================================================
//                           加热 / 循环泵（数字开关）
// ============================================================================
void heaterOn() { if (heaterPinGlobal >= 0) { digitalWrite(heaterPinGlobal, HIGH); Serial.println("[Heater] ON"); } }
void heaterOff() { if (heaterPinGlobal >= 0) { digitalWrite(heaterPinGlobal, LOW);  Serial.println("[Heater] OFF"); } }
void pumpOn() { if (pumpPinGlobal >= 0) { digitalWrite(pumpPinGlobal, HIGH);   Serial.println("[Pump] ON"); } }
void pumpOff() { if (pumpPinGlobal >= 0) { digitalWrite(pumpPinGlobal, LOW);    Serial.println("[Pump] OFF"); } }

// ============================================================================
//                              曝气（PWM，内置软启停）
// ============================================================================
static inline int pctToDuty(int pct) {
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	const int maxCount = (1 << AERATION_LEDC_RES_BITS) - 1; // 1023
	return (int)((long)maxCount * pct / 100L);
}

static inline void writeDutyPctImmediate(int pct) {
	if (pct < 0) pct = 0;
	if (pct > g_aerMaxDutyPct) pct = g_aerMaxDutyPct;
	g_aerCurrentDutyPct = pct;
	if (aerationChannel >= 0) ledcWrite(aerationChannel, pctToDuty(pct));
}

bool aerationIsActive() { return g_aerCurrentDutyPct > 0; }

void aerationSetDutyPct(int pct) {
	writeDutyPctImmediate(pct);
	Serial.printf("[Aeration] duty=%d%% (hard)\n", g_aerCurrentDutyPct);
}

void aerationSetMaxDutyPct(int pctLimit) {
	if (pctLimit < 10)  pctLimit = 10;
	if (pctLimit > 100) pctLimit = 100;
	g_aerMaxDutyPct = pctLimit;
	if (g_aerCurrentDutyPct > g_aerMaxDutyPct) writeDutyPctImmediate(g_aerMaxDutyPct);
	Serial.printf("[Aeration] MaxDuty=%d%%\n", g_aerMaxDutyPct);
}

void aerationConfigSoft(int onMs, int offMs, int kickPct, int kickMs) {
	if (onMs >= 0) g_softOnMs = onMs;
	if (offMs >= 0) g_softOffMs = offMs;
	if (kickPct >= 0) g_kickPct = kickPct;
	if (kickMs >= 0) g_kickMs = kickMs;
	Serial.printf("[Aeration] Soft(on=%dms, off=%dms, kick=%d%%/%dms)\n",
		g_softOnMs, g_softOffMs, g_kickPct, g_kickMs);
}

// 软启动到目标占空比
void aerationOn() {
	const int target = g_aerMaxDutyPct;
	int from = g_aerCurrentDutyPct;

	// 起动脉冲（可选）
	if (g_kickPct > 0 && g_kickMs > 0 && from == 0) {
		int kp = g_kickPct;
		if (kp > g_aerMaxDutyPct) kp = g_aerMaxDutyPct;
		writeDutyPctImmediate(kp);
		delay(g_kickMs);
		from = g_aerCurrentDutyPct;
	}

	// 软爬升
	int to = target;
	if (g_softOnMs <= 0 || to == from) {
		writeDutyPctImmediate(to);
	}
	else {
		int steps = abs(to - from);
		int stepDelay = g_softOnMs / steps;
		if (stepDelay <= 0) stepDelay = 1;
		int dir = (to > from) ? 1 : -1;
		unsigned long last = millis();
		int pct = from;
		while (pct != to) {
			writeDutyPctImmediate(pct);
			pct += dir;
			while ((millis() - last) < (unsigned long)stepDelay) delay(1);
			last += stepDelay;
		}
		writeDutyPctImmediate(to);
	}
	Serial.printf("[Aeration] ON soft -> %d%%\n", g_aerCurrentDutyPct);
}

// 软停止到 0%
void aerationOff() {
	int from = g_aerCurrentDutyPct;
	int to = 0;

	if (g_softOffMs <= 0 || from == to) {
		writeDutyPctImmediate(0);
	}
	else {
		int steps = abs(from - to);
		int stepDelay = g_softOffMs / steps;
		if (stepDelay <= 0) stepDelay = 1;
		int dir = (to > from) ? 1 : -1;
		unsigned long last = millis();
		int pct = from;
		while (pct != to) {
			writeDutyPctImmediate(pct);
			pct += dir;
			while ((millis() - last) < (unsigned long)stepDelay) delay(1);
			last += stepDelay;
		}
		writeDutyPctImmediate(0);
	}
	Serial.println("[Aeration] OFF soft -> 0%");
}
