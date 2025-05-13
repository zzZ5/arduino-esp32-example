#include <Arduino.h>

#include "DFRobot_EOxygenSensor.h"

/**
 *  iic slave Address, The default is E_OXYGEN_ADDRESS_0
 *     E_OXYGEN_ADDRESS_0               0x70
 *     E_OXYGEN_ADDRESS_1               0x71
 *     E_OXYGEN_ADDRESS_2               0x72
 *     E_OXYGEN_ADDRESS_3               0x73
 */
#define OXYGEN_I2C_ADDRESS E_OXYGEN_ADDRESS_0
DFRobot_EOxygenSensor_I2C oxygen(&Wire, OXYGEN_I2C_ADDRESS);

void setup()
{
  Serial.begin(115200);
  while (!Serial);
  while (!oxygen.begin()) {
    Serial.println("NO Deivces !");
    delay(1000);
  } Serial.println("Device connected successfully !");
}

void loop()
{
  Serial.print("oxygen concetnration is ");
  Serial.print(oxygen.readOxygenConcentration());
  Serial.println("% VOL");
  delay(10000);
}