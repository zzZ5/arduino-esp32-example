#define SENSOR_PIN 4  // 使用 GPIO4（ADC）

const int airValue = 3700; // 空气中（最干）读数
const int waterValue = 0;  // 水中（最湿）读数

void setup() {
  Serial.begin(9600);
}

void loop() {
  int sensorValue = analogRead(SENSOR_PIN);

  // 线性映射：干3700 → 0%，湿0 → 100%
  int moisturePercent = map(sensorValue, airValue, waterValue, 0, 100);
  moisturePercent = constrain(moisturePercent, 0, 100); // 限制在0~100%

  Serial.print("Raw ADC: ");
  Serial.print(sensorValue);
  Serial.print(" => Soil Moisture: ");
  Serial.print(moisturePercent);
  Serial.println("%");

  delay(1000); // 每秒输出一次
}