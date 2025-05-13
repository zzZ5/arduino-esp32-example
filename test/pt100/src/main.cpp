#include <Adafruit_MAX31865.h>

// 配置 PT100 参数
#define RREF 430.0       // 参考电阻值（常见为 430Ω）
#define RNOMINAL 100.0   // PT100 标称阻值（100Ω）

// 初始化 MAX31865（GPIO5 为 CS）
Adafruit_MAX31865 sensor = Adafruit_MAX31865(5);

void setup() {
  Serial.begin(115200);
  Serial.println("初始化 MAX31865 传感器...");
  sensor.begin(MAX31865_3WIRE); // 设置为三线制模式
}

void loop() {
  // 读取温度
  float temperature = sensor.temperature(RNOMINAL, RREF);

  // 显示温度
  Serial.print("当前温度为：");
  Serial.print(temperature);
  Serial.println(" °C");

  // 检查故障标志
  uint8_t fault = sensor.readFault();
  if (fault) {
    Serial.print("⚠️ 检测到故障代码: 0x");
    Serial.println(fault, HEX);

    if (fault & MAX31865_FAULT_HIGHTHRESH) {
      Serial.println("故障：RTD 超过高阈值（可能电阻过高）");
    }
    if (fault & MAX31865_FAULT_LOWTHRESH) {
      Serial.println("故障：RTD 低于低阈值（可能电阻过低）");
    }
    if (fault & MAX31865_FAULT_REFINLOW) {
      Serial.println("故障：参考电压过低");
    }
    if (fault & MAX31865_FAULT_REFINHIGH) {
      Serial.println("故障：参考电压过高");
    }
    if (fault & MAX31865_FAULT_RTDINLOW) {
      Serial.println("故障：RTDIN 电压过低（可能短路）");
    }
    if (fault & MAX31865_FAULT_OVUV) {
      Serial.println("故障：偏置电源过压/欠压");
    }

    // 清除故障标志
    sensor.clearFault();
  }

  delay(1000);  // 每秒检测一次
}
