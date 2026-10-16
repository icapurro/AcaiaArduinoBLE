// Tests for the sketch's copy of the scale library (../../AcaiaArduinoBLE.cpp)
// against the fake BLE stack and FakeScale.
#include <memory>

#include "../../AcaiaArduinoBLE.h"
#include "fake_scale.h"
#include "test.h"

// Static storage on purpose: the constructor leaves _lastHeartBeat
// uninitialised, so it relies on zero-initialisation like the sketch's global.
static AcaiaArduinoBLE scale(false);

static bool connectTo(FakeScale& fake) {
  fakeble::addPeripheral(&fake);
  BLE.begin();
  return scale.init();
}

static float deliver(FakeScale& fake, const Bytes& packet, bool* accepted = nullptr) {
  fake.queuePacket(packet);
  bool ok = scale.newWeightAvailable();
  if (accepted) *accepted = ok;
  return scale.getWeight();
}

// ---------------------------------------------------------------- 4.1 connect sequence

static void checkConnectSequence(ScaleType type) {
  FakeScale fake(type);
  REQUIRE(connectTo(fake));
  CHECK(scale.isConnected());
  std::string r = FakeScale::readUuid(type), w = FakeScale::writeUuid(type);
  CHECK_EQ(fake.events(), (std::vector<std::string>{"connect", "discover", "subscribe " + r, "write " + w, "write " + w}));
  CHECK_EQ(fake.writtenBytes(), (std::vector<Bytes>{acaia::IDENTIFY, acaia::NOTIFICATION_REQUEST}));
}

TEST(connect_sequence_old_lunar) { checkConnectSequence(ScaleType::OLD); }
TEST(connect_sequence_new_lunar_pyxis) { checkConnectSequence(ScaleType::NEW); }
TEST(connect_sequence_generic) { checkConnectSequence(ScaleType::GENERIC); }

TEST(connect_by_mac_address_only_finds_that_scale) {
  FakeScale other(ScaleType::NEW, "PYXIS-OTHER", "aa:aa:aa:aa:aa:aa");
  FakeScale mine(ScaleType::NEW, "PYXIS-MINE", "bb:bb:bb:bb:bb:bb");
  fakeble::addPeripheral(&other);
  fakeble::addPeripheral(&mine);
  BLE.begin();
  REQUIRE(scale.init("BB:BB:BB:BB:BB:BB"));
  CHECK(mine.connected());
  CHECK_EQ(other.connectCount(), 0);
}

// ---------------------------------------------------------------- 4.2 type detection / names

TEST(detects_scale_type_from_characteristics) {
  struct Case { ScaleType type; const char* message; };
  for (Case c : {Case{ScaleType::OLD, "Old version Acaia Detected"}, Case{ScaleType::NEW, "New version Acaia Detected"},
                 Case{ScaleType::GENERIC, "Generic Scale Detected"}}) {
    fake::clearSerial();
    auto fake = std::make_unique<FakeScale>(c.type);
    REQUIRE(connectTo(*fake));
    CHECK(fake::serialContains(c.message));
    fake->visible = false;  // keep it out of the next scan
    scale.disconnect();
    fake.release();  // still registered with the fake stack
  }
}

TEST(scale_without_known_characteristics_is_rejected) {
  FakeScale fake(ScaleType::NONE);
  CHECK(!connectTo(fake));
  CHECK(fake::serialContains("unable to determine scale type"));
  CHECK(!scale.isConnected());
}

TEST(accepted_scale_name_prefixes) {
  for (const char* name : {"CINCO-1", "ACAIA-1", "PYXIS-1", "LUNAR-1", "PEARL-1", "PROCH-1", "BOOKO-1"}) {
    auto fake = std::make_unique<FakeScale>(ScaleType::NEW, name);
    CHECK(connectTo(*fake));
    CHECK_EQ(fake->connectCount(), 1);
    fake->visible = false;
    scale.disconnect();
    fake.release();
  }
}

