#include "GrinderClient.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

void copyString(char* dest, size_t size, const char* src) {
  strncpy(dest, src ? src : "", size - 1);
  dest[size - 1] = '\0';
}

const char* stateName(int state) {
  static const char* names[] = {"not configured", "idle", "job taken", "in shot", "reporting"};
  return names[state];
}

const char* wifiStatusName(int status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "network not found (the ESP32 only sees 2.4 GHz)";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect failed (wrong password?)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

const char* outcomeName(ShotOutcome outcome) {
  switch (outcome) {
    case ShotOutcome::Success: return "Success";
    case ShotOutcome::Canceled: return "Canceled";
    case ShotOutcome::Flush: return "Flush";
  }
  return "?";
}

void vlog(const char* source, const char* format, va_list args) {
  char line[400];
  int n = snprintf(line, sizeof(line), "[%7lu %s] ", (unsigned long)millis(), source);
  if (n < 0) return;
  vsnprintf(line + n, sizeof(line) - n - 2, format, args);
  strcat(line, "\n");
  Serial.print(line);
}

}  // namespace

void GrinderClient::log(const char* source, const char* format, ...) {
  va_list args;
  va_start(args, format);
  vlog(source, format, args);
  va_end(args);
}

void GrinderClient::debugLog(const char* format, ...) {
  if (!debug_) return;
  va_list args;
  va_start(args, format);
  vlog("grinder", format, args);
  va_end(args);
}

// ---------------------------------------------------------------- loop() side

void GrinderClient::begin() {
  if (!queue_) queue_ = xQueueCreate(8, sizeof(GrinderEvent));
}

bool GrinderClient::configure(const char* ip, uint32_t pairingId, const char* ssid, const char* pass) {
  GrinderEvent e;
  memset(&e, 0, sizeof(e));
  e.type = GrinderEventType::Config;
  copyString(e.ip, sizeof(e.ip), ip);
  e.pairingId = pairingId;
  copyString(e.ssid, sizeof(e.ssid), ssid);
  copyString(e.pass, sizeof(e.pass), pass);
  return queue_ && xQueueSend(queue_, &e, 0) == pdTRUE;
}

bool GrinderClient::setActive(bool active) {
  GrinderEvent e;
  memset(&e, 0, sizeof(e));
  e.type = GrinderEventType::Active;
  e.active = active;
  return queue_ && xQueueSend(queue_, &e, 0) == pdTRUE;
}

bool GrinderClient::shotStarted() {
  GrinderEvent e;
  memset(&e, 0, sizeof(e));
  e.type = GrinderEventType::ShotStarted;
  return queue_ && xQueueSend(queue_, &e, 0) == pdTRUE;
}

bool GrinderClient::shotFinished(ShotOutcome outcome, uint32_t durationMs, uint32_t massMg) {
  GrinderEvent e;
  memset(&e, 0, sizeof(e));
  e.type = GrinderEventType::ShotFinished;
  e.outcome = outcome;
  e.durationMs = durationMs;
  e.massMg = massMg;
  return queue_ && xQueueSend(queue_, &e, 0) == pdTRUE;
}

bool GrinderClient::isValidIPv4(const char* text) {
  if (!text) return false;
  int octets = 0;
  const char* p = text;
  while (true) {
    int digits = 0;
    int value = 0;
    while (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
      if (++digits > 3) return false;
      p++;
    }
    if (digits == 0 || value > 255) return false;
    octets++;
    if (*p == '\0') return octets == 4;
    if (*p != '.' || octets == 4) return false;
    p++;
  }
}

// ---------------------------------------------------------------- task side

