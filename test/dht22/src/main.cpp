#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>

#define DHTPIN 14       // 数据引脚接 ESP32 的 GPIO14
#define DHTTYPE DHT22   // DHT22 传感器类型

DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(115200);
  dht.begin();
}

void loop() {
  float h = dht.readHumidity();      // 湿度
  float t = dht.readTemperature();   // 摄氏温度

  if (isnan(h) || isnan(t)) {
    Serial.println("读取失败！");
    return;
  }

  Serial.print("温度: ");
  Serial.print(t);
  Serial.print(" °C\t湿度: ");
  Serial.print(h);
  Serial.println(" %RH");

  delay(2000);  // 每2秒读取一次
}
