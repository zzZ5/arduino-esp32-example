#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

/**
 * 初始化抽气泵、曝气泵及所有传感器。
 * @param exhaustPin   抽气泵控制引脚（低电平触发，采样用）
 * @param aerationPin  曝气泵控制引脚（高电平触发，供氧用）
 * @param ser          使用的串口对象（例如 Serial1）
 * @param rxPin        串口接收引脚（用于 MH-Z16）
 * @param txPin        串口发送引脚
 * @param timeoutMs    超时时间（毫秒）
 * @return 初始化是否成功
 */
bool initSensorAndPump(int exhaustPin, int aerationPin,
	HardwareSerial& ser, int rxPin, int txPin,
	unsigned long timeoutMs);

// 抽气泵控制函数（GPIO25，低电平打开）
void exhaustPumpOn();
void exhaustPumpOff();

// 曝气泵控制函数（GPIO26，高电平打开）
void aerationOn();
void aerationOff();

// MH-Z16 二氧化碳传感器读取（ppm）
int readMHZ16();

// DFRobot 氧气传感器读取（%VOL）
float readEOxygen();

// DS18B20 温度传感器读取（摄氏度）
float readDS18B20();

// FDS100 土壤水分传感器（电压型 → 含水率 %）
float readFDS100(int pin);

#endif
