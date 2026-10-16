// Host-side fake of the ESP32 EEPROM library.
//
// Like the real one, writes go to a RAM buffer and only reach "flash" on
// commit(). Tests pre-load and inspect flash through `fake::`.
#pragma once

#include <cstddef>
#include <cstdint>

class EEPROMClass {
 public:
  bool begin(size_t size);
  uint8_t read(int address);
  void write(int address, uint8_t value);
  bool commit();
};

extern EEPROMClass EEPROM;

namespace fake {

constexpr size_t kEepromFlashSize = 512;

// Write directly to flash, as if persisted by an earlier boot.
void eepromPreload(int address, uint8_t value);
void eepromPreloadString(int address, const char* text);
// Read what has been committed to flash.
uint8_t eepromFlash(int address);
// Size requested by EEPROM.begin(), 0 if not called.
size_t eepromBeginSize();
int eepromCommitCount();

}  // namespace fake
