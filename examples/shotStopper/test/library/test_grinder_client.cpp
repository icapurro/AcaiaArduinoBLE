// GrinderClient driven directly with events against FakeGrinder (no sketch).
#include <ArduinoJson.h>
#include <WiFi.h>

#include "../../GrinderClient.h"
#include "fake_grinder.h"
#include "test.h"

static GrinderClient client;
static std::vector<int> statusChanges;

// One task iteration, the way taskMain runs it (minus the real 20 ms sleep).
static void iterate() {
  {
    fakehttp::ScopedContext ctx("grinder");
    client.runOnce();
  }
  int s = client.status();
  if (statusChanges.empty() || statusChanges.back() != s) statusChanges.push_back(s);
  fake::advance(20);
}

static void runFor(unsigned long ms) {
  unsigned long end = millis() + ms;
  while (millis() < end) iterate();
}

static bool runUntil(const std::function<bool()>& cond, unsigned long maxMs) {
  unsigned long end = millis() + maxMs;
  while (!cond()) {
    if (millis() >= end) return false;
    iterate();
  }
  return true;
}

// Configured, active and on WiFi.
static void start(const char* ip = "192.168.1.30", uint32_t pairingId = 42) {
  fake::setWifiConnectsOnBegin(true);
  client.begin();
  client.configure(ip, pairingId, "HomeWifi", "secret");
  client.setActive(true);
}

static JsonDocument parse(const std::string& body) {
  JsonDocument doc;
  deserializeJson(doc, body);
  return doc;
}

static std::vector<unsigned long> times(const std::vector<FakeGrinder::Received>& rs) {
  std::vector<unsigned long> out;
  for (auto& r : rs) out.push_back(r.timeMs);
  return out;
}

// ---------------------------------------------------------------- 2.1 transport

TEST(client_request_shape_and_transport_settings) {
  FakeGrinder grinder;
  start();
  REQUIRE(runUntil([&] { return grinder.count("brewJobs") > 0; }, 1000));
  const fakehttp::Request& r = fakehttp::requests().at(0);
  CHECK_EQ(r.url, std::string("https://192.168.1.30:30001/grinderApi/brewJobs"));
  CHECK_EQ(r.body, std::string("{\"version\":1,\"pairingId\":42}"));
  CHECK_EQ(r.headers.at("Content-Type"), std::string("application/json"));
  CHECK(r.tlsInsecure);
  CHECK(r.tlsPreconnected);                   // TLS connected by us, with our handshake limit
  CHECK_EQ(r.tlsHandshakeTimeoutS, 8UL);      // the core's default is 120 s
  CHECK(!r.reuse);
  CHECK_EQ(r.timeoutMs, (uint16_t)8000);      // what's left of the 8 s budget
  CHECK_EQ(r.connectTimeoutMs, 8000);
  CHECK_EQ(r.followRedirects, HTTPC_DISABLE_FOLLOW_REDIRECTS);
  CHECK_EQ(r.context, std::string("grinder"));
}

TEST(client_validates_ipv4_addresses) {
  CHECK(GrinderClient::isValidIPv4("192.168.1.30"));
  CHECK(GrinderClient::isValidIPv4("0.0.0.0"));
  CHECK(GrinderClient::isValidIPv4("255.255.255.255"));
  for (const char* bad : {"", "192.168.1", "192.168.1.30.4", "256.1.1.1", "a.b.c.d", "1..2.3", "1.2.3.4 ", "1234.1.1.1",
                          "grinder.local"})
    CHECK(!GrinderClient::isValidIPv4(bad));
}

// ---------------------------------------------------------------- 2.2 config / WiFi

TEST(client_without_grinder_makes_no_wifi_or_requests) {
  client.begin();
  client.setActive(true);
  runFor(20000);
  CHECK_EQ(fake::wifiBeginCount(), 0);
  CHECK(fakehttp::requests().empty());
  CHECK_EQ(client.status(), (uint8_t)GRINDER_NOT_CONFIGURED);
}

