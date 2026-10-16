// Prototypes the Arduino IDE would generate for shotStopper.ino: functions it
// calls before defining them. If the sketch adds or renames one, the sketch
// test build fails with "use of undeclared identifier" - add it here.
#pragma once

#include <Arduino.h>
#include <ArduinoBLE.h>

struct Shot;

void writeStringToCharacteristic(BLECharacteristic& characteristic, const String& str);
void blePeripheralDisconnectHandler(BLEDevice central);
void setBrewingState(bool brewing);
void calculateEndTime(Shot* s);
float seconds_f();
void setColor(int rgb[3]);
