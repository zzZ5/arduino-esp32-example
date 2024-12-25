#include <Arduino.h>

// 定义连接的水分传感器模拟引脚
const int sensorPin = 34;  // 这里假设水分传感器连接在 GPIO34 引脚

// 定义变量
float sensorValue = 0.0;  // 传感器的原始读数
float voltage = 0.0;      // 电压值
float humidity = 0.0;     // 含水率（湿度）

void setup() {
  // 初始化串口通信
  Serial.begin(115200);

  // 设置传感器引脚为输入
  pinMode(sensorPin, INPUT);
}

void loop() {
  // 读取传感器的模拟值（0 - 4095）
  sensorValue = analogRead(sensorPin);

  // 将读取的值转换为电压（范围 0-3.3V）
  voltage = sensorValue * (3.3 / 4095.0);

  // 计算湿度（百分比），假设输出电压为最大电压的 50% 时为最大湿度（即 100%）
  humidity = (voltage / 2.0) * 100.0;

  // 输出结果到串口监视器
  Serial.print("Sensor Value: ");
  Serial.print(sensorValue);
  Serial.print("\tVoltage: ");
  Serial.print(voltage);
  Serial.print("V\tHumidity: ");
  Serial.print(humidity);
  Serial.println("%");

  // 每隔 60 秒读取一次数据
  delay(60000);
}