TEST(client_connects_wifi_with_the_given_credentials) {
  FakeGrinder grinder;
  start();
  runFor(100);
  CHECK_EQ(fake::wifiBeginCount(), 1);
  CHECK(fake::wifiLastSsid() == "HomeWifi");
  CHECK_EQ(fake::wifiMode(), WIFI_STA);
}

TEST(client_reconnects_after_wifi_drop_then_polls_again) {
  FakeGrinder grinder;
  start();
  runFor(3000);
  int polls = grinder.count("brewJobs");
  REQUIRE(polls > 0);
  fake::setWifiConnectsOnBegin(false);
  fake::setWifiStatus(WL_DISCONNECTED);
  int begins = fake::wifiBeginCount();
  runFor(25000);
  CHECK_EQ(grinder.count("brewJobs"), polls);  // nothing while offline
  CHECK_EQ(client.status(), (uint8_t)GRINDER_CONNECTING_WIFI);
  CHECK_EQ(fake::wifiBeginCount(), begins);    // the core's auto-reconnect gets 30 s first
  CHECK(fake::serialContains("WiFi disconnected (disconnected), reconnecting"));
  runFor(10000);
  CHECK_EQ(fake::wifiBeginCount(), begins + 1);  // then a fresh attempt
  CHECK_EQ(fake::wifiBeginRejectedCount(), 0);
  fake::setWifiStatus(WL_CONNECTED);
  REQUIRE(runUntil([&] { return grinder.count("brewJobs") > polls; }, 3000));
  REQUIRE(runUntil([&] { return client.status() == GRINDER_READY; }, 1000));
}

TEST(client_inactive_sends_nothing_and_leaves_wifi_alone) {
  FakeGrinder grinder;
  start();
  runFor(3000);
  int requests = (int)fakehttp::requests().size();
  int begins = fake::wifiBeginCount();
  client.setActive(false);
  fake::setWifiStatus(WL_DISCONNECTED);  // e.g. OTA dropped WiFi
  runFor(30000);
  CHECK_EQ((int)fakehttp::requests().size(), requests);
  CHECK_EQ(fake::wifiBeginCount(), begins);
  client.setActive(true);
  REQUIRE(runUntil([&] { return (int)fakehttp::requests().size() > requests; }, 3000));
}

TEST(client_turned_off_by_empty_ip) {
  FakeGrinder grinder;
  start();
  runFor(3000);
  client.configure("", 42, "HomeWifi", "secret");
  runFor(100);
  int requests = (int)fakehttp::requests().size();
  runFor(10000);
  CHECK_EQ((int)fakehttp::requests().size(), requests);
  CHECK_EQ(client.status(), (uint8_t)GRINDER_NOT_CONFIGURED);
}

// ---------------------------------------------------------------- 2.3 polling / failures

TEST(client_polls_every_2s_when_idle) {
  FakeGrinder grinder;
  start();
  runFor(10100);
  auto t = times(grinder.received("brewJobs"));
  REQUIRE(t.size() == 6);
  for (size_t i = 1; i < t.size(); i++) {
    CHECK(t[i] - t[i - 1] >= 2000);
    CHECK(t[i] - t[i - 1] <= 2020);
  }
  CHECK_EQ(client.status(), (uint8_t)GRINDER_READY);
}

TEST(client_backs_off_when_grinder_unreachable) {
  FakeGrinder grinder;
  grinder.unreachable = true;
  start();
  runFor(250000);
  auto t = times(grinder.received("brewJobs"));
  REQUIRE(t.size() >= 8);
  unsigned long expected[] = {2000, 4000, 8000, 16000, 32000, 60000, 60000};
  for (size_t i = 0; i < 7; i++) {
    CHECK(t[i + 1] - t[i] >= expected[i]);
    CHECK(t[i + 1] - t[i] <= expected[i] + 20);
  }
  CHECK_EQ(client.status(), (uint8_t)GRINDER_UNREACHABLE);
}

TEST(client_backoff_resets_after_recovery) {
  FakeGrinder grinder;
  grinder.failNext(3, FakeGrinder::Fault::ConnectionError);
  start();
  REQUIRE(runUntil([&] { return grinder.count("brewJobs") == 4; }, 30000));  // 3 failures, then Ok
  CHECK_EQ(client.status(), (uint8_t)GRINDER_READY);
  runFor(4100);
  auto t = times(grinder.received("brewJobs"));
  REQUIRE(t.size() >= 6);
  CHECK(t[4] - t[3] <= 2020);
  CHECK(t[5] - t[4] <= 2020);
}

