#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

bool initSensors();

bool readAnalogCapacitive(float& moisturePercent);
bool readFDS100(float& moisturePercent);
bool readRS485SoilMoisture(float& moisturePercent);

#endif
