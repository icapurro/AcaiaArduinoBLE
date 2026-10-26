// Grind-by-Sync through the whole sketch: config over BLE, EEPROM, the shot
// timeline, and isolation from shot control.
#include <ArduinoJson.h>
#include <WiFi.h>

#include "scenario.h"

static bool latchingPump() { return brewInputActive() || outHigh(); }

static std::vector<uint8_t> pairingIdBytes(uint32_t id) {
  return {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id >> 16),
          static_cast<uint8_t>(id >> 24)};
}

static uint32_t flashPairingId() {
  uint32_t id = 0;
  for (int i = 0; i < 4; i++) id |= static_cast<uint32_t>(fake::eepromFlash(sketch::kGrinderPairingIdAddr + i)) << (8 * i);
  return id;
}

static unsigned long firstWriteAt(const FakeScale& scale, const Bytes& bytes, unsigned long since = 0) {
  for (auto& w : scale.writes())
    if (w.bytes == bytes && w.timeMs >= since) return w.timeMs;
  return 0;
}

// Pull a latching shot stopped by weight and wait out the drip delay.
static void pullShotByWeight() {
  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));
  releaseBrew();
  REQUIRE(runUntil(outLow, 40000));
}

// ---------------------------------------------------------------- 3.1 EEPROM

TEST(grinder_fields_written_on_first_boot) {
  boot();
  CHECK_EQ(fake::eepromFlash(sketch::kGrinderSignatureAddr), (uint8_t)sketch::kGrinderSignatureValue);
  CHECK_EQ(eepromFlashString(sketch::kGrinderIpAddr, 16), std::string(""));
  CHECK_EQ(flashPairingId(), 0u);
  CHECK(grinderIp == "");
  runFor(100);
  CHECK_EQ(FakePhone().readByte("0xFF27"), (int)GRINDER_NOT_CONFIGURED);
}

static void checkUpgradeFromOldFirmware(uint8_t byte75) {
  StoredConfig config;
  config.goalWeight = 40;
  config.wifiSsid = "HomeWifi";
  preloadConfig(config);
  fake::eepromPreload(sketch::kGrinderSignatureAddr, byte75);
  boot();
  runFor(1000);
  CHECK_EQ(goalWeight, (uint8_t)40);
  CHECK(wifiSsid == "HomeWifi");
  CHECK(grinderIp == "");
  CHECK_EQ(grinderPairingId, 0u);
  CHECK_EQ(fake::eepromFlash(sketch::kGrinderSignatureAddr), (uint8_t)sketch::kGrinderSignatureValue);
  CHECK_EQ(fake::eepromFlash(sketch::kWeightAddr), (uint8_t)40);
  CHECK_EQ(FakePhone().readByte("0xFF27"), (int)GRINDER_NOT_CONFIGURED);
  CHECK_EQ(fake::wifiBeginCount(), 0);
}

TEST(upgrade_from_old_firmware_erased_bytes) { checkUpgradeFromOldFirmware(0xFF); }
TEST(upgrade_from_old_firmware_zeroed_bytes) { checkUpgradeFromOldFirmware(0x00); }

TEST(stored_grinder_config_is_loaded_at_boot) {
  FakeGrinder g("192.168.1.30", 77);
  preloadGrinder(g);
  boot();
  CHECK(grinderIp == "192.168.1.30");
  CHECK_EQ(grinderPairingId, 77u);
  REQUIRE(runUntil([&] { return g.count("brewJobs") > 0; }, 3000));  // no phone needed
  JsonDocument body;
  deserializeJson(body, g.received("brewJobs")[0].body);
  CHECK_EQ(body["pairingId"].as<uint32_t>(), 77u);
  FakePhone phone;
  CHECK_EQ(phone.readString("0xFF25"), std::string("192.168.1.30"));
  CHECK_EQ(phone.read("0xFF26"), pairingIdBytes(77));
}

// ---------------------------------------------------------------- 3.2 BLE config

