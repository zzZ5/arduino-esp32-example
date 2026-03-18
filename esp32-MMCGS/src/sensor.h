#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

// ZCE04B 当前输出的四种气体读数。
struct ZCE04BGasData {
  float co;
  float h2s;
  float o2;
  float ch4;
};

// DHT21 温湿度读数。
struct DHT21Data {
  float temperature;
  float humidity;
};

// 初始化泵和传感器。
bool initSensorAndPump(
  int exhaustPin,
  int aerationPin,
  HardwareSerial& mhzSerial,
  int mhzRxPin,
  int mhzTxPin,
  HardwareSerial& zceSerial,
  int zceRxPin,
  int zceTxPin,
  uint8_t dhtPin,
  unsigned long timeoutMs);

// 泵控制。
void exhaustPumpOn();
void exhaustPumpOff();
void aerationOn();
void aerationOff();

// 传感器读取接口。
int readMHZ16();
bool readZCE04B(ZCE04BGasData& data);
float readEOxygen();
bool readDHT21(DHT21Data& data);
float readDHT21Temp();
float readDHT21Hum();

#endif
