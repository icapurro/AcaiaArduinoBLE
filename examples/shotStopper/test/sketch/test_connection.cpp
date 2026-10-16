// Scale connection lifecycle and BLE start-up failures.
#include "scenario.h"

TEST(scale_timeout_reports_disconnected_then_reconnects) {
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  bootConnected(scale);
  FakePhone phone;
  runFor(1000);
  REQUIRE(phone.readByte("0xFF19") == 1);

  scale.silent = true;  // scale stops sending weights
  REQUIRE(runUntil([] { return fake::serialContains("timeout!"); }, 7000));
  scale.silent = false;
  REQUIRE(runUntil([&] { return scale.connectCount() == 2 && scale.isSubscribed(); }, 15000));
  runFor(1000);

  CHECK_EQ(phone.byteChanges("0xFF19"), (std::vector<int>{0, 1, 0, 1}));
  CHECK(scale.connected());
}

TEST(scale_out_of_range_at_boot_is_found_later) {
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  scale.visible = false;
  fakeble::addPeripheral(&scale);
  boot();
  runFor(25000);  // a couple of 10 s scans with nothing found
  CHECK(fakeble::scanCount() >= 2);
  CHECK(fake::serialContains("failed to find scale"));
  CHECK_EQ(scale.connectCount(), 0);

  scale.visible = true;
  REQUIRE(runUntil([&] { return scale.connected(); }, 15000));
  FakePhone phone;
  runFor(100);
  CHECK_EQ(phone.readByte("0xFF19"), 1);
}

TEST(ble_init_failure_is_retried_every_3s) {
  fakeble::setBeginResults({false, false, true});
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long) { return 0.0f; });
  fakeble::addPeripheral(&scale);

  boot();
  CHECK(!bleReady);
  CHECK(fake::serialContains("BLE begin failed"));

  runFor(5000);  // second attempt fails too
  CHECK_EQ(fakeble::beginCallTimes().size(), (size_t)2);
  CHECK_EQ(fakeble::scanCount(), 0);  // no scale scans while BLE is down
  CHECK_EQ(scale.connectCount(), 0);
  CHECK(fake::serialContains("Retrying BLE init..."));

  REQUIRE(runUntil([&] { return scale.connected(); }, 10000));
  CHECK(bleReady);
  const auto& times = fakeble::beginCallTimes();
  REQUIRE(times.size() == 3);
  CHECK(times[1] - times[0] > 3000);
  CHECK(times[1] - times[0] <= 3010);
  CHECK(times[2] - times[1] > 3000);
  CHECK(times[2] - times[1] <= 3010);
  CHECK_EQ(fakeble::advertiseCount(), 1);  // config service comes up with it
}