TEST(client_three_403s_mean_pairing_rejected) {
  FakeGrinder grinder("192.168.1.30", 7);  // the grinder doesn't know pairingId 42
  start();
  REQUIRE(runUntil([&] { return grinder.count("brewJobs") == 3; }, 10000));
  runFor(40);
  CHECK_EQ(client.status(), (uint8_t)GRINDER_PAIRING_REJECTED);
  unsigned long third = grinder.received("brewJobs")[2].timeMs;
  REQUIRE(runUntil([&] { return grinder.count("brewJobs") == 4; }, 70000));
  CHECK(grinder.received("brewJobs")[3].timeMs - third >= 60000);
}

TEST(client_tolerates_a_single_403) {
  FakeGrinder grinder;
  grinder.failNext(1, FakeGrinder::Fault::Forbidden, "brewJobs");
  start();
  runFor(5000);
  CHECK_EQ(grinder.count("brewJobs"), 3);
  for (int s : statusChanges) CHECK(s != GRINDER_PAIRING_REJECTED);
  CHECK_EQ(client.status(), (uint8_t)GRINDER_READY);
}

// ---------------------------------------------------------------- 2.4 taking jobs

TEST(client_takes_a_new_job) {
  FakeGrinder grinder;
  start();
  runFor(3000);
  FakeGrinder::Job job;
  job.grindUuid = "g1";
  job.doseTypeExpected = 2;
  grinder.addJob(job);
  REQUIRE(runUntil([&] { return grinder.isTaken("g1"); }, 3000));
  JsonDocument body = parse(grinder.received("brewAccept")[0].body);
  CHECK_EQ(std::string(body["grindUuid"] | ""), std::string("g1"));
  CHECK_EQ(body["groupHeadId"].as<int>(), 0);
  CHECK_EQ(body["doseType"].as<int>(), 2);
  CHECK_EQ(body["pairingId"].as<int>(), 42);
  runFor(40);
  CHECK_EQ(client.status(), (uint8_t)GRINDER_JOB_TAKEN);
}

TEST(client_echoes_group_head_and_missing_dose_type) {
  FakeGrinder grinder;
  FakeGrinder::Job job;
  job.grindUuid = "g1";
  job.hasGroupHead = true;
  job.groupHeadId = 1;
  job.hasDoseType = false;
  grinder.addJob(job);
  start();
  REQUIRE(runUntil([&] { return grinder.isTaken("g1"); }, 3000));
  JsonDocument body = parse(grinder.received("brewAccept")[0].body);
  CHECK_EQ(body["groupHeadId"].as<int>(), 1);
  CHECK(body["doseType"].isNull());
  CHECK(grinder.received("brewAccept")[0].body.find("\"doseType\":null") != std::string::npos);
}

TEST(client_takes_the_newest_of_several_new_jobs) {
  FakeGrinder grinder;
  grinder.addJob("old");
  grinder.addJob("new");
  start();
  REQUIRE(runUntil([&] { return grinder.count("brewAccept") > 0; }, 3000));
  runFor(5000);
  CHECK(grinder.isTaken("new"));
  CHECK(!grinder.isTaken("old"));  // left for the grinder to drop
  CHECK_EQ(grinder.count("brewAccept"), 1);
}

TEST(client_retakes_a_job_listed_again) {
  FakeGrinder grinder;
  grinder.addJob("g1");
  start();
  REQUIRE(runUntil([&] { return grinder.isTaken("g1"); }, 3000));
  runFor(53000);
  CHECK_EQ(grinder.count("brewAccept"), 2);
  CHECK(grinder.isTaken("g1"));
}

