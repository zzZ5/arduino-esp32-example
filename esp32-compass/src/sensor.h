#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>
#include <HardwareSerial.h>

/**
 * 初始化气泵和传感器 (合并进sensor模块)
 * @param pumpPin: 用于控制气泵的GPIO引脚
 * @param ser:     要使用的硬件串口引用
 * @param rxPin:   传感器的RX
 * @param txPin:   传感器的TX
 */
bool initSensorAndPump(int pumpPin,
	HardwareSerial& ser,
	int rxPin,
	int txPin,
	unsigned long timeoutMs);

/**
 * 打开/关闭氣泵
 */
void pumpOn();
void pumpOff();

/**
 * 读取四合一气体传感器数据 (CO/H2S/O2/CH4)
 * @return true成功，false失败
 */
bool readFourInOneSensor(uint16_t& coVal,
	uint16_t& h2sVal,
	float& o2Val,
	uint16_t& ch4Val);
bool readSHT30(float& temperature, float& humidity);

#endif
