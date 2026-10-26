// A scriptable Mahlkönig E65/E80 GbS grinder serving the Generic WiFi API
// (brewJobs / brewAccept / brewDone) through the fake HTTPClient.
//
//   FakeGrinder grinder;                 // 192.168.1.30:30001, pairingId 42
//   grinder.addJob("g1");                // a Grind-by-Sync grind
//   ... run the sketch ...
//   grinder.dones()[0].durationFinalMs;
//
// Job lifecycle follows the API notes, in simulated time: an untaken job is
// dropped after ~60 s; a taken job with no brewDone is listed again every
// ~50 s, and brewDone for it then fails with ClientErrorOngoingJobNotExisting
// until it is taken again.
#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <ArduinoJson.h>
#include <HTTPClient.h>

class FakeGrinder : public fakehttp::Server {
 public:
  explicit FakeGrinder(std::string ip = "192.168.1.30", uint32_t pairingId = 42, int port = 30001);
  ~FakeGrinder() override;

  struct Job {
    std::string grindUuid;
    bool hasGroupHead = false;
    int groupHeadId = 0;
    bool hasDoseType = true;
    int doseTypeExpected = 2;
    double durationExpectedMs = 28000;
    double brewWeightExpectedMg = 36000;
    bool numbersAsFloats = true;  // send 28000.0 like the real grinder may
  };

  // ---- scripting
  void addJob(const Job& job);
  void addJob(const std::string& grindUuid) { Job j; j.grindUuid = grindUuid; addJob(j); }
  unsigned long dropUntakenAfterMs = 60000;
  unsigned long relistTakenAfterMs = 50000;

  enum class Fault { Timeout, ConnectionError, Forbidden, ClientError };
  // The next `count` requests (any operation, or only `operation` if given)
  // fail with `fault`. ClientError uses `clientError` as the status name.
  void failNext(int count, Fault fault, const std::string& operation = "", const std::string& clientError = "");
  bool unreachable = false;  // every request times out
  // Called as each request arrives, with its path, before it is answered; time
  // may pass inside it (see runLoopDuringRequests in sketch/scenario.h).
  std::function<void(const std::string& path)> onRequest;

  // ---- observation
  struct Received {
    unsigned long timeMs;
    std::string context;    // "loop", "grinder" or "test"
    std::string operation;  // "brewJobs", "brewAccept", "brewDone", ...
    std::string body;
    int replyCode;
    std::string replyStatus;  // "Ok", "ClientError...", "" for 403/transport failures
  };
  const std::vector<Received>& received() const { return received_; }
  std::vector<Received> received(const std::string& operation) const;
  int count(const std::string& operation) const { return static_cast<int>(received(operation).size()); }

  struct Done {
    unsigned long timeMs;
    std::string grindUuid;
    long durationFinalMs;
    long massFinalMg;
    std::string brewEventUuid;
    std::string result;
  };
  const std::vector<Done>& dones() const { return dones_; }  // accepted brewDone reports

  bool isListed(const std::string& grindUuid);
  bool isTaken(const std::string& grindUuid);
  bool exists(const std::string& grindUuid);

  const std::string& ip() const { return ip_; }
  uint32_t pairingId() const { return pairingId_; }

  fakehttp::Response handle(const fakehttp::Request& request) override;

 private:
  enum class State { Listed, Taken, Done, Dropped };
  struct JobState {
    Job job;
    State state;
    unsigned long sinceMs;
  };
  struct PendingFault {
    Fault fault;
    std::string operation;
    std::string clientError;
  };

  void advanceLifecycle(unsigned long now);
  JobState* find(const std::string& grindUuid);
  fakehttp::Response reply(const char* status, const char* msgType, const JsonDocument* extra = nullptr);

  std::string ip_;
  uint32_t pairingId_;
  int port_;
  std::vector<JobState> jobs_;
  std::deque<std::pair<int, PendingFault>> faults_;
  std::vector<Received> received_;
  std::vector<Done> dones_;
};