void GrinderClient::taskMain(void* client) {
  GrinderClient* self = static_cast<GrinderClient*>(client);
  for (;;) {
    self->runOnce();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void GrinderClient::runOnce() {
  GrinderEvent event;
  while (queue_ && xQueueReceive(queue_, &event, 0) == pdTRUE) handleEvent(event);

  unsigned long now = millis();
  if (state_ == State::NotConfigured || !active_) {
    updateStatus();
    return;
  }
  if (!ensureWifi(now)) {
    updateStatus();
    return;
  }

  // Expire a job that no shot has used.
  if (state_ == State::JobTaken && hasJob_ && job_.everTaken && now - job_.firstTakenMs > kJobExpiryMs) {
    log("grinder", "job %s expired unused, canceling", job_.grindUuid.c_str());
    queueReport(job_, ShotOutcome::Canceled, 0, 0, needAccept_, false, now);
    clearJob();
    setState(State::Idle);
  }

  // Drop reports whose retry window has passed.
  while (!reports_.empty() && (long)(now - reports_.front().deadlineMs) > 0) {
    log("grinder", "report for %s abandoned after its retry window", reports_.front().job.grindUuid.c_str());
    bool wasShot = reports_.front().isShotReport;
    reports_.pop_front();
    if (wasShot && state_ == State::Reporting) setState(hasJob_ ? State::JobTaken : State::Idle);
  }

  // Requests continue during a shot: a taken job goes back on the list ~50 s
  // after its last brewAccept, and must be taken again before the shot ends or
  // brewDone is refused.
  if ((long)(now - backoffUntilMs_) >= 0) {
    updateStatus();  // publish before a request, which can take seconds
    if (hasJob_ && needAccept_ && state_ != State::Reporting) {
      acceptCurrentJob(now);
    } else if (!reports_.empty()) {
      deliverReport(now);
    } else if ((state_ == State::Idle || state_ == State::JobTaken || state_ == State::InShot) &&
               (long)(now - nextPollMs_) >= 0) {
      poll(now);
    }
  }
  updateStatus();
}

void GrinderClient::handleEvent(const GrinderEvent& event) {
  switch (event.type) {
    case GrinderEventType::Config:
      debugLog("event: config ip=\"%s\" pairingId=%lu ssid=\"%s\"", event.ip, (unsigned long)event.pairingId, event.ssid);
      break;
    case GrinderEventType::Active:
      debugLog("event: %s", event.active ? "active" : "inactive (disabled or OTA)");
      break;
    case GrinderEventType::ShotStarted:
      debugLog("event: shot started (%s)", hasJob_ && job_.everTaken ? job_.grindUuid.c_str() : "no job");
      break;
    case GrinderEventType::ShotFinished:
      debugLog("event: shot finished %s, %lu ms, %lu mg", outcomeName(event.outcome), (unsigned long)event.durationMs,
               (unsigned long)event.massMg);
      break;
  }
  switch (event.type) {
    case GrinderEventType::Config: {
      bool changed = strcmp(ip_, event.ip) != 0 || pairingId_ != event.pairingId;
      bool wifiChanged = strcmp(ssid_, event.ssid) != 0 || strcmp(pass_, event.pass) != 0;
      copyString(ip_, sizeof(ip_), event.ip);
      pairingId_ = event.pairingId;
      copyString(ssid_, sizeof(ssid_), event.ssid);
      copyString(pass_, sizeof(pass_), event.pass);
      if (wifiChanged) wifiBegun_ = false;
      if (ip_[0] == '\0') {
        clearJob();
        reports_.clear();
        seen_.clear();
        setState(State::NotConfigured);
      } else if (changed || state_ == State::NotConfigured) {
        resetPollTiming();
        clearJob();
        reports_.clear();
        seen_.clear();
        replied_ = false;
        failing_ = false;
        backoffMs_ = 0;
        backoffUntilMs_ = 0;
        nextPollMs_ = millis();
        forbiddenCount_ = 0;
        setState(State::Idle);
      }
      break;
    }
    case GrinderEventType::Active:
      active_ = event.active;
      resetPollTiming();
      // OTA mode disconnects WiFi on its way out, and the core doesn't
      // auto-reconnect after a deliberate disconnect: start afresh.
      if (active_) wifiBegun_ = false;
      if (!active_ && state_ == State::InShot) {
        // Disabled or OTA mid-shot: report nothing, leave the job for the
        // grinder to drop, and ignore this shot's ShotFinished.
        clearJob();
        setState(State::Idle);
      }
      break;
    case GrinderEventType::ShotStarted:
      // Polling goes on during the shot (to re-take the job if it's listed
      // again), but no new job is taken until this shot is settled.
      if (state_ != State::NotConfigured) setState(State::InShot);
      break;
    case GrinderEventType::ShotFinished:
      if (state_ != State::InShot) break;
      if (!hasJob_ || !job_.everTaken) {
        setState(State::Idle);
        clearJob();
        nextPollMs_ = millis();
      } else if (event.outcome == ShotOutcome::Flush) {
        setState(State::JobTaken);
        nextPollMs_ = millis();
      } else {
        // Report right away, even if earlier failures had us backing off: a job
        // listed again is likely dropped ~60 s later, and then can't be reported.
        backoffMs_ = 0;
        backoffUntilMs_ = millis();
        uint32_t mass = event.massMg > kMaxMassMg ? kMaxMassMg : event.massMg;
        if (event.outcome == ShotOutcome::Success) {
          queueReport(job_, ShotOutcome::Success, event.durationMs, mass, needAccept_, true, millis());
        } else {
          queueReport(job_, ShotOutcome::Canceled, 0, 0, needAccept_, true, millis());
        }
        clearJob();
        setState(State::Reporting);
      }
      break;
  }
}

bool GrinderClient::ensureWifi(unsigned long now) {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != wifiWasConnected_) {
    wifiWasConnected_ = connected;
    if (connected) {
      log("grinder", "WiFi connected, IP %s", WiFi.localIP().toString().c_str());
    } else {
      log("grinder", "WiFi disconnected (%s), reconnecting", wifiStatusName(WiFi.status()));
      resetPollTiming();
      lastWifiBeginMs_ = now;  // give the core's auto-reconnect a full attempt first
    }
  }
  if (connected) return true;
  if (!wifiBegun_ || now - lastWifiBeginMs_ >= kWifiAttemptMs) {
    if (wifiBegun_) {
      log("grinder", "WiFi still not connected after %lu s (%s), retrying", kWifiAttemptMs / 1000,
          wifiStatusName(WiFi.status()));
    } else {
      log("grinder", "connecting to WiFi \"%s\"", ssid_);
    }
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();  // abort any attempt in progress, or begin() can't set the config
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid_, pass_);
    wifiBegun_ = true;
    lastWifiBeginMs_ = now;
    replied_ = false;
  }
  return WiFi.status() == WL_CONNECTED;
}

