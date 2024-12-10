#include <Arduino.h>
HardwareSerial mySerial(1);  // 使用串口1

// 发送 AT 命令并等待响应的函数
void sendATCommandAndReceiveResponse(String command) {
  // 发送 AT 命令
  mySerial.print(command);

  // 等待并读取 EC800M 的反馈
  unsigned long timeout = millis() + 10000;  // 增加等待时间为 10 秒
  while (millis() < timeout) {
    if (mySerial.available()) {
      String response = mySerial.readString();  // 获取模块反馈
      Serial.print("EC800M Response: ");
      Serial.println(response);
      break;  // 收到响应后退出循环
    }
  }
}

void setup() {
  // 启动串口通信 
  Serial.begin(115200);   // Serial Monitor 通信
  mySerial.begin(115200, SERIAL_8N1, 16, 17);  // ESP32 的硬件串口1：TX = 17, RX = 16（根据实际接线调整）

  // 等待串口初始化完成
  delay(1000);
  Serial.println("Enter AT command:");
}

void loop() {
  // 检查是否有输入
  String command = "";
  bool waitingForInput = true;

  while (command.length() == 0) {
    // 只在第一次循环时输出提示信息
    if (waitingForInput) {
      Serial.println("Waiting for input...");
      waitingForInput = false;  // 只在第一次显示提示信息
    }

    // 循环等待直到输入
    if (Serial.available()) {
      command = Serial.readString();  // 获取命令字符串
      command.trim();  // 去掉命令前后的空格或换行符
      command += "\r\n";  // 添加回车换行符，确保 AT 命令正确格式
    }
  }

  // 打印用户输入的命令
  Serial.print("Sending: ");
  Serial.println(command);

  // 发送命令并等待响应
  sendATCommandAndReceiveResponse(command);
}
