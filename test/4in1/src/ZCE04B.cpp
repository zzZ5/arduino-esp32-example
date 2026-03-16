#include "ZCE04B.h"

/*
  构造函数
*/
ZCE04B::ZCE04B(HardwareSerial& serial, int rxPin, int txPin)
	: _serial(&serial), _rxPin(rxPin), _txPin(txPin) {
}

/*
  begin()
  ------------------------------------------------------------
  初始化串口，并更新命令校验和，然后切换到问答模式
*/
bool ZCE04B::begin(uint32_t baud) {
	// 初始化硬件串口
	_serial->begin(baud, SERIAL_8N1, _rxPin, _txPin);

	// 自动计算命令校验和，避免手写错误
	updateCommandChecksum(_cmdSetQueryMode, sizeof(_cmdSetQueryMode));
	updateCommandChecksum(_cmdReadGas, sizeof(_cmdReadGas));

	delay(200);

	// 切换到问答模式
	return setQueryMode();
}

/*
  setQueryMode()
  ------------------------------------------------------------
  发送“切换到问答模式”命令
*/
bool ZCE04B::setQueryMode() {
	clearBuffer();
	sendCommand(_cmdSetQueryMode, sizeof(_cmdSetQueryMode));
	delay(500);       // 给传感器一点切换模式的时间
	clearBuffer();    // 清掉可能残留的数据
	return true;
}

/*
  readGasData()
  ------------------------------------------------------------
  1. 清空缓存
  2. 发送查询命令
  3. 接收 11 字节返回帧
  4. 解析并校验
*/
bool ZCE04B::readGasData(GasData& gas) {
	uint8_t frame[FRAME_LEN] = { 0 };

	clearBuffer();
	sendCommand(_cmdReadGas, sizeof(_cmdReadGas));

	// 读取完整返回帧
	if (!readFrame(frame, FRAME_LEN, 500)) {
		return false;
	}

	// 解析帧
	return parseFrame(frame, gas);
}

/*
  printGasData()
  ------------------------------------------------------------
  打印气体数据
*/
void ZCE04B::printGasData(const GasData& gas, Stream& out) {
	out.print("CO: ");
	out.print(gas.co);
	out.print(" ppm, ");

	out.print("H2S: ");
	out.print(gas.h2s);
	out.print(" ppm, ");

	out.print("O2: ");
	out.print(gas.o2, 1);
	out.print(" %VOL, ");

	out.print("CH4: ");
	out.print(gas.ch4);
	out.println(" %LEL");
}

/*
  clearBuffer()
  ------------------------------------------------------------
  清空串口接收缓冲区，避免读到残留旧数据
*/
void ZCE04B::clearBuffer() {
	while (_serial->available()) {
		_serial->read();
	}
}

/*
  sendCommand()
  ------------------------------------------------------------
  发送命令，并等待发送完成
*/
void ZCE04B::sendCommand(const uint8_t* cmd, uint8_t len) {
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
bool ZCE04B::readFrame(uint8_t* buffer, uint8_t len, uint32_t timeoutMs) {
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
uint8_t ZCE04B::calcChecksum(const uint8_t* data, uint8_t len) {
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
void ZCE04B::updateCommandChecksum(uint8_t* cmd, uint8_t len) {
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
bool ZCE04B::parseFrame(const uint8_t* frame, GasData& gas) {
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
	if (checksum != frame[10]) {
		return false;
	}

	// 4) 解析原始值
	gas.coRaw = ((uint16_t)frame[2] << 8) | frame[3];
	gas.h2sRaw = ((uint16_t)frame[4] << 8) | frame[5];
	gas.o2Raw = ((uint16_t)frame[6] << 8) | frame[7];
	gas.ch4Raw = ((uint16_t)frame[8] << 8) | frame[9];

	// 5) 换算工程值
	gas.co = gas.coRaw * CO_RESOLUTION;
	gas.h2s = gas.h2sRaw * H2S_RESOLUTION;
	gas.o2 = gas.o2Raw * O2_RESOLUTION;
	gas.ch4 = gas.ch4Raw * CH4_RESOLUTION;

	return true;
}