TEST(client_cancels_previous_job_when_grinding_again) {
  FakeGrinder grinder;
  grinder.addJob("g1");
  start();
  REQUIRE(runUntil([&] { return grinder.isTaken("g1"); }, 3000));
  runFor(5000);
  grinder.addJob("g2");
  REQUIRE(runUntil([&] { return grinder.isTaken("g2"); }, 3000));
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  CHECK_EQ(grinder.dones()[0].grindUuid, std::string("g1"));
  CHECK_EQ(grinder.dones()[0].result, std::string("Canceled"));
  CHECK_EQ(grinder.dones()[0].durationFinalMs, 0L);
  CHECK_EQ(grinder.dones()[0].massFinalMg, 0L);
}

TEST(client_cancels_a_job_unused_for_5_minutes) {
  FakeGrinder grinder;
  grinder.addJob("g1");
  start();
  REQUIRE(runUntil([&] { return grinder.isTaken("g1"); }, 3000));
  unsigned long takenAt = grinder.received("brewAccept")[0].timeMs;
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 6UL * 60 * 1000));
  CHECK_EQ(grinder.dones()[0].result, std::string("Canceled"));
  unsigned long doneAt = grinder.dones()[0].timeMs;
  CHECK(doneAt - takenAt > 5UL * 60 * 1000);
  CHECK(doneAt - takenAt < 5UL * 60 * 1000 + 3000);
  runFor(3000);
  CHECK_EQ(client.status(), (uint8_t)GRINDER_READY);
}

// ---------------------------------------------------------------- 2.5 shots and reports

static void takeJob(FakeGrinder& grinder, const std::string& uuid = "g1") {
  grinder.addJob(uuid);
  REQUIRE(runUntil([&] { return grinder.isTaken(uuid); }, 3000));
  runFor(100);
}

TEST(client_keeps_polling_during_a_shot_but_takes_no_new_job) {
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  client.shotStarted();
  runFor(100);
  int polls = grinder.count("brewJobs");
  grinder.addJob("g2");  // someone grinds during the shot
  runFor(30000);
  CHECK(grinder.count("brewJobs") >= polls + 14);  // still every 2 s
  CHECK(!grinder.isTaken("g2"));                    // not this shot's job
  CHECK_EQ(client.status(), (uint8_t)GRINDER_JOB_TAKEN);
  client.shotFinished(ShotOutcome::Success, 30000, 36000);
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  CHECK_EQ(grinder.dones()[0].grindUuid, std::string("g1"));
  REQUIRE(runUntil([&] { return grinder.isTaken("g2"); }, 3000));  // taken once the shot is settled
}

TEST(client_reports_a_successful_shot) {
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  client.shotStarted();
  runFor(28000);
  client.shotFinished(ShotOutcome::Success, 28400, 37200);
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  const auto& d = grinder.dones()[0];
  CHECK_EQ(d.grindUuid, std::string("g1"));
  CHECK_EQ(d.durationFinalMs, 28400L);
  CHECK_EQ(d.massFinalMg, 37200L);
  CHECK_EQ(d.result, std::string("Success"));
  CHECK_EQ(d.brewEventUuid.size(), (size_t)36);
  CHECK_EQ(d.brewEventUuid[14], '4');  // UUID v4
  int polls = grinder.count("brewJobs");
  runFor(2100);
  CHECK(grinder.count("brewJobs") > polls);  // polling resumed
  CHECK_EQ(client.status(), (uint8_t)GRINDER_READY);
}

TEST(client_caps_reported_mass) {
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  client.shotStarted();
  client.shotFinished(ShotOutcome::Success, 25000, 9000000);
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  CHECK_EQ(grinder.dones()[0].massFinalMg, 5000000L);
}

TEST(client_flush_keeps_the_job_for_the_real_shot) {
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  client.shotStarted();
  runFor(2000);
  client.shotFinished(ShotOutcome::Flush);
  runFor(3000);
  CHECK(grinder.dones().empty());
  CHECK(grinder.count("brewJobs") > 0);
  client.shotStarted();
  runFor(25000);
  client.shotFinished(ShotOutcome::Success, 25000, 36000);
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  CHECK_EQ(grinder.dones()[0].grindUuid, std::string("g1"));
  CHECK_EQ(grinder.dones()[0].result, std::string("Success"));
}