TEST(configure_the_grinder_from_the_phone) {
  FakeGrinder g;
  fake::setWifiConnectsOnBegin(true);
  boot();
  FakePhone phone;
  REQUIRE(phone.writeString("0xFF22", "HomeWifi"));
  REQUIRE(phone.writeString("0xFF23", "secret"));
  REQUIRE(phone.writeString("0xFF25", "192.168.1.30"));
  REQUIRE(phone.write("0xFF26", pairingIdBytes(42)));
  REQUIRE(runUntil([&] { return g.count("brewJobs") > 0; }, 3000));
  CHECK(grinderIp == "192.168.1.30");
  CHECK_EQ(grinderPairingId, 42u);
  CHECK_EQ(eepromFlashString(sketch::kGrinderIpAddr, 16), std::string("192.168.1.30"));
  CHECK_EQ(flashPairingId(), 42u);
  CHECK_EQ(phone.readString("0xFF25"), std::string("192.168.1.30"));
  CHECK_EQ(phone.read("0xFF26"), pairingIdBytes(42));
  REQUIRE(runUntil([&] { return phone.readByte("0xFF27") == GRINDER_READY; }, 3000));
}

TEST(trailing_null_in_ip_write_is_accepted) {
  boot();
  FakePhone phone;
  REQUIRE(phone.write("0xFF25", {'1', '0', '.', '0', '.', '0', '.', '9', 0}));
  runFor(20);
  CHECK(grinderIp == "10.0.0.9");
}

TEST(invalid_grinder_address_is_rejected) {
  FakeGrinder g;
  preloadGrinder(g);
  boot();
  runFor(100);
  int commits = fake::eepromCommitCount();
  FakePhone phone;
  REQUIRE(phone.writeString("0xFF25", "grinder.local"));
  runFor(20);
  CHECK(grinderIp == "192.168.1.30");
  CHECK_EQ(eepromFlashString(sketch::kGrinderIpAddr, 16), std::string("192.168.1.30"));
  CHECK_EQ(fake::eepromCommitCount(), commits);
  CHECK_EQ(phone.readString("0xFF25"), std::string("192.168.1.30"));
  CHECK(fake::serialContains("invalid IP address ignored"));
}

TEST(pairing_id_of_wrong_length_is_rejected) {
  FakeGrinder g;
  preloadGrinder(g);
  boot();
  FakePhone phone;
  REQUIRE(phone.write("0xFF26", {1, 2}));
  runFor(20);
  CHECK_EQ(grinderPairingId, 42u);
  CHECK_EQ(phone.read("0xFF26"), pairingIdBytes(42));
}

TEST(turning_grinder_sync_off) {
  FakeGrinder g;
  preloadGrinder(g);
  boot();
  REQUIRE(runUntil([] { return grinder.status() == GRINDER_READY; }, 5000));
  FakePhone phone;
  REQUIRE(phone.writeString("0xFF25", ""));
  runFor(100);
  size_t requests = g.received().size();
  runFor(10000);
  CHECK_EQ(g.received().size(), requests);
  CHECK(grinderIp == "");
  CHECK_EQ(eepromFlashString(sketch::kGrinderIpAddr, 16), std::string(""));
  CHECK_EQ(phone.readByte("0xFF27"), (int)GRINDER_NOT_CONFIGURED);
}

TEST(no_grinder_no_wifi) {
  StoredConfig config;
  config.wifiSsid = "HomeWifi";
  preloadConfig(config);
  boot();
  runFor(20000);
  CHECK_EQ(fake::wifiBeginCount(), 0);
  CHECK(fakehttp::requests().empty());
}

// ---------------------------------------------------------------- 3.3 task and activity

TEST(grinder_task_runs_on_core_0) {
  boot();
  REQUIRE(fake::tasks().size() == 1);
  const auto& t = fake::tasks()[0];
  CHECK_EQ(t.name, std::string("grinder"));
  CHECK(t.function == &GrinderClient::taskMain);
  CHECK(t.parameters == &grinder);
  CHECK_EQ(t.coreId, 0);
  CHECK_EQ(t.stackDepth, 16384u);
  CHECK_EQ(t.priority, 1u);
}

TEST(ota_mode_pauses_grinder_sync) {
  FakeGrinder g;
  preloadGrinder(g);
  boot();
  REQUIRE(runUntil([] { return grinder.status() == GRINDER_READY; }, 5000));
  FakePhone phone;
  REQUIRE(phone.writeByte("0xFF21", 1));
  runFor(100);
  size_t requests = g.received().size();
  runFor(20000);
  CHECK_EQ(g.received().size(), requests);
  CHECK(fake::serialContains("Web Server Started"));  // OTASetup unchanged, owns WiFi meanwhile

  REQUIRE(phone.writeByte("0xFF21", 0));  // OTASetup drops WiFi on the way out
  REQUIRE(runUntil([&] { return g.received().size() > requests; }, 15000));
  CHECK_EQ(WiFi.status(), WL_CONNECTED);
}

