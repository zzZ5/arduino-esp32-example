#include "sensor.h"
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "DFRobot_EOxygenSensor.h"

// ========== 全局变量定义 ==========

// MH-Z16 CO₂
static HardwareSerial* mhzSerial = nullptr;
static int mhz_rx = -1, mhz_tx = -1;

// O₂ 传感器
static DFRobot_EOxygenSensor_I2C o2sensor(&Wire, 0x70);

// DS18B20
static OneWire* oneWire = nullptr;
static DallasTemperature* dallas = nullptr;

// 气泵控制引脚
static int exhaustPinGlobal = -1;   // 抽气泵控制引脚
static int aerationPinGlobal = -1;  // 曝气泵控制引脚

// ========== 统一初始化入口 ==========

/**
 * 初始化抽气泵、曝气泵、MH-Z16、O₂传感器和DS18B20
 */
bool initSensorAndPump(int exhaustPin, int aerationPin,
	HardwareSerial& ser, int rxPin, int txPin,
	unsigned long timeoutMs) {

	unsigned long start = millis();

	// 记录泵引脚
	exhaustPinGlobal = exhaustPin;
	aerationPinGlobal = aerationPin;

	// 设置泵引脚方向及默认状态
	pinMode(exhaustPinGlobal, OUTPUT);
	digitalWrite(exhaustPinGlobal, HIGH);  // 抽气泵默认关闭（低电平触发）

	pinMode(aerationPinGlobal, OUTPUT);
	digitalWrite(aerationPinGlobal, LOW);  // 曝气泵默认关闭（高电平触发）

	// 初始化 MH-Z16
	mhzSerial = &ser;
	mhz_rx = rxPin;
	mhz_tx = txPin;
	mhzSerial->begin(9600, SERIAL_8N1, mhz_rx, mhz_tx);

	// 初始化氧气传感器
	Wire.begin();
	while (!o2sensor.begin()) {
		Serial.println("[O₂] Sensor not detected, retrying...");
		delay(500);
	}
	Serial.println("[O₂] Sensor initialized");

	// 初始化 DS18B20，数据线固定接 GPIO4
	oneWire = new OneWire(4);
	dallas = new DallasTemperature(oneWire);
	dallas->begin();

	// 简单等待确保稳定
	delay(500);
	if (millis() - start > timeoutMs) {
		Serial.println("[Sensor] Init timeout");
		return false;
	}

	Serial.println("[Sensor] Sensors & pumps initialized");
	return true;
}

// ========== 气泵控制函数 ==========

void exhaustPumpOn() {
	digitalWrite(exhaustPinGlobal, LOW);
	Serial.println("[Pump] Exhaust ON");
}

void exhaustPumpOff() {
	digitalWrite(exhaustPinGlobal, HIGH);
	Serial.println("[Pump] Exhaust OFF");
}

void aerationOn() {
	digitalWrite(aerationPinGlobal, HIGH);
	Serial.println("[Pump] Aeration ON");
}

void aerationOff() {
	digitalWrite(aerationPinGlobal, LOW);
	Serial.println("[Pump] Aeration OFF");
}

// ========== MH-Z16 二氧化碳浓度读取 ==========

int readMHZ16() {
	if (!mhzSerial) return -1;

	byte cmd[9] = { 0xFF, 0x01, 0x86, 0, 0, 0, 0, 0, 0x79 };
	byte response[9];

	mhzSerial->write(cmd, 9);
	delay(10);

	if (mhzSerial->available() >= 9) {
		mhzSerial->readBytes(response, 9);
		if (response[0] == 0xFF && response[1] == 0x86) {
			uint8_t checksum = 0;
			for (int i = 1; i < 8; i++) checksum += response[i];
			checksum = 0xFF - checksum + 1;
			if (checksum == response[8]) {
				int ppm = response[2] * 256 + response[3];
				Serial.printf("[CO₂] %d ppm\n", ppm);
				return ppm;
			}
		}
	}
	Serial.println("[CO₂] Read failed");
	return -1;
}

// ========== 氧气传感器读取 ==========

float readEOxygen() {
	float val = o2sensor.readOxygenConcentration();
	Serial.printf("[O₂] %.2f %%VOL\n", val);
	return val;
}

// ========== DS18B20 温度读取 ==========

float readDS18B20() {
	if (!dallas) return NAN;
	dallas->requestTemperatures();
	float temp = dallas->getTempCByIndex(0);
	Serial.printf("[Temp] %.1f °C\n", temp);
	return temp;
}

// ========== FDS100 土壤水分读取 ==========

float readFDS100(int pin) {
	int adc = analogRead(pin);
	float voltage = adc * 3.3 / 4095.0;
	float mois = (voltage / 2.0) * 100.0;
	if (mois > 100.0) mois = 100.0;
	Serial.printf("[Soil] %.1f %%RH\n", mois);
	return mois;
}
