#include "sensor.h"

// 静态/全局变量，保存泵引脚、串口指针
static int g_pumpPin;
static HardwareSerial* g_sensorSer = nullptr;

//------------------------------
// 初始化: 氣泵 + 传感器
//------------------------------
void initSensorAndPump(int pumpPin,
	HardwareSerial& ser,
	int rxPin,
	int txPin)
{
	// 1) 记录泵引脚
	g_pumpPin = pumpPin;
	pinMode(g_pumpPin, OUTPUT);
	digitalWrite(g_pumpPin, HIGH); // 默认关闭气泵

	// 2) 初始化串口
	g_sensorSer = &ser;
	g_sensorSer->begin(9600, SERIAL_8N1, rxPin, txPin);

	// 3) 切换传感器到问答模式
	uint8_t cmd[] = { 0xFF,0x01,0x78,0x41,0x00,0x00,0x00,0x00,0x46 };
	g_sensorSer->write(cmd, sizeof(cmd));
	delay(1000);

	Serial.println("[Sensor] init sensor & pump done.");
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
