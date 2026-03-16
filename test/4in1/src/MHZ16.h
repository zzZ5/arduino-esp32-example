#ifndef MHZ16_H
#define MHZ16_H

#include <Arduino.h>
#include <HardwareSerial.h>

/*
  ============================================================
  MH-Z16 CO2 传感器类（UART版）
  ------------------------------------------------------------
  功能：
  1. 初始化串口
  2. 发送查询命令
  3. 读取并解析 CO2 浓度
  4. 自动进行帧头校验和校验和校验
  ============================================================
*/
class MHZ16 {
public:
	/*
	  CO2 数据结构体
	  ----------------------------------------------------------
	  co2ppm : CO2 浓度，单位 ppm
	*/
	struct Data {
		uint16_t co2ppm;
	};

	/*
	  构造函数
	  ----------------------------------------------------------
	  serial : 传入一个 HardwareSerial 对象，例如 Serial1 / Serial2
	  rxPin  : ESP32 的 RX 引脚
	  txPin  : ESP32 的 TX 引脚
	*/
	MHZ16(HardwareSerial& serial, int rxPin, int txPin);

	/*
	  begin()
	  ----------------------------------------------------------
	  初始化传感器串口
	  参数：
	  baud -> 串口波特率，默认 9600
	  返回值：
	  true  -> 初始化成功
	  false -> 初始化失败（当前版本通常直接返回 true）
	*/
	bool begin(uint32_t baud = 9600);

	/*
	  readData()
	  ----------------------------------------------------------
	  发送查询命令，并读取一帧完整数据
	  参数：
	  data -> 用于接收解析后的 CO2 数据
	  返回值：
	  true  -> 读取并解析成功
	  false -> 失败
	*/
	bool readData(Data& data);

	/*
	  readCO2()
	  ----------------------------------------------------------
	  只读取 CO2 数值，方便主程序直接调用
	  参数：
	  co2ppm -> 返回 CO2 浓度
	  返回值：
	  true  -> 成功
	  false -> 失败
	*/
	bool readCO2(uint16_t& co2ppm);

	/*
	  printData()
	  ----------------------------------------------------------
	  将解析后的数据打印到串口监视器
	*/
	void printData(const Data& data, Stream& out = Serial);

private:
	HardwareSerial* _serial;   // 指向传感器串口对象
	int _rxPin;                // 接收引脚
	int _txPin;                // 发送引脚

	static const uint8_t FRAME_LEN = 9;   // MH-Z16 返回帧固定长度 9 字节

	// 查询 CO2 命令
	// 格式：FF 01 86 00 00 00 00 00 79
	uint8_t _cmdReadCO2[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	// 内部方法
	void clearBuffer();
	void sendCommand(const uint8_t* cmd, uint8_t len);
	bool readFrame(uint8_t* buffer, uint8_t len, uint32_t timeoutMs = 500);
	uint8_t calcChecksum(const uint8_t* data, uint8_t len);
	void updateCommandChecksum(uint8_t* cmd, uint8_t len);
	bool parseFrame(const uint8_t* frame, Data& data);
};

#endif