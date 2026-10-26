// Host-side stub of the ESP32 WiFi library: never connects unless a test says so.
#pragma once

#include "Arduino.h"

typedef enum {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6,
} wl_status_t;

// Base of WiFiClientSecure, as in the ESP32 core (WiFiClient.h).
class WiFiClient {
 public:
  virtual ~WiFiClient() = default;
};

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;

class WiFiClass {
 public:
  bool mode(wifi_mode_t mode);
  wl_status_t begin(const String& ssid, const String& pass);
  wl_status_t status();
  IPAddress localIP();
  bool disconnect(bool wifiOff = false, bool eraseAp = false);
  bool setAutoReconnect(bool autoReconnect);
  int8_t RSSI() { return -55; }
};

extern WiFiClass WiFi;

namespace fake {

void setWifiStatus(wl_status_t status);
// When on, WiFi.begin() connects immediately (status becomes WL_CONNECTED).
void setWifiConnectsOnBegin(bool connects);
wifi_mode_t wifiMode();
void setWifiIp(const IPAddress& ip);
int wifiBeginCount();
// begin() calls refused because a connection attempt was still in progress
// (on the ESP32: "sta is connecting, cannot set config").
int wifiBeginRejectedCount();
bool wifiAutoReconnect();
String wifiLastSsid();

}  // namespace fake
