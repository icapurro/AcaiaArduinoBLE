// Host-side stub of the ESP32 Update (OTA flashing) library.
#pragma once

#include "Arduino.h"

#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF

class UpdateClass {
 public:
  bool begin(size_t size = UPDATE_SIZE_UNKNOWN) { (void)size; return false; }
  size_t write(uint8_t* data, size_t length) { (void)data; return length; }
  bool end(bool evenIfRemaining = false) { (void)evenIfRemaining; return false; }
  void printError(Print& out) { out.println("Update not supported on host"); }
};

extern UpdateClass Update;
