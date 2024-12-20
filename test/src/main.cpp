#include <Arduino.h>

const int pin = 2;  // 假设我们使用 GPIO 2

void setup() {
  // 初始化串口通讯
  Serial.begin(115200);

  // 初始化GPIO引脚为输出模式
  pinMode(pin, OUTPUT);

  Serial.println("请随时输入 '1' 设置为高电平，'0' 设置为低电平");
}

void loop() {
  // 检查是否有串口输入
  if (Serial.available() > 0) {
    char input = Serial.read();  // 读取输入的字符

    // 根据输入字符控制 GPIO 电平
    if (input == '1') {
      digitalWrite(pin, HIGH);  // 设置为高电平
      Serial.println("GPIO 设置为高电平");
    }
    else if (input == '0') {
      digitalWrite(pin, LOW);   // 设置为低电平
      Serial.println("GPIO 设置为低电平");
    }
    else {
      Serial.println("无效输入，请输入 '1' 或 '0'");
    }
  }
}
