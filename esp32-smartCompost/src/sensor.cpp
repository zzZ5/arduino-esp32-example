#include "sensor.h"
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "DFRobot_EOxygenSensor.h"
#include <DHTesp.h>   // ★ 使用更稳定的 DHTesp

// ========== 全局变量 ==========

// MH-Z16
static HardwareSerial* mhzSerial = nullptr;
static int mhz_rx = -1, mhz_tx = -1;

// O2
static DFRobot_EOxygenSensor_I2C o2sensor(&Wire, 0x70);

// DS18B20
static OneWire* oneWire = nullptr;
static DallasTemperature* dallas = nullptr;

// DHT22（使用 DHTesp）
static DHTesp dht;
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
	digitalWrite(exhaustPinGlobal, HIGH);

	pinMode(aerationPinGlobal, OUTPUT);
	digitalWrite(aerationPinGlobal, LOW);

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

	// ---- DHT22（使用 DHTesp） ----
	dhtPinGlobal = dhtPin;
	dht.setup(dhtPinGlobal, DHTesp::DHT22);
	Serial.println("[DHT22] Sensor initialized using DHTesp");

	delay(300);

	TempAndHumidity test = dht.getTempAndHumidity();
	if (isnan(test.temperature) || isnan(test.humidity)) {
		Serial.println("[DHT22] Init WARNING: first read failed (will retry later)");
	}

	// ---- 超时 ----
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
float readEOxygen() {
	float o2 = o2sensor.readOxygenConcentration();

	// 检查是否读取失败(返回值为负或 >100% 为无效)
	if (o2 < 0 || o2 > 100.0) {
		return -1.0;
	}
	return o2;
}


// ========== DS18B20 ==========
float readDS18B20() {
	if (!dallas) return -127.0;

	dallas->requestTemperatures();
	// 等待温度转换完成(约 750ms)
	delay(750);
	float temp = dallas->getTempCByIndex(0);

	// 检查是否读取失败
	if (temp == -127.0 || temp == 85.0) {
		return -127.0;
	}
	return temp;
}


// ========== FDS100 ==========
float readFDS100(int pin) {
	if (pin < 0 || pin > 39) return -1.0;

	int adc = analogRead(pin);
	// ADC 范围检查
	if (adc < 0 || adc > 4095) return -1.0;

	float voltage = adc * 3.3 / 4095.0;
	float mois = (voltage / 2.0) * 100.0;

	// 边界检查
	if (mois < 0.0) mois = 0.0;
	if (mois > 100.0) mois = 100.0;

	return mois;
}


// ========== DHT22（使用 DHTesp，不会再永久 NAN） ==========
float readDHT22Temp() {
	TempAndHumidity data = dht.getTempAndHumidity();
	return data.temperature;  // DHTesp 自动处理 NAN
}

float readDHT22Hum() {
	TempAndHumidity data = dht.getTempAndHumidity();
	return data.humidity;
}
