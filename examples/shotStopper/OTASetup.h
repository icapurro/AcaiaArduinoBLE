#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

#define WIFI_SSID "shotStopperAP"
#define WIFI_PASS ""

bool otaMode = false;

WebServer server(80);

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
   <head>
      <title>ShotStopper OTA Update</title>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <style>body{background-color:#dedcd3;font-family:Arial,sans-serif;display:flex;justify-content:center;align-items:center;height:100vh;margin:0}
      .container{background-color:#e9e7e0;padding:40px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,.1);text-align:center;width:400px;max-width:90%}
      h1{margin-bottom:10px}h2{margin-bottom:20px;color:#111110}input[type=file]{margin-bottom:20px}
      input[type=submit]{padding:10px 20px;border:none;background-color:#e84b29;color:#fff;font-size:16px;border-radius:6px;cursor:pointer}
      input[type=submit]:hover{background-color:#be3011}@media (max-width:600px){body{height:100%}
      .container{width:100%;height:100vh;max-width:100%;border-radius:0;box-shadow:none;padding:20px}}</style>
   </head>
   <body>
      <div class="container">
         <h1>ShotStopper</h1>
         <h2>Over The Air Update</h2>
         <form method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="update"><br><input type="submit" value="Upload"></form>
      </div>
   </body>
</html>
)rawliteral";

const char success_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
   <head>
      <title>ShotStopper OTA Update Success</title>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <style>body{background-color:#dedcd3;font-family:Arial,sans-serif;display:flex;justify-content:center;align-items:center;height:100vh;margin:0}
      .container{background-color:#e9e7e0;padding:40px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,.1);text-align:center;width:400px;max-width:90%}
      h1{margin-bottom:10px}h2{margin-bottom:20px;color:#111110}input[type=file]{margin-bottom:20px}
      input[type=submit]{padding:10px 20px;border:none;background-color:#e84b29;color:#fff;font-size:16px;border-radius:6px;cursor:pointer}
      input[type=submit]:hover{background-color:#be3011}@media (max-width:600px){body{height:100%}
      .container{width:100%;height:100vh;max-width:100%;border-radius:0;box-shadow:none;padding:20px}}</style>
   </head>
   <body>
      <div class="container">
         <h1>ShotStopper</h1>
         <h2>Over The Air Update</h2>
         <p>Update successful</p>
      </div>
   </body>
</html>
)rawliteral";


void setupOTA() {
    // Serve a simple webpage for OTA update
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", index_html);
    });

    server.on("/status", HTTP_GET, []() {
        server.send(200, "application/json", "{\"status\": \"running\", \"ip\": \"" + WiFi.localIP().toString() + ", \"freeHeap\": " + ESP.getFreeHeap() + "}");
    });

    // Define the OTA update route
    server.on("/update", HTTP_POST, []() {
        server.send(200, "text/html", success_html);
        delay(5000);  // Give client time to receive response
        ESP.restart();
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("Start OTA Update: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
            Serial.printf("Received %d bytes\n", upload.currentSize);
            delay(1); // Feed the watchdog
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.println("Update Complete!");
            } else {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            Serial.println("Update was aborted");
            Update.end();
        }
    });

    server.begin();
}

void connectToWifi(String wifiSsid, String wifiPass) {
    if (!otaMode) {
        if (wifiSsid == "") {
            Serial.println("No WiFi SSID provided");
            return;
        } 
        Serial.print("Connecting to WiFi: ");
        Serial.println(wifiSsid);
        WiFi.begin(wifiSsid, wifiPass);
        delay(1000);
        if (WiFi.status() == WL_CONNECTED) {
            // print LAN IP address
            Serial.println("Connected to WiFi");
            Serial.println(WiFi.localIP());
            setupOTA();
            server.begin();
            Serial.println("Web Server Started");
            otaMode = true;
        }
    }
}

String getWifiIp() {
    if (!otaMode) {
        return "disconnected";
    }
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "disconnected";
}

void disconnectFromWifi() {
    if (otaMode) {
        WiFi.disconnect(true);
        server.stop();
        Serial.println("AP Mode Stopped");
        otaMode = false;
    }
}

bool checkOTAMode(bool otaModeRequested, String wifiSsid, String wifiPass) {
    if (otaModeRequested && !otaMode) {
        connectToWifi(wifiSsid, wifiPass);
    } else if (!otaModeRequested && otaMode) {
        disconnectFromWifi();
    }
    server.handleClient();
    delay(1);
    return otaMode || otaModeRequested;
}
