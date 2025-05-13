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
