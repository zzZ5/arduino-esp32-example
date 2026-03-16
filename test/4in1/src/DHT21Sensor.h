#ifndef DHT21SENSOR_H
#define DHT21SENSOR_H

#include <Arduino.h>
#include <DHT.h>

/*
  ============================================================
  DHT21 温湿度传感器类
  ------------------------------------------------------------
  功能：
  1. 初始化 DHT21
  2. 读取温度和湿度
  3. 自动限制最小读取间隔，避免读取过快失败
  4. 支持打印输出
  ============================================================
*/
class DHT21Sensor {
public:
	/*
	  温湿度数据结构体
	  ----------------------------------------------------------
	  temperature : 温度，单位 ℃
	  humidity    : 湿度，单位 %RH
	*/
	struct Data {
		float temperature;
		float humidity;
	};

	/*
	  构造函数
	  ----------------------------------------------------------
	  pin -> DHT21 数据引脚
	*/
	explicit DHT21Sensor(uint8_t pin);

	/*
	  begin()
	  ----------------------------------------------------------
	  初始化 DHT21
	  返回值：
	  true  -> 初始化成功
	  false -> 当前版本一般直接返回 true
	*/
	bool begin();

	/*
	  readData()
	  ----------------------------------------------------------
	  读取一次温湿度数据
	  参数：
	  data -> 用于接收温湿度数据
	  返回值：
	  true  -> 成功
	  false -> 失败（通常为读取过快或通信失败）
	*/
	bool readData(Data& data);

	/*
	  readTemperature()
	  ----------------------------------------------------------
	  只读取温度
	*/
	bool readTemperature(float& temperature);

	/*
	  readHumidity()
	  ----------------------------------------------------------
	  只读取湿度
	*/
	bool readHumidity(float& humidity);

	/*
	  printData()
	  ----------------------------------------------------------
	  打印温湿度数据
	*/
	void printData(const Data& data, Stream& out = Serial);

private:
	uint8_t _pin;       // DHT21 数据引脚
	DHT _dht;           // DHT 对象

	// DHT21 不适合高频读取，建议至少 2 秒一次
	unsigned long _lastReadMs = 0;
	static const uint32_t MIN_READ_INTERVAL_MS = 2000;

	// 缓存最近一次成功读取的数据
	bool _hasCache = false;
	Data _lastData = { NAN, NAN };

	/*
	  canReadNow()
	  ----------------------------------------------------------
	  判断当前是否满足最小读取间隔
	*/
	bool canReadNow() const;
};

#endif