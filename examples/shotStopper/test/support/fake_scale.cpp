#include "fake_scale.h"

#include <algorithm>
#include <cmath>

namespace acaia {
const Bytes IDENTIFY = {0xef, 0xdd, 0x0b, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
                        0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x9a, 0x6d};
const Bytes HEARTBEAT = {0xef, 0xdd, 0x00, 0x02, 0x00, 0x02, 0x00};
const Bytes NOTIFICATION_REQUEST = {0xef, 0xdd, 0x0c, 0x09, 0x00, 0x01, 0x01,
                                    0x02, 0x02, 0x05, 0x03, 0x04, 0x15, 0x06};
const Bytes START_TIMER = {0xef, 0xdd, 0x0d, 0x00, 0x00, 0x00, 0x00};
const Bytes STOP_TIMER = {0xef, 0xdd, 0x0d, 0x00, 0x02, 0x00, 0x02};
const Bytes RESET_TIMER = {0xef, 0xdd, 0x0d, 0x00, 0x01, 0x00, 0x01};
const Bytes TARE = {0xef, 0xdd, 0x04, 0x00, 0x00, 0x00};
const Bytes TARE_GENERIC = {0x03, 0x0a, 0x01, 0x00, 0x00, 0x08};
const Bytes START_TIMER_GENERIC = {0x03, 0x0a, 0x04, 0x00, 0x00, 0x0a};
const Bytes STOP_TIMER_GENERIC = {0x03, 0x0a, 0x05, 0x00, 0x00, 0x0d};
const Bytes RESET_TIMER_GENERIC = {0x03, 0x0a, 0x06, 0x00, 0x00, 0x0c};
}  // namespace acaia

namespace {

std::string defaultName(ScaleType type) {
  switch (type) {
    case ScaleType::OLD: return "LUNAR-FAKE";
    case ScaleType::NEW: return "PYXIS-FAKE";
    // isScaleName() has no Felicita prefix, so a generic scale has to
    // advertise one of the accepted names to be found at all.
    case ScaleType::GENERIC: return "PROCH-FAKE";
    case ScaleType::NONE: return "ACAIA-FAKE";
  }
  return "";
}

}  // namespace

FakeScale::FakeScale(ScaleType type, std::string name, std::string address)
    : type_(type), name_(name.empty() ? defaultName(type) : std::move(name)), address_(std::move(address)) {}

std::string FakeScale::readUuid(ScaleType type) {
  switch (type) {
    case ScaleType::OLD: return "2a80";
    case ScaleType::NEW: return "49535343-1e4d-4bd9-ba61-23c647249616";
    case ScaleType::GENERIC: return "ff11";
    case ScaleType::NONE: return "";
  }
  return "";
}

std::string FakeScale::writeUuid(ScaleType type) {
  switch (type) {
    case ScaleType::OLD: return "2a80";
    case ScaleType::NEW: return "49535343-8841-43f4-a8d4-ecbe34729bb3";
    case ScaleType::GENERIC: return "ff12";
    case ScaleType::NONE: return "";
  }
  return "";
}

