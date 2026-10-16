// Host-side stub of the ESP32 WiFi library: never connects unless a test says so.
#pragma once

#include "Arduino.h"

typedef enum {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_DISCONNECTED = 6,
} wl_status_t;

class WiFiClass {
 public:
  wl_status_t begin(const String& ssid, const String& pass);
  wl_status_t status();
  IPAddress localIP();
  bool disconnect(bool wifiOff = false, bool eraseAp = false);
};

extern WiFiClass WiFi;

namespace fake {

void setWifiStatus(wl_status_t status);
void setWifiIp(const IPAddress& ip);
int wifiBeginCount();
String wifiLastSsid();

}  // namespace fake
