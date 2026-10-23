#include "HTTPClient.h"

#include <cstdlib>

#include "WiFiClientSecure.h"

namespace {

struct Registry {
  std::map<std::string, fakehttp::Server*> servers;
  std::vector<fakehttp::Request> requests;
  std::string context = "test";
};

Registry& registry() {
  static Registry r;
  return r;
}

std::string key(const std::string& host, int port) { return host + ":" + std::to_string(port); }

// Splits scheme://host[:port]/path. Returns false if the URL has no scheme.
bool parseUrl(const std::string& url, fakehttp::Request& r) {
  size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;
  r.scheme = url.substr(0, schemeEnd);
  size_t hostStart = schemeEnd + 3;
  size_t pathStart = url.find('/', hostStart);
  std::string authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
  r.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  size_t colon = authority.find(':');
  r.host = authority.substr(0, colon);
  r.port = colon == std::string::npos ? (r.scheme == "https" ? 443 : 80) : std::atoi(authority.c_str() + colon + 1);
  return true;
}

}  // namespace

int WiFiClientSecure::connect(const char* host, uint16_t port, int32_t timeoutMs) {
  connectTimeoutMs_ = timeoutMs;
  connected_ = registry().servers.count(key(host ? host : "", port)) > 0;
  return connected_ ? 1 : 0;
}

bool HTTPClient::begin(WiFiClient& client, const String& url) {
  client_ = &client;
  url_ = url.str();
  headers_.clear();
  response_.clear();
  fakehttp::Request probe{};
  return parseUrl(url_, probe);
}

void HTTPClient::end() {
  client_ = nullptr;
  response_.clear();
}

void HTTPClient::addHeader(const String& name, const String& value, bool first, bool replace) {
  (void)first;
  if (!replace && headers_.count(name.str())) return;
  headers_[name.str()] = value.str();
}

String HTTPClient::errorToString(int error) {
  switch (error) {
    case HTTPC_ERROR_CONNECTION_REFUSED: return String("connection refused");
    case HTTPC_ERROR_SEND_PAYLOAD_FAILED: return String("send payload failed");
    case HTTPC_ERROR_NOT_CONNECTED: return String("not connected");
    case HTTPC_ERROR_CONNECTION_LOST: return String("connection lost");
    case HTTPC_ERROR_READ_TIMEOUT: return String("read Timeout");
    default: return String();
  }
}

int HTTPClient::POST(uint8_t* payload, size_t size) {
  return POST(String(reinterpret_cast<const char*>(payload), static_cast<unsigned int>(size)));
}

int HTTPClient::POST(const String& payload) {
  response_.clear();
  if (!client_) return HTTPC_ERROR_NOT_CONNECTED;
  fakehttp::Request r{};
  r.timeMs = millis();
  r.context = registry().context;
  r.url = url_;
  if (!parseUrl(url_, r)) return HTTPC_ERROR_CONNECTION_REFUSED;
  r.headers = headers_;
  r.body = payload.str();
  auto* tls = dynamic_cast<WiFiClientSecure*>(client_);
  r.tlsInsecure = tls && tls->insecure();
  r.tlsPreconnected = tls && tls->connected();
  r.tlsHandshakeTimeoutS = tls ? tls->handshakeTimeoutS() : 0;
  r.reuse = reuse_;
  r.timeoutMs = timeoutMs_;
  r.connectTimeoutMs = connectTimeoutMs_;
  r.followRedirects = follow_;
  registry().requests.push_back(r);

  auto it = registry().servers.find(key(r.host, r.port));
  if (it == registry().servers.end()) return HTTPC_ERROR_CONNECTION_REFUSED;
  fakehttp::Response response = it->second->handle(r);
  if (response.code > 0) response_ = response.body;
  return response.code;
}

namespace fakehttp {

void registerServer(const std::string& host, int port, Server* server) { registry().servers[key(host, port)] = server; }
void unregisterServer(const std::string& host, int port) { registry().servers.erase(key(host, port)); }
const std::vector<Request>& requests() { return registry().requests; }
void setContext(const std::string& context) { registry().context = context; }
const std::string& context() { return registry().context; }

}  // namespace fakehttp