TEST(rejected_scale_names_including_felicita) {
  // Current behavior: isScaleName() has no Felicita prefix, so an Arc
  // advertising "FELICITA..." is never connected even though the GENERIC
  // protocol is meant for it.
  FakeScale felicita(ScaleType::GENERIC, "FELICITA ARC");
  FakeScale lowercase(ScaleType::NEW, "pyxis-1");
  FakeScale phone(ScaleType::NEW, "iPhone");
  fakeble::addPeripheral(&felicita);
  fakeble::addPeripheral(&lowercase);
  fakeble::addPeripheral(&phone);
  BLE.begin();
  CHECK(!scale.init());
  CHECK_EQ(felicita.connectCount() + lowercase.connectCount() + phone.connectCount(), 0);
}

// ---------------------------------------------------------------- 4.3 failure paths

TEST(no_scale_times_out_after_ten_seconds_of_simulated_time) {
  BLE.begin();
  CHECK(!scale.init());
  CHECK(millis() >= 10000UL);
  CHECK(millis() < 10100UL);
  CHECK(fake::serialContains("failed to find scale"));
}

TEST(connect_failure) {
  FakeScale fake(ScaleType::NEW);
  fake.failConnect = true;
  CHECK(!connectTo(fake));
  CHECK(fake::serialContains("Failed to connect!"));
  CHECK(!scale.isConnected());
}

TEST(discovery_failure_disconnects) {
  FakeScale fake(ScaleType::NEW);
  fake.failDiscover = true;
  CHECK(!connectTo(fake));
  CHECK(fake::serialContains("Attribute discovery failed!"));
  CHECK(!fake.connected());
  CHECK(!scale.isConnected());
}

TEST(subscribe_failure) {
  FakeScale fake(ScaleType::NEW);
  fake.failSubscribe = true;
  CHECK(!connectTo(fake));
  CHECK(fake::serialContains("subscription failed"));
  CHECK(!scale.isConnected());
}

TEST(identify_write_failure) {
  FakeScale fake(ScaleType::NEW);
  fake.failWrites = true;
  CHECK(!connectTo(fake));
  CHECK(fake::serialContains("identify write failed"));
  CHECK(!scale.isConnected());
}

TEST(failed_command_write_marks_scale_disconnected) {
  FakeScale fake(ScaleType::NEW);
  REQUIRE(connectTo(fake));
  bool (AcaiaArduinoBLE::*commands[])() = {&AcaiaArduinoBLE::tare, &AcaiaArduinoBLE::startTimer,
                                           &AcaiaArduinoBLE::stopTimer, &AcaiaArduinoBLE::resetTimer,
                                           &AcaiaArduinoBLE::heartbeat};
  for (auto command : commands) {
    fake.failWrites = false;
    scale.disconnect();
    REQUIRE(scale.init());
    REQUIRE(scale.isConnected());
    fake.failWrites = true;
    CHECK(!(scale.*command)());
    CHECK(!scale.isConnected());
  }
}

TEST(disconnect_drops_the_link_but_isConnected_stays_true) {
  // Current behavior: disconnect() doesn't clear _connected, so callers keep
  // seeing a connected scale until the 5 s packet timeout notices.
  FakeScale fake(ScaleType::NEW);
  REQUIRE(connectTo(fake));
  scale.disconnect();
  CHECK(!fake.connected());
  CHECK(scale.isConnected());
}

// ---------------------------------------------------------------- 4.4 command bytes

static void checkCommands(ScaleType type, const Bytes& tare, const Bytes& start, const Bytes& stop,
                          const Bytes& reset) {
  FakeScale fake(type);
  REQUIRE(connectTo(fake));
  CHECK(scale.tare());
  CHECK(scale.startTimer());
  CHECK(scale.stopTimer());
  CHECK(scale.resetTimer());
  CHECK(scale.heartbeat());
  auto writes = fake.writtenBytes();
  REQUIRE(writes.size() == 7);
  CHECK_EQ(writes[2], tare);
  CHECK_EQ(writes[3], start);
  CHECK_EQ(writes[4], stop);
  CHECK_EQ(writes[5], reset);
  CHECK_EQ(writes[6], acaia::HEARTBEAT);
}

