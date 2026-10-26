#include "ArduinoBLE.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <set>

BLELocalDevice BLE;

namespace fakeble {

struct CharacteristicState {
  std::string uuid;  // as given (returned by uuid())
  std::string key;   // normalized
  uint8_t properties = 0;
  size_t valueSize = 0;
  std::vector<uint8_t> value;

  // Remote characteristics belong to a peripheral; local ones to the sketch.
  RemotePeripheral* peripheral = nullptr;
  RemoteCharacteristicLayout remote;  // remote: discovered properties, as ArduinoBLE holds them

  bool written = false;              // local: set by a central's write
  std::vector<ValueChange> history;  // local: values the sketch wrote
};

struct ServiceState {
  std::string uuid;
  std::vector<BLECharacteristic> characteristics;
};

struct DeviceState {
  RemotePeripheral* peripheral = nullptr;
  bool discovered = false;
};

namespace {

struct Registry {
  std::vector<RemotePeripheral*> peripherals;
  std::deque<bool> beginResults;
  std::vector<unsigned long> beginTimes;
  bool begun = false;

  std::vector<BLEService> services;
  String localName;
  String advertisedService;
  BLEDeviceEventHandler handlers[3] = {nullptr, nullptr, nullptr};
  int advertiseCount = 0;

  bool scanning = false;
  int scanCount = 0;
  std::string scanAddress;
  std::set<RemotePeripheral*> reported;
};

Registry& registry() {
  static Registry r;
  return r;
}

const std::vector<ValueChange>& emptyHistory() {
  static const std::vector<ValueChange> empty;
  return empty;
}

}  // namespace

std::string normalizeUuid(const std::string& uuid) {
  std::string out = uuid;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  if (out.rfind("0x", 0) == 0) out = out.substr(2);
  return out;
}

void addPeripheral(RemotePeripheral* peripheral) { registry().peripherals.push_back(peripheral); }

void setBeginResults(const std::vector<bool>& results) {
  registry().beginResults.assign(results.begin(), results.end());
}

const std::vector<unsigned long>& beginCallTimes() { return registry().beginTimes; }
bool begun() { return registry().begun; }
int scanCount() { return registry().scanCount; }
bool scanning() { return registry().scanning; }
int advertiseCount() { return registry().advertiseCount; }
String localName() { return registry().localName; }
String advertisedServiceUuid() { return registry().advertisedService; }

BLECharacteristic findLocalCharacteristic(const std::string& uuid) {
  std::string key = normalizeUuid(uuid);
  for (auto& service : registry().services) {
    for (auto& c : service.state()->characteristics) {
      if (c.state()->key == key) return c;
    }
  }
  return BLECharacteristic();
}

bool centralWrite(const BLECharacteristic& characteristic, const uint8_t* data, size_t length) {
  auto& s = characteristic.state();
  if (!s || s->peripheral) return false;
  if (!(s->properties & (BLEWrite | BLEWriteWithoutResponse))) return false;
  length = std::min(length, s->valueSize);
  s->value.assign(data, data + length);
  s->written = true;
  return true;
}

const std::vector<ValueChange>& valueHistory(const BLECharacteristic& characteristic) {
  return characteristic.state() ? characteristic.state()->history : emptyHistory();
}

bool fireEvent(BLEDeviceEvent event) {
  BLEDeviceEventHandler handler = registry().handlers[event];
  if (!handler) return false;
  handler(BLEDevice());
  return true;
}

}  // namespace fakeble

using fakeble::registry;

// ---------------------------------------------------------------- BLECharacteristic

BLECharacteristic::BLECharacteristic() = default;

BLECharacteristic::BLECharacteristic(std::shared_ptr<fakeble::CharacteristicState> state) : state_(std::move(state)) {
  if (state_ && state_->peripheral) {
    remote_ = &state_->remote;
  } else {
    local_ = state_.get();
  }
}

BLECharacteristic::BLECharacteristic(const char* uuid, uint8_t properties, int valueSize, bool fixedLength)
    : state_(std::make_shared<fakeble::CharacteristicState>()) {
  (void)fixedLength;
  local_ = state_.get();
  state_->uuid = uuid;
  state_->key = fakeble::normalizeUuid(uuid);
  state_->properties = properties;
  state_->valueSize = static_cast<size_t>(valueSize);
}

