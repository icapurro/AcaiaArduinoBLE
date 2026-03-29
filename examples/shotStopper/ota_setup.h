#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

#define WIFI_SSID "shotStopperAP"
#define WIFI_PASS ""

bool otaMode = false;

AsyncWebServer server(80);

void restartESP(void *param) {
    delay(2000);  // Give client time to receive response
    ESP.restart();
}

void setupOTA() {
    // Serve a simple webpage for OTA update
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", "<html><body><h1>ESP32 OTA Update V6</h1>"
                                        "<form method='POST' action='/update' enctype='multipart/form-data'>"
                                        "<input type='file' name='update'><br>"
                                        "<input type='submit' value='Upload'></form></body></html>");
    });

    // OTA Upload Handler
    server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", "{\"status\": \"success\"}");
        xTaskCreate(restartESP, "restartTask", 1024, NULL, 1, NULL);
    }, [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!index) {
            Serial.printf("Start OTA Update: %s\n", filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        }
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
        }
        if (final) {
            if (Update.end(true)) {
                Serial.println("Update Complete!");
            } else {
                Update.printError(Serial);
            }
        }
    });
}

void startAPMode() {
    if (!otaMode) {
        // Start Wi-Fi in AP mode
        WiFi.softAP(WIFI_SSID, WIFI_PASS);
        Serial.print("AP Mode Started: ");
        Serial.println(WIFI_SSID);
        setupOTA();
        server.begin();
        Serial.println("Web Server Started on 192.168.4.1");
        otaMode = true;
    }
}

// Function to stop Wi-Fi AP mode
void stopAPMode() {
    if (otaMode) {
        WiFi.softAPdisconnect(true);
        server.end();
        Serial.println("AP Mode Stopped");
        otaMode = false;
    }
}

bool checkOTAMode(bool otaModeRequested) {
    if (otaModeRequested && !otaMode) {
        startAPMode();
    } else if (!otaModeRequested && otaMode) {
        stopAPMode();
    }
    return otaMode || otaModeRequested;
}