TEST(command_bytes_old_lunar) {
  checkCommands(ScaleType::OLD, acaia::TARE, acaia::START_TIMER, acaia::STOP_TIMER, acaia::RESET_TIMER);
}
TEST(command_bytes_new_lunar_pyxis) {
  checkCommands(ScaleType::NEW, acaia::TARE, acaia::START_TIMER, acaia::STOP_TIMER, acaia::RESET_TIMER);
}
TEST(command_bytes_generic) {
  checkCommands(ScaleType::GENERIC, acaia::TARE_GENERIC, acaia::START_TIMER_GENERIC, acaia::STOP_TIMER_GENERIC,
                acaia::RESET_TIMER_GENERIC);
}

// ---------------------------------------------------------------- 4.5 heartbeat / timeout

static void checkHeartbeatTiming(ScaleType type) {
  FakeScale fake(type);
  REQUIRE(connectTo(fake));
  REQUIRE(scale.heartbeat());
  fake::advance(2750);
  CHECK(!scale.heartbeatRequired());
  fake::advance(1);
  CHECK(scale.heartbeatRequired());
  REQUIRE(scale.heartbeat());
  CHECK(!scale.heartbeatRequired());
}

TEST(heartbeat_required_after_2750ms_old_lunar) { checkHeartbeatTiming(ScaleType::OLD); }
TEST(heartbeat_required_after_2750ms_new_lunar_pyxis) { checkHeartbeatTiming(ScaleType::NEW); }

TEST(heartbeat_never_required_for_generic) {
  FakeScale fake(ScaleType::GENERIC);
  REQUIRE(connectTo(fake));
  fake::advance(60000);
  CHECK(!scale.heartbeatRequired());
}

TEST(packet_timeout_disconnects_after_5000ms) {
  FakeScale fake(ScaleType::NEW);
  // The library treats _lastPacket == 0 as "no packet yet", so a packet at
  // exactly millis() == 0 would never time out. A real board has been up for
  // a while by the time the scale connects.
  fake::advance(1000);
  REQUIRE(connectTo(fake));
  bool accepted = false;
  deliver(fake, FakeScale::encodeWeight(ScaleType::NEW, 1.0f), &accepted);
  REQUIRE(accepted);
  fake::advance(5000);
  CHECK(!scale.newWeightAvailable());
  CHECK(scale.isConnected());
  fake::advance(1);
  CHECK(!scale.newWeightAvailable());
  CHECK(!scale.isConnected());
  CHECK(!fake.connected());
  CHECK(fake::serialContains("timeout!"));
}

TEST(no_timeout_before_the_first_packet) {
  FakeScale fake(ScaleType::NEW);
  REQUIRE(connectTo(fake));
  fake::advance(60000);
  CHECK(!scale.newWeightAvailable());
  CHECK(scale.isConnected());
}

// ---------------------------------------------------------------- 4.6 weight decoding

static Bytes newPacket(uint16_t raw, uint8_t decimals, bool negative, size_t length = 13, uint8_t kind = 0x05) {
  Bytes p(length, 0);
  p[0] = 0xef; p[1] = 0xdd; p[4] = kind;
  p[5] = raw & 0xff; p[6] = raw >> 8;
  p[9] = decimals;
  p[10] = negative ? 0x02 : 0x00;
  return p;
}

static Bytes oldPacket(uint16_t raw, uint8_t decimals, bool negative, size_t length = 10) {
  Bytes p(length, 0);
  p[0] = 0xef; p[1] = 0xdd;
  p[2] = raw & 0xff; p[3] = raw >> 8;
  p[6] = decimals;
  p[7] = negative ? 0x02 : 0x00;
  return p;
}

