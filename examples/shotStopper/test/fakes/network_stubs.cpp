#include "Update.h"
#include "WebServer.h"
#include "WiFi.h"

WiFiClass WiFi;
UpdateClass Update;

namespace {

wl_status_t g_status = WL_DISCONNECTED;
IPAddress g_ip(192, 168, 1, 50);
int g_beginCount = 0;
bool g_connectOnBegin = false;
bool g_connecting = false;  // begin() called, not connected yet, not disconnected
int g_beginRejected = 0;
bool g_autoReconnect = false;
wifi_mode_t g_mode = WIFI_OFF;
String g_lastSsid;

}  // namespace

wl_status_t WiFiClass::begin(const String& ssid, const String& pass) {
  (void)pass;
  g_beginCount++;
  if (g_connecting && g_status != WL_CONNECTED) {
    g_beginRejected++;  // the real station refuses new config mid-attempt
    Serial.println("E (fake) wifi:sta is connecting, cannot set config");
    return g_status;
  }
  g_lastSsid = ssid;
  g_connecting = true;
  if (g_connectOnBegin) g_status = WL_CONNECTED;
  return g_status;
}

bool WiFiClass::setAutoReconnect(bool autoReconnect) {
  g_autoReconnect = autoReconnect;
  return true;
}

bool WiFiClass::mode(wifi_mode_t mode) {
  g_mode = mode;
  return true;
}

wl_status_t WiFiClass::status() { return g_status; }
IPAddress WiFiClass::localIP() { return g_status == WL_CONNECTED ? g_ip : IPAddress(); }

bool WiFiClass::disconnect(bool wifiOff, bool eraseAp) {
  (void)wifiOff;
  (void)eraseAp;
  if (g_status == WL_CONNECTED) g_status = WL_DISCONNECTED;
  g_connecting = false;
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
void setWifiConnectsOnBegin(bool connects) { g_connectOnBegin = connects; }
wifi_mode_t wifiMode() { return g_mode; }
void setWifiIp(const IPAddress& ip) { g_ip = ip; }
int wifiBeginCount() { return g_beginCount; }
int wifiBeginRejectedCount() { return g_beginRejected; }
bool wifiAutoReconnect() { return g_autoReconnect; }
String wifiLastSsid() { return g_lastSsid; }

}  // namespace fake
