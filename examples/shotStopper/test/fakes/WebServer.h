// Host-side stub of the ESP32 WebServer: accepts route registration so
// OTASetup.h compiles, but never serves a request.
#pragma once

#include <functional>

#include "Arduino.h"

typedef enum { HTTP_ANY, HTTP_GET, HTTP_POST } HTTPMethod;

enum HTTPUploadStatus { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };

struct HTTPUpload {
  HTTPUploadStatus status = UPLOAD_FILE_START;
  String filename;
  size_t totalSize = 0;
  size_t currentSize = 0;
  uint8_t buf[1436] = {};
};

class WebServer {
 public:
  explicit WebServer(int port = 80) { (void)port; }
  void on(const String& uri, HTTPMethod method, std::function<void()> handler);
  void on(const String& uri, HTTPMethod method, std::function<void()> handler, std::function<void()> upload);
  void send(int code, const char* contentType, const String& content);
  void send_P(int code, const char* contentType, const char* content);
  void begin();
  void stop();
  void handleClient() {}
  HTTPUpload& upload() { return upload_; }

  int routeCount() const { return routes_; }
  bool running() const { return running_; }

 private:
  int routes_ = 0;
  bool running_ = false;
  HTTPUpload upload_;
};