TEST(client_cancels_a_shot_that_was_not_stopped_by_weight) {
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  client.shotStarted();
  runFor(10000);
  client.shotFinished(ShotOutcome::Canceled);
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  CHECK_EQ(grinder.dones()[0].result, std::string("Canceled"));
  CHECK_EQ(grinder.dones()[0].durationFinalMs, 0L);
}

TEST(client_shot_without_a_job_sends_nothing) {
  FakeGrinder grinder;
  start();
  runFor(3000);
  client.shotStarted();
  runFor(25000);
  client.shotFinished(ShotOutcome::Success, 25000, 36000);
  runFor(5000);
  CHECK_EQ(grinder.count("brewDone"), 0);
  CHECK(grinder.count("brewJobs") > 2);
}

TEST(client_retakes_a_job_listed_again_during_a_shot) {
  // The grinder re-lists a taken job 50 s after the last brewAccept, shot or
  // not. Seen by polling during the shot, it is taken again in time.
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  unsigned long takenAt = grinder.received("brewAccept")[0].timeMs;
  runFor(25000);  // puck prep: 25 s of the window used before the shot
  client.shotStarted();
  runFor(28000);  // the shot crosses the 50 s mark
  REQUIRE(grinder.count("brewAccept") == 2);
  unsigned long retakenAt = grinder.received("brewAccept")[1].timeMs;
  CHECK(retakenAt - takenAt >= 50000);
  CHECK(retakenAt - takenAt <= 52100);  // at the next poll after the re-list
  client.shotFinished(ShotOutcome::Success, 28000, 40000);
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  auto dones = grinder.received("brewDone");
  REQUIRE(dones.size() == 1);
  CHECK_EQ(dones[0].replyStatus, std::string("Ok"));  // no recovery needed
}

TEST(client_recovers_when_the_re_list_was_missed) {
  // Polls fail during the shot (e.g. a WiFi blip), so the re-list goes
  // unnoticed: brewDone is refused, the job is taken again straight away, and
  // the same brewDone is resent.
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  runFor(25000);
  client.shotStarted();
  grinder.failNext(1000, FakeGrinder::Fault::ConnectionError, "brewJobs");
  runFor(28000);
  client.shotFinished(ShotOutcome::Success, 28000, 40000);
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  auto dones = grinder.received("brewDone");
  REQUIRE(dones.size() == 2);
  CHECK_EQ(dones[0].replyStatus, std::string("ClientErrorOngoingJobNotExisting"));
  CHECK_EQ(dones[1].replyStatus, std::string("Ok"));
  CHECK_EQ(dones[0].body, dones[1].body);  // same brewEventUuid and values
  auto accepts = grinder.received("brewAccept");
  REQUIRE(accepts.size() == 2);
  CHECK(accepts[1].timeMs > dones[0].timeMs);
  CHECK(dones[1].timeMs - dones[0].timeMs <= 100);  // straight away, despite the poll backoff
}

TEST(client_does_not_retake_a_job_that_is_still_taken) {
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  client.shotStarted();
  runFor(20000);  // inside the 50 s window
  client.shotFinished(ShotOutcome::Success, 20000, 36000);
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  CHECK_EQ(grinder.count("brewAccept"), 1);
  for (auto& r : grinder.received()) CHECK(r.replyStatus != "ClientErrorBrewJobNotExisting");
}

TEST(client_retries_a_report_with_the_same_uuid) {
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  client.shotStarted();
  client.shotFinished(ShotOutcome::Success, 27000, 38000);
  grinder.failNext(2, FakeGrinder::Fault::ConnectionError, "brewDone");
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 30000));
  auto dones = grinder.received("brewDone");
  REQUIRE(dones.size() == 3);
  for (auto& d : dones) CHECK_EQ(d.body, dones[0].body);
  CHECK(dones[1].timeMs - dones[0].timeMs >= 2000);
  CHECK(dones[2].timeMs - dones[1].timeMs >= 4000);
  int polls = grinder.count("brewJobs");
  runFor(2100);
  CHECK(grinder.count("brewJobs") > polls);
}