const char* BLECharacteristic::uuid() const { return state_ ? state_->uuid.c_str() : ""; }

uint8_t BLECharacteristic::properties() const {
  if (!state_) return 0;
  if (state_->peripheral) return state_->remote.properties;
  return state_->properties;
}

int BLECharacteristic::valueSize() const { return state_ ? static_cast<int>(state_->valueSize) : 0; }
const uint8_t* BLECharacteristic::value() const { return state_ ? state_->value.data() : nullptr; }
int BLECharacteristic::valueLength() const { return state_ ? static_cast<int>(state_->value.size()) : 0; }

int BLECharacteristic::readValue(uint8_t value[], int length) {
  if (!state_ || length <= 0) return 0;
  int n = std::min(length, valueLength());
  std::copy(state_->value.begin(), state_->value.begin() + n, value);
  return n;
}

int BLECharacteristic::writeValue(const uint8_t value[], int length, bool withResponse) {
  if (!state_ || length < 0) return 0;
  if (state_->peripheral) {
    if (!state_->peripheral->connected()) return 0;
    // As BLERemoteCharacteristic::writeValue in ArduinoBLE 1.4.1: a write
    // request if it's writable and a response is wanted, else a write command
    // if the characteristic allows that, else nothing.
    uint8_t props = state_->remote.properties;
    bool request = (props & BLEWrite) && withResponse;
    if (!request && !(props & BLEWriteWithoutResponse)) return 0;
    return state_->peripheral->write(state_->key, value, static_cast<size_t>(length), request) ? 1 : 0;
  }
  // Local: ArduinoBLE truncates to the characteristic's size.
  size_t n = std::min(static_cast<size_t>(length), state_->valueSize);
  state_->value.assign(value, value + n);
  state_->history.push_back({millis(), state_->value});
  return 1;
}

bool BLECharacteristic::written() {
  if (!state_ || state_->peripheral) return false;
  bool w = state_->written;
  state_->written = false;  // ArduinoBLE clears the flag when it is read
  return w;
}

bool BLECharacteristic::valueUpdated() {
  if (!state_ || !state_->peripheral || !state_->peripheral->connected()) return false;
  std::vector<uint8_t> packet;
  if (!state_->peripheral->nextNotification(state_->key, packet)) return false;
  state_->value = packet;
  return true;
}

bool BLECharacteristic::canRead() { return (properties() & BLERead) != 0; }
bool BLECharacteristic::read() { return canRead(); }
bool BLECharacteristic::canWrite() { return (properties() & (BLEWrite | BLEWriteWithoutResponse)) != 0; }
bool BLECharacteristic::canSubscribe() { return (properties() & (BLENotify | BLEIndicate)) != 0; }

bool BLECharacteristic::subscribe() {
  if (!state_ || !state_->peripheral || !state_->peripheral->connected()) return false;
  return state_->peripheral->subscribe(state_->key);
}

// ---------------------------------------------------------------- BLEService

BLEService::BLEService() = default;

BLEService::BLEService(const char* uuid) : state_(std::make_shared<fakeble::ServiceState>()) {
  state_->uuid = uuid;
}

const char* BLEService::uuid() const { return state_ ? state_->uuid.c_str() : ""; }

void BLEService::addCharacteristic(BLECharacteristic& characteristic) {
  if (state_) state_->characteristics.push_back(characteristic);
}

int BLEService::characteristicCount() const {
  return state_ ? static_cast<int>(state_->characteristics.size()) : 0;
}

BLECharacteristic BLEService::characteristic(int index) const {
  if (!state_ || index < 0 || index >= characteristicCount()) return BLECharacteristic();
  return state_->characteristics[static_cast<size_t>(index)];
}

BLECharacteristic BLEService::characteristic(const char* uuid) const {
  if (!state_) return BLECharacteristic();
  std::string key = fakeble::normalizeUuid(uuid);
  for (auto& c : state_->characteristics) {
    if (c.state()->key == key) return c;
  }
  return BLECharacteristic();
}

// ---------------------------------------------------------------- BLEDevice