TEST(disabled_pauses_grinder_sync) {
  // With a scale in range, as in real use (the harness only runs the grinder
  // client between loop() iterations).
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  FakeGrinder g;
  bootWithGrinder(scale, g);
  FakePhone phone;
  REQUIRE(phone.writeByte("0xFF10", 0));
  runFor(100);
  size_t requests = g.received().size();
  g.addJob("g1");
  runFor(10000);
  CHECK_EQ(g.received().size(), requests);
  CHECK(!g.isTaken("g1"));
  REQUIRE(phone.writeByte("0xFF10", 1));
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 5000));
}

TEST(disabled_during_a_shot_reports_nothing) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  FakeGrinder g;
  bootWithGrinder(scale, g);
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));
  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));
  releaseBrew();
  runFor(3000);
  FakePhone phone;
  REQUIRE(phone.writeByte("0xFF10", 0));
  runFor(100);
  CHECK(outLow());
  REQUIRE(phone.writeByte("0xFF10", 1));
  runFor(30000);
  CHECK_EQ(g.count("brewDone"), 0);
}

// ---------------------------------------------------------------- 3.4 shot outcomes

TEST(shot_stopped_by_weight_is_reported_with_time_and_weight) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  cup.dripGrams = 1.5f;
  scale.setWeight(cup.weight());
  FakeGrinder g;
  bootWithGrinder(scale, g);
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));

  unsigned long pressedAt = millis();
  pullShotByWeight();
  unsigned long startedAt = firstWriteAt(scale, acaia::TARE, pressedAt);
  unsigned long stoppedAt = outChangedAt(LOW, pressedAt + 1);
  CHECK_EQ(g.count("brewDone"), 0);  // waits for the drip delay
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 6000));

  runFor(500);  // the sketch's own final-weight check runs just after the drip delay
  const auto& d = g.dones()[0];
  CHECK_EQ(d.grindUuid, std::string("g1"));
  CHECK_EQ(d.result, std::string("Success"));
  CHECK_NEAR(d.durationFinalMs, (double)(stoppedAt - startedAt), 2);
  CHECK(d.timeMs - stoppedAt >= 3000);  // dripDelayS
  CHECK(d.timeMs - stoppedAt < 3200);
  // The weight once the drip has settled: what the sketch's own final-weight check sees.
  CHECK_NEAR(d.massFinalMg, detectedFinalWeight() * 1000.0, 200);
  CHECK(d.massFinalMg > 34500);
}

TEST(manual_stop_is_reported_as_canceled) {
  StoredConfig config;
  config.momentary = true;
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 5.0f; });
  FakeGrinder g;
  preloadGrinder(g);
  config.wifiSsid = "HomeWifi";
  config.wifiPass = "secret";
  preloadConfig(config);
  fake::eepromPreload(sketch::kGrinderSignatureAddr, static_cast<uint8_t>(sketch::kGrinderSignatureValue));
  bootConnected(scale);
  REQUIRE(runUntil([] { return grinder.status() == GRINDER_READY; }, 5000));
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));

  pressBrew();  // momentary: press + release starts...
  runFor(200);
  releaseBrew();
  runFor(10000);
  pressBrew();  // ...and press + release stops (after minShotDurationS)
  runFor(200);
  releaseBrew();
  REQUIRE(runUntil([] { return lastShotEndReason() == "button"; }, 2000));
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 3000));
  CHECK_EQ(g.dones()[0].result, std::string("Canceled"));
  CHECK_EQ(g.dones()[0].durationFinalMs, 0L);
}

TEST(max_duration_is_reported_as_canceled) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  cup.flowGramsPerS = 0.1f;  // choked
  scale.setWeight(cup.weight());
  FakeGrinder g;
  bootWithGrinder(scale, g);
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));
  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));
  releaseBrew();
  REQUIRE(runUntil(outLow, 60000));
  CHECK_EQ(lastShotEndReason(), std::string("time"));
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 3000));
  CHECK_EQ(g.dones()[0].result, std::string("Canceled"));
}

// ---------------------------------------------------------------- 3.6 timelines