GrinderClient::Result GrinderClient::request(const char* operation, JsonDocument& body, JsonDocument& reply) {
  body["version"] = 1;
  body["pairingId"] = pairingId_;
  char payload[512];
  size_t length = serializeJson(body, payload, sizeof(payload));
  if (length == 0 || length >= sizeof(payload)) return Result::Failed;
  unsigned long started = millis();

  String url = String("https://") + ip_ + ":" + String(kPort) + "/grinderApi/" + operation;
  WiFiClientSecure tls;
  tls.setInsecure();  // the grinder's certificate is self-signed and names no host
  // HTTPClient's timeouts don't cover the TLS handshake (default 120 s), so
  // connect here with our own limit; HTTPClient then reuses this connection.
  tls.setHandshakeTimeout(kRequestTimeoutMs / 1000);
  int code = HTTPC_ERROR_CONNECTION_REFUSED;
  bool tlsUp = tls.connect(ip_, kPort, kRequestTimeoutMs);
  unsigned long connectMs = millis() - started;
  HTTPClient http;
  if (tlsUp && http.begin(tls, url)) {
    // Whatever is left of the request's time budget, at least 1 s.
    unsigned long remaining = connectMs + 1000 < kRequestTimeoutMs ? kRequestTimeoutMs - connectMs : 1000;
    http.setReuse(false);  // the grinder silently drops kept-alive connections
    http.setTimeout((uint16_t)remaining);
    http.setConnectTimeout(kRequestTimeoutMs);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/json");
    code = http.POST(String(payload));
  }

  Result result = Result::Failed;
  if (code == 403) {
    result = Result::Forbidden;
  } else if (code == 200 || code == 400) {
    int size = http.getSize();
    if (size <= kMaxReplyBytes) {
      String text = http.getString();
      if ((int)text.length() <= kMaxReplyBytes && deserializeJson(reply, text.c_str()) == DeserializationError::Ok) {
        const char* status = reply["status"] | "";
        if (code == 200 && strcmp(status, "Ok") == 0) {
          result = Result::Ok;
        } else if (strncmp(status, "ClientError", 11) == 0) {
          result = Result::ClientError;
        }
      }
    }
  }
  http.end();
  lastRequestMs_ = millis() - started;

  if (debug_) {
    // An empty, successful brewJobs poll happens every 2 s: stay quiet unless it was late.
    bool emptyPoll = strcmp(operation, "brewJobs") == 0 && result == Result::Ok && reply["brewJobs"].size() == 0;
    if (!emptyPoll) {
      char json[200] = "";
      if (code == 200 || code == 400) {
        size_t n = serializeJson(reply, json, sizeof(json));
        if (n >= sizeof(json) - 1) strcpy(json + sizeof(json) - 5, "...");
      }
      const char* status = reply["status"] | "";
      debugLog("-> %s %s%s", operation, payload, requestNote_);
      if (!tlsUp) {
        debugLog("<- %s TLS connect failed after %lu ms, RSSI %d dBm, heap %u", operation, connectMs, (int)WiFi.RSSI(),
                 (unsigned)ESP.getFreeHeap());
      } else {
        debugLog("<- %s HTTP %d %s in %lu ms (TLS connect %lu ms), RSSI %d dBm, heap %u %s", operation, code,
                 code < 0 ? HTTPClient::errorToString(code).c_str() : status, millis() - started, connectMs,
                 (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), json);
      }
    } else if (pollLate_) {
      debugLog("brewJobs poll late%s", requestNote_);
    }
  }
  requestNote_[0] = '\0';
  pollLate_ = false;
  return result;
}

