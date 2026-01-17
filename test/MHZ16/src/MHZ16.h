#ifndef MHZ16_H
#define MHZ16_H

#include <Arduino.h>
#include <HardwareSerial.h>

class MHZ16 {
public:
	MHZ16(HardwareSerial& serial, int rxPin, int txPin);
	void begin();

	int readCO2();                // 读取 CO₂ 浓度（ppm）
	void calibrateZero();         // 零点校准（400ppm）
	void calibrateSpan(int ppm);  // 跨度校准（标准气体 ppm）

private:
	HardwareSerial& mhzSerial;
	int rx, tx;
	const byte readCmd[9] = {
	  0xFF, 0x01, 0x86,
	  0x00, 0x00, 0x00,
	  0x00, 0x00, 0x79
	};
};

#endif
