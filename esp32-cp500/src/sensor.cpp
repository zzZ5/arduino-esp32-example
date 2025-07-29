#include "sensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>

// ========== DS18B20 结构 ==========

static OneWire* oneWireIn = nullptr;
static DallasTemperature* sensorIn = nullptr;

static OneWire* oneWireOut = nullptr;
static DallasTemperature* sensorOut = nullptr;
static int numOutSensors = 0;

// ========== 控制引脚 ==========

static int heaterPinGlobal = -1;
static int aerationPinGlobal = -1;

/**
 * 初始化温度传感器和加热/曝气控制引脚
 */
bool initTemperatureSensors(int tempInPin, int tempOutPin, int heaterPin, int aerationPin) {
	// 内部温度（GPIO4）
	oneWireIn = new OneWire(tempInPin);
	sensorIn = new DallasTemperature(oneWireIn);
	sensorIn->begin();

	// 外部温度（GPIO5）
	oneWireOut = new OneWire(tempOutPin);
	sensorOut = new DallasTemperature(oneWireOut);
	sensorOut->begin();

	sensorOut->requestTemperatures();
	numOutSensors = sensorOut->getDeviceCount();
	Serial.printf("[TempOut] Found %d sensors\n", numOutSensors);

	// 控制引脚初始化
	heaterPinGlobal = heaterPin;
	aerationPinGlobal = aerationPin;

	pinMode(heaterPinGlobal, OUTPUT);
	digitalWrite(heaterPinGlobal, LOW);  // 默认关闭

	pinMode(aerationPinGlobal, OUTPUT);
	digitalWrite(aerationPinGlobal, HIGH); // 默认关闭

	return true;
}

// ========== 温度读取 ==========

float readTempIn() {
	if (!sensorIn) return NAN;
	sensorIn->requestTemperatures();
	float t = sensorIn->getTempCByIndex(0);
	Serial.printf("[TempIn] %.1f °C\n", t);
	return t;
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

// ========== 曝气控制 ==========

void aerationOn() {
	if (aerationPinGlobal >= 0) {
		digitalWrite(aerationPinGlobal, LOW);
		Serial.println("[Aeration] ON");
	}
}

void aerationOff() {
	if (aerationPinGlobal >= 0) {
		digitalWrite(aerationPinGlobal, HIGH);
		Serial.println("[Aeration] OFF");
	}
}