void GrinderClient::noteFailure(unsigned long now) {
  failing_ = true;
  backoffMs_ = backoffMs_ == 0 ? kBackoffStartMs : backoffMs_ * 2;
  if (backoffMs_ > kBackoffMaxMs) backoffMs_ = kBackoffMaxMs;
  backoffUntilMs_ = now + backoffMs_;
}

void GrinderClient::noteSuccess() {
  failing_ = false;
  replied_ = true;
  backoffMs_ = 0;
}

void GrinderClient::notePollTiming(unsigned long now) {
  if (!debug_) return;
  if (!pollTimed_) {
    snprintf(requestNote_, sizeof(requestNote_), " (first poll since start/pause)");
  } else {
    // Late means sent well after it was due (nextPollMs_, or the end of a
    // backoff). Usually that's because the request before it was slow, not
    // because the task didn't get to run; say which.
    unsigned long due = nextPollMs_;
    if (failing_ && (long)(backoffUntilMs_ - due) > 0) due = backoffUntilMs_;
    unsigned long lateBy = (long)(now - due) > 0 ? now - due : 0;
    unsigned long gap = now - lastPollMs_;
    bool late = lateBy > kPollLateToleranceMs;
    bool afterSlow = lastRequestMs_ > kPollIntervalMs;
    pollLate_ = late;
    if (late) {
      snprintf(requestNote_, sizeof(requestNote_),
               " (+%lu ms since previous poll; sent %lu ms after it was due, previous request took %lu ms)", gap,
               lateBy, lastRequestMs_);
    } else {
      snprintf(requestNote_, sizeof(requestNote_), " (+%lu ms since previous poll)", gap);
    }
    if (statsPolls_ == 0 || gap < statsMinMs_) statsMinMs_ = gap;
    if (gap > statsMaxMs_) statsMaxMs_ = gap;
    statsSumMs_ += gap;
    statsPolls_++;
    if (late) {
      statsLate_++;
      if (afterSlow) statsLateAfterSlow_++;
    }
    if (now - statsStartMs_ >= kPollStatsWindowMs) {
      debugLog("brewJobs cadence: %lu polls in the last %lu s, every %lu-%lu ms (avg %lu ms), %lu sent late "
               "(%lu right after a request slower than %lu ms)",
               (unsigned long)statsPolls_, (now - statsStartMs_) / 1000, statsMinMs_, statsMaxMs_,
               statsSumMs_ / statsPolls_, (unsigned long)statsLate_, (unsigned long)statsLateAfterSlow_,
               kPollIntervalMs);
      statsPolls_ = 0;
    }
  }
  if (!pollTimed_ || statsPolls_ == 0) {
    statsStartMs_ = now;
    statsPolls_ = 0;
    statsLate_ = 0;
    statsLateAfterSlow_ = 0;
    statsSumMs_ = 0;
    statsMaxMs_ = 0;
  }
  pollTimed_ = true;
  lastPollMs_ = now;
}

