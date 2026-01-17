#include "MHZ16.h"

MHZ16 co2Sensor(Serial1, 16, 17);  // RX=16, TX=17

void setup() {
  Serial.begin(115200);
  co2Sensor.begin();

  Serial.println("预热中（建议 ≥20分钟）...");
  delay(900000);  // 实验用短延时，真实建议20分钟后校零

  Serial.println("执行零点校准（确保环境为400ppm）...");
  co2Sensor.calibrateZero();
  delay(10000);  // 等待稳定
  Serial.println("执行零点校准（确保环境为400ppm）...");
  co2Sensor.calibrateZero();
  delay(10000);  // 等待稳定
}

void loop() {
  int co2 = co2Sensor.readCO2();
  if (co2 >= 0) {
    Serial.print("CO₂: ");
    Serial.print(co2);
    Serial.println(" ppm");
  }
  else {
    Serial.println("读取失败");
  }

  delay(2000);
}
