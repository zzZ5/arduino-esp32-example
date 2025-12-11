#pragma once
#include <vector>

bool initSensors(int pin4, int pin5);
std::vector<float> readTemps4();
std::vector<float> readTemps5();
