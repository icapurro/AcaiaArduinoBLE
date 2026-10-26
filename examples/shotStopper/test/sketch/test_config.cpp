// Config written over BLE, and the disabled / OTA modes.
#include <WiFi.h>

#include "scenario.h"

static void writeAndPoll(FakePhone& phone, const char* uuid, uint8_t value) {
  REQUIRE(phone.writeByte(uuid, value));
  runFor(20);
}

TEST(config_written_over_ble_is_applied_and_persisted) {
  boot();
  FakePhone phone;

  writeAndPoll(phone, "0xFF11", 40);
  CHECK_EQ(goalWeight, (uint8_t)40);
  CHECK_EQ(fake::eepromFlash(sketch::kWeightAddr), (uint8_t)40);

  writeAndPoll(phone, "0xFF13", 1);
  CHECK(momentary);
  CHECK_EQ(fake::eepromFlash(sketch::kMomentaryAddr), (uint8_t)1);

  writeAndPoll(phone, "0xFF14", 0);
  CHECK(!autoTare);
  CHECK_EQ(fake::eepromFlash(sketch::kAutoTareAddr), (uint8_t)0);

  writeAndPoll(phone, "0xFF15", 5);
  CHECK_EQ(minShotDurationS, (uint8_t)5);
  CHECK_EQ(fake::eepromFlash(sketch::kMinShotDurationAddr), (uint8_t)5);

  writeAndPoll(phone, "0xFF16", 60);
  CHECK_EQ(maxShotDurationS, (uint8_t)60);
  CHECK_EQ(fake::eepromFlash(sketch::kMaxShotDurationAddr), (uint8_t)60);

  writeAndPoll(phone, "0xFF17", 4);
  CHECK_EQ(dripDelayS, (uint8_t)4);
  CHECK_EQ(fake::eepromFlash(sketch::kDripDelayAddr), (uint8_t)4);

  REQUIRE(phone.writeString("0xFF22", "HomeWifi"));
  runFor(20);
  CHECK(wifiSsid == "HomeWifi");
  CHECK_EQ(eepromFlashString(sketch::kWifiSsidAddr), std::string("HomeWifi"));

  REQUIRE(phone.writeString("0xFF23", "secret"));
  runFor(20);
  CHECK(wifiPass == "secret");
  CHECK_EQ(eepromFlashString(sketch::kWifiPassAddr), std::string("secret"));

  writeAndPoll(phone, "0xFF10", 0);
  CHECK(!enabled);
  CHECK_EQ(fake::eepromFlash(sketch::kEnabledAddr), (uint8_t)0);
}

TEST(reed_switch_setting_moves_the_brew_input) {
  boot();
  FakePhone phone;
  CHECK_EQ(in, sketch::kPinIn);
  writeAndPoll(phone, "0xFF12", 1);
  CHECK(reedSwitch);
  CHECK_EQ(in, sketch::kPinReedIn);
  CHECK_EQ(fake::eepromFlash(sketch::kReedSwitchAddr), (uint8_t)1);
  writeAndPoll(phone, "0xFF12", 0);
  CHECK_EQ(in, sketch::kPinIn);
}

TEST(over_long_ssid_is_truncated_to_the_characteristic_size) {
  boot();
  FakePhone phone;
  std::string longSsid(40, 'S');
  REQUIRE(phone.writeString("0xFF22", longSsid));
  runFor(20);
  CHECK_EQ(wifiSsid.length(), 32u);
  CHECK_EQ(eepromFlashString(sketch::kWifiSsidAddr, 64), std::string(32, 'S'));
  CHECK(fake::eepromFlash(sketch::kEepromSize - 1) != 'S');  // nothing written past EEPROM_SIZE
}

TEST(ssid_of_32_chars_erases_the_stored_password) {
  // Current behavior (likely bug): the SSID slot is 32 bytes (10..41), but
  // writeStringToEEPROM() puts the terminator at 10 + 32 = 42, which is the
  // first byte of the password slot. A max-length SSID wipes the password.
  boot();
  FakePhone phone;
  REQUIRE(phone.writeString("0xFF23", "secret"));
  runFor(20);
  REQUIRE(phone.writeString("0xFF22", std::string(32, 'S')));
  runFor(20);
  CHECK_EQ(eepromFlashString(sketch::kWifiPassAddr), std::string(""));
  CHECK(wifiPass == "secret");  // in RAM until the next reboot
}

static bool latchingPump() { return brewInputActive() || outHigh(); }

TEST(disabling_during_a_shot_stops_it_and_drops_the_scale) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  bootConnected(scale);
  FakePhone phone;

  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));
  releaseBrew();
  runFor(2000);

  writeAndPoll(phone, "0xFF10", 0);
  CHECK(outLow());
  CHECK_EQ(lastShotEndReason(), std::string("time"));
  CHECK(!scale.connected());
  CHECK_EQ(phone.readByte("0xFF19"), 0);
  CHECK_EQ(phone.readByte("0xFF20"), 0);

  runFor(10000);
  CHECK_EQ(scale.connectCount(), 1);  // no reconnect while disabled
  CHECK(!scale.connected());
}

TEST(re_enabling_reconnects_to_the_scale) {
  // AcaiaArduinoBLE::disconnect() doesn't clear isConnected(), so after
  // re-enabling the sketch first treats the dropped scale as connected. The
  // next heartbeat write fails, which clears it, and the sketch reconnects.
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  bootConnected(scale);
  FakePhone phone;

  writeAndPoll(phone, "0xFF10", 0);
  REQUIRE(!scale.connected());
  runFor(1000);
  writeAndPoll(phone, "0xFF10", 1);
  REQUIRE(runUntil([&] { return scale.connected(); }, 4000));  // within one heartbeat period
  CHECK_EQ(scale.connectCount(), 2);
  CHECK_EQ(phone.byteChanges("0xFF19").back(), 1);
}

TEST(ota_request_without_wifi_credentials_keeps_scale_disconnected) {
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  bootConnected(scale);
  FakePhone phone;

  writeAndPoll(phone, "0xFF21", 1);
  CHECK(otaModeRequested);
  CHECK(fake::serialContains("No WiFi SSID provided"));
  CHECK(!scale.connected());
  runFor(10000);
  CHECK_EQ(scale.connectCount(), 1);
  CHECK_EQ(fake::wifiBeginCount(), 0);
}

TEST(ota_mode_connects_wifi_and_publishes_ip) {
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  bootConnected(scale);
  FakePhone phone;

  REQUIRE(phone.writeString("0xFF22", "HomeWifi"));
  runFor(20);
  writeAndPoll(phone, "0xFF21", 1);
  CHECK_EQ(fake::wifiBeginCount(), 1);
  CHECK(fake::wifiLastSsid() == "HomeWifi");
  CHECK(!scale.connected());

  fake::setWifiStatus(WL_CONNECTED);
  runFor(50);
  CHECK(fake::serialContains("Web Server Started"));
  CHECK_EQ(phone.readString("0xFF24"), std::string("192.168.1.50"));

  writeAndPoll(phone, "0xFF21", 0);
  CHECK_EQ(phone.readString("0xFF24"), std::string("disconnected"));
  REQUIRE(runUntil([&] { return scale.connected(); }, 15000));  // back to brewing duty
}
