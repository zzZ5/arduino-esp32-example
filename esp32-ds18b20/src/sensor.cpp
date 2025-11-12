#include "sensor.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Arduino.h>

static OneWire* ow4 = nullptr;
static OneWire* ow5 = nullptr;
static DallasTemperature* ds4 = nullptr;
static DallasTemperature* ds5 = nullptr;
static int cnt4 = 0, cnt5 = 0;

static void beginBus(OneWire*& ow, DallasTemperature*& ds, int& cnt, int pin) {
	if (pin < 0) return;
	ow = new OneWire(pin);
	ds = new DallasTemperature(ow);
	ds->begin();
	ds->setResolution(12);
	ds->requestTemperatures();
	cnt = ds->getDeviceCount();
	Serial.printf("[Sensors] GPIO%d found %d DS18B20\n", pin, cnt);
}

bool initSensors(int pin4, int pin5) {
	if (pin4 >= 0) beginBus(ow4, ds4, cnt4, pin4);
	if (pin5 >= 0) beginBus(ow5, ds5, cnt5, pin5);
	return (cnt4 > 0 || cnt5 > 0);
}

static std::vector<float> readBus(DallasTemperature* ds, int& cnt) {
	std::vector<float> temps;
	if (!ds) return temps;
	ds->requestTemperatures();
	cnt = ds->getDeviceCount();
	for (int i = 0; i < cnt; ++i) {
		float t = ds->getTempCByIndex(i);
		if (!isnan(t) && t > -55 && t < 125) temps.push_back(t);
	}
	return temps;
}

std::vector<float> readTemps4() { return readBus(ds4, cnt4); }
std::vector<float> readTemps5() { return readBus(ds5, cnt5); }
