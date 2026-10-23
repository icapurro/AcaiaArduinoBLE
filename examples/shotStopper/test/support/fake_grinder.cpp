#include "fake_grinder.h"

#include <Arduino.h>

FakeGrinder::FakeGrinder(std::string ip, uint32_t pairingId, int port)
    : ip_(std::move(ip)), pairingId_(pairingId), port_(port) {
  fakehttp::registerServer(ip_, port_, this);
}

FakeGrinder::~FakeGrinder() { fakehttp::unregisterServer(ip_, port_); }

void FakeGrinder::addJob(const Job& job) { jobs_.push_back({job, State::Listed, millis()}); }

void FakeGrinder::failNext(int count, Fault fault, const std::string& operation, const std::string& clientError) {
  faults_.push_back({count, {fault, operation, clientError}});
}

std::vector<FakeGrinder::Received> FakeGrinder::received(const std::string& operation) const {
  std::vector<Received> out;
  for (auto& r : received_)
    if (r.operation == operation) out.push_back(r);
  return out;
}

FakeGrinder::JobState* FakeGrinder::find(const std::string& grindUuid) {
  for (auto& j : jobs_)
    if (j.job.grindUuid == grindUuid) return &j;
  return nullptr;
}

bool FakeGrinder::isListed(const std::string& grindUuid) {
  advanceLifecycle(millis());
  JobState* j = find(grindUuid);
  return j && j->state == State::Listed;
}

bool FakeGrinder::isTaken(const std::string& grindUuid) {
  advanceLifecycle(millis());
  JobState* j = find(grindUuid);
  return j && j->state == State::Taken;
}

bool FakeGrinder::exists(const std::string& grindUuid) {
  advanceLifecycle(millis());
  JobState* j = find(grindUuid);
  return j && (j->state == State::Listed || j->state == State::Taken);
}

void FakeGrinder::advanceLifecycle(unsigned long now) {
  for (auto& j : jobs_) {
    if (j.state == State::Listed && now - j.sinceMs > dropUntakenAfterMs) {
      j.state = State::Dropped;
    } else if (j.state == State::Taken && now - j.sinceMs >= relistTakenAfterMs) {
      j.state = State::Listed;  // listed again; must be taken again before brewDone
      j.sinceMs = now;
    }
  }
}

fakehttp::Response FakeGrinder::reply(const char* status, const char* msgType, const JsonDocument* extra) {
  JsonDocument doc;
  if (extra) doc.set(*extra);
  doc["status"] = status;
  doc["msgType"] = msgType;
  std::string body;
  serializeJsonPretty(doc, body);  // the grinder sends indented JSON
  return {std::string(status) == "Ok" ? 200 : 400, body};
}

fakehttp::Response FakeGrinder::handle(const fakehttp::Request& request) {
  if (onRequest) onRequest(request.path);
  unsigned long now = millis();  // after onRequest, which may let time pass
  std::string prefix = "/grinderApi/";
  std::string operation = request.path.rfind(prefix, 0) == 0 ? request.path.substr(prefix.size()) : request.path;
  received_.push_back({now, request.context, operation, request.body, 0, ""});
  Received& log = received_.back();
  auto finish = [&](fakehttp::Response r) {
    log.replyCode = r.code;
    if (r.code == 200 || r.code == 400) {
      JsonDocument parsed;
      if (deserializeJson(parsed, r.body) == DeserializationError::Ok) log.replyStatus = parsed["status"] | "";
    }
    return r;
  };

  if (unreachable) return finish({HTTPC_ERROR_READ_TIMEOUT, ""});
  for (auto it = faults_.begin(); it != faults_.end(); ++it) {
    if (!it->second.operation.empty() && it->second.operation != operation) continue;
    PendingFault f = it->second;
    if (--it->first == 0) faults_.erase(it);
    switch (f.fault) {
      case Fault::Timeout: return finish({HTTPC_ERROR_READ_TIMEOUT, ""});
      case Fault::ConnectionError: return finish({HTTPC_ERROR_CONNECTION_REFUSED, ""});
      case Fault::Forbidden: return finish({403, ""});
      case Fault::ClientError: return finish(reply(f.clientError.c_str(), (operation + "Res").c_str()));
    }
  }

  JsonDocument body;
  if (deserializeJson(body, request.body) != DeserializationError::Ok || body["version"] != 1)
    return finish(reply("ClientErrorBadRequest", (operation + "Res").c_str()));
  if (body["pairingId"].as<uint32_t>() != pairingId_ || !body["pairingId"].is<uint32_t>())
    return finish({403, ""});  // unknown pairingId: empty body

  advanceLifecycle(now);

  if (operation == "brewJobs") {
    JsonDocument extra;
    JsonArray list = extra["brewJobs"].to<JsonArray>();
    for (auto& j : jobs_) {
      if (j.state != State::Listed) continue;
      JsonObject o = list.add<JsonObject>();
      o["grindUuid"] = j.job.grindUuid;
      if (j.job.hasGroupHead) o["groupHeadId"] = j.job.groupHeadId;
      if (j.job.hasDoseType) o["doseTypeExpected"] = j.job.doseTypeExpected;
      if (j.job.numbersAsFloats) {
        // ArduinoJson prints whole doubles without ".0"; mimic "28000.0".
        o["durationExpectedMs"] = serialized(std::to_string(static_cast<long>(j.job.durationExpectedMs)) + ".0");
        o["brewWeightExpectedMg"] = serialized(std::to_string(static_cast<long>(j.job.brewWeightExpectedMg)) + ".0");
      } else {
        o["durationExpectedMs"] = static_cast<long>(j.job.durationExpectedMs);
        o["brewWeightExpectedMg"] = static_cast<long>(j.job.brewWeightExpectedMg);
      }
    }
    return finish(reply("Ok", "brewJobsRes", &extra));
  }

  if (operation == "brewAccept") {
    JobState* j = find(body["grindUuid"] | "");
    if (!j || j->state != State::Listed) return finish(reply("ClientErrorBrewJobNotExisting", "brewAcceptRes"));
    j->state = State::Taken;
    j->sinceMs = now;
    return finish(reply("Ok", "brewAcceptRes"));
  }

  if (operation == "brewDone") {
    JobState* j = find(body["grindUuid"] | "");
    if (!j || j->state != State::Taken) return finish(reply("ClientErrorOngoingJobNotExisting", "brewDoneRes"));
    j->state = State::Done;
    dones_.push_back({now, j->job.grindUuid, body["durationFinalMs"].as<long>(), body["massFinalMg"].as<long>(),
                      body["brewEventUuid"] | "", body["result"] | ""});
    return finish(reply("Ok", "brewDoneRes"));
  }

  return finish(reply("ClientErrorUnknownRequest", (operation + "Res").c_str()));
}
