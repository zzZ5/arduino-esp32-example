#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

/**
 * 初始化所有传感器：
 * - 抽气泵 (exhaustPin)
 * - 曝气泵 (aerationPin)
 * - MH-Z16 (CO₂)
 * - DFRobot O₂
 * - DS18B20 温度
 * - SHT30 (气体温湿度, I2C)
 *
 * @param exhaustPin 抽气泵 GPIO
 * @param aerationPin 曝气泵 GPIO
 * @param ser MH-Z16 使用的串口 (如 Serial1)
 * @param rxPin MH-Z16 RX
 * @param txPin MH-Z16 TX
 * @param timeoutMs 初始化最大等待时间
 */
bool initSensorAndPump(int exhaustPin, int aerationPin,
	HardwareSerial& ser, int rxPin, int txPin,
	unsigned long timeoutMs);

// 泵控制
void exhaustPumpOn();
void exhaustPumpOff();
void aerationOn();
void aerationOff();

// 传感器读取
int   readMHZ16();         // CO₂ 浓度（ppm）
float readEOxygen();       // O₂ 浓度（%）
float readDS18B20();       // DS18B20 物料温度（°C）
float readFDS100(int pin); // 含水率（%）

// SHT30（I2C 温湿度传感器）
float readSHT30Temp();     // 气体温度（°C）
float readSHT30Hum();      // 气体湿度（%RH）

#endif
