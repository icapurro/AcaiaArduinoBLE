// A fake BLE central (e.g. a phone running LightBlue) talking to the sketch's
// local config service through the fake ArduinoBLE stack.
//
//   FakePhone phone;
//   phone.writeByte("0xFF11", 40);           // goal weight
//   phone.byteHistory("0xFF20");             // shot status values published
#pragma once

#include <string>
#include <vector>

#include <ArduinoBLE.h>

class FakePhone {
 public:
  bool exists(const std::string& uuid) const;
  bool write(const std::string& uuid, const std::vector<uint8_t>& bytes);
  bool writeByte(const std::string& uuid, uint8_t value) { return write(uuid, {value}); }
  bool writeString(const std::string& uuid, const std::string& text);

  // Current value (empty if the characteristic doesn't exist).
  std::vector<uint8_t> read(const std::string& uuid) const;
  int readByte(const std::string& uuid) const;  // -1 if empty or missing
  std::string readString(const std::string& uuid) const;

  // Every value the sketch wrote, in order.
  std::vector<std::vector<uint8_t>> history(const std::string& uuid) const;
  // First byte of each value the sketch wrote, with consecutive repeats collapsed.
  std::vector<int> byteChanges(const std::string& uuid) const;

  // Simulate the phone disconnecting (fires the sketch's disconnect handler).
  bool disconnect() { return fakeble::fireEvent(BLEDisconnected); }
};
