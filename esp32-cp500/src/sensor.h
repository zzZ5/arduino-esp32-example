#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>
#include <vector>

// 初始化温度传感器和控制引脚
bool initTemperatureSensors(int tempInPin, int tempOutPin, int heaterPin, int aerationPin);

// 温度读取
float readTempIn();                      // 读取内部温度
std::vector<float> readTempOut();       // 读取最多三个外部温度

// 水浴加热控制（GPIO25）
void heaterOn();
void heaterOff();

// 曝气控制（GPIO26）
void aerationOn();
void aerationOff();

#endif
