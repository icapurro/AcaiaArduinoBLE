// A scriptable BLE scale for the fake ArduinoBLE stack.
//
//   FakeScale scale(ScaleType::NEW);          // advertises as "PYXIS-FAKE"
//   scale.setWeight([](unsigned long t) { return t / 500.0f; });
//   fakeble::addPeripheral(&scale);
//
// It exposes the characteristics of its protocol, records every write, emits
// weight notifications every packetPeriodMs of simulated time, and zeroes its
// reported weight when it receives a tare command.
#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <ArduinoBLE.h>

using Bytes = std::vector<uint8_t>;

enum class ScaleType {
  OLD,      // Lunar (pre-2021): one characteristic 2a80
  NEW,      // Lunar 2021, Pyxis: 49535343-... pair
  GENERIC,  // Bookoo Themis and similar: ff11 notify / ff12 write, binary protocol
  FELICITA, // Felicita Arc: one ffe1 characteristic, ASCII protocol
  NONE,     // advertises a scale name but exposes no known characteristics
};

class FakeScale : public fakeble::RemotePeripheral {
 public:
  explicit FakeScale(ScaleType type, std::string name = "", std::string address = "aa:bb:cc:dd:ee:01");

  // ---- configuration
  bool failConnect = false;
  bool failDiscover = false;
  bool failSubscribe = false;
  bool failWrites = false;
  bool silent = false;  // stop sending notifications
  bool visible = true;  // advertising while not connected
  unsigned long packetPeriodMs = 100;

  // Raw weight (grams) as a function of simulated time (ms). Packets are only
  // generated while a weight function is set.
  void setWeight(std::function<float(unsigned long)> weightAt) { weightAt_ = std::move(weightAt); }
  // Deliver this exact packet next (before any generated ones).
  void queuePacket(const Bytes& packet) { queued_.push_back(packet); }

  // ---- observation
  struct Write {
    unsigned long timeMs;
    std::string uuid;
    Bytes bytes;
    bool withResponse;  // write request (acknowledged) vs write command
  };
  const std::vector<Write>& writes() const { return writes_; }
  std::vector<Bytes> writtenBytes() const;
  int countWrites(const Bytes& bytes) const;
  // "connect", "discover", "subscribe <uuid>", "write <uuid>", "disconnect"
  const std::vector<std::string>& events() const { return events_; }
  int connectCount() const { return connectCount_; }
  bool isSubscribed() const { return subscribed_; }
  float tareOffset() const { return tareOffset_; }
  float rawWeightNow() const;  // weight function at the current time (0 if none)
  ScaleType type() const { return type_; }

  // Characteristic UUIDs (normalized) for a scale type; empty for NONE.
  static std::string readUuid(ScaleType type);
  static std::string writeUuid(ScaleType type);
  // Encode a weight packet the way this scale type sends it.
  static Bytes encodeWeight(ScaleType type, float grams);

  // ---- fakeble::RemotePeripheral
  String localName() override { return String(name_.c_str()); }
  String address() override { return String(address_.c_str()); }
  bool advertising() override { return visible && !connected_; }
  bool connect() override;
  bool discoverAttributes() override;
  void disconnect() override;
  bool connected() override { return connected_; }
  bool hasCharacteristic(const std::string& uuid) override;
  uint8_t properties(const std::string& uuid) override;
  bool subscribe(const std::string& uuid) override;
  bool write(const std::string& uuid, const uint8_t* data, size_t length, bool withResponse) override;
  bool nextNotification(const std::string& uuid, Bytes& out) override;

 private:
  ScaleType type_;
  std::string name_;
  std::string address_;
  bool connected_ = false;
  bool subscribed_ = false;
  int connectCount_ = 0;
  float tareOffset_ = 0;
  unsigned long nextPacketMs_ = 0;
  std::function<float(unsigned long)> weightAt_;
  std::deque<Bytes> queued_;
  std::vector<Write> writes_;
  std::vector<std::string> events_;
};

// Protocol messages the library sends (copied from AcaiaArduinoBLE.cpp so
// tests check against an independent definition).
namespace acaia {
extern const Bytes IDENTIFY;
extern const Bytes HEARTBEAT;
extern const Bytes NOTIFICATION_REQUEST;
extern const Bytes START_TIMER;
extern const Bytes STOP_TIMER;
extern const Bytes RESET_TIMER;
extern const Bytes TARE;
extern const Bytes TARE_GENERIC;
extern const Bytes START_TIMER_GENERIC;
extern const Bytes STOP_TIMER_GENERIC;
extern const Bytes RESET_TIMER_GENERIC;
extern const Bytes TARE_START_TIMER_BOOKOO;
extern const Bytes BEEP_LEVEL_1_BOOKOO;
extern const Bytes TARE_FELICITA;
extern const Bytes START_TIMER_FELICITA;
extern const Bytes STOP_TIMER_FELICITA;
extern const Bytes RESET_TIMER_FELICITA;
extern const Bytes WEIGHT_TIMER_MODE_FELICITA;
}  // namespace acaia
