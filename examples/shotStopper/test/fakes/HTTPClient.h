// Host-side fake of the ESP32 HTTPClient. Requests are routed by host:port to
// a registered fakehttp::Server (e.g. support/fake_grinder.h) and recorded;
// with no server registered they fail like a refused connection.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "WiFi.h"

#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_SEND_PAYLOAD_FAILED (-3)
#define HTTPC_ERROR_NOT_CONNECTED (-4)
#define HTTPC_ERROR_CONNECTION_LOST (-5)
#define HTTPC_ERROR_READ_TIMEOUT (-11)

typedef enum {
  HTTPC_DISABLE_FOLLOW_REDIRECTS,
  HTTPC_STRICT_FOLLOW_REDIRECTS,
  HTTPC_FORCE_FOLLOW_REDIRECTS
} followRedirects_t;

class HTTPClient {
 public:
  bool begin(WiFiClient& client, const String& url);
  void end();
  void setReuse(bool reuse) { reuse_ = reuse; }
  void setTimeout(uint16_t timeoutMs) { timeoutMs_ = timeoutMs; }
  void setConnectTimeout(int32_t timeoutMs) { connectTimeoutMs_ = timeoutMs; }
  void setFollowRedirects(followRedirects_t follow) { follow_ = follow; }
  void addHeader(const String& name, const String& value, bool first = false, bool replace = true);
  int POST(const String& payload);
  int POST(uint8_t* payload, size_t size);
  int getSize() { return static_cast<int>(response_.size()); }
  String getString() { return String(response_); }
  static String errorToString(int error);

 private:
  WiFiClient* client_ = nullptr;
  std::string url_;
  std::map<std::string, std::string> headers_;
  bool reuse_ = true;  // the ESP32 core's default
  uint16_t timeoutMs_ = 5000;
  int32_t connectTimeoutMs_ = 5000;
  followRedirects_t follow_ = HTTPC_DISABLE_FOLLOW_REDIRECTS;
  std::string response_;
};

namespace fakehttp {

struct Request {
  unsigned long timeMs;
  std::string context;  // who sent it: see setContext()
  std::string url;
  std::string scheme;
  std::string host;
  int port;
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
  bool tlsInsecure;
  bool tlsPreconnected;             // the TLS client was connected before POST
  unsigned long tlsHandshakeTimeoutS;
  bool reuse;
  uint16_t timeoutMs;
  int32_t connectTimeoutMs;
  followRedirects_t followRedirects;
};

struct Response {
  int code;  // HTTP status, or a negative HTTPC_ERROR_* for transport failures
  std::string body;
};

class Server {
 public:
  virtual ~Server() = default;
  virtual Response handle(const Request& request) = 0;
};

void registerServer(const std::string& host, int port, Server* server);
void unregisterServer(const std::string& host, int port);

// Every request sent, in order.
const std::vector<Request>& requests();

// Label recorded with each request; scenario helpers set "loop" or "grinder"
// around loop() and the grinder client's iterations.
void setContext(const std::string& context);
const std::string& context();

class ScopedContext {
 public:
  explicit ScopedContext(const std::string& context) : previous_(fakehttp::context()) { setContext(context); }
  ~ScopedContext() { setContext(previous_); }

 private:
  std::string previous_;
};

}  // namespace fakehttp
