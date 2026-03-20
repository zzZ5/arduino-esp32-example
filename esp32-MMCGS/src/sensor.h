#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

struct ZCE04BGasData {
  float co;
  float h2s;
  float o2;
  float ch4;
};

struct SHT30Data {
  float temperature;
  float humidity;
};

bool initSensorAndPump(
  int pumpPin,
  HardwareSerial& mhzSerial,
  int mhzRxPin,
  int mhzTxPin,
  HardwareSerial& zceSerial,
  int zceRxPin,
  int zceTxPin,
  uint8_t shtSdaPin,
  uint8_t shtSclPin,
  unsigned long timeoutMs);

void exhaustPumpOn();
void exhaustPumpOff();

int readMHZ16();
bool readZCE04B(ZCE04BGasData& data);
float readEOxygen();
bool readSHT30(SHT30Data& data);
float readSHT30Temp();
float readSHT30Hum();

#endif
