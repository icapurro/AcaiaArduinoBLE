#include "Update.h"
#include "WebServer.h"
#include "WiFi.h"

WiFiClass WiFi;
UpdateClass Update;

namespace {

wl_status_t g_status = WL_DISCONNECTED;
IPAddress g_ip(192, 168, 1, 50);
int g_beginCount = 0;
String g_lastSsid;

}  // namespace

wl_status_t WiFiClass::begin(const String& ssid, const String& pass) {
  (void)pass;
  g_beginCount++;
  g_lastSsid = ssid;
  return g_status;
}

wl_status_t WiFiClass::status() { return g_status; }
IPAddress WiFiClass::localIP() { return g_status == WL_CONNECTED ? g_ip : IPAddress(); }

bool WiFiClass::disconnect(bool wifiOff, bool eraseAp) {
  (void)wifiOff;
  (void)eraseAp;
  if (g_status == WL_CONNECTED) g_status = WL_DISCONNECTED;
  return true;
}

void WebServer::on(const String& uri, HTTPMethod method, std::function<void()> handler) {
  (void)uri;
  (void)method;
  (void)handler;
  routes_++;
}

void WebServer::on(const String& uri, HTTPMethod method, std::function<void()> handler,
                   std::function<void()> upload) {
  (void)upload;
  on(uri, method, handler);
}

void WebServer::send(int code, const char* contentType, const String& content) {
  (void)code;
  (void)contentType;
  (void)content;
}

void WebServer::send_P(int code, const char* contentType, const char* content) {
  (void)code;
  (void)contentType;
  (void)content;
}

void WebServer::begin() { running_ = true; }
void WebServer::stop() { running_ = false; }

namespace fake {

void setWifiStatus(wl_status_t status) { g_status = status; }
void setWifiIp(const IPAddress& ip) { g_ip = ip; }
int wifiBeginCount() { return g_beginCount; }
String wifiLastSsid() { return g_lastSsid; }

}  // namespace fake
