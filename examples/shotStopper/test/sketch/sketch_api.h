// What sketch tests can see of shotStopper.ino (compiled in sketch_tu.cpp).
//
// Prefer observing behavior through pins, EEPROM, Serial and the fake phone;
// the globals are here for state that has no other outlet.
#pragma once

#include <Arduino.h>

void setup();
void loop();

// Sketch globals.
extern uint8_t goalWeight;
extern float weightOffset;
extern float currentWeight;
extern bool enabled;
extern bool momentary;
extern bool reedSwitch;
extern bool autoTare;
extern bool otaModeRequested;
extern bool bleReady;
extern uint8_t minShotDurationS;
extern uint8_t maxShotDurationS;
extern uint8_t dripDelayS;
extern int in;
extern String wifiSsid;
extern String wifiPass;

// Sketch #defines.
namespace sketch {
extern const int kPinIn;
extern const int kPinOut;
extern const int kPinReedIn;
extern const int kEepromSize;
extern const int kSignatureAddr;
extern const int kSignatureValue;
extern const int kEnabledAddr;
extern const int kWeightAddr;
extern const int kOffsetAddr;
extern const int kMomentaryAddr;
extern const int kReedSwitchAddr;
extern const int kAutoTareAddr;
extern const int kMinShotDurationAddr;
extern const int kMaxShotDurationAddr;
extern const int kDripDelayAddr;
extern const int kWifiSsidAddr;
extern const int kWifiPassAddr;
extern const int kMaxOffset;
extern const int kFirmwareVersion;
}  // namespace sketch
