#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>
#include <vector>

// 初始化温度传感器和控制引脚
bool initSensors(int tempInPin, int tempOutPin, int heaterPin, int pumpPin, int aerationPin);

// 温度读取
float readTempIn();                     // 读取内部温度
float readTempTank();					// 读取外部水箱的温度
float readTempInByIndex(int index);     // 按索引读温度
std::vector<float> readTempOut();       // 读取最多三个外部温度

// 水浴加热控制（GPIO25）
void heaterOn();
void heaterOff();

// 水浴泵控制（GPIO26）
void pumpOn();
void pumpOff();

// 曝气控制（GPIO27）
void aerationOn();
void aerationOff();

#endif
