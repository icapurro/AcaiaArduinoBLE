/*
  GrinderClient.h - Reports shots to a Mahlkönig E65/E80 GbS grinder over its
  "Generic WiFi" (Grind-by-Sync) API, so the grinder adjusts its grind setting
  from each shot's brew time.

  The grinder is an HTTPS server on port 30001. Every request is a slow TLS
  handshake that can hang for seconds, so the client runs in its own FreeRTOS
  task (taskMain, pinned to core 0) and never touches BLE, EEPROM or sketch
  globals. loop() talks to it only through non-blocking events (configure,
  setActive, shotStarted, shotFinished) and reads back status().

  Pairing happens elsewhere (e.g. the companion app); this client only needs the
  grinder's IP address and the pairingId.

  API reference: "Grind-by-Sync Generic WiFi API" v0.2.0 (from gbs-anywhere).
  Requires ArduinoJson 7.
*/
#ifndef GrinderClient_h
#define GrinderClient_h

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <deque>
#include <string>
#include <vector>

enum class GrinderEventType : uint8_t { Config, Active, ShotStarted, ShotFinished };

// How a shot ended, as far as the grinder is concerned.
enum class ShotOutcome : uint8_t {
  Success,   // stopped by weight: the grinder learns from it
  Canceled,  // any other real shot: tell the grinder not to learn from it
  Flush,     // shorter than minShotDurationS: not a shot, keep the job
};

struct GrinderEvent {
  GrinderEventType type;
  bool active;                  // Active
  ShotOutcome outcome;          // ShotFinished
  uint32_t durationMs;          // ShotFinished (Success)
  uint32_t massMg;              // ShotFinished (Success)
  char ip[16];                  // Config: "" turns grinder sync off
  uint32_t pairingId;           // Config
  char ssid[33];                // Config
  char pass[33];                // Config
};

// Values of the grinder link status characteristic (0xFF27).
enum GrinderLinkStatus : uint8_t {
  GRINDER_NOT_CONFIGURED = 0,
  GRINDER_CONNECTING_WIFI = 1,
  GRINDER_UNREACHABLE = 2,
  GRINDER_PAIRING_REJECTED = 3,
  GRINDER_READY = 4,
  GRINDER_JOB_TAKEN = 5,
  GRINDER_REPORTING = 6,
};

class GrinderClient {
 public:
  static const int kPort = 30001;
  static const unsigned long kPollIntervalMs = 2000;
  static const unsigned long kRejectedPollIntervalMs = 60000;
  static const unsigned long kBackoffStartMs = 2000;
  static const unsigned long kBackoffMaxMs = 60000;
  // A connection attempt (with the core's auto-reconnect) gets this long before
  // it is aborted and restarted. Calling WiFi.begin() while the station is still
  // connecting fails with "sta is connecting, cannot set config".
  static const unsigned long kWifiAttemptMs = 30000;
  static const unsigned long kJobExpiryMs = 5UL * 60UL * 1000UL;
  static const unsigned long kSuccessReportWindowMs = 120000;
  static const unsigned long kCanceledReportWindowMs = 20000;
  static const uint16_t kRequestTimeoutMs = 8000;
  static const int kMaxReplyBytes = 65536;
  static const uint32_t kMaxMassMg = 5000000;
  static const int kForbiddenLimit = 3;
  static const uint32_t kTaskStackBytes = 16384;
  static const UBaseType_t kTaskPriority = 1;
  static const BaseType_t kTaskCore = 0;

  // Call once from setup(), before starting the task.
  void begin();

  // ---- loop() side: never block. Return false if the event queue is full.
  bool configure(const char* ip, uint32_t pairingId, const char* ssid, const char* pass);
  bool setActive(bool active);
  bool shotStarted();
  bool shotFinished(ShotOutcome outcome, uint32_t durationMs = 0, uint32_t massMg = 0);
  uint8_t status() const { return status_.load(); }

  // Log every request, reply, event and decision (call before starting the task).
  void setDebug(bool on) { debug_ = on; }
  bool debug() const { return debug_; }

