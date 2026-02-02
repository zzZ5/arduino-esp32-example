#include "sensor.h"
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "DFRobot_EOxygenSensor.h"
#include "Adafruit_SHT31.h"   // ★ Adafruit SHT31 温湿度传感器

// ========== 全局变量 ==========

// MH-Z16
static HardwareSerial* mhzSerial = nullptr;
static int mhz_rx = -1, mhz_tx = -1;

// O2
static DFRobot_EOxygenSensor_I2C o2sensor(&Wire, 0x70);

// DS18B20
static OneWire* oneWire = nullptr;
static DallasTemperature* dallas = nullptr;
static OneWire oneWireStatic(4);
static DallasTemperature dallasStatic(&oneWireStatic);
static bool ds18b20Initialized = false;

// SHT31（I2C 温湿度传感器）
Adafruit_SHT31 sht31 = Adafruit_SHT31();  // I2C 地址: 0x44

// 泵引脚
static int exhaustPinGlobal = -1;
static int aerationPinGlobal = -1;


// ========== 初始化函数 ==========

bool initSensorAndPump(int exhaustPin, int aerationPin,
	HardwareSerial& ser, int rxPin, int txPin,
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

	// ---- I2C 初始化（O2 和 SHT31 共用） ----
	Wire.begin();

	// ---- O2 ----
	while (!o2sensor.begin()) {
		Serial.println("[O2] Sensor not detected, retrying...");
		delay(500);
	}
	Serial.println("[O2] Sensor initialized");

	// ---- DS18B20 ----
	// 使用引脚 4 作为 DS18B20 的数据线
	oneWire = &oneWireStatic;
	dallas = &dallasStatic;
	if (!ds18b20Initialized) {
		dallas->begin();
		ds18b20Initialized = true;
	}

	// ---- SHT31（I2C 温湿度传感器） ----
	int sht31Retries = 0;
	while (!sht31.begin(0x44)) {
		sht31Retries++;
		Serial.printf("[SHT31] Sensor not detected, retry %d...\n", sht31Retries);
		if (sht31Retries >= 5) {
			Serial.println("[SHT31] WARNING: SHT31 init failed after 5 retries");
			break;
		}
		delay(500);
	}
	if (sht31Retries < 5) {
		Serial.println("[SHT31] Sensor initialized");
	}

	// 测试读取
	float testTemp = sht31.readTemperature();
	float testHum = sht31.readHumidity();
	if (isnan(testTemp) || isnan(testHum)) {
		Serial.println("[SHT31] Init WARNING: first read failed (will retry later)");
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


// ========== SHT31（I2C 温湿度传感器） ==========
float readSHT30Temp() {
	float temp = sht31.readTemperature();

	// 检查是否读取失败
	if (isnan(temp) || temp < -40.0 || temp > 125.0) {
		return -127.0;
	}
	return temp;
}

float readSHT30Hum() {
	float hum = sht31.readHumidity();

	// 检查是否读取失败
	if (isnan(hum) || hum < 0.0 || hum > 100.0) {
		return -1.0;
	}
	return hum;
}