TEST(client_abandons_a_report_after_its_window) {
  FakeGrinder grinder;
  start();
  takeJob(grinder);
  client.shotStarted();
  client.shotFinished(ShotOutcome::Success, 27000, 38000);
  grinder.unreachable = true;
  runFor(125000);
  grinder.unreachable = false;
  CHECK(grinder.dones().empty());
  int dones = grinder.count("brewDone");
  REQUIRE(runUntil([&] { return grinder.received().back().operation == "brewJobs" &&
                                grinder.received().back().replyStatus == "Ok"; },
                   70000));
  CHECK_EQ(grinder.count("brewDone"), dones);  // no more report attempts
}

// ---------------------------------------------------------------- 2.6 status

TEST(client_status_sequence_through_a_shot) {
  FakeGrinder grinder;
  // Also sample the status while each request is in flight, as the phone
  // would see it during a slow TLS request on the device.
  grinder.onRequest = [](const std::string&) {
    int s = client.status();
    if (statusChanges.empty() || statusChanges.back() != s) statusChanges.push_back(s);
  };
  statusChanges.clear();
  iterate();
  CHECK_EQ(statusChanges, (std::vector<int>{GRINDER_NOT_CONFIGURED}));
  statusChanges.clear();
  start();
  fake::setWifiConnectsOnBegin(false);  // WiFi takes a moment to join
  runFor(1000);
  fake::setWifiStatus(WL_CONNECTED);
  runFor(3000);
  takeJob(grinder);
  client.shotStarted();
  runFor(25000);
  client.shotFinished(ShotOutcome::Success, 25000, 36000);
  REQUIRE(runUntil([&] { return grinder.dones().size() == 1; }, 3000));
  runFor(100);
  CHECK_EQ(statusChanges, (std::vector<int>{1, 4, 5, 6, 4}));
  CHECK(fake::serialContains("free heap"));
}

// ---------------------------------------------------------------- debug log

TEST(client_debug_log_shows_requests_replies_and_decisions) {
  FakeGrinder grinder;
  client.setDebug(true);
  grinder.failNext(1, FakeGrinder::Fault::Timeout, "brewJobs");
  start();
  runFor(2500);
  grinder.addJob("g1");
  REQUIRE(runUntil([&] { return grinder.isTaken("g1"); }, 3000));
  const std::string& out = fake::serialOutput();
  CHECK(fake::serialContains("grinder] event: config ip=\"192.168.1.30\" pairingId=42 ssid=\"HomeWifi\""));
  CHECK(fake::serialContains("grinder] WiFi connected, IP 192.168.1.50"));
  // Failed polls are logged...
  CHECK(fake::serialContains("grinder] -> brewJobs {\"version\":1,\"pairingId\":42}"));
  CHECK(fake::serialContains("grinder] <- brewJobs HTTP -11 read Timeout in 0 ms (TLS connect 0 ms), RSSI -55 dBm"));
  // ...empty successful ones are not...
  CHECK(!fake::serialContains("{\"brewJobs\":[],"));
  // ...and ones that bring a job back are.
  CHECK(fake::serialContains("grinder] <- brewJobs HTTP 200 Ok in 0 ms (TLS connect 0 ms), RSSI -55 dBm, heap 200000 {\"brewJobs\":[{\"grindUuid\":\"g1\""));
  CHECK(fake::serialContains("grinder] new grind g1 (groupHead 0, doseType 2), taking it"));
  CHECK(fake::serialContains("grinder] took job g1"));
  // One "[timestamp source]" prefix per line.
  size_t lines = 0, prefixes = 0;
  for (size_t i = 0; i < out.size(); i++) {
    if (out[i] == '\n') lines++;
    if (out[i] == '[' && (i == 0 || out[i - 1] == '\n')) prefixes++;
  }
  CHECK_EQ(prefixes, lines);
}

TEST(client_debug_off_logs_only_milestones) {
  FakeGrinder grinder;
  start();
  grinder.addJob("g1");
  REQUIRE(runUntil([&] { return grinder.isTaken("g1"); }, 3000));
  CHECK(fake::serialContains("grinder] took job g1"));
  CHECK(!fake::serialContains("-> brewJobs"));
  CHECK(!fake::serialContains("event:"));
}