void GrinderClient::poll(unsigned long now) {
  JsonDocument body;
  JsonDocument reply;
  notePollTiming(now);
  Result result = request("brewJobs", body, reply);
  nextPollMs_ = now + kPollIntervalMs;

  if (result == Result::Forbidden) {
    // One 403 can be a grinder still starting up; three in a row means the
    // pairing is gone (cleared on the grinder, or a wrong pairingId).
    noteSuccess();
    forbiddenCount_++;
    if (forbiddenCount_ >= kForbiddenLimit) {
      if (forbiddenCount_ == kForbiddenLimit) log("grinder", "pairing rejected (3 x HTTP 403): check the pairingId");
      nextPollMs_ = now + kRejectedPollIntervalMs;
    }
    return;
  }
  if (result != Result::Ok) {
    noteFailure(now);
    return;
  }
  noteSuccess();
  forbiddenCount_ = 0;

  JsonArray jobs = reply["brewJobs"].as<JsonArray>();
  bool currentListed = false;
  int newest = -1;
  int index = 0;
  for (JsonObject j : jobs) {
    std::string uuid = j["grindUuid"] | "";
    if (!uuid.empty()) {
      if (hasJob_ && uuid == job_.grindUuid) {
        currentListed = true;
      } else if (!seen(uuid)) {
        newest = index;  // the last unseen job in the reply is the newest grind
      }
    }
    index++;
  }
  if (currentListed) {
    needAccept_ = true;  // listed again: take it again to keep it open
    debugLog("job %s listed again, re-taking it", job_.grindUuid.c_str());
  }
  // A grind that shows up during a shot isn't this shot's: leave it (unseen)
  // until the shot is settled, then take it as usual.
  if (state_ == State::InShot) return;
  for (JsonObject j : jobs) markSeen(j["grindUuid"] | "");
  if (newest < 0) return;

  JsonObject j = jobs[newest];
  if (hasJob_) {
    log("grinder", "newer grind, canceling previous job %s", job_.grindUuid.c_str());
    queueReport(job_, ShotOutcome::Canceled, 0, 0, needAccept_, false, now);
    clearJob();
  }
  job_ = Job();
  job_.grindUuid = j["grindUuid"] | "";
  job_.groupHeadId = j["groupHeadId"].isNull() ? 0 : (int)lround(j["groupHeadId"].as<double>());
  job_.hasDoseType = !j["doseTypeExpected"].isNull();
  job_.doseType = j["doseTypeExpected"].as<double>();
  hasJob_ = true;
  needAccept_ = true;
  debugLog("new grind %s (groupHead %d, doseType %s), taking it", job_.grindUuid.c_str(), job_.groupHeadId,
           job_.hasDoseType ? String((long)job_.doseType).c_str() : "none");
}

void GrinderClient::fillAccept(JsonDocument& body, const Job& job) {
  body["grindUuid"] = job.grindUuid.c_str();
  body["groupHeadId"] = job.groupHeadId;
  // Echo doseTypeExpected unchanged (its meaning is unknown).
  if (!job.hasDoseType) {
    body["doseType"] = nullptr;
  } else if (job.doseType == floor(job.doseType)) {
    body["doseType"] = (long)job.doseType;
  } else {
    body["doseType"] = job.doseType;
  }
}

void GrinderClient::acceptCurrentJob(unsigned long now) {
  JsonDocument body;
  JsonDocument reply;
  fillAccept(body, job_);
  Result result = request("brewAccept", body, reply);
  if (result == Result::Ok) {
    noteSuccess();
    needAccept_ = false;
    if (!job_.everTaken) {
      job_.everTaken = true;
      job_.firstTakenMs = now;
      log("grinder", "took job %s", job_.grindUuid.c_str());
    }
    if (state_ == State::Idle) setState(State::JobTaken);
  } else if (result == Result::ClientError) {
    // ClientErrorBrewJobNotExisting: gone (timed out, or taken elsewhere).
    noteSuccess();
    log("grinder", "job %s no longer exists", job_.grindUuid.c_str());
    clearJob();
    if (state_ == State::JobTaken) setState(State::Idle);
  } else {
    noteFailure(now);
  }
}

