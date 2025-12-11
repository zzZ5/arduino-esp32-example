#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

// 初始化：抽气泵、曝气泵、MH-Z16、O2、DS18B20、DHT22
bool initSensorAndPump(int exhaustPin, int aerationPin,
	HardwareSerial& ser, int rxPin, int txPin,
	int dhtPin,
	unsigned long timeoutMs);

// 泵控制
void exhaustPumpOn();
void exhaustPumpOff();
void aerationOn();
void aerationOff();

// 传感器读取
int readMHZ16();          // CO2
float readEOxygen();      // O2
float readDS18B20();      // 物料温度
float readFDS100(int pin);// 含水率
float readDHT22Temp();    // 气体温度
float readDHT22Hum();     // 气体湿度

#endif