TEST(client_debug_log_shows_brewjobs_cadence) {
  FakeGrinder grinder;
  client.setDebug(true);
  start();
  runFor(62100);
  CHECK(!fake::serialContains("-> brewJobs"));  // empty polls on schedule stay quiet
  CHECK(!fake::serialContains("after it was due"));
  CHECK(fake::serialContains("brewJobs cadence: 30 polls in the last 60 s, every 2000-2000 ms (avg 2000 ms), 0 sent late (0 right after a request slower than 2000 ms)"));
}

TEST(client_debug_log_flags_late_polls_but_not_backoff) {
  FakeGrinder grinder;
  client.setDebug(true);
  grinder.failNext(2, FakeGrinder::Fault::Timeout, "brewJobs");
  start();
  runFor(9000);  // fail, +2 s fail, +4 s ok: stretched by backoff, not late
  CHECK(!fake::serialContains("after it was due"));
  // A job delays the next poll while it is being taken: still on time.
  grinder.addJob("g1");
  REQUIRE(runUntil([&] { return grinder.isTaken("g1"); }, 3000));
  runFor(4100);
  CHECK(!fake::serialContains("after it was due"));
  // A shot pauses polling: the first poll afterwards starts a new measurement.
  client.shotStarted();
  runFor(10000);
  client.shotFinished(ShotOutcome::Flush);
  runFor(100);
  CHECK(fake::serialContains("event: shot finished Flush"));
  // The gap across the shot pause is not reported as late.
  CHECK(!fake::serialContains("poll late"));
}

TEST(client_debug_log_flags_a_late_poll) {
  FakeGrinder grinder;
  client.setDebug(true);
  start();
  runFor(2100);       // polls at 0 and 2000
  fake::advance(3000);  // the task doesn't get to run for 3 s
  runFor(100);
  CHECK(fake::serialContains("grinder] brewJobs poll late (+3100 ms since previous poll; sent 1100 ms after it was due, previous request took 0 ms)"));
  CHECK(!fake::serialContains("-> brewJobs"));
}

// ---------------------------------------------------------------- WiFi attempts

TEST(client_gives_a_wifi_attempt_30s_and_says_why_it_failed) {
  FakeGrinder grinder;
  client.begin();
  client.configure("192.168.1.30", 42, "HomeWifi", "secret");
  client.setActive(true);
  runFor(100);
  CHECK_EQ(fake::wifiBeginCount(), 1);
  CHECK(fake::wifiAutoReconnect());
  fake::setWifiStatus(WL_NO_SSID_AVAIL);
  runFor(29000);
  CHECK_EQ(fake::wifiBeginCount(), 1);  // no begin() while the attempt is in progress
  runFor(2000);
  CHECK_EQ(fake::wifiBeginCount(), 2);
  CHECK_EQ(fake::wifiBeginRejectedCount(), 0);
  CHECK(fake::serialContains("WiFi still not connected after 30 s (network not found (the ESP32 only sees 2.4 GHz)), retrying"));
}

TEST(client_logs_a_failed_tls_connect) {
  client.setDebug(true);
  start("10.0.0.99");  // nothing answers there
  runFor(100);
  CHECK(fake::serialContains("grinder] <- brewJobs TLS connect failed after 0 ms, RSSI -55 dBm"));
  CHECK_EQ(client.status(), (uint8_t)GRINDER_UNREACHABLE);
  CHECK(fakehttp::requests().empty());  // never got as far as HTTP
}

TEST(client_debug_log_attributes_late_polls_to_a_slow_request) {
  // The task is stuck in a 3.3 s request (like a slow TLS handshake on the
  // board): the next poll goes out late, and the log says why.
  FakeGrinder grinder;
  client.setDebug(true);
  start();
  runFor(2100);  // polls at 0 and 2000
  grinder.onRequest = [](const std::string&) { fake::advance(3300); };
  runFor(2000);  // the poll due at 4000 takes 3.3 s
  grinder.onRequest = nullptr;
  runFor(1000);
  CHECK(fake::serialContains("(+3320 ms since previous poll; sent 1320 ms after it was due, previous request took 3300 ms)"));
}
