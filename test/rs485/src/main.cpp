#include <Arduino.h>
#include <ModbusMaster.h>

// 实例化 ModbusMaster 对象
ModbusMaster node;

// 控制 RS485 方向（如果模块需要）
#define MAX485_DE_RE 4

// 串口定义
#define RXD2 16
#define TXD2 17

void preTransmission() {
  digitalWrite(MAX485_DE_RE, HIGH); // 发送模式
}

void postTransmission() {
  digitalWrite(MAX485_DE_RE, LOW); // 接收模式
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2); // 确保波特率为9600

  pinMode(MAX485_DE_RE, OUTPUT);
  digitalWrite(MAX485_DE_RE, LOW);

  node.begin(1, Serial2); // Modbus 地址 1
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  Serial.println("RS485 Soil Moisture Sensor Test Start");
}

void loop() {
  uint8_t result;
  uint16_t moisture_raw;

  // 读取含水率寄存器 0x0001，1个寄存器
  result = node.readHoldingRegisters(0x0001, 1);

  if (result == node.ku8MBSuccess) {
    moisture_raw = node.getResponseBuffer(0);
    float moisture_percent = moisture_raw / 10.0;

    Serial.print("Soil Moisture: ");
    Serial.print(moisture_percent);
    Serial.println(" %");
  }
  else {
    Serial.print("Modbus error: ");
    Serial.println(result);
  }

  delay(10000); // 每10秒读取一次
}
