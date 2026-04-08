#include <Arduino.h>
#include "sensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>

static OneWire* oneWireIn = nullptr;
static DallasTemperature* sensorIn = nullptr;
static int numInSensors = 0;

static OneWire* oneWireOut = nullptr;
static DallasTemperature* sensorOut = nullptr;
static int numOutSensors = 0;

static int heaterPinGlobal = -1;
static int pumpPinGlobal = -1;
static int aerationPinGlobal = -1;

static const int AERATION_LEDC_FREQ_HZ = 1000;
static const int AERATION_LEDC_RES_BITS = 10;
static const bool AERATION_ACTIVE_HIGH = true;

static int g_aerCurrentDutyPct = 0;
static int g_aerMaxDutyPct = 100;
static int g_softOnMs = 1200;
static int g_softOffMs = 800;
static int g_kickPct = 0;
static int g_kickMs = 0;

static inline int maxCount() {
	return (1 << AERATION_LEDC_RES_BITS) - 1;
}

static inline int pctToRaw(int pct) {
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	long raw = (long)maxCount() * pct / 100L;
	if (!AERATION_ACTIVE_HIGH) raw = maxCount() - raw;
	return (int)raw;
}

bool initSensors(int tempInPin, int tempOutPin, int heaterPin, int pumpPin, int aerationPin) {
	oneWireIn = new OneWire(tempInPin);
	sensorIn = new DallasTemperature(oneWireIn);
	sensorIn->begin();
	sensorIn->requestTemperatures();
	numInSensors = sensorIn->getDeviceCount();
	Serial.printf("[TempIn] Found %d sensors\n", numInSensors);

	oneWireOut = new OneWire(tempOutPin);
	sensorOut = new DallasTemperature(oneWireOut);
	sensorOut->begin();
	sensorOut->requestTemperatures();
	numOutSensors = sensorOut->getDeviceCount();
	Serial.printf("[TempOut] Found %d sensors\n", numOutSensors);

	heaterPinGlobal = heaterPin;
	pumpPinGlobal = pumpPin;
	aerationPinGlobal = aerationPin;

	if (heaterPinGlobal >= 0) {
		pinMode(heaterPinGlobal, OUTPUT);
		digitalWrite(heaterPinGlobal, LOW);
	}
	if (pumpPinGlobal >= 0) {
		pinMode(pumpPinGlobal, OUTPUT);
		digitalWrite(pumpPinGlobal, LOW);
	}

	if (aerationPinGlobal >= 0) {
		ledcAttach(aerationPinGlobal, AERATION_LEDC_FREQ_HZ, AERATION_LEDC_RES_BITS);
		ledcWrite(aerationPinGlobal, pctToRaw(0));
		g_aerCurrentDutyPct = 0;

		for (int i = 0; i < 3; ++i) {
			ledcWrite(aerationPinGlobal, pctToRaw(0));
			delay(120);
			ledcWrite(aerationPinGlobal, pctToRaw(100));
			delay(180);
			ledcWrite(aerationPinGlobal, pctToRaw(0));
			delay(120);
		}

		Serial.printf("[Aeration] PWM ready @%dHz, %dbit, pin=%d, active_%s\n",
			AERATION_LEDC_FREQ_HZ, AERATION_LEDC_RES_BITS,
			aerationPinGlobal, AERATION_ACTIVE_HIGH ? "HIGH" : "LOW");
	}

	return true;
}

static float readTempInByIndex(int index) {
	if (!sensorIn) return NAN;
	if (index < 0 || index >= numInSensors) return NAN;
	float t = sensorIn->getTempCByIndex(index);
	Serial.printf("[TempInBus idx=%d] %.1f C\n", index, t);
	return t;
}

void readInternalTemps(float& tempIn, float& tempTank) {
	tempIn = NAN;
	tempTank = NAN;

	if (!sensorIn) return;

	sensorIn->requestTemperatures();
	tempIn = readTempInByIndex(0);

	if (numInSensors < 2) {
		Serial.println("[Tank] Not found (need 2nd sensor on internal bus).");
		return;
	}

	tempTank = readTempInByIndex(1);
}

float readTempIn() {
	float tempIn = NAN;
	float tempTank = NAN;
	readInternalTemps(tempIn, tempTank);
	return tempIn;
}

float readTempTank() {
	float tempIn = NAN;
	float tempTank = NAN;
	readInternalTemps(tempIn, tempTank);
	return tempTank;
}

std::vector<float> readTempOut() {
	std::vector<float> temps;
	if (!sensorOut) return temps;

	sensorOut->requestTemperatures();
	for (int i = 0; i < numOutSensors && i < 3; ++i) {
		float t = sensorOut->getTempCByIndex(i);
		Serial.printf("[TempOut-%d] %.1f C\n", i, t);
		temps.push_back(t);
	}
	return temps;
}

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

static inline void writeDutyPctImmediate(int pct) {
	if (pct < 0) pct = 0;
	if (pct > g_aerMaxDutyPct) pct = g_aerMaxDutyPct;
	g_aerCurrentDutyPct = pct;

	if (aerationPinGlobal >= 0) {
		int raw = pctToRaw(pct);
		ledcWrite(aerationPinGlobal, raw);
		Serial.printf("[Aeration] duty=%d%% -> raw=%d/%d\n", pct, raw, maxCount());
	}
}

bool aerationIsActive() { return g_aerCurrentDutyPct > 0; }

void aerationSetDutyPct(int pct) {
	writeDutyPctImmediate(pct);
}

void aerationSetMaxDutyPct(int pctLimit) {
	if (pctLimit < 10) pctLimit = 10;
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

void aerationOn() {
	const int target = g_aerMaxDutyPct;
	int from = g_aerCurrentDutyPct;

	if (g_kickPct > 0 && g_kickMs > 0 && from == 0) {
		int kp = g_kickPct;
		if (kp > g_aerMaxDutyPct) kp = g_aerMaxDutyPct;
		writeDutyPctImmediate(kp);
		delay(g_kickMs);
		from = g_aerCurrentDutyPct;
	}

	if (g_softOnMs <= 0 || target == from) {
		writeDutyPctImmediate(target);
	} else {
		int steps = abs(target - from);
		int stepDelay = g_softOnMs / steps;
		if (stepDelay <= 0) stepDelay = 1;
		int dir = (target > from) ? 1 : -1;
		unsigned long last = millis();
		int pct = from;
		while (pct != target) {
			writeDutyPctImmediate(pct);
			pct += dir;
			while ((millis() - last) < (unsigned long)stepDelay) {
				delayMicroseconds(1000);
				yield();
			}
			last += stepDelay;
		}
		writeDutyPctImmediate(target);
	}
	Serial.printf("[Aeration] ON soft -> %d%%\n", g_aerCurrentDutyPct);
}

void aerationOff() {
	int from = g_aerCurrentDutyPct;
	int to = 0;

	if (g_softOffMs <= 0 || from == to) {
		writeDutyPctImmediate(0);
	} else {
		int steps = abs(from - to);
		int stepDelay = g_softOffMs / steps;
		if (stepDelay <= 0) stepDelay = 1;
		int dir = -1;
		unsigned long last = millis();
		int pct = from;
		while (pct != to) {
			writeDutyPctImmediate(pct);
			pct += dir;
			while ((millis() - last) < (unsigned long)stepDelay) {
				delayMicroseconds(1000);
				yield();
			}
			last += stepDelay;
		}
		writeDutyPctImmediate(0);
	}
	Serial.println("[Aeration] OFF soft -> 0%");
}
