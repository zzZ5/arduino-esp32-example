#include <Arduino.h>

#define O2_SENSOR_PIN 34   // AO03 氧气传感器连接的模拟引脚

int pinA0 = 0;  // 用于存储 A0 值

void setup() {
  // 初始化串口
  Serial.begin(115200);

  // 等待用户操作
  Serial.println("Please short-circuit Vsensor+ and Vsensor- for A0 reading.");
  delay(2000);  // 等待 2 秒
}

void loop() {
  // 读取 A0 值
  pinA0 = analogRead(O2_SENSOR_PIN);
  Serial.print("A1 (short-circuit) = ");
  Serial.println(pinA0);

  delay(60000);  // 每 60 秒读取一次
}
