#include "EEPROM.h"

#include <cstring>

EEPROMClass EEPROM;

namespace {

struct EepromState {
  uint8_t flash[fake::kEepromFlashSize];
  uint8_t buffer[fake::kEepromFlashSize];
  size_t size = 0;
  int commits = 0;
  EepromState() {
    std::memset(flash, 0xFF, sizeof(flash));  // erased flash
    std::memset(buffer, 0xFF, sizeof(buffer));
  }
};

EepromState& state() {
  static EepromState s;
  return s;
}

bool inRange(int address) { return address >= 0 && static_cast<size_t>(address) < state().size; }

}  // namespace

bool EEPROMClass::begin(size_t size) {
  if (size == 0 || size > fake::kEepromFlashSize) return false;
  state().size = size;
  std::memcpy(state().buffer, state().flash, size);
  return true;
}

// Out-of-range accesses behave like the ESP32 library: reads return 0 and
// writes are ignored.
uint8_t EEPROMClass::read(int address) { return inRange(address) ? state().buffer[address] : 0; }

void EEPROMClass::write(int address, uint8_t value) {
  if (inRange(address)) state().buffer[address] = value;
}

bool EEPROMClass::commit() {
  if (state().size == 0) return false;
  std::memcpy(state().flash, state().buffer, state().size);
  state().commits++;
  return true;
}

namespace fake {

void eepromPreload(int address, uint8_t value) { state().flash[address] = value; }

void eepromPreloadString(int address, const char* text) {
  size_t n = std::strlen(text);
  std::memcpy(state().flash + address, text, n + 1);
}

uint8_t eepromFlash(int address) { return state().flash[address]; }
size_t eepromBeginSize() { return state().size; }
int eepromCommitCount() { return state().commits; }

}  // namespace fake
