#include <Arduino.h>

int sensorPin = 4;
int sensorValue = 0;

void setup()
{
  Serial.begin(115200); //Set serial baud rate to 9600 bps
}

void loop()
{
  sensorValue = analogRead(sensorPin);
  Serial.println(sensorValue);
  delay(1000);
}
