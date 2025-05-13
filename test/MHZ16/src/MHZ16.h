#ifndef MHZ16_H
#define MHZ16_H

#include <Arduino.h>
#include <HardwareSerial.h>

class MHZ16 {
public:
	MHZ16(HardwareSerial& serial, int rxPin, int txPin);
	void begin();
	int readCO2();

private:
	HardwareSerial& mhzSerial;
	int rx, tx;
	const byte readCmd[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };
};

#endif
