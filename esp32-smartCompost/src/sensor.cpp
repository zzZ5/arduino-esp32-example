#include "sensor.h"
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "DFRobot_EOxygenSensor.h"
#include <DHT.h>

// ========== 全局变量 ==========

// MH-Z16
static HardwareSerial* mhzSerial = nullptr;
static int mhz_rx = -1, mhz_tx = -1;

// O2
static DFRobot_EOxygenSensor_I2C o2sensor(&Wire, 0x70);

// DS18B20
static OneWire* oneWire = nullptr;
static DallasTemperature* dallas = nullptr;

// DHT22
static DHT* dht22 = nullptr;
static int dhtPinGlobal = -1;

// 泵引脚
static int exhaustPinGlobal = -1;
static int aerationPinGlobal = -1;


// ========== 初始化函数 ==========

bool initSensorAndPump(int exhaustPin, int aerationPin,
	HardwareSerial& ser, int rxPin, int txPin,
	int dhtPin,
	unsigned long timeoutMs)
{
	unsigned long start = millis();

	// ---- 泵 ----
	exhaustPinGlobal = exhaustPin;
	aerationPinGlobal = aerationPin;

	pinMode(exhaustPinGlobal, OUTPUT);
	digitalWrite(exhaustPinGlobal, HIGH);   // 默认关闭（低电平打开）

	pinMode(aerationPinGlobal, OUTPUT);
	digitalWrite(aerationPinGlobal, LOW);   // 默认关闭（高电平打开）

	// ---- MH-Z16 ----
	mhzSerial = &ser;
	mhz_rx = rxPin;
	mhz_tx = txPin;
	mhzSerial->begin(9600, SERIAL_8N1, mhz_rx, mhz_tx);

	// ---- O2 ----
	Wire.begin();
	while (!o2sensor.begin()) {
		Serial.println("[O2] Sensor not detected, retrying...");
		delay(500);
	}
	Serial.println("[O2] Sensor initialized");

	// ---- DS18B20 ----
	oneWire = new OneWire(4);
	dallas = new DallasTemperature(oneWire);
	dallas->begin();

	// ---- DHT22（新增：整合到统一初始化） ----
	dhtPinGlobal = dhtPin;
	dht22 = new DHT(dhtPinGlobal, DHT22);
	dht22->begin();

	delay(500);
	float testT = dht22->readTemperature();
	float testH = dht22->readHumidity();
	if (isnan(testT) || isnan(testH)) {
		Serial.println("[DHT22] Init failed (no data)");
	}
	else {
		Serial.println("[DHT22] Sensor initialized");
	}

	// ---- 超时检测 ----
	if (millis() - start > timeoutMs) {
		Serial.println("[Sensor] Init timeout");
		return false;
	}

	Serial.println("[Sensor] All sensors initialized");
	return true;
}


// ========== 泵控制 ==========
void exhaustPumpOn() { digitalWrite(exhaustPinGlobal, LOW); }
void exhaustPumpOff() { digitalWrite(exhaustPinGlobal, HIGH); }
void aerationOn() { digitalWrite(aerationPinGlobal, HIGH); }
void aerationOff() { digitalWrite(aerationPinGlobal, LOW); }


// ========== MH-Z16 ==========
bool readWithTimeout(HardwareSerial* ser, byte* buf, int len, unsigned long timeoutMs) {
	int received = 0;
	unsigned long start = millis();
	while (received < len && millis() - start < timeoutMs) {
		if (ser->available()) buf[received++] = ser->read();
		else delay(1);
	}
	return (received == len);
}

int readMHZ16() {
	if (!mhzSerial) return -1;

	while (mhzSerial->available()) mhzSerial->read();

	byte cmd[9] = { 0xFF,0x01,0x86,0,0,0,0,0,0x79 };
	mhzSerial->write(cmd, 9);

	byte response[9];
	if (!readWithTimeout(mhzSerial, response, 9, 2000)) return -1;

	uint8_t checksum = 0;
	for (int i = 1; i < 8; i++) checksum += response[i];
	checksum = 0xFF - checksum + 1;

	if (response[8] == checksum)
		return response[2] * 256 + response[3];

	return -1;
}


// ========== O2 ==========
float readEOxygen() { return o2sensor.readOxygenConcentration(); }


// ========== DS18B20 ==========
float readDS18B20() {
	dallas->requestTemperatures();
	return dallas->getTempCByIndex(0);
}


// ========== FDS100 ==========
float readFDS100(int pin) {
	int adc = analogRead(pin);
	float voltage = adc * 3.3 / 4095.0;
	float mois = (voltage / 2.0) * 100.0;
	if (mois > 100) mois = 100;
	return mois;
}


// ========== DHT22（温湿度） ==========
float readDHT22Temp() {
	float t = dht22->readTemperature();
	return isnan(t) ? NAN : t;
}

float readDHT22Hum() {
	float h = dht22->readHumidity();
	return isnan(h) ? NAN : h;
}
