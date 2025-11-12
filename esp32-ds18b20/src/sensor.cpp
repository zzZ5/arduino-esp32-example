#include <Arduino.h>
#include "sensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>

// GPIO4
static OneWire* ow4 = nullptr;
static DallasTemperature* ds4 = nullptr;
static int cnt4 = 0;
static int pin4_ = -1;

// GPIO5
static OneWire* ow5 = nullptr;
static DallasTemperature* ds5 = nullptr;
static int cnt5 = 0;
static int pin5_ = -1;

static void beginBus(OneWire*& ow, DallasTemperature*& ds, int& cnt, int pin) {
	if (pin < 0) return;
	ow = new OneWire(pin);
	ds = new DallasTemperature(ow);
	ds->begin();
	ds->setWaitForConversion(true);
	ds->setResolution(12);
	ds->requestTemperatures();
	delay(10);
	cnt = ds->getDeviceCount();
	Serial.printf("[Sensors] GPIO%d found %d DS18B20\n", pin, cnt);
}

bool initSensors(int pin4, int pin5) {
	pin4_ = pin4;
	pin5_ = pin5;
	if (pin4_ >= 0) beginBus(ow4, ds4, cnt4, pin4_);
	if (pin5_ >= 0) beginBus(ow5, ds5, cnt5, pin5_);
	return (cnt4 > 0 || cnt5 > 0);
}

static std::vector<float> readBus(DallasTemperature* ds, int& cnt) {
	std::vector<float> temps;
	if (!ds) return temps;
	ds->requestTemperatures();
	int n = ds->getDeviceCount();
	if (n != cnt) cnt = n;
	for (int i = 0; i < cnt; ++i) {
		float t = ds->getTempCByIndex(i);
		if (!isnan(t) && t > -55.0f && t < 125.0f) temps.push_back(t);
	}
	return temps;
}

std::vector<float> readTemps4() { return readBus(ds4, cnt4); }
std::vector<float> readTemps5() { return readBus(ds5, cnt5); }
