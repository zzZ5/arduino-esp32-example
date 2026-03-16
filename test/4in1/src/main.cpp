#include <Arduino.h>
#include <HardwareSerial.h>
#include "ZCE04B.h"
#include "MHZ16.h"
#include "DHT21Sensor.h"

// -------------------- 串口对象 --------------------
HardwareSerial zcSerial(1);   // 给 ZCE04B
HardwareSerial co2Serial(2);  // 给 MH-Z16

// -------------------- 引脚定义 --------------------
#define ZCE04B_RX_PIN 16
#define ZCE04B_TX_PIN 17

#define MHZ16_RX_PIN  18
#define MHZ16_TX_PIN  19

#define DHT21_PIN     4

// -------------------- 传感器对象 --------------------
ZCE04B gasSensor(zcSerial, ZCE04B_RX_PIN, ZCE04B_TX_PIN);
MHZ16 co2Sensor(co2Serial, MHZ16_RX_PIN, MHZ16_TX_PIN);
DHT21Sensor dht21(DHT21_PIN);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("多传感器系统启动...");

  gasSensor.begin(9600);
  co2Sensor.begin(9600);
  dht21.begin();

  Serial.println("全部传感器初始化完成");
}

void loop() {
  // 1. 读取 ZCE04B
  ZCE04B::GasData gas;
  if (gasSensor.readGasData(gas)) {
    gasSensor.printGasData(gas);
  }
  else {
    Serial.println("ZCE04B 读取失败");
  }

  // 2. 读取 MH-Z16
  MHZ16::Data co2;
  if (co2Sensor.readData(co2)) {
    co2Sensor.printData(co2);
  }
  else {
    Serial.println("MH-Z16 读取失败");
  }

  // 3. 读取 DHT21
  DHT21Sensor::Data th;
  if (dht21.readData(th)) {
    dht21.printData(th);
  }
  else {
    Serial.println("DHT21 读取失败");
  }

  Serial.println("------------------------");
  delay(3000);
}