void GrinderClient::deliverReport(unsigned long now) {
  Report& report = reports_.front();
  JsonDocument reply;
  if (report.needsAccept) {
    JsonDocument body;
    fillAccept(body, report.job);
    Result accepted = request("brewAccept", body, reply);
    if (accepted == Result::Failed || accepted == Result::Forbidden) {
      noteFailure(now);
      return;
    }
    // Ok, or BrewJobNotExisting (maybe already taken): brewDone says which.
    noteSuccess();
    report.needsAccept = false;
    return;
  }

  JsonDocument body;
  body["grindUuid"] = report.job.grindUuid.c_str();
  body["durationFinalMs"] = report.durationMs;
  body["massFinalMg"] = report.massMg;
  body["brewEventUuid"] = report.brewEventUuid.c_str();
  body["result"] = report.result == ShotOutcome::Success ? "Success" : "Canceled";
  Result result = request("brewDone", body, reply);
  if (result == Result::Ok) {
    noteSuccess();
    log("grinder", "reported %s for job %s (%lu ms, %lu mg)", outcomeName(report.result),
        report.job.grindUuid.c_str(), (unsigned long)report.durationMs, (unsigned long)report.massMg);
    bool wasShot = report.isShotReport;
    reports_.pop_front();
    if (wasShot && state_ == State::Reporting) {
      setState(hasJob_ ? State::JobTaken : State::Idle);
      nextPollMs_ = now;
    }
  } else if (result == Result::ClientError) {
    noteSuccess();
    const char* status = reply["status"] | "";
    if (strcmp(status, "ClientErrorOngoingJobNotExisting") == 0) {
      report.needsAccept = true;  // listed again meanwhile: take it, then resend
      debugLog("job %s was listed again, re-taking it before resending", report.job.grindUuid.c_str());
    } else {
      noteFailure(now);
    }
  } else {
    noteFailure(now);
  }
}

void GrinderClient::queueReport(const Job& job, ShotOutcome result, uint32_t durationMs, uint32_t massMg,
                                bool needsAccept, bool isShotReport, unsigned long now) {
  Report report;
  report.job = job;
  report.result = result;
  report.durationMs = durationMs;
  report.massMg = massMg;
  report.brewEventUuid = newUuid();
  report.deadlineMs = now + (result == ShotOutcome::Success ? kSuccessReportWindowMs : kCanceledReportWindowMs);
  report.needsAccept = needsAccept;
  report.isShotReport = isShotReport;
  reports_.push_back(report);
}

void GrinderClient::clearJob() {
  hasJob_ = false;
  needAccept_ = false;
  job_ = Job();
}

bool GrinderClient::seen(const std::string& grindUuid) const {
  for (size_t i = 0; i < seen_.size(); i++)
    if (seen_[i] == grindUuid) return true;
  return false;
}

void GrinderClient::markSeen(const std::string& grindUuid) {
  if (grindUuid.empty() || seen(grindUuid)) return;
  if (seen_.size() >= 32) seen_.erase(seen_.begin());
  seen_.push_back(grindUuid);
}

void GrinderClient::setState(State state) {
  if (state == state_) return;
  state_ = state;
  log("grinder", "state: %s (free heap %u)", stateName((int)state), (unsigned)ESP.getFreeHeap());
}

void GrinderClient::updateStatus() {
  uint8_t s;
  if (state_ == State::NotConfigured) {
    s = GRINDER_NOT_CONFIGURED;
  } else if (!active_) {
    return;  // keep the last status while idle in OTA / disabled
  } else if (WiFi.status() != WL_CONNECTED) {
    s = GRINDER_CONNECTING_WIFI;
  } else if (forbiddenCount_ >= kForbiddenLimit) {
    s = GRINDER_PAIRING_REJECTED;
  } else if (failing_) {
    s = GRINDER_UNREACHABLE;
  } else if (!replied_) {
    s = GRINDER_CONNECTING_WIFI;  // connected, waiting for the grinder's first answer
  } else if (state_ == State::Reporting) {
    s = GRINDER_REPORTING;
  } else if ((state_ == State::JobTaken || state_ == State::InShot) && hasJob_ && job_.everTaken) {
    s = GRINDER_JOB_TAKEN;
  } else {
    s = GRINDER_READY;
  }
  status_.store(s);
}

std::string GrinderClient::newUuid() {
  uint8_t b[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    memcpy(b + i, &r, 4);
  }
  b[6] = (b[6] & 0x0f) | 0x40;  // version 4
  b[8] = (b[8] & 0x3f) | 0x80;  // RFC 4122 variant
  char out[37];
  snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
           b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return std::string(out);
}
