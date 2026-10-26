// Self-tests for FakeGrinder, driven by raw requests through the fake HTTPClient.
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "fake_grinder.h"
#include "test.h"

struct Reply {
  int code;
  JsonDocument json;
  std::string status() const { return json["status"] | ""; }
};

static Reply post(const std::string& operation, const std::string& body, const std::string& ip = "192.168.1.30") {
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.begin(tls, String(("https://" + ip + ":30001/grinderApi/" + operation).c_str()));
  Reply r;
  r.code = http.POST(String(body.c_str()));
  if (r.code > 0) deserializeJson(r.json, http.getString().c_str());
  http.end();
  return r;
}

static std::string accept(const std::string& uuid) {
  return post("brewAccept", R"({"version":1,"pairingId":42,"grindUuid":")" + uuid + R"(","groupHeadId":0,"doseType":2})").status();
}

static std::string done(const std::string& uuid, const std::string& result = "Success") {
  return post("brewDone", R"({"version":1,"pairingId":42,"grindUuid":")" + uuid +
                              R"(","durationFinalMs":28400,"massFinalMg":37200,"brewEventUuid":"e1","result":")" +
                              result + R"("})")
      .status();
}

static const std::string kJobs = R"({"version":1,"pairingId":42})";

TEST(grinder_lists_added_jobs_with_their_fields) {
  FakeGrinder grinder;
  Reply empty = post("brewJobs", kJobs);
  CHECK_EQ(empty.code, 200);
  CHECK_EQ(empty.status(), std::string("Ok"));
  CHECK_EQ(empty.json["msgType"] | "", std::string("brewJobsRes"));
  CHECK_EQ(empty.json["brewJobs"].size(), (size_t)0);

  FakeGrinder::Job job;
  job.grindUuid = "g1";
  job.doseTypeExpected = 2;
  grinder.addJob(job);
  Reply r = post("brewJobs", kJobs);
  REQUIRE(r.json["brewJobs"].size() == 1);
  JsonObject j = r.json["brewJobs"][0];
  CHECK_EQ(std::string(j["grindUuid"] | ""), std::string("g1"));
  CHECK(j["groupHeadId"].isNull());
  CHECK_EQ(j["doseTypeExpected"].as<int>(), 2);
  CHECK_EQ(j["durationExpectedMs"].as<double>(), 28000.0);
  CHECK_EQ(grinder.count("brewJobs"), 2);
}

TEST(grinder_replies_403_for_unknown_pairing_id) {
  FakeGrinder grinder;
  Reply r = post("brewJobs", R"({"version":1,"pairingId":7})");
  CHECK_EQ(r.code, 403);
  CHECK(r.json.isNull());
}

TEST(grinder_drops_untaken_jobs_after_60s) {
  FakeGrinder grinder;
  grinder.addJob("g1");
  fake::advance(60000);
  CHECK(grinder.isListed("g1"));
  fake::advance(1);
  CHECK(!grinder.exists("g1"));
  CHECK_EQ(accept("g1"), std::string("ClientErrorBrewJobNotExisting"));
}

TEST(grinder_relists_taken_jobs_after_50s) {
  FakeGrinder grinder;
  grinder.addJob("g1");
  CHECK_EQ(accept("g1"), std::string("Ok"));
  CHECK_EQ(accept("g1"), std::string("ClientErrorBrewJobNotExisting"));  // already taken
  CHECK_EQ(post("brewJobs", kJobs).json["brewJobs"].size(), (size_t)0);  // taken jobs are not listed
  fake::advance(50000);
  CHECK_EQ(post("brewJobs", kJobs).json["brewJobs"].size(), (size_t)1);
  CHECK_EQ(done("g1"), std::string("ClientErrorOngoingJobNotExisting"));
  CHECK_EQ(accept("g1"), std::string("Ok"));
  CHECK_EQ(done("g1"), std::string("Ok"));
}

TEST(grinder_records_brew_done) {
  FakeGrinder grinder;
  grinder.addJob("g1");
  fake::advance(1000);
  REQUIRE(accept("g1") == "Ok");
  fake::advance(30000);
  CHECK_EQ(done("g1"), std::string("Ok"));
  REQUIRE(grinder.dones().size() == 1);
  const auto& d = grinder.dones()[0];
  CHECK_EQ(d.timeMs, 31000UL);
  CHECK_EQ(d.grindUuid, std::string("g1"));
  CHECK_EQ(d.durationFinalMs, 28400L);
  CHECK_EQ(d.massFinalMg, 37200L);
  CHECK_EQ(d.brewEventUuid, std::string("e1"));
  CHECK_EQ(d.result, std::string("Success"));
  CHECK(!grinder.exists("g1"));
  CHECK_EQ(done("g1"), std::string("ClientErrorOngoingJobNotExisting"));  // gone
}

TEST(grinder_fault_injection) {
  FakeGrinder grinder;
  grinder.failNext(1, FakeGrinder::Fault::Timeout);
  grinder.failNext(1, FakeGrinder::Fault::Forbidden, "brewJobs");
  grinder.failNext(1, FakeGrinder::Fault::ClientError, "brewAccept", "ClientErrorSomething");
  CHECK_EQ(post("brewJobs", kJobs).code, HTTPC_ERROR_READ_TIMEOUT);
  CHECK_EQ(post("brewJobs", kJobs).code, 403);
  Reply r = post("brewAccept", R"({"version":1,"pairingId":42,"grindUuid":"x","groupHeadId":0})");
  CHECK_EQ(r.code, 400);
  CHECK_EQ(r.status(), std::string("ClientErrorSomething"));
  CHECK_EQ(post("brewJobs", kJobs).code, 200);  // faults used up
  grinder.unreachable = true;
  CHECK_EQ(post("brewJobs", kJobs).code, HTTPC_ERROR_READ_TIMEOUT);
  CHECK_EQ(grinder.received().back().replyStatus, std::string(""));
}

TEST(grinder_only_answers_at_its_own_address) {
  FakeGrinder grinder;
  CHECK_EQ(post("brewJobs", kJobs, "192.168.1.31").code, HTTPC_ERROR_CONNECTION_REFUSED);
  CHECK_EQ(grinder.count("brewJobs"), 0);
}
