#include "sensor_control.h"
#include "config_manager.h"

OneWire oneWire(4);  // DS18B20 数据线连接 D4（GPIO4）
DallasTemperature sensors(&oneWire);
DeviceAddress deviceAddresses[6];  // 最多6个地址

Adafruit_SGP30 sgp30;

unsigned long lastSGP30BaselineSave = 0;
bool hasSGP30Baseline = false;

void initSensors() {
	sensors.begin();
	delay(100);
	int count = sensors.getDeviceCount();
	Serial.printf("[Temp] Found %d DS18B20 sensors\n", count);

	int index = 0;
	OneWire romReader(4);
	while (romReader.search(deviceAddresses[index]) && index < 6) {
		++index;
	}

	if (!sgp30.begin()) {
		Serial.println("[SGP30] Initialization failed!");
	}
	else {
		Serial.println("[SGP30] Initialized.");
		sgp30.IAQinit();
		delay(15000);  // 必须等待15秒初始化完成
		lastSGP30BaselineSave = millis();
	}
}

bool readTemperatures(std::vector<float>& temps, std::vector<String>& keys) {
	sensors.requestTemperatures();

	// 创建临时数组，长度为6，填入占位值
	temps.resize(6, -127.0f);  // -127 表示初始化失败时的占位
	keys.resize(6, "");

	for (int i = 0; i < 6; ++i) {
		float tempC = sensors.getTempCByIndex(i);
		if (tempC == DEVICE_DISCONNECTED_C) {
			Serial.printf("[Temp] Sensor %d disconnected\n", i);
			return false;
		}

		int rankPos = appConfig.rank[i] - 1;  // 排名从1开始，转为0索引
		if (rankPos < 0 || rankPos >= 6) {
			Serial.printf("[Temp] Invalid rank %d at index %d\n", appConfig.rank[i], i);
			return false;
		}

		temps[rankPos] = tempC;
		keys[rankPos] = appConfig.ds[i];
	}

	return true;
}


bool readCO2(uint16_t& co2, String& key) {
	if (!sgp30.IAQmeasure()) {
		Serial.println("[SGP30] Measurement failed");
		return false;
	}

	co2 = sgp30.eCO2;
	key = appConfig.sgp30;

	// 每小时保存一次 baseline（仿 MicroPython 逻辑）
	if ((millis() - lastSGP30BaselineSave >= 3600000) || !hasSGP30Baseline) {
		uint16_t tvoc_base, co2_base;
		if (sgp30.getIAQBaseline(&co2_base, &tvoc_base)) {
			Serial.printf("[SGP30] Saved Baseline → eCO2: 0x%04X, TVOC: 0x%04X\n", co2_base, tvoc_base);
			lastSGP30BaselineSave = millis();
			hasSGP30Baseline = true;
		}
	}
	return true;
}