  // One timestamped line, "[  12345 source] message", in a single Serial write
  // so lines from loop() (core 1) and the grinder task (core 0) don't interleave.
  static void log(const char* source, const char* format, ...) __attribute__((format(printf, 2, 3)));

  // ---- task side
  // One step: drain events, then at most one grinder request.
  void runOnce();
  // FreeRTOS task body: xTaskCreatePinnedToCore(GrinderClient::taskMain, ..., &client, ...).
  static void taskMain(void* client);

  // Accepts four dot-separated decimal octets (0-255), e.g. "192.168.1.30".
  static bool isValidIPv4(const char* text);

 private:
  enum class State : uint8_t { NotConfigured, Idle, JobTaken, InShot, Reporting };
  enum class Result : uint8_t { Ok, ClientError, Forbidden, Failed };

  struct Job {
    std::string grindUuid;
    int groupHeadId = 0;
    bool hasDoseType = false;
    double doseType = 0;
    unsigned long firstTakenMs = 0;
    bool everTaken = false;
  };

  struct Report {
    Job job;
    ShotOutcome result;
    uint32_t durationMs;
    uint32_t massMg;
    std::string brewEventUuid;
    unsigned long deadlineMs;
    bool needsAccept;  // the job was listed again: take it before brewDone
    bool isShotReport;
  };

  void handleEvent(const GrinderEvent& event);
  bool ensureWifi(unsigned long now);
  Result request(const char* operation, JsonDocument& body, JsonDocument& reply);
  void noteFailure(unsigned long now);
  void noteSuccess();

  void poll(unsigned long now);
  static void fillAccept(JsonDocument& body, const Job& job);
  void acceptCurrentJob(unsigned long now);
  void deliverReport(unsigned long now);
  void queueReport(const Job& job, ShotOutcome result, uint32_t durationMs, uint32_t massMg, bool needsAccept,
                   bool isShotReport, unsigned long now);
  void clearJob();
  bool seen(const std::string& grindUuid) const;
  void markSeen(const std::string& grindUuid);
  void setState(State state);
  void updateStatus();
  static std::string newUuid();

  void debugLog(const char* format, ...) __attribute__((format(printf, 2, 3)));

  void resetPollTiming() { pollTimed_ = false; }
  void notePollTiming(unsigned long now);

  bool debug_ = false;
  bool wifiWasConnected_ = false;
  char requestNote_[128] = "";  // appended to the next "->" debug line
  bool pollLate_ = false;       // the poll about to be sent is later than scheduled

  // brewJobs cadence, measured between back-to-back polls (debug log only).
  static const unsigned long kPollStatsWindowMs = 60000;
  static const unsigned long kPollLateToleranceMs = 250;
  bool pollTimed_ = false;
  unsigned long lastPollMs_ = 0;
  unsigned long lastRequestMs_ = 0;  // how long the most recent request (any operation) took
  unsigned long statsStartMs_ = 0;
  uint32_t statsPolls_ = 0;
  uint32_t statsLate_ = 0;
  uint32_t statsLateAfterSlow_ = 0;  // late because the request before it took longer than a poll interval
  unsigned long statsSumMs_ = 0;
  unsigned long statsMinMs_ = 0;
  unsigned long statsMaxMs_ = 0;
  QueueHandle_t queue_ = nullptr;
  std::atomic<uint8_t> status_{GRINDER_NOT_CONFIGURED};

  // Configuration (copies from Config events).
  char ip_[16] = "";
  uint32_t pairingId_ = 0;
  char ssid_[33] = "";
  char pass_[33] = "";
  bool active_ = false;

  State state_ = State::NotConfigured;
  bool wifiBegun_ = false;
  unsigned long lastWifiBeginMs_ = 0;
  bool replied_ = false;   // the grinder has answered since (re)configuration
  bool failing_ = false;   // the last request failed at the transport level
  unsigned long backoffMs_ = 0;
  unsigned long backoffUntilMs_ = 0;
  unsigned long nextPollMs_ = 0;
  int forbiddenCount_ = 0;

  bool hasJob_ = false;
  bool needAccept_ = false;
  Job job_;
  std::vector<std::string> seen_;
  std::deque<Report> reports_;
};

#endif