TEST(timeline_grind_shot_report_then_polling_resumes) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  FakeGrinder g;
  bootWithGrinder(scale, g);
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));

  pressBrew();
  unsigned long shotStart = millis();
  REQUIRE(runUntil(outHigh, 5000));
  releaseBrew();
  REQUIRE(runUntil(outLow, 40000));
  unsigned long shotEnd = millis();
  int pollsDuringShot = 0;
  for (auto& r : g.received())
    if (r.operation == "brewJobs" && r.timeMs > shotStart && r.timeMs < shotEnd) pollsDuringShot++;
  CHECK(pollsDuringShot >= (int)((shotEnd - shotStart) / 2000) - 1);  // polling continues
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 6000));
  CHECK_EQ(g.dones()[0].result, std::string("Success"));
  int polls = g.count("brewJobs");
  runFor(4100);
  CHECK(g.count("brewJobs") >= polls + 2);
}

TEST(timeline_flush_then_real_shot_uses_the_same_job) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  FakeGrinder g;
  bootWithGrinder(scale, g);
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));

  pressBrew();  // a 1.5 s flush
  runFor(1500);
  releaseBrew();
  REQUIRE(runUntil([] { return lastShotEndReason() == "button"; }, 1000));
  runFor(5000);
  CHECK_EQ(g.count("brewDone"), 0);

  pullShotByWeight();
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 6000));
  CHECK_EQ(g.dones()[0].grindUuid, std::string("g1"));
  CHECK_EQ(g.dones()[0].result, std::string("Success"));
}

TEST(timeline_job_relisted_during_the_shot) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  FakeGrinder g;
  bootWithGrinder(scale, g);
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));
  unsigned long takenAt = g.received("brewAccept")[0].timeMs;
  runFor(35000);  // puck prep: the 50 s re-list falls inside the ~20 s shot
  pullShotByWeight();
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 10000));
  CHECK_EQ(g.dones()[0].result, std::string("Success"));
  auto accepts = g.received("brewAccept");
  REQUIRE(accepts.size() == 2);
  CHECK(accepts[1].timeMs - takenAt >= 50000);
  CHECK(accepts[1].timeMs - takenAt <= 52100);  // re-taken during the shot
  auto dones = g.received("brewDone");
  REQUIRE(dones.size() == 1);
  CHECK_EQ(dones[0].replyStatus, std::string("Ok"));
}

TEST(timeline_grinding_again_before_pulling) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  FakeGrinder g;
  bootWithGrinder(scale, g);
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));
  runFor(10000);
  g.addJob("g2");
  REQUIRE(runUntil([&] { return g.isTaken("g2"); }, 3000));
  pullShotByWeight();
  REQUIRE(runUntil([&] { return g.dones().size() == 2; }, 6000));
  CHECK_EQ(g.dones()[0].grindUuid, std::string("g1"));
  CHECK_EQ(g.dones()[0].result, std::string("Canceled"));
  CHECK_EQ(g.dones()[1].grindUuid, std::string("g2"));
  CHECK_EQ(g.dones()[1].result, std::string("Success"));
}

TEST(timeline_job_expires_without_a_shot) {
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  FakeGrinder g;
  bootWithGrinder(scale, g);
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 6UL * 60 * 1000));
  CHECK_EQ(g.dones()[0].result, std::string("Canceled"));
  runFor(1000);
  CHECK_EQ(FakePhone().readByte("0xFF27"), (int)GRINDER_READY);
}

TEST(timeline_status_seen_by_the_phone) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  FakeGrinder g;
  runLoopDuringRequests(g);  // lets loop() publish the status while a request is in flight
  preloadGrinder(g);
  fake::setWifiConnectsOnBegin(false);
  fakeble::addPeripheral(&scale);
  boot();
  runFor(2000);
  fake::setWifiStatus(WL_CONNECTED);
  REQUIRE(runUntil([] { return grinder.status() == GRINDER_READY; }, 5000));
  REQUIRE(runUntil([&] { return scale.connected(); }, 15000));
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));
  pullShotByWeight();
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 6000));
  runFor(500);
  // 0 is published before the client's first iteration processes the config.
  CHECK_EQ(FakePhone().byteChanges("0xFF27"), (std::vector<int>{0, 1, 4, 5, 6, 4}));
}

// ---------------------------------------------------------------- 3.7 isolation

TEST(no_grinder_request_is_made_from_loop) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  FakeGrinder g;
  bootWithGrinder(scale, g);
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));
  pullShotByWeight();
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 6000));
  REQUIRE(!fakehttp::requests().empty());
  for (auto& r : fakehttp::requests()) CHECK_EQ(r.context, std::string("grinder"));
}