static Bytes genericPacket(uint32_t hundredths, bool negative, size_t length = 20) {
  Bytes p(length, 0);
  p[6] = negative ? '-' : '+';
  p[7] = (hundredths >> 16) & 0xff; p[8] = (hundredths >> 8) & 0xff; p[9] = hundredths & 0xff;
  return p;
}

TEST(decodes_new_protocol_packets) {
  FakeScale fake(ScaleType::NEW);
  REQUIRE(connectTo(fake));
  bool ok = false;
  CHECK_NEAR(deliver(fake, newPacket(1234, 1, false), &ok), 123.4, 1e-4);
  CHECK(ok);
  CHECK_NEAR(deliver(fake, newPacket(1234, 1, true), &ok), -123.4, 1e-4);
  CHECK(ok);
  CHECK_NEAR(deliver(fake, newPacket(1234, 2, false), &ok), 12.34, 1e-4);
  CHECK(ok);
  CHECK_NEAR(deliver(fake, newPacket(365, 1, false, 17), &ok), 36.5, 1e-4);  // newer firmware, 17 bytes
  CHECK(ok);
}

TEST(ignores_unsupported_new_protocol_packets) {
  FakeScale fake(ScaleType::NEW);
  REQUIRE(connectTo(fake));
  bool ok = true;
  deliver(fake, newPacket(100, 1, false), &ok);
  REQUIRE(ok);
  CHECK_NEAR(deliver(fake, newPacket(999, 1, false, 13, 0x04), &ok), 10.0, 1e-4);  // not a weight message
  CHECK(!ok);
  CHECK_NEAR(deliver(fake, newPacket(999, 1, false, 20), &ok), 10.0, 1e-4);  // wrong length
  CHECK(!ok);
  CHECK_NEAR(deliver(fake, newPacket(999, 1, false, 15), &ok), 10.0, 1e-4);
  CHECK(!ok);
}

TEST(decodes_old_protocol_packets) {
  FakeScale fake(ScaleType::OLD);
  REQUIRE(connectTo(fake));
  bool ok = false;
  CHECK_NEAR(deliver(fake, oldPacket(1234, 1, false), &ok), 123.4, 1e-4);
  CHECK(ok);
  CHECK_NEAR(deliver(fake, oldPacket(25, 1, true), &ok), -2.5, 1e-4);
  CHECK(ok);
  CHECK_NEAR(deliver(fake, oldPacket(360, 1, false, 14), &ok), 36.0, 1e-4);  // Lunar 2021 AL008
  CHECK(ok);
  CHECK_NEAR(deliver(fake, oldPacket(999, 1, false, 13), &ok), 36.0, 1e-4);
  CHECK(!ok);
}

TEST(decodes_generic_packets) {
  FakeScale fake(ScaleType::GENERIC);
  REQUIRE(connectTo(fake));
  bool ok = false;
  CHECK_NEAR(deliver(fake, genericPacket(12345, false), &ok), 123.45, 1e-3);
  CHECK(ok);
  CHECK_NEAR(deliver(fake, genericPacket(250, true), &ok), -2.5, 1e-4);
  CHECK(ok);
  CHECK_NEAR(deliver(fake, genericPacket(999, false, 13), &ok), -2.5, 1e-4);
  CHECK(!ok);
}

static void checkRoundTrip(ScaleType type, double tolerance) {
  FakeScale fake(type);
  REQUIRE(connectTo(fake));
  for (float grams : {0.0f, 0.1f, 18.5f, 36.0f, 512.3f, -2.5f}) {
    bool ok = false;
    CHECK_NEAR(deliver(fake, FakeScale::encodeWeight(type, grams), &ok), grams, tolerance);
    CHECK(ok);
  }
}

TEST(fake_scale_packets_round_trip_through_decoder_old) { checkRoundTrip(ScaleType::OLD, 0.051); }
TEST(fake_scale_packets_round_trip_through_decoder_new) { checkRoundTrip(ScaleType::NEW, 0.051); }
TEST(fake_scale_packets_round_trip_through_decoder_generic) { checkRoundTrip(ScaleType::GENERIC, 0.0051); }