Bytes FakeScale::encodeWeight(ScaleType type, float grams) {
  bool negative = grams < 0;
  float magnitude = std::fabs(grams);
  switch (type) {
    case ScaleType::NEW: {
      // 13 bytes: [4]=0x05 weight message, [5..6] little-endian value,
      // [9] decimal places, [10] bit 1 = negative
      unsigned raw = static_cast<unsigned>(std::lround(magnitude * 10));
      Bytes p(13, 0);
      p[0] = 0xef; p[1] = 0xdd; p[2] = 0x0c; p[3] = 0x08; p[4] = 0x05;
      p[5] = raw & 0xff; p[6] = (raw >> 8) & 0xff;
      p[9] = 1;
      p[10] = negative ? 0x02 : 0x00;
      return p;
    }
    case ScaleType::OLD: {
      // 10 bytes: [2..3] little-endian value, [6] decimal places, [7] bit 1 = negative
      unsigned raw = static_cast<unsigned>(std::lround(magnitude * 10));
      Bytes p(10, 0);
      p[0] = 0xef; p[1] = 0xdd;
      p[2] = raw & 0xff; p[3] = (raw >> 8) & 0xff;
      p[6] = 1;
      p[7] = negative ? 0x02 : 0x00;
      return p;
    }
    case ScaleType::GENERIC: {
      // 20 bytes: [6] '-' if negative, [7..9] big-endian hundredths
      unsigned raw = static_cast<unsigned>(std::lround(magnitude * 100));
      Bytes p(20, 0);
      p[0] = 0x01; p[1] = 0x02;
      p[6] = negative ? '-' : '+';
      p[7] = (raw >> 16) & 0xff; p[8] = (raw >> 8) & 0xff; p[9] = raw & 0xff;
      return p;
    }
    case ScaleType::NONE: return Bytes();
  }
  return Bytes();
}

std::vector<Bytes> FakeScale::writtenBytes() const {
  std::vector<Bytes> out;
  for (auto& w : writes_) out.push_back(w.bytes);
  return out;
}

int FakeScale::countWrites(const Bytes& bytes) const {
  return static_cast<int>(std::count_if(writes_.begin(), writes_.end(), [&](const Write& w) { return w.bytes == bytes; }));
}

float FakeScale::rawWeightNow() const { return weightAt_ ? weightAt_(millis()) : 0.0f; }

bool FakeScale::connect() {
  if (failConnect || !advertising()) return false;
  connected_ = true;
  subscribed_ = false;
  connectCount_++;
  nextPacketMs_ = millis();
  events_.push_back("connect");
  return true;
}

bool FakeScale::discoverAttributes() {
  if (failDiscover) return false;
  events_.push_back("discover");
  return true;
}

void FakeScale::disconnect() {
  if (!connected_) return;
  connected_ = false;
  subscribed_ = false;
  events_.push_back("disconnect");
}

bool FakeScale::hasCharacteristic(const std::string& uuid) {
  return type_ != ScaleType::NONE && (uuid == readUuid(type_) || uuid == writeUuid(type_));
}

uint8_t FakeScale::properties(const std::string& uuid) {
  uint8_t props = 0;
  if (!hasCharacteristic(uuid)) return props;
  if (uuid == readUuid(type_)) props |= BLERead | BLENotify;
  if (uuid == writeUuid(type_)) props |= BLEWrite | BLEWriteWithoutResponse;
  return props;
}

bool FakeScale::subscribe(const std::string& uuid) {
  if (failSubscribe || uuid != readUuid(type_)) return false;
  subscribed_ = true;
  events_.push_back("subscribe " + uuid);
  return true;
}

bool FakeScale::write(const std::string& uuid, const uint8_t* data, size_t length) {
  if (failWrites || uuid != writeUuid(type_)) return false;
  Bytes bytes(data, data + length);
  writes_.push_back({millis(), uuid, bytes});
  events_.push_back("write " + uuid);
  const Bytes& tare = type_ == ScaleType::GENERIC ? acaia::TARE_GENERIC : acaia::TARE;
  if (bytes == tare) tareOffset_ = rawWeightNow();
  return true;
}

bool FakeScale::nextNotification(const std::string& uuid, Bytes& out) {
  if (!connected_ || !subscribed_ || silent || uuid != readUuid(type_)) return false;
  if (!queued_.empty()) {
    out = queued_.front();
    queued_.pop_front();
    return true;
  }
  if (!weightAt_ || millis() < nextPacketMs_) return false;
  // One packet per period; if the reader fell behind, skip ahead rather than burst.
  nextPacketMs_ = std::max(nextPacketMs_ + packetPeriodMs, millis() + 1);
  out = encodeWeight(type_, rawWeightNow() - tareOffset_);
  return true;
}
