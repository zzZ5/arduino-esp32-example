#include <HardwareSerial.h>

// 串口定义
HardwareSerial mySerial(1);

// 引脚定义
#define RX_PIN 16
#define TX_PIN 17

// 方法声明
void initializeSensor();  // 初始化传感器
void sendCommand(uint8_t* command, uint8_t length);  // 发送命令
bool readFrame(uint8_t* buffer, uint8_t length);  // 读取完整数据帧
uint8_t FucCheckSum(uint8_t* data, uint8_t length);  // 校验和计算
void parseData(uint8_t* data);  // 解析返回数据

void setup() {
  Serial.begin(115200);         // 初始化串口监视器
  mySerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN); // 初始化硬件串口
  initializeSensor();           // 初始化传感器
}

void loop() {
  // 发送查询命令
  uint8_t queryCommand[] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };
  sendCommand(queryCommand, sizeof(queryCommand));

  // 读取完整数据帧
  uint8_t response[11];
  if (readFrame(response, 11)) {

    // 校验数据
    uint8_t checksum = FucCheckSum(response, 11);
    if (checksum == response[10]) {
      parseData(response);  // 校验通过，解析数据
    }
    else {
      Serial.print("校验失败！计算校验值: ");
      Serial.print(checksum, HEX);
      Serial.print(", 接收校验值: ");
      Serial.println(response[10], HEX);
    }
  }
  else {
    Serial.println("未接收到完整数据帧！");
  }

  delay(60000);
}

// 方法定义

// 初始化传感器
void initializeSensor() {
  Serial.println("初始化完成，开始通信...");

  // 切换到问答模式
  uint8_t switchToQueryMode[] = { 0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x46 };
  sendCommand(switchToQueryMode, sizeof(switchToQueryMode));
  delay(1000);
}

// 发送命令
void sendCommand(uint8_t* command, uint8_t length) {
  mySerial.write(command, length);
}

// 读取完整数据帧
bool readFrame(uint8_t* buffer, uint8_t length) {
  uint8_t index = 0;
  unsigned long startTime = millis();  // 记录开始时间

  // 等待数据并逐字节读取
  while (index < length && millis() - startTime < 200) {  // 超时时间设置为 200ms
    if (mySerial.available()) {
      buffer[index] = mySerial.read();
      // Serial.print("读取字节: ");
      // Serial.print(buffer[index], HEX);
      // Serial.print(" (位置 ");
      // Serial.print(index);
      // Serial.println(")");
      index++;
    }
  }

  // 检查是否读取到完整数据帧
  if (index == length) {
    return true;  // 数据帧读取完整
  }
  else {
    Serial.println("数据帧读取不完整！");
    return false;  // 数据帧读取失败
  }
}

// 校验和计算
uint8_t FucCheckSum(uint8_t* data, uint8_t length) {
  uint8_t tempq = 0;
  for (uint8_t j = 1; j < length - 1; j++) {  // 从第 2 个字节到倒数第 2 个字节
    tempq += data[j];
  }
  tempq = (~tempq) + 1;  // 取反加 1
  return tempq;
}

// 解析返回数据
void parseData(uint8_t* data) {
  // CO 浓度 (分辨率 1 ppm)
  uint16_t CO = (data[2] << 8) | data[3];
  // H2S 浓度 (分辨率 1 ppm)
  uint16_t H2S = (data[4] << 8) | data[5];
  // O2 浓度 (分辨率 0.1 %VOL)
  float O2 = ((data[6] << 8) | data[7]) * 0.1;
  // CH4 浓度 (分辨率 1 %LEL)
  uint16_t CH4 = (data[8] << 8) | data[9];

  Serial.print("CO: ");
  Serial.print(CO);
  Serial.print(" ppm  ");

  Serial.print("H2S: ");
  Serial.print(H2S);
  Serial.print(" ppm  ");

  Serial.print("O2: ");
  Serial.print(O2, 1); // 保留 1 位小数
  Serial.print(" %VOL  ");

  Serial.print("CH4: ");
  Serial.print(CH4);
  Serial.println(" %LEL");
}
