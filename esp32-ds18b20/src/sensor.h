#pragma once
#include <vector>

// 初始化两路 OneWire（建议 4 和 5）
bool initSensors(int pin4, int pin5);

// 分别读取两路温度（单位：℃）
std::vector<float> readTemps4();
std::vector<float> readTemps5();