TEST(hanging_grinder_requests_do_not_change_shot_timing) {
  // Every request times out after 8 s, during which loop() keeps running (as
  // on core 1). The shot must latch and stop exactly as in
  // test_shots.cpp's latching_shot_stops_by_weight, which runs without a grinder.
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  FakeGrinder g;
  g.unreachable = true;
  runLoopDuringRequests(g, 8000);
  preloadGrinder(g);
  bootConnected(scale);
  REQUIRE(runUntil([&] { return g.count("brewJobs") > 0; }, 5000));

  unsigned long pressedAt = millis();
  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));
  unsigned long latchedAt = outChangedAt(HIGH, pressedAt);
  CHECK(latchedAt - pressedAt > 3000);
  CHECK(latchedAt - pressedAt <= 3150);
  releaseBrew();
  REQUIRE(runUntil(outLow, 40000));
  unsigned long stoppedAt = outChangedAt(LOW, latchedAt);
  CHECK(stoppedAt - latchedAt >= 17250);
  CHECK(stoppedAt - latchedAt <= 17350);
  CHECK_EQ(lastShotEndReason(), std::string("weight"));
  CHECK(g.count("brewJobs") >= 1);
  for (auto& r : fakehttp::requests()) CHECK_EQ(r.context, std::string("grinder"));
}

// ---------------------------------------------------------------- debug log

TEST(grinder_debug_log_follows_the_shot) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  FakeGrinder g;
  bootWithGrinder(scale, g);
  CHECK(grinder.debug());  // GRINDER_DEBUG is on in the sketch
  g.addJob("g1");
  REQUIRE(runUntil([&] { return g.isTaken("g1"); }, 3000));
  pressBrew();
  runFor(1500);
  releaseBrew();
  runFor(1000);
  pullShotByWeight();
  REQUIRE(runUntil([&] { return g.dones().size() == 1; }, 6000));
  runFor(100);  // loop() publishes the new status on its next pass
  CHECK(fake::serialContains("loop] grinder config: ip=\"192.168.1.30\" pairingId=42"));
  CHECK(fake::serialContains("loop] shot started: grinder keeps the job taken until it's reported"));
  CHECK(fake::serialContains("loop] flush ("));
  CHECK(fake::serialContains("grinder job kept for the real shot"));
  CHECK(fake::serialContains("loop] shot stopped by weight after"));
  CHECK(fake::serialContains("loop] shot result to grinder: Success,"));
  CHECK(fake::serialContains("grinder] -> brewDone {"));
  CHECK(fake::serialContains("grinder] reported Success for job g1"));
  CHECK(fake::serialContains("loop] grinder status 5 -> 4 (0xFF27)"));
}

// ---------------------------------------------------------------- OTA and grinder WiFi

TEST(ota_while_grinder_wifi_is_still_connecting) {
  // Field bug: OTA's WiFi.begin() hit "sta is connecting, cannot set config"
  // because grinder sync had a connection attempt in progress.
  FakeGrinder g;
  preloadGrinder(g);
  fake::setWifiConnectsOnBegin(false);
  boot();
  runFor(5000);
  REQUIRE(fake::wifiBeginCount() == 1);  // grinder sync's attempt, still connecting
  FakePhone phone;
  REQUIRE(phone.writeByte("0xFF21", 1));
  runFor(500);
  CHECK_EQ(fake::wifiBeginRejectedCount(), 0);
  CHECK(fake::serialContains("Connecting to WiFi: HomeWifi"));
  fake::setWifiStatus(WL_CONNECTED);
  REQUIRE(runUntil([] { return fake::serialContains("Web Server Started"); }, 2000));
}

TEST(ota_reuses_wifi_already_connected_by_grinder_sync) {
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  FakeGrinder g;
  bootWithGrinder(scale, g);
  int begins = fake::wifiBeginCount();
  FakePhone phone;
  REQUIRE(phone.writeByte("0xFF21", 1));
  REQUIRE(runUntil([] { return fake::serialContains("Web Server Started"); }, 2000));
  CHECK(fake::serialContains("WiFi already connected"));
  CHECK_EQ(fake::wifiBeginCount(), begins);
  // Leaving OTA drops WiFi; grinder sync reconnects right away.
  REQUIRE(phone.writeByte("0xFF21", 0));
  REQUIRE(runUntil([] { return grinder.status() == GRINDER_READY; }, 5000));
  CHECK_EQ(fake::wifiBeginRejectedCount(), 0);
}
