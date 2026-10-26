#include "fake_phone.h"

bool FakePhone::exists(const std::string& uuid) const {
  return static_cast<bool>(fakeble::findLocalCharacteristic(uuid));
}

bool FakePhone::write(const std::string& uuid, const std::vector<uint8_t>& bytes) {
  return fakeble::centralWrite(fakeble::findLocalCharacteristic(uuid), bytes.data(), bytes.size());
}

bool FakePhone::writeString(const std::string& uuid, const std::string& text) {
  return write(uuid, std::vector<uint8_t>(text.begin(), text.end()));
}

std::vector<uint8_t> FakePhone::read(const std::string& uuid) const {
  BLECharacteristic c = fakeble::findLocalCharacteristic(uuid);
  if (!c || !c.value()) return {};
  return std::vector<uint8_t>(c.value(), c.value() + c.valueLength());
}

int FakePhone::readByte(const std::string& uuid) const {
  std::vector<uint8_t> v = read(uuid);
  return v.empty() ? -1 : v[0];
}

std::string FakePhone::readString(const std::string& uuid) const {
  std::vector<uint8_t> v = read(uuid);
  return std::string(v.begin(), v.end());
}

std::vector<std::vector<uint8_t>> FakePhone::history(const std::string& uuid) const {
  std::vector<std::vector<uint8_t>> out;
  for (auto& change : fakeble::valueHistory(fakeble::findLocalCharacteristic(uuid))) out.push_back(change.value);
  return out;
}

std::vector<int> FakePhone::byteChanges(const std::string& uuid) const {
  std::vector<int> out;
  for (auto& value : history(uuid)) {
    int b = value.empty() ? -1 : value[0];
    if (out.empty() || out.back() != b) out.push_back(b);
  }
  return out;
}
