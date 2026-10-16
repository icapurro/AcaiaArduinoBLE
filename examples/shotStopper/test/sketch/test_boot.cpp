// Boot, EEPROM defaults and the BLE config service.
#include "scenario.h"

TEST(smoke_boot_and_connect_to_a_scale) {
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  bootConnected(scale);
  runFor(6000);
  FakePhone phone;
  CHECK(scale.connected());
  CHECK_EQ(phone.readByte("0xFF19"), 1);  // scale status: connected
  CHECK(scale.countWrites(acaia::HEARTBEAT) >= 2);  // every 2750 ms
}

TEST(first_boot_writes_defaults_to_eeprom) {
  boot();
  CHECK(fake::serialContains("EEPROM initialized with defaults"));
  CHECK_EQ(fake::eepromBeginSize(), (size_t)sketch::kEepromSize);
  CHECK_EQ(fake::eepromFlash(sketch::kSignatureAddr), (uint8_t)sketch::kSignatureValue);
  CHECK_EQ(fake::eepromFlash(sketch::kEnabledAddr), (uint8_t)1);
  CHECK_EQ(fake::eepromFlash(sketch::kWeightAddr), (uint8_t)36);
  CHECK_EQ(fake::eepromFlash(sketch::kOffsetAddr), (uint8_t)15);  // 1.5 g in tenths
  CHECK_EQ(fake::eepromFlash(sketch::kMomentaryAddr), (uint8_t)0);
  CHECK_EQ(fake::eepromFlash(sketch::kReedSwitchAddr), (uint8_t)0);
  CHECK_EQ(fake::eepromFlash(sketch::kAutoTareAddr), (uint8_t)1);
  CHECK_EQ(fake::eepromFlash(sketch::kMinShotDurationAddr), (uint8_t)3);
  CHECK_EQ(fake::eepromFlash(sketch::kMaxShotDurationAddr), (uint8_t)50);
  CHECK_EQ(fake::eepromFlash(sketch::kDripDelayAddr), (uint8_t)3);
  CHECK_EQ(eepromFlashString(sketch::kWifiSsidAddr), std::string(""));
  CHECK_EQ(eepromFlashString(sketch::kWifiPassAddr), std::string(""));
  CHECK_EQ(goalWeight, (uint8_t)36);
  CHECK_NEAR(weightOffset, 1.5, 1e-6);
}

TEST(boot_loads_stored_config) {
  StoredConfig config;
  config.goalWeight = 42;
  config.offsetTenths = 20;
  config.momentary = true;
  config.maxShotDurationS = 45;
  config.wifiSsid = "HomeWifi";
  preloadConfig(config);
  boot();
  CHECK(fake::serialContains("Goal Weight retrieved: 42"));
  CHECK(!fake::serialContains("initialized with defaults"));
  CHECK_EQ(goalWeight, (uint8_t)42);
  CHECK_NEAR(weightOffset, 2.0, 1e-6);
  CHECK(momentary);
  CHECK_EQ(maxShotDurationS, (uint8_t)45);
  CHECK(wifiSsid == "HomeWifi");
  CHECK_EQ(fake::eepromCommitCount(), 0);  // nothing rewritten
}

TEST(boot_publishes_config_service) {
  StoredConfig config;
  config.goalWeight = 40;
  config.wifiSsid = "HomeWifi";
  preloadConfig(config);
  boot();
  CHECK(bleReady);
  CHECK(fakeble::localName() == "shotStopper");
  CHECK(fakeble::advertisedServiceUuid() == "0x0FFE");
  CHECK_EQ(fakeble::advertiseCount(), 1);
  FakePhone phone;
  CHECK_EQ(phone.readByte("0xFF10"), 1);   // enabled
  CHECK_EQ(phone.readByte("0xFF11"), 40);  // goal weight
  CHECK_EQ(phone.readByte("0xFF18"), sketch::kFirmwareVersion);
  CHECK_EQ(phone.readByte("0xFF19"), 0);   // scale disconnected
  CHECK_EQ(phone.readByte("0xFF20"), 0);   // not brewing
  CHECK_EQ(phone.readString("0xFF22"), std::string("HomeWifi"));
  CHECK(phone.read("0xFF23").empty());     // password is write-only and never published
}

TEST(boot_configures_gpio) {
  boot();
  CHECK_EQ(fake::pinModeOf(sketch::kPinIn), INPUT_PULLUP);
  CHECK_EQ(fake::pinModeOf(sketch::kPinOut), OUTPUT);
  CHECK_EQ(fake::outputLevel(sketch::kPinOut), LOW);
  CHECK_EQ(fake::analogValue(LED_RED), 255);  // RGB LED off (active low)
  CHECK_EQ(fake::cpuFrequencyMhz(), 0u);      // LOW_VOLTAGE false: clock untouched
}

TEST(phone_disconnect_restarts_advertising) {
  boot();
  FakePhone phone;
  CHECK_EQ(fakeble::advertiseCount(), 1);
  CHECK(phone.disconnect());
  CHECK_EQ(fakeble::advertiseCount(), 2);
}
