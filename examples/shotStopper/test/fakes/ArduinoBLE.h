// Host-side fake of the ArduinoBLE library.
//
// Covers both roles shotStopper uses:
//  - central: scanning for and talking to a scale (a fakeble::RemotePeripheral,
//    see support/fake_scale.h)
//  - peripheral: the local config service a phone writes to (see
//    support/fake_phone.h)
//
// As in ArduinoBLE, BLEDevice / BLEService / BLECharacteristic are handles that
// get copied around freely; every copy points at the same shared state.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Arduino.h"

enum BLEProperty {
  BLEBroadcast = 0x01,
  BLERead = 0x02,
  BLEWriteWithoutResponse = 0x04,
  BLEWrite = 0x08,
  BLENotify = 0x10,
  BLEIndicate = 0x20,
};

enum BLEDeviceEvent {
  BLEConnected = 0,
  BLEDisconnected = 1,
  BLEDiscovered = 2,
};

class BLEDevice;
typedef void (*BLEDeviceEventHandler)(BLEDevice device);

namespace fakeble {

// A remote peripheral (e.g. a scale) visible to BLE.scan(). Implemented by
// test doubles.
class RemotePeripheral {
 public:
  virtual ~RemotePeripheral() = default;
  virtual String localName() = 0;
  virtual String address() = 0;
  virtual String advertisedServiceUuid() { return String(""); }
  virtual bool advertising() = 0;
  virtual bool connect() = 0;
  virtual bool discoverAttributes() = 0;
  virtual void disconnect() = 0;
  virtual bool connected() = 0;
  // uuid arguments are normalized (see normalizeUuid).
  virtual bool hasCharacteristic(const std::string& uuid) = 0;
  virtual uint8_t properties(const std::string& uuid) = 0;
  virtual bool subscribe(const std::string& uuid) = 0;
  virtual bool write(const std::string& uuid, const uint8_t* data, size_t length) = 0;
  // Pops the next pending notification for uuid into out, if there is one.
  virtual bool nextNotification(const std::string& uuid, std::vector<uint8_t>& out) = 0;
};

// Lower-case, without a leading "0x".
std::string normalizeUuid(const std::string& uuid);

struct ValueChange {
  unsigned long timeMs;
  std::vector<uint8_t> value;
};

struct CharacteristicState;
struct ServiceState;
struct DeviceState;

}  // namespace fakeble

class BLEDescriptor {
 public:
  const char* uuid() const { return ""; }
  bool read() { return false; }
  const uint8_t* value() const { return nullptr; }
  int valueLength() const { return 0; }
};

class BLECharacteristic {
 public:
  BLECharacteristic();
  BLECharacteristic(const char* uuid, uint8_t properties, int valueSize, bool fixedLength = false);

  explicit operator bool() const { return state_ != nullptr; }

  const char* uuid() const;
  uint8_t properties() const;
  int valueSize() const;
  const uint8_t* value() const;
  int valueLength() const;
  int readValue(uint8_t value[], int length);
  int writeValue(const uint8_t value[], int length, bool withResponse = true);

  // Local characteristics: true once after a central wrote to it.
  bool written();
  // Remote characteristics: true once per received notification.
  bool valueUpdated();

  bool canRead();
  bool read();
  bool canWrite();
  bool canSubscribe();
  bool subscribe();

  int descriptorCount() { return 0; }
  BLEDescriptor descriptor(int) { return BLEDescriptor(); }

  // Fake internals, used by fakeble:: helpers.
  explicit BLECharacteristic(std::shared_ptr<fakeble::CharacteristicState> state) : state_(std::move(state)) {}
  const std::shared_ptr<fakeble::CharacteristicState>& state() const { return state_; }

 private:
  std::shared_ptr<fakeble::CharacteristicState> state_;
};

class BLEByteCharacteristic : public BLECharacteristic {
 public:
  BLEByteCharacteristic(const char* uuid, uint8_t properties) : BLECharacteristic(uuid, properties, 1, true) {}
  int writeValue(uint8_t value, bool withResponse = true) {
    return BLECharacteristic::writeValue(&value, 1, withResponse);
  }
  uint8_t value() const {
    const uint8_t* v = BLECharacteristic::value();
    return v && valueLength() > 0 ? v[0] : 0;
  }
};

class BLEService {
 public:
  BLEService();
  explicit BLEService(const char* uuid);

  const char* uuid() const;
  void addCharacteristic(BLECharacteristic& characteristic);
  int characteristicCount() const;
  BLECharacteristic characteristic(int index) const;
  BLECharacteristic characteristic(const char* uuid) const;

  const std::shared_ptr<fakeble::ServiceState>& state() const { return state_; }

 private:
  std::shared_ptr<fakeble::ServiceState> state_;
};

class BLEDevice {
 public:
  BLEDevice();
  explicit BLEDevice(fakeble::RemotePeripheral* peripheral);

  explicit operator bool() const;

  String address() const;
  String localName() const;
  String advertisedServiceUuid() const;
  String deviceName() const { return localName(); }
  uint16_t appearance() const { return 0; }

  bool connect();
  bool discoverAttributes();
  bool disconnect();
  bool connected() const;

  int serviceCount() const { return 0; }
  BLEService service(int) const { return BLEService(); }
  BLECharacteristic characteristic(const char* uuid) const;

 private:
  std::shared_ptr<fakeble::DeviceState> state_;
};

class BLELocalDevice {
 public:
  int begin();
  void end();
  void poll(unsigned long timeout = 0);

  bool setLocalName(const char* name);
  bool setDeviceName(const char* name);
  bool setAdvertisedService(const BLEService& service);
  void addService(BLEService& service);
  int advertise();
  void stopAdvertise();
  void setEventHandler(BLEDeviceEvent event, BLEDeviceEventHandler handler);

  int scan(bool withDuplicates = false);
  int scanForAddress(String address, bool withDuplicates = false);
  BLEDevice available();
  void stopScan();
};

extern BLELocalDevice BLE;

// ---------------------------------------------------------------- test controls

namespace fakeble {

// Make a remote peripheral visible to scans. Not owned.
void addPeripheral(RemotePeripheral* peripheral);

// Results returned by successive BLE.begin() calls; after the list runs out
// begin() succeeds.
void setBeginResults(const std::vector<bool>& results);
const std::vector<unsigned long>& beginCallTimes();
bool begun();

int scanCount();
bool scanning();
int advertiseCount();
String localName();
String advertisedServiceUuid();

// Local GATT database (services added with BLE.addService).
BLECharacteristic findLocalCharacteristic(const std::string& uuid);

// A central (phone) writing to a local characteristic: truncated to the
// characteristic's size, sets written(). Returns false if the characteristic
// is missing or not writable.
bool centralWrite(const BLECharacteristic& characteristic, const uint8_t* data, size_t length);

// Values the sketch wrote to a local characteristic, in order.
const std::vector<ValueChange>& valueHistory(const BLECharacteristic& characteristic);

// Invoke the handler registered with BLE.setEventHandler, if any.
bool fireEvent(BLEDeviceEvent event);

}  // namespace fakeble
