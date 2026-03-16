#include "DHT21Sensor.h"

/*
  构造函数
  ------------------------------------------------------------
  DHT21 在 DHT 库里通常对应 DHT21 类型
*/
DHT21Sensor::DHT21Sensor(uint8_t pin)
	: _pin(pin), _dht(pin, DHT21) {
}

/*
  begin()
  ------------------------------------------------------------
  初始化 DHT21
*/
bool DHT21Sensor::begin() {
	_dht.begin();
	delay(100);
	return true;
}

/*
  canReadNow()
  ------------------------------------------------------------
  判断是否满足最小读取间隔
*/
bool DHT21Sensor::canReadNow() const {
	return (millis() - _lastReadMs) >= MIN_READ_INTERVAL_MS;
}

/*
  readData()
  ------------------------------------------------------------
  读取一次温湿度
  逻辑：
  1. 若距离上次成功读取不足 2 秒，则直接返回缓存值
  2. 若满足间隔，则重新读取
  3. 若读取失败且有缓存，则返回 false，但保留缓存
*/
bool DHT21Sensor::readData(Data& data) {
	// 如果读取过快，直接返回上次缓存数据
	if (!canReadNow() && _hasCache) {
		data = _lastData;
		return true;
	}

	float humidity = _dht.readHumidity();
	float temperature = _dht.readTemperature();

	// 判断是否读取失败
	if (isnan(humidity) || isnan(temperature)) {
		return false;
	}

	// 更新缓存
	_lastReadMs = millis();
	_lastData.humidity = humidity;
	_lastData.temperature = temperature;
	_hasCache = true;

	data = _lastData;
	return true;
}

/*
  readTemperature()
  ------------------------------------------------------------
  只读取温度
*/
bool DHT21Sensor::readTemperature(float& temperature) {
	Data data;
	if (!readData(data)) {
		return false;
	}

	temperature = data.temperature;
	return true;
}

/*
  readHumidity()
  ------------------------------------------------------------
  只读取湿度
*/
bool DHT21Sensor::readHumidity(float& humidity) {
	Data data;
	if (!readData(data)) {
		return false;
	}

	humidity = data.humidity;
	return true;
}

/*
  printData()
  ------------------------------------------------------------
  打印温湿度数据
*/
void DHT21Sensor::printData(const Data& data, Stream& out) {
	out.print("Temp: ");
	out.print(data.temperature, 1);
	out.print(" °C, ");

	out.print("Humi: ");
	out.print(data.humidity, 1);
	out.println(" %RH");
}