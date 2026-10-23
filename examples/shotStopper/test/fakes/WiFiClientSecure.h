// Host-side fake of the ESP32 WiFiClientSecure: carries the TLS settings that
// the fake HTTPClient records with each request.
#pragma once

#include "WiFi.h"

class WiFiClientSecure : public WiFiClient {
 public:
  void setInsecure() { insecure_ = true; }
  bool insecure() const { return insecure_; }
  void setHandshakeTimeout(unsigned long seconds) { handshakeTimeoutS_ = seconds; }
  unsigned long handshakeTimeoutS() const { return handshakeTimeoutS_; }
  // Succeeds when a fake server is registered at host:port (see HTTPClient.h).
  int connect(const char* host, uint16_t port, int32_t timeoutMs);
  bool connected() const { return connected_; }
  int32_t connectTimeoutMs() const { return connectTimeoutMs_; }

 private:
  bool insecure_ = false;
  bool connected_ = false;
  unsigned long handshakeTimeoutS_ = 120;  // the ESP32 core's default
  int32_t connectTimeoutMs_ = 0;
};
