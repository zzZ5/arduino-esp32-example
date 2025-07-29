#ifndef SENSOR_CONTROL_H
#define SENSOR_CONTROL_H

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_SGP30.h>
#include <vector>

void initSensors();
bool readTemperatures(std::vector<float>& temps, std::vector<String>& keys);
bool readCO2(uint16_t& co2, String& key);
String getTimeString();

#endif
