#ifndef ZCE04B_H
#define ZCE04B_H

#include <Arduino.h>
#include <HardwareSerial.h>

/*
  ============================================================
  ZCE04B 四合一气体传感器类
  ------------------------------------------------------------
  功能：
  1. 初始化串口
  2. 切换传感器到问答模式
  3. 发送查询命令
  4. 读取并解析 CO / H2S / O2 / CH4 数据
  5. 自动进行帧头校验、命令字校验、校验和校验
  ============================================================
*/
class ZCE04B {
public:
	/*
	  气体数据结构体
	  ----------------------------------------------------------
	  raw  : 原始值
	  value: 换算后的工程值
	*/
	struct GasData {
		uint16_t coRaw;
		uint16_t h2sRaw;
		uint16_t o2Raw;
		uint16_t ch4Raw;

		float co;
		float h2s;
		float o2;
		float ch4;
	};

	/*
	  构造函数
	  ----------------------------------------------------------
	  serial : 传入一个 HardwareSerial 对象，例如 Serial1 / Serial2
	  rxPin  : ESP32 的 RX 引脚
	  txPin  : ESP32 的 TX 引脚
	*/
	ZCE04B(HardwareSerial& serial, int rxPin, int txPin);

	/*
	  begin()
	  ----------------------------------------------------------
	  初始化传感器串口，并切换到问答模式
	  返回值：
	  true  -> 初始化成功
	  false -> 初始化失败（当前版本一般都会返回 true）
	*/
	bool begin(uint32_t baud = 9600);

	/*
	  setQueryMode()
	  ----------------------------------------------------------
	  切换到问答模式
	*/
	bool setQueryMode();

	/*
	  readGasData()
	  ----------------------------------------------------------
	  发送查询命令，并读取一帧完整数据
	  参数：
	  gas -> 用于接收解析后的气体数据
	  返回值：
	  true  -> 读取并解析成功
	  false -> 失败
	*/
	bool readGasData(GasData& gas);

	/*
	  printGasData()
	  ----------------------------------------------------------
	  将解析后的数据打印到串口监视器
	*/
	void printGasData(const GasData& gas, Stream& out = Serial);

private:
	HardwareSerial* _serial;   // 指向传感器串口对象
	int _rxPin;                // 接收引脚
	int _txPin;                // 发送引脚

	static const uint8_t FRAME_LEN = 11;   // 返回帧固定长度

	/*
	  分辨率设置
	  ----------------------------------------------------------
	  O2 按协议示例可确认是 0.1 %VOL
	  其余气体默认按 1.0 输出原始工程值
	  如果后续确认具体量程，可在这里修改
	*/
	const float CO_RESOLUTION = 1.0f;
	const float H2S_RESOLUTION = 1.0f;
	const float O2_RESOLUTION = 0.1f;
	const float CH4_RESOLUTION = 1.0f;

	// 协议命令
	uint8_t _cmdSetQueryMode[9] = { 0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8_t _cmdReadGas[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	// 内部方法
	void clearBuffer();
	void sendCommand(const uint8_t* cmd, uint8_t len);
	bool readFrame(uint8_t* buffer, uint8_t len, uint32_t timeoutMs = 500);
	uint8_t calcChecksum(const uint8_t* data, uint8_t len);
	void updateCommandChecksum(uint8_t* cmd, uint8_t len);
	bool parseFrame(const uint8_t* frame, GasData& gas);
};

#endif