#include "MHZ16.h"

/*
  构造函数
*/
MHZ16::MHZ16(HardwareSerial& serial, int rxPin, int txPin)
	: _serial(&serial), _rxPin(rxPin), _txPin(txPin) {
}

/*
  begin()
  ------------------------------------------------------------
  初始化硬件串口，并自动更新查询命令的校验和
*/
bool MHZ16::begin(uint32_t baud) {
	// 初始化串口：9600, 8N1
	_serial->begin(baud, SERIAL_8N1, _rxPin, _txPin);

	// 自动计算命令校验和，避免手写错误
	updateCommandChecksum(_cmdReadCO2, sizeof(_cmdReadCO2));

	delay(200);
	clearBuffer();

	return true;
}

/*
  readData()
  ------------------------------------------------------------
  1. 清空缓存
  2. 发送查询命令
  3. 接收 9 字节返回帧
  4. 解析并校验
*/
bool MHZ16::readData(Data& data) {
	uint8_t frame[FRAME_LEN] = { 0 };

	clearBuffer();
	sendCommand(_cmdReadCO2, sizeof(_cmdReadCO2));

	// 读取完整返回帧
	if (!readFrame(frame, FRAME_LEN, 500)) {
		return false;
	}

	// 解析帧
	return parseFrame(frame, data);
}

/*
  readCO2()
  ------------------------------------------------------------
  只返回 CO2 数值，适合主程序直接调用
*/
bool MHZ16::readCO2(uint16_t& co2ppm) {
	Data data;
	if (!readData(data)) {
		return false;
	}

	co2ppm = data.co2ppm;
	return true;
}

/*
  printData()
  ------------------------------------------------------------
  打印 CO2 数据
*/
void MHZ16::printData(const Data& data, Stream& out) {
	out.print("CO2: ");
	out.print(data.co2ppm);
	out.println(" ppm");
}

/*
  clearBuffer()
  ------------------------------------------------------------
  清空串口接收缓冲区，避免读到残留旧数据
*/
void MHZ16::clearBuffer() {
	while (_serial->available()) {
		_serial->read();
	}
}

/*
  sendCommand()
  ------------------------------------------------------------
  发送命令，并等待发送完成
*/
void MHZ16::sendCommand(const uint8_t* cmd, uint8_t len) {
	_serial->write(cmd, len);
	_serial->flush();
}

/*
  readFrame()
  ------------------------------------------------------------
  读取固定长度的一帧数据
  逻辑：
  1. 先找帧头 0xFF
  2. 找到后继续读取后续字节
  3. 收满 len 个字节则成功
*/
bool MHZ16::readFrame(uint8_t* buffer, uint8_t len, uint32_t timeoutMs) {
	uint8_t index = 0;
	unsigned long startTime = millis();

	while (millis() - startTime < timeoutMs) {
		if (_serial->available()) {
			uint8_t b = _serial->read();

			// 第一个字节必须是 0xFF，用于帧头同步
			if (index == 0 && b != 0xFF) {
				continue;
			}

			buffer[index++] = b;

			if (index >= len) {
				return true;
			}
		}
	}

	return false;
}

/*
  calcChecksum()
  ------------------------------------------------------------
  协议校验和算法：
  从第2字节加到倒数第2字节，再取反加1
*/
uint8_t MHZ16::calcChecksum(const uint8_t* data, uint8_t len) {
	uint8_t sum = 0;
	for (uint8_t i = 1; i < len - 1; i++) {
		sum += data[i];
	}
	sum = (~sum) + 1;
	return sum;
}

/*
  updateCommandChecksum()
  ------------------------------------------------------------
  自动更新命令最后一个字节的校验和
*/
void MHZ16::updateCommandChecksum(uint8_t* cmd, uint8_t len) {
	cmd[len - 1] = calcChecksum(cmd, len);
}

/*
  parseFrame()
  ------------------------------------------------------------
  对返回帧进行：
  1. 帧头检查
  2. 命令字检查
  3. 校验和检查
  4. 数据解析
*/
bool MHZ16::parseFrame(const uint8_t* frame, Data& data) {
	// 1) 帧头检查
	if (frame[0] != 0xFF) {
		return false;
	}

	// 2) 命令字检查
	if (frame[1] != 0x86) {
		return false;
	}

	// 3) 校验和检查
	uint8_t checksum = calcChecksum(frame, FRAME_LEN);
	if (checksum != frame[8]) {
		return false;
	}

	// 4) 解析 CO2 浓度
	// CO2 = 高字节 * 256 + 低字节
	data.co2ppm = ((uint16_t)frame[2] << 8) | frame[3];

	return true;
}