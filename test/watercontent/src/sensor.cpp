#include "sensor.h"
#include <ModbusMaster.h>

#define ANALOG1_PIN 32
#define FDS100_PIN 34
#define RXD2 16
#define TXD2 17

static ModbusMaster node;

bool initSensors() {
	pinMode(ANALOG1_PIN, INPUT);
	pinMode(FDS100_PIN, INPUT);
	Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
	node.begin(1, Serial2);  // 地址 0x01
	return true;
}

bool readAnalogCapacitive(float& moisturePercent) {
	int raw = analogRead(ANALOG1_PIN);
	moisturePercent = map(raw, 3600, 0, 0, 100);
	moisturePercent = constrain(moisturePercent, 0, 100);
	return true;
}

bool readFDS100(float& moisturePercent) {
	int raw = analogRead(FDS100_PIN);
	float voltage = raw * (3.3 / 4095.0);
	moisturePercent = (voltage / 2.0) * 100.0;
	moisturePercent = constrain(moisturePercent, 0.0, 100.0);
	return true;
}

bool readRS485SoilMoisture(float& moisturePercent) {
	uint8_t result = node.readHoldingRegisters(0x0001, 1);
	if (result == node.ku8MBSuccess) {
		moisturePercent = node.getResponseBuffer(0) / 10.0;
		return true;
	}
	return false;
}