BLEDevice::BLEDevice() = default;

BLEDevice::BLEDevice(fakeble::RemotePeripheral* peripheral) : state_(std::make_shared<fakeble::DeviceState>()) {
  state_->peripheral = peripheral;
}

BLEDevice::operator bool() const { return state_ && state_->peripheral; }

String BLEDevice::address() const { return *this ? state_->peripheral->address() : String(""); }
String BLEDevice::localName() const { return *this ? state_->peripheral->localName() : String(""); }
String BLEDevice::advertisedServiceUuid() const {
  return *this ? state_->peripheral->advertisedServiceUuid() : String("");
}

bool BLEDevice::connect() { return *this && state_->peripheral->connect(); }

bool BLEDevice::discoverAttributes() {
  if (!*this || !state_->peripheral->connected()) return false;
  state_->discovered = state_->peripheral->discoverAttributes();
  return state_->discovered;
}

bool BLEDevice::disconnect() {
  if (!*this) return false;
  state_->peripheral->disconnect();
  state_->discovered = false;
  return true;
}

bool BLEDevice::connected() const { return *this && state_->peripheral->connected(); }

BLECharacteristic BLEDevice::characteristic(const char* uuid) const {
  // Like ArduinoBLE, remote characteristics only exist after discovery.
  if (!*this || !state_->discovered) return BLECharacteristic();
  std::string key = fakeble::normalizeUuid(uuid);
  if (!state_->peripheral->hasCharacteristic(key)) return BLECharacteristic();
  auto s = std::make_shared<fakeble::CharacteristicState>();
  s->uuid = uuid;
  s->key = key;
  s->peripheral = state_->peripheral;
  s->remote.uuid = String(uuid);
  s->remote.properties = state_->peripheral->properties(key);
  return BLECharacteristic(s);
}

// ---------------------------------------------------------------- BLELocalDevice

int BLELocalDevice::begin() {
  auto& r = registry();
  r.beginTimes.push_back(millis());
  bool ok = true;
  if (!r.beginResults.empty()) {
    ok = r.beginResults.front();
    r.beginResults.pop_front();
  }
  r.begun = ok;
  return ok ? 1 : 0;
}

void BLELocalDevice::end() { registry().begun = false; }
void BLELocalDevice::poll(unsigned long timeout) { (void)timeout; }

bool BLELocalDevice::setLocalName(const char* name) {
  registry().localName = name;
  return true;
}

bool BLELocalDevice::setDeviceName(const char* name) {
  (void)name;
  return true;
}

bool BLELocalDevice::setAdvertisedService(const BLEService& service) {
  registry().advertisedService = service.uuid();
  return true;
}

void BLELocalDevice::addService(BLEService& service) { registry().services.push_back(service); }

int BLELocalDevice::advertise() {
  if (!registry().begun) return 0;
  registry().advertiseCount++;
  return 1;
}

void BLELocalDevice::stopAdvertise() {}

void BLELocalDevice::setEventHandler(BLEDeviceEvent event, BLEDeviceEventHandler handler) {
  registry().handlers[event] = handler;
}

int BLELocalDevice::scan(bool withDuplicates) {
  (void)withDuplicates;
  auto& r = registry();
  if (!r.begun) return 0;
  r.scanning = true;
  r.scanCount++;
  r.scanAddress.clear();
  r.reported.clear();
  return 1;
}

int BLELocalDevice::scanForAddress(String address, bool withDuplicates) {
  if (!scan(withDuplicates)) return 0;
  registry().scanAddress = fakeble::normalizeUuid(address.c_str());
  return 1;
}

BLEDevice BLELocalDevice::available() {
  auto& r = registry();
  if (r.scanning) {
    for (auto* p : r.peripherals) {
      if (!p->advertising() || r.reported.count(p)) continue;
      if (!r.scanAddress.empty() && fakeble::normalizeUuid(p->address().c_str()) != r.scanAddress) continue;
      r.reported.insert(p);
      return BLEDevice(p);
    }
  }
  // Nothing new: time passes while the radio listens, so polling loops that
  // wait on millis() always finish.
  delay(10);
  return BLEDevice();
}

void BLELocalDevice::stopScan() { registry().scanning = false; }
