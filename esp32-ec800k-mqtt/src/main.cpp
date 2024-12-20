#include <Arduino.h>
#include <HardwareSerial.h>

// 函数声明
void sendATCommand(String command);
void readSerialTask(void* parameter);

HardwareSerial ec800kSerial(1); // 使用 ESP32 的 UART1

// 定义缓冲区大小
#define BUFFER_SIZE 512
char buffer[BUFFER_SIZE];
volatile int bufferIndex = 0;
volatile bool responseComplete = false;

// 超时相关变量
unsigned long lastReceiveTime = 0;
const unsigned long timeout = 5000; // 超时时间（毫秒）

void setup() {
  // 初始化主机与 ESP32 的串口通信
  Serial.begin(115200);  // 调试用串口

  // 初始化 EC800K 的硬件串口
  ec800kSerial.begin(115200, SERIAL_8N1, 16, 17);  // RX, TX 引脚配置
  Serial.println("ESP32 与 EC800K 异步通信初始化完成！");

  // 启动异步任务读取 EC800K 响应
  xTaskCreate(readSerialTask, "ReadEC800K", 2048, NULL, 2, NULL);
}

void loop() {
  // 检查用户输入
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n'); // 从串口监视器读取指令
    command.trim();                                // 去除空白字符
    if (command.length() > 0) {
      command += "\r\n";                           // 添加 \r\n 符号
      sendATCommand(command);                      // 发送命令到 EC800K
    }
  }

  // 超时检测
  if (!responseComplete && (millis() - lastReceiveTime > timeout)) {
    Serial.println("AT 命令响应超时！");
    lastReceiveTime = millis(); // 重置时间，避免重复提示
  }

  // 主循环其他逻辑可在此添加
  delay(100); // 简单延时
}

// 发送 AT 命令到 EC800K
void sendATCommand(String command) {
  ec800kSerial.print(command); // 发送到 EC800K
  Serial.print("发送命令: ");
  Serial.println(command);
  lastReceiveTime = millis(); // 更新最后发送时间
}

// 异步任务：读取 EC800K 响应
void readSerialTask(void* parameter) {
  while (true) {
    // 如果有数据可读
    if (ec800kSerial.available()) {
      char incomingByte = ec800kSerial.read(); // 读取一个字节
      lastReceiveTime = millis();             // 更新接收时间

      // 缓冲区未满，存储数据
      if (bufferIndex < BUFFER_SIZE - 1) {
        buffer[bufferIndex++] = incomingByte;

        // 检测到响应结束符号 (\r\n)
        if (incomingByte == '\n' && bufferIndex > 1 && buffer[bufferIndex - 2] == '\r') {
          responseComplete = true;
        }
      }
      else {
        Serial.println("缓冲区已满，部分数据可能丢失！");
        bufferIndex = 0; // 重置缓冲区，避免溢出
      }

      // 如果响应完整，处理数据
      if (responseComplete) {
        buffer[bufferIndex] = '\0'; // 字符串结束符
        String response = String(buffer);
        response.trim();            // 去除空白字符

        // 过滤无意义的响应（例如 "RDY" 或空响应）
        if (response.length() > 0 && response != "RDY") {
          Serial.print("EC800K 返回的响应: ");
          Serial.println(response);
        }

        // 重置缓冲区与标志位
        bufferIndex = 0;
        responseComplete = false;
      }
    }

    // 延时以避免占用过多 CPU 时间
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
