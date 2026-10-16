// Helpers for writing shotStopper scenarios as a timeline:
//
//   FakeScale scale(ScaleType::NEW);
//   Espresso cup([] { return brewInputActive() || outHigh(); });
//   scale.setWeight(cup.weight());
//   fakeble::addPeripheral(&scale);
//   boot();
//   runFor(1000);            // scale connects
//   pressBrew();
//   runUntil(outHigh, 5000); // latched
#pragma once

#include <algorithm>
#include <functional>
#include <string>

#include <Arduino.h>
#include <EEPROM.h>

#include "fake_phone.h"
#include "fake_scale.h"
#include "sketch_api.h"
#include "test.h"

// Run the sketch's setup().
inline void boot() { setup(); }

// One loop() iteration. The sketch's loop() normally advances the clock itself
// (delay(1) in checkOTAMode, BLE scans); if it didn't, move time on 1 ms so
// runFor/runUntil always finish.
inline void step() {
  unsigned long before = millis();
  loop();
  if (millis() == before) fake::advance(1);
}

inline void runFor(unsigned long ms) {
  unsigned long end = millis() + ms;
  while (millis() < end) step();
}

// Runs loop() until cond() holds or maxMs of simulated time pass.
inline bool runUntil(const std::function<bool()>& cond, unsigned long maxMs) {
  unsigned long end = millis() + maxMs;
  while (!cond()) {
    if (millis() >= end) return false;
    step();
  }
  return true;
}

// Brew input (active low) on whichever pin the sketch currently reads.
inline void pressBrew() { fake::setInput(static_cast<uint8_t>(in), LOW); }
inline void releaseBrew() { fake::setInput(static_cast<uint8_t>(in), HIGH); }
inline bool brewInputActive() { return digitalRead(static_cast<uint8_t>(in)) == LOW; }

inline bool outHigh() { return fake::outputLevel(sketch::kPinOut) == HIGH; }
inline bool outLow() { return !outHigh(); }
inline const std::vector<fake::PinEvent>& outHistory() { return fake::outputHistory(sketch::kPinOut); }

// Time of the first OUT transition to `level` at or after `since`, or 0.
inline unsigned long outChangedAt(int level, unsigned long since = 0) {
  for (auto& e : outHistory())
    if (e.value == level && e.timeMs >= since) return e.timeMs;
  return 0;
}

// The stop reason the sketch printed for the last shot ("weight", "time", ...).
inline std::string lastShotEndReason() {
  const std::string& out = fake::serialOutput();
  const std::string marker = "ShotEnded by ";
  size_t pos = out.rfind(marker);
  if (pos == std::string::npos) return "";
  size_t start = pos + marker.size();
  return out.substr(start, out.find_first_of("\r\n", start) - start);
}

// The final weight from the sketch's "I detected a final weight of Xg" line, or NAN.
inline float detectedFinalWeight() {
  const std::string& out = fake::serialOutput();
  const std::string marker = "I detected a final weight of ";
  size_t pos = out.rfind(marker);
  if (pos == std::string::npos) return NAN;
  return std::stof(out.substr(pos + marker.size()));
}

inline std::string eepromFlashString(int address, int maxLength = 32) {
  std::string s;
  for (int i = 0; i < maxLength; i++) {
    char c = static_cast<char>(fake::eepromFlash(address + i));
    if (c == '\0') break;
    s += c;
  }
  return s;
}

// Pre-load a valid configuration, as if saved by an earlier boot.
struct StoredConfig {
  bool enabled = true;
  uint8_t goalWeight = 36;
  uint8_t offsetTenths = 15;
  bool momentary = false;
  bool reedSwitch = false;
  bool autoTare = true;
  uint8_t minShotDurationS = 3;
  uint8_t maxShotDurationS = 50;
  uint8_t dripDelayS = 3;
  const char* wifiSsid = "";
  const char* wifiPass = "";
};

inline void preloadConfig(const StoredConfig& c) {
  fake::eepromPreload(sketch::kSignatureAddr, static_cast<uint8_t>(sketch::kSignatureValue));
  fake::eepromPreload(sketch::kEnabledAddr, c.enabled);
  fake::eepromPreload(sketch::kWeightAddr, c.goalWeight);
  fake::eepromPreload(sketch::kOffsetAddr, c.offsetTenths);
  fake::eepromPreload(sketch::kMomentaryAddr, c.momentary);
  fake::eepromPreload(sketch::kReedSwitchAddr, c.reedSwitch);
  fake::eepromPreload(sketch::kAutoTareAddr, c.autoTare);
  fake::eepromPreload(sketch::kMinShotDurationAddr, c.minShotDurationS);
  fake::eepromPreload(sketch::kMaxShotDurationAddr, c.maxShotDurationS);
  fake::eepromPreload(sketch::kDripDelayAddr, c.dripDelayS);
  fake::eepromPreloadString(sketch::kWifiSsidAddr, c.wifiSsid);
  fake::eepromPreloadString(sketch::kWifiPassAddr, c.wifiPass);
}

// Espresso flowing into the cup on the scale: weight rises at flowGramsPerS
// while pumpRunning() is true; after the pump stops, dripGrams more arrive
// over dripMs.
class Espresso {
 public:
  explicit Espresso(std::function<bool()> pumpRunning) : pumpRunning_(std::move(pumpRunning)) {}

  float flowGramsPerS = 2.0f;
  float dripGrams = 0.0f;
  unsigned long dripMs = 2000;

  float weightAt(unsigned long t) {
    bool on = pumpRunning_();
    if (t > lastMs_) {
      if (wasOn_) poured_ += flowGramsPerS * static_cast<float>(t - lastMs_) / 1000.0f;
      lastMs_ = t;
    }
    if (wasOn_ && !on) stoppedMs_ = t;
    wasOn_ = on;
    float drip = 0;
    if (stoppedMs_ && !on)
      drip = dripGrams * std::min(1.0f, static_cast<float>(t - stoppedMs_) / static_cast<float>(dripMs));
    return poured_ + drip;
  }

  // For FakeScale::setWeight. The Espresso must outlive the scale's use of it.
  std::function<float(unsigned long)> weight() {
    return [this](unsigned long t) { return weightAt(t); };
  }

 private:
  std::function<bool()> pumpRunning_;
  float poured_ = 0;
  unsigned long lastMs_ = 0;
  bool wasOn_ = false;
  unsigned long stoppedMs_ = 0;
};

// Boots the sketch with `scale` in range and runs until it is connected.
inline void bootConnected(FakeScale& scale) {
  fakeble::addPeripheral(&scale);
  boot();
  REQUIRE(runUntil([&] { return scale.connected() && scale.isSubscribed(); }, 15000));
  runFor(200);
}
