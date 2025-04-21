#include "sensor.h"
#include <Wire.h>
#include <Adafruit_SHT31.h>

// 静态/全局变量，保存泵引脚、串口指针
static int g_pumpPin;
static HardwareSerial* g_sensorSer = nullptr;
static Adafruit_SHT31 sht30 = Adafruit_SHT31();

//------------------------------
// 初始化: 气泵 + 传感器
//------------------------------
bool initSensorAndPump(int pumpPin,
	HardwareSerial& ser,
	int rxPin,
	int txPin,
	unsigned long timeoutMs)
{
	// 记录开始时间
	unsigned long start = millis();

	// 1) 设置泵引脚为默认关闭状态
	g_pumpPin = pumpPin;
	pinMode(g_pumpPin, OUTPUT);
	digitalWrite(g_pumpPin, HIGH); // 默认关闭

	// 2) 初始化串口
	g_sensorSer = &ser;
	g_sensorSer->begin(9600, SERIAL_8N1, rxPin, txPin);


	// 3) 初始化 SHT30（I2C）
	Wire.begin(); // 默认 SDA=21, SCL=22
	if (!sht30.begin(0x44)) {
		Serial.println("[Sensor] Failed to init SHT30!");
		return false;
	}
	else {
		Serial.println("[Sensor] SHT30 init OK.");
	}

	// 4) 启动四合一气体传感器，发送切换到问答模式命令
	uint8_t cmd[] = { 0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x46 };
	g_sensorSer->write(cmd, sizeof(cmd));

	// 5) 等待1秒(阻塞)，然后在末尾检查耗时
	delay(1000);

	// 在此检查整个流程是否超过了 timeoutMs
	if (millis() - start > timeoutMs) {
		Serial.println("[Sensor] initSensorAndPump timed out!");
		return false;
	}

	// 若未超时，即视为成功
	Serial.println("[Sensor] init sensor & pump done.");
	return true;
}

//------------------------------
// 打开/关闭 氣泵
//------------------------------
void pumpOn() {
	digitalWrite(g_pumpPin, LOW);
	Serial.println("Pump ON");
}

void pumpOff() {
	digitalWrite(g_pumpPin, HIGH);
	Serial.println("Pump OFF");
}

//------------------------------
// 读取 SHT30 温湿度
//------------------------------
bool readSHT30(float& temperature, float& humidity) {
	temperature = sht30.readTemperature();
	humidity = sht30.readHumidity();

	if (!isnan(temperature) && !isnan(humidity)) {
		return true;
	}
	else {
		Serial.println("[Sensor] SHT30 read fail!");
		return false;
	}
}
//------------------------------
// 读取四合一气体传感器
//------------------------------
bool readFourInOneSensor(uint16_t& coVal,
	uint16_t& h2sVal,
	float& o2Val,
	uint16_t& ch4Val)
{
	if (!g_sensorSer) {
		Serial.println("[Sensor] Error: sensor serial not inited!");
		return false;
	}

	// 1) 发送查询命令
	uint8_t cmd[] = { 0xFF,0x01,0x86,0x00,0x00,0x00,0x00,0x00,0x79 };
	g_sensorSer->write(cmd, sizeof(cmd));

	// 2) 读取 11 字节响应
	uint8_t resp[11];
	unsigned long start = millis();
	uint8_t idx = 0;
	while (idx < 11 && (millis() - start) < 200) {
		if (g_sensorSer->available()) {
			resp[idx++] = g_sensorSer->read();
		}
	}
	if (idx < 11) {
		Serial.println("[Sensor] Incomplete frame!");
		return false;
	}

	// 3) 校验和
	uint8_t sum = 0;
	for (int i = 1; i < 10; i++) {
		sum += resp[i];
	}
	sum = (~sum) + 1;
	if (sum != resp[10]) {
		Serial.println("[Sensor] Checksum fail!");
		return false;
	}

	// 4) 解析
	coVal = (resp[2] << 8) | resp[3];
	h2sVal = (resp[4] << 8) | resp[5];
	o2Val = ((resp[6] << 8) | resp[7]) * 0.1f;
	ch4Val = (resp[8] << 8) | resp[9];

	return true;
}
