#include "MHZ16.h"

MHZ16::MHZ16(HardwareSerial& serial, int rxPin, int txPin)
	: mhzSerial(serial), rx(rxPin), tx(txPin) {
}

void MHZ16::begin() {
	mhzSerial.begin(9600, SERIAL_8N1, rx, tx);
}

int MHZ16::readCO2() {
	byte response[9];
	mhzSerial.write(readCmd, 9);
	delay(10);

	if (mhzSerial.available() >= 9) {
		mhzSerial.readBytes(response, 9);
		if (response[0] == 0xFF && response[1] == 0x86) {
			return response[2] * 256 + response[3];
		}
	}

	return -1; // 读取失败
}

void MHZ16::calibrateZero() {
	byte zeroCmd[9] = {
	  0xFF, 0x01, 0x87,
	  0x00, 0x00, 0x00,
	  0x00, 0x00, 0x78
	};
	mhzSerial.write(zeroCmd, 9);
	delay(100);
}

void MHZ16::calibrateSpan(int ppm) {
	if (ppm < 1000) return; // 最低跨度限制
	byte high = ppm / 256;
	byte low = ppm % 256;

	byte checksum = 0xFF - (0x01 + 0x88 + high + low) + 1;

	byte spanCmd[9] = {
	  0xFF, 0x01, 0x88,
	  high, low,
	  0x00, 0x00, 0x00,
	  checksum
	};

	mhzSerial.write(spanCmd, 9);
	delay(100);
}
