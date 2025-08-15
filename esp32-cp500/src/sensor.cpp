#include "sensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>

// ========== DS18B20 结构 ==========

static OneWire* oneWireIn = nullptr;
static DallasTemperature* sensorIn = nullptr;
static int numInSensors = 0;                 // 内部总线数量

static OneWire* oneWireOut = nullptr;
static DallasTemperature* sensorOut = nullptr;
static int numOutSensors = 0;

// ========== 控制引脚 ==========

static int heaterPinGlobal = -1;
static int pumpPinGlobal = -1;
static int aerationPinGlobal = -1;

/**
 * 初始化温度传感器和加热/曝气控制引脚
 */
bool initSensors(int tempInPin, int tempOutPin, int heaterPin, int pumpPin, int aerationPin) {
	// 内部温度（GPIO4）
	oneWireIn = new OneWire(tempInPin);
	sensorIn = new DallasTemperature(oneWireIn);
	sensorIn->begin();

	sensorIn->requestTemperatures();
	numInSensors = sensorIn->getDeviceCount();
	Serial.printf("[TempIn] Found %d sensors\n", numInSensors);

	// 外部温度（GPIO5）
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

	pinMode(heaterPinGlobal, OUTPUT);
	digitalWrite(heaterPinGlobal, LOW);  // 默认关闭

	pinMode(pumpPinGlobal, OUTPUT);
	digitalWrite(pumpPinGlobal, LOW);

	pinMode(aerationPinGlobal, OUTPUT);
	digitalWrite(aerationPinGlobal, LOW); // 默认关闭

	return true;
}

// ========== 温度读取 ==========

// 按索引读内部总线（GPIO4）多个探头
float readTempInByIndex(int index) {
	if (!sensorIn) return NAN;
	if (index < 0 || index >= numInSensors) return NAN;

	sensorIn->requestTemperatures();            // 触发转换
	float t = sensorIn->getTempCByIndex(index); // 按索引取值
	Serial.printf("[TempInBus idx=%d] %.1f °C\n", index, t);
	return t;
}


float readTempIn() {
	return readTempInByIndex(0);
}

float readTempTank() {
	if (numInSensors < 2) {
		Serial.println("[Tank] Not found (need 2nd sensor on GPIO4).");
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

// ========== 水浴加热控制 ==========

void heaterOn() {
	if (heaterPinGlobal >= 0) {
		digitalWrite(heaterPinGlobal, HIGH);
		Serial.println("[Heater] ON");
	}
}

void heaterOff() {
	if (heaterPinGlobal >= 0) {
		digitalWrite(heaterPinGlobal, LOW);
		Serial.println("[Heater] OFF");
	}
}

// ========== 水浴泵控制 ==========

void pumpOn() {
	if (pumpPinGlobal >= 0) {
		digitalWrite(pumpPinGlobal, HIGH);
		Serial.println("[Pump] ON");
	}
}

void pumpOff() {
	if (pumpPinGlobal >= 0) {
		digitalWrite(pumpPinGlobal, LOW);
		Serial.println("[Pump] OFF");
	}
}


// ========== 曝气控制 ==========

void aerationOn() {
	if (aerationPinGlobal >= 0) {
		digitalWrite(aerationPinGlobal, HIGH);
		Serial.println("[Aeration] ON");
	}
}

void aerationOff() {
	if (aerationPinGlobal >= 0) {
		digitalWrite(aerationPinGlobal, LOW);
		Serial.println("[Aeration] OFF");
	}
}
