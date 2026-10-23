/*
  shotStopper.ino - Example of using an acaia scale to brew by weight with an espresso machine

  Immediately Connects to a nearby acaia scale, 
  tare's the scale when the "in" gpio is triggered (active low),
  and then triggers the "out" gpio to stop the shot once ( goalWeight - weightOffset ) is achieved.

  Tested on a Acaia Pyxis, Arduino Nano ESP32, ESP32-WROOM-DA, and La Marzocco GS3.
  On ESP32-WROOM-DA, do not use GPIO 2 or 25 for brew I/O (antenna switch lines).

  Note that only the EEPROM library only supports ESP32-based controllers.

  To set the Weight over BLE, use a BLE app such as LightBlue to connect
  to the "shotStopper" BLE device and read/write to the weight characteristic,
  otherwise the weight is defaulted to 36g.

  Grind-by-Sync (optional): reports each shot to a paired Mahlkonig E65/E80 GbS
  grinder over WiFi so it can adjust its grind. Pair the grinder elsewhere (e.g.
  the companion app), then over BLE set the WiFi SSID/password (0xFF22/0xFF23),
  the grinder IP (0xFF25) and the Pairing ID (0xFF26, 4 bytes little-endian).
  0xFF27 notifies the link status. See GrinderClient.h and the README.

  Requires the ArduinoJson library, version 7 (tested with 7.2.0).

  Created by Tate Mazer, 2023.

  Released under the MIT license.

  https://github.com/tatemazer/AcaiaArduinoBLE

*/

#include "AcaiaArduinoBLE.h"
#include <EEPROM.h>
#include <math.h>
#include "OTASetup.h"
#include "GrinderClient.h"

#define FIRMWARE_VERSION 3
#define MAX_OFFSET 5                // In case an error in brewing occured
#define BUTTON_READ_PERIOD_MS 5

#define EEPROM_SIZE 128
#define SIGNATURE_ADDR 0 // Use the first byte to store a magic number/signature to know if the memory has been initialized
#define ENABLED_ADDR 1
#define WEIGHT_ADDR 2 // Use the second byte of EEPROM to store the goal weight
#define OFFSET_ADDR 3 
#define MOMENTARY_ADDR 4 
#define REEDSWITCH_ADDR 5 
#define AUTOTARE_ADDR 6
#define MIN_SHOT_DURATION_S_ADDR 7
#define MAX_SHOT_DURATION_S_ADDR 8 
#define DRIP_DELAY_S_ADDR 9
#define WIFI_SSID_ADDR 10
#define WIFI_PASS_ADDR 42
#define GRINDER_SIGNATURE_ADDR 75 // Set once the grinder fields below exist (added after the first 75 bytes)
#define GRINDER_IP_ADDR 76        // 16 bytes: dotted IPv4, null-terminated
#define GRINDER_PAIRING_ID_ADDR 92 // 4 bytes, little-endian
#define SIGNATURE_VALUE 0xAA
#define GRINDER_SIGNATURE_VALUE 0xA5

#define DEBUG false
#define GRINDER_DEBUG true          // Log every grinder request/reply and shot outcome to Serial
#define WEIGHT_DEBUG false          // Log every scale weight packet (and, while brewing, timer and predicted end)

#define N 10                        // Number of datapoints used to calculate trend line


// Reduce CPU clock to lower V3 power draw (~130mA→100mA)
#define LOW_VOLTAGE false

// Board Hardware
#ifdef ARDUINO_ESP32S3_DEV
  #define LED_RED     46
  #define LED_BLUE    45
  #define LED_GREEN   47
  #define LED_BUILTIN 48
  #define IN          21
  #define OUT         38
  #define REED_IN     18
#elif defined(ARDUINO_ESP32_WROOM_DA)
  // GPIO 2 and 25 drive the RF antenna switch; using them for brew I/O breaks BLE/WiFi
  #define IN          26
  #define OUT         4
  #define REED_IN     9
#else //todo: find nano esp32 identifier
  //LED's are defined by framework
  #define IN          10
  #define OUT         11
  #define REED_IN     9
#endif

#define BUTTON_STATE_ARRAY_LENGTH 31
#define SHOT_ARRAY_CAPACITY 1000


// configuration variables
bool reedSwitch = false;            // Set to true if the brew state is being determined 
                                    // by a reed switch attached to the brew solenoid
bool momentary = false;             // Define brew switch style. 
                                    // True for momentary switches such as GS3 AV, Silvia Pro
                                    // false for latching switches such as Linea Mini/Micra
bool autoTare = true;               // Automatically tare when shot is started 
                                    // and 3 seconds after a latching switch brew 
                                    // (as defined by MOMENTARY)
uint8_t minShotDurationS = 3;       // Useful for flushing the group.
                                    // This ensure that the system will ignore
                                    // "shots" that last less than this duration
uint8_t maxShotDurationS = 50;      // Primarily useful for latching switches, since user
                                    // looses control of the paddle once the system
                                    // latches.
uint8_t dripDelayS = 3;             // Time after the shot ended to measure the final weight

bool otaModeRequested = false;      // Set to true if OTA mode is being requested.

bool enabled = true;                // The shotStopper status, if disabled it won't connect to 
                                    // the scale nor it will take over the brew.
String wifiSsid = "";
String wifiPass = "";
String grinderIp = "";              // Mahlkonig GbS grinder for Grind-by-Sync; "" = off
uint32_t grinderPairingId = 0;      // From pairing done outside the shotStopper

typedef enum {BUTTON, WEIGHT, TIME, UNDEF} ENDTYPE;

// RGB Colors {Red,Green,Blue}
int RED[3] = {255, 0, 0};
int GREEN[3] = {0, 255, 0};
int BLUE[3] = {0, 0, 255};
int MAGENTA[3] = {255, 0, 255};
int CYAN[3] = {0, 255, 255};
int YELLOW[3] = {255, 255, 0};
int WHITE[3] = {255, 255, 255};
int OFF[3] = {0,0,0};
int currentColor[3] = {0,0,0};

AcaiaArduinoBLE scale(DEBUG);
float currentWeight = 0;
uint8_t goalWeight = 0;      // Goal Weight to be read from EEPROM
float weightOffset = 0;
float error = 0;
int buttonArr[BUTTON_STATE_ARRAY_LENGTH];            // last 4 readings of the button

// button 
int in = reedSwitch ? REED_IN : IN;
bool buttonPressed = false; //physical status of button
bool buttonLatched = false; //electrical status of button
unsigned long lastButtonRead_ms = 0;
int newButtonState = 0;

struct Shot {
  float start_timestamp_s; // Relative to runtime
  float shotTimer;         // Reset when the final drip measurement is made
  float end_s;             // Number of seconds after the shot started
  float expected_end_s;    // Estimated duration of the shot
  float weight[SHOT_ARRAY_CAPACITY];      // A scatter plot of the weight measurements, along with time_s[]
  float time_s[SHOT_ARRAY_CAPACITY];      // Number of seconds after the shot starte
  int datapoints;          // Number of datapoitns in the scatter plot
  bool brewing;            // True when actively brewing, otherwise false
  ENDTYPE end;
};

//Initialize shot
Shot shot = {0,0,0,0,{},{},0,false,ENDTYPE::UNDEF};

//BLE peripheral device
BLEService shotStopperService("0x0FFE"); // create service
BLEByteCharacteristic enabledCharacteristic("0xFF10",  BLEWrite | BLERead);
BLEByteCharacteristic weightCharacteristic("0xFF11",  BLEWrite | BLERead);
BLEByteCharacteristic reedSwitchCharacteristic("0xFF12",  BLEWrite | BLERead);
BLEByteCharacteristic momentaryCharacteristic("0xFF13",  BLEWrite | BLERead);
BLEByteCharacteristic autoTareCharacteristic("0xFF14",  BLEWrite | BLERead);
BLEByteCharacteristic minShotDurationSCharacteristic("0xFF15",  BLEWrite | BLERead);
BLEByteCharacteristic maxShotDurationSCharacteristic("0xFF16",  BLEWrite | BLERead);
BLEByteCharacteristic dripDelaySCharacteristic("0xFF17",  BLEWrite | BLERead);
BLEByteCharacteristic firmwareVersionCharacteristic("0xFF18",  BLERead);
BLEByteCharacteristic scaleStatusCharacteristic("0xFF19",  BLERead | BLENotify);
BLEByteCharacteristic shotStatusCharacteristic("0xFF20",  BLERead | BLENotify);
BLEByteCharacteristic otaModeRequestedCharacteristic("0xFF21",  BLEWrite | BLERead);
BLECharacteristic wifiSsidCharacteristic("0xFF22",  BLEWrite | BLERead, 32);
BLECharacteristic wifiPassCharacteristic("0xFF23",  BLEWrite, 32);
BLECharacteristic wifiIpCharacteristic("0xFF24",  BLERead | BLENotify, 16);
BLECharacteristic grinderIpCharacteristic("0xFF25",  BLEWrite | BLERead, 16);
BLECharacteristic grinderPairingIdCharacteristic("0xFF26",  BLEWrite | BLERead, 4);
BLEByteCharacteristic grinderStatusCharacteristic("0xFF27",  BLERead | BLENotify);


enum ScaleStatus {
  STATUS_DISCONNECTED = 0,
  STATUS_CONNECTED = 1,
};

// ESP32 core 3.x frees the Bluetooth controller's memory before setup() unless
// a BT library claims it (ArduinoBLE doesn't), and BLE.begin() then crashes in
// esp_bt_controller_init. Claim it here. Core 2.x already returns true (and
// defines it non-weak on the classic ESP32), so only override it on 3.x.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
extern "C" bool btInUse() { return true; }
#endif
// (Kept below the type definitions: the Arduino IDE inserts its generated
// prototypes, e.g. calculateEndTime(Shot*), right before the first function.)

uint8_t lastScaleStatus = 255; // Invalid initial value to force first update
static uint8_t lastShotStatusValue = 0xFF; // Initialize with a value that is unlikely to be a valid status
String lastWifiIp = "empty";
bool bleReady = false;
unsigned long lastBleInitAttemptMs = 0;

// Grind-by-Sync: the grinder client runs in its own task (see GrinderClient.h);
// loop() only sends it events and publishes its status.
GrinderClient grinder;
uint8_t lastGrinderStatus = 0xFF;
bool grinderActive = false;
bool pendingGrinderSuccess = false;   // stopped by weight, waiting out the drip delay
unsigned long pendingGrinderAtMs = 0;
uint32_t pendingGrinderDurationMs = 0;

void updateScaleStatus(uint8_t newScaleStatus) {
  if (newScaleStatus != lastScaleStatus) {
    scaleStatusCharacteristic.writeValue(newScaleStatus);
    lastScaleStatus = newScaleStatus;
  }
}

void updateBLEShotStatus(bool brewing) {
  uint8_t newValue = brewing ? 1 : 0;
  if (newValue != lastShotStatusValue) {
    shotStatusCharacteristic.writeValue(newValue);
    lastShotStatusValue = newValue; // Update the last written value
  }
}

void updateBLEWifiIp() {
  String wifiIp = getWifiIp();
  if (wifiIp != lastWifiIp) {
    writeStringToCharacteristic(wifiIpCharacteristic, wifiIp);
    lastWifiIp = wifiIp;
  }
}

void sendGrinderConfig() {
  if (GRINDER_DEBUG) {
    GrinderClient::log("loop", "grinder config: ip=\"%s\" pairingId=%lu ssid=\"%s\"", grinderIp.c_str(),
                       (unsigned long)grinderPairingId, wifiSsid.c_str());
  }
  if (!grinder.configure(grinderIp.c_str(), grinderPairingId, wifiSsid.c_str(), wifiPass.c_str())) {
    Serial.println("grinder: event queue full, config dropped");
  }
}

void updateGrinderActive() {
  bool active = enabled && !otaModeRequested;
  if (active != grinderActive) {
    grinderActive = active;
    if (GRINDER_DEBUG) {
      GrinderClient::log("loop", "grinder %s", active ? "active" : "inactive (disabled or OTA)");
    }
    grinder.setActive(active);
    if (!active) {
      pendingGrinderSuccess = false;
    }
  }
}

void updateBLEGrinderStatus() {
  uint8_t status = grinder.status();
  if (status != lastGrinderStatus) {
    if (GRINDER_DEBUG) {
      GrinderClient::log("loop", "grinder status %d -> %d (0xFF27)", lastGrinderStatus, status);
    }
    grinderStatusCharacteristic.writeValue(status);
    lastGrinderStatus = status;
  }
}

void writePairingIdToCharacteristic(uint32_t pairingId) {
  uint8_t bytes[4] = {(uint8_t)(pairingId & 0xff), (uint8_t)((pairingId >> 8) & 0xff),
                      (uint8_t)((pairingId >> 16) & 0xff), (uint8_t)((pairingId >> 24) & 0xff)};
  grinderPairingIdCharacteristic.writeValue(bytes, 4);
}

void writeStringToEEPROM(int startAddress, const String &str) {
    int maxWritable = EEPROM_SIZE - startAddress - 1; // Keep room for null terminator
    if (maxWritable < 0) {
        return;
    }
    int length = str.length();
    if (length > maxWritable) {
        length = maxWritable;
    }
    for (int i = 0; i < length; i++) {
        EEPROM.write(startAddress + i, str[i]);
    }
    EEPROM.write(startAddress + length, '\0'); // Null-terminate the string
}

String readStringFromEEPROM(int startAddress, int maxLength) {
    char data[maxLength + 1]; // +1 for the null terminator
    int length = 0;

    for (int i = 0; i < maxLength; i++) {
        data[i] = EEPROM.read(startAddress + i);
        if (data[i] == '\0') {
            break;
        }
        length++;
    }
    data[length] = '\0'; // Ensure the string is null-terminated

    return String(data);
}

void writePairingIdToEEPROM(uint32_t pairingId) {
  for (int i = 0; i < 4; i++) {
    EEPROM.write(GRINDER_PAIRING_ID_ADDR + i, (pairingId >> (8 * i)) & 0xff);
  }
}

void writeGrinderDefaultsToEEPROM() {
  grinderIp = "";
  grinderPairingId = 0;
  EEPROM.write(GRINDER_SIGNATURE_ADDR, GRINDER_SIGNATURE_VALUE);
  writeStringToEEPROM(GRINDER_IP_ADDR, grinderIp);
  writePairingIdToEEPROM(grinderPairingId);
}

void writeStringToCharacteristic(BLECharacteristic& characteristic, const String& str) {
    const char* data = str.c_str();
    int length = str.length();
    characteristic.writeValue((const uint8_t*)data, length);
}

void loadOrInitEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(SIGNATURE_ADDR) != SIGNATURE_VALUE) {
    goalWeight = 36;
    weightOffset = 1.5;
    EEPROM.write(SIGNATURE_ADDR, SIGNATURE_VALUE);
    EEPROM.write(ENABLED_ADDR, enabled ? 1 : 0);
    EEPROM.write(WEIGHT_ADDR, goalWeight);
    EEPROM.write(OFFSET_ADDR, (uint8_t)(weightOffset * 10));
    EEPROM.write(MOMENTARY_ADDR, momentary ? 1 : 0);
    EEPROM.write(REEDSWITCH_ADDR, reedSwitch ? 1 : 0);
    EEPROM.write(AUTOTARE_ADDR, autoTare ? 1 : 0);
    EEPROM.write(MIN_SHOT_DURATION_S_ADDR, minShotDurationS);
    EEPROM.write(MAX_SHOT_DURATION_S_ADDR, maxShotDurationS);
    EEPROM.write(DRIP_DELAY_S_ADDR, dripDelayS);
    writeStringToEEPROM(WIFI_SSID_ADDR, wifiSsid);
    writeStringToEEPROM(WIFI_PASS_ADDR, wifiPass);
    writeGrinderDefaultsToEEPROM();
    EEPROM.commit();
    Serial.println("EEPROM initialized with defaults");
  } else {
    enabled = EEPROM.read(ENABLED_ADDR);
    goalWeight = EEPROM.read(WEIGHT_ADDR);
    weightOffset = EEPROM.read(OFFSET_ADDR) / 10.0;
    Serial.print("Goal Weight retrieved: ");
    Serial.println(goalWeight);
    Serial.print("Offset retrieved: ");
    Serial.println(weightOffset);
    momentary = EEPROM.read(MOMENTARY_ADDR) != 0;
    reedSwitch = EEPROM.read(REEDSWITCH_ADDR) != 0;
    in = reedSwitch ? REED_IN : IN;
    autoTare = EEPROM.read(AUTOTARE_ADDR) != 0;
    minShotDurationS = EEPROM.read(MIN_SHOT_DURATION_S_ADDR);
    maxShotDurationS = EEPROM.read(MAX_SHOT_DURATION_S_ADDR);
    dripDelayS = EEPROM.read(DRIP_DELAY_S_ADDR);
    wifiSsid = readStringFromEEPROM(WIFI_SSID_ADDR, 32);
    wifiPass = readStringFromEEPROM(WIFI_PASS_ADDR, 32);
    if (EEPROM.read(GRINDER_SIGNATURE_ADDR) != GRINDER_SIGNATURE_VALUE) {
      // Written by firmware without grinder support: add the grinder fields.
      writeGrinderDefaultsToEEPROM();
      EEPROM.commit();
      Serial.println("EEPROM migrated: grinder fields added");
    } else {
      grinderIp = readStringFromEEPROM(GRINDER_IP_ADDR, 15);
      if (!GrinderClient::isValidIPv4(grinderIp.c_str())) {
        grinderIp = "";
      }
      grinderPairingId = (uint32_t)EEPROM.read(GRINDER_PAIRING_ID_ADDR)
                       | ((uint32_t)EEPROM.read(GRINDER_PAIRING_ID_ADDR + 1) << 8)
                       | ((uint32_t)EEPROM.read(GRINDER_PAIRING_ID_ADDR + 2) << 16)
                       | ((uint32_t)EEPROM.read(GRINDER_PAIRING_ID_ADDR + 3) << 24);
    }
  }
}

void initializeBLE() {
  // Do not call BLE.end() after a failed BLE.begin(); it can trip heap assertions on ESP32
  if (!BLE.begin()) {
    Serial.println("BLE begin failed");
    bleReady = false;
    return;
  }
  bleReady = true;

  BLE.setLocalName("shotStopper");
  BLE.setDeviceName("shotStopper");
  BLE.setAdvertisedService(shotStopperService);
  shotStopperService.addCharacteristic(enabledCharacteristic);
  shotStopperService.addCharacteristic(weightCharacteristic);
  shotStopperService.addCharacteristic(momentaryCharacteristic);
  shotStopperService.addCharacteristic(reedSwitchCharacteristic);
  shotStopperService.addCharacteristic(autoTareCharacteristic);
  shotStopperService.addCharacteristic(minShotDurationSCharacteristic);
  shotStopperService.addCharacteristic(maxShotDurationSCharacteristic);
  shotStopperService.addCharacteristic(dripDelaySCharacteristic);
  shotStopperService.addCharacteristic(firmwareVersionCharacteristic);
  shotStopperService.addCharacteristic(scaleStatusCharacteristic);
  shotStopperService.addCharacteristic(shotStatusCharacteristic);
  shotStopperService.addCharacteristic(otaModeRequestedCharacteristic);
  shotStopperService.addCharacteristic(wifiSsidCharacteristic);
  shotStopperService.addCharacteristic(wifiPassCharacteristic);
  shotStopperService.addCharacteristic(wifiIpCharacteristic);
  shotStopperService.addCharacteristic(grinderIpCharacteristic);
  shotStopperService.addCharacteristic(grinderPairingIdCharacteristic);
  shotStopperService.addCharacteristic(grinderStatusCharacteristic);
  BLE.addService(shotStopperService);
  enabledCharacteristic.writeValue(enabled ? 1 : 0);
  weightCharacteristic.writeValue(goalWeight);
  momentaryCharacteristic.writeValue(momentary ? 1 : 0);
  reedSwitchCharacteristic.writeValue(reedSwitch ? 1 : 0);
  autoTareCharacteristic.writeValue(autoTare ? 1 : 0);
  minShotDurationSCharacteristic.writeValue(minShotDurationS);
  maxShotDurationSCharacteristic.writeValue(maxShotDurationS);
  dripDelaySCharacteristic.writeValue(dripDelayS);
  firmwareVersionCharacteristic.writeValue(FIRMWARE_VERSION);
  scaleStatusCharacteristic.writeValue(STATUS_DISCONNECTED);
  shotStatusCharacteristic.writeValue(0);
  otaModeRequestedCharacteristic.writeValue(otaModeRequested ? 1 : 0);
  writeStringToCharacteristic(wifiSsidCharacteristic, wifiSsid);
  writeStringToCharacteristic(wifiIpCharacteristic, lastWifiIp);
  writeStringToCharacteristic(grinderIpCharacteristic, grinderIp);
  writePairingIdToCharacteristic(grinderPairingId);
  lastGrinderStatus = grinder.status();
  grinderStatusCharacteristic.writeValue(lastGrinderStatus);
  BLE.advertise();
  Serial.println("Bluetooth® device active, waiting for connections...");
  BLE.setEventHandler(BLEDisconnected, blePeripheralDisconnectHandler);
}

void blePeripheralDisconnectHandler(BLEDevice central) {
  BLE.advertise();
}

void pollAndReadBLE() {
  if (!bleReady) {
    return;
  }
  bool updated = false;
  BLE.poll();
  if (enabledCharacteristic.written()) {
    enabled = enabledCharacteristic.value() != 0;
    EEPROM.write(ENABLED_ADDR, enabled ? 1 : 0);
    updated = true;
  }
  if (weightCharacteristic.written()) {
    Serial.print("goal weight updated from ");
    Serial.print(goalWeight);
    Serial.print(" to ");
    goalWeight = weightCharacteristic.value();
    Serial.println(goalWeight);
    EEPROM.write(WEIGHT_ADDR, goalWeight); //1 byte, 0-255
    updated = true;
    EEPROM.commit();
  }
  if (momentaryCharacteristic.written()) {
    momentary = momentaryCharacteristic.value() != 0;
    EEPROM.write(MOMENTARY_ADDR, momentary ? 1 : 0);
    updated = true;
  }
  if (reedSwitchCharacteristic.written()) {
    reedSwitch = reedSwitchCharacteristic.value() != 0;
    in = reedSwitch ? REED_IN : IN;
    EEPROM.write(REEDSWITCH_ADDR, reedSwitch ? 1 : 0);
    updated = true;
  }
  if (autoTareCharacteristic.written()) {
    autoTare = autoTareCharacteristic.value() != 0;
    EEPROM.write(AUTOTARE_ADDR, autoTare ? 1 : 0);
    updated = true;
  }
  if (minShotDurationSCharacteristic.written()) {
    minShotDurationS = minShotDurationSCharacteristic.value();
    EEPROM.write(MIN_SHOT_DURATION_S_ADDR, minShotDurationS);
    updated = true;
  }
  if (maxShotDurationSCharacteristic.written()) {
    maxShotDurationS = maxShotDurationSCharacteristic.value();
    EEPROM.write(MAX_SHOT_DURATION_S_ADDR, maxShotDurationS);
    updated = true;
  }
  if (dripDelaySCharacteristic.written()) {
    dripDelayS = dripDelaySCharacteristic.value();
    EEPROM.write(DRIP_DELAY_S_ADDR, dripDelayS);
    updated = true;
  }
  if (otaModeRequestedCharacteristic.written()) {
    otaModeRequested = otaModeRequestedCharacteristic.value() != 0;
    Serial.print("OTA mode requested: ");
    Serial.println(otaModeRequested);
  }
  bool grinderConfigChanged = false;
  if (wifiSsidCharacteristic.written()) {
    const uint8_t* ssidData = wifiSsidCharacteristic.value();
    int ssidLength = wifiSsidCharacteristic.valueLength();
    wifiSsid = String((const char*)ssidData, ssidLength);
    writeStringToEEPROM(WIFI_SSID_ADDR, wifiSsid);
    updated = true;
    grinderConfigChanged = true;
  }
  if (wifiPassCharacteristic.written()) {
    const uint8_t* passData = wifiPassCharacteristic.value();
    int passLength = wifiPassCharacteristic.valueLength();
    wifiPass = String((const char*)passData, passLength);
    writeStringToEEPROM(WIFI_PASS_ADDR, wifiPass);
    updated = true;
    grinderConfigChanged = true;
  }
  if (grinderIpCharacteristic.written()) {
    int length = grinderIpCharacteristic.valueLength();
    const char* data = (const char*)grinderIpCharacteristic.value();
    while (length > 0 && data[length - 1] == '\0') {
      length--;  // some apps send a trailing null
    }
    String ip = String(data, length);
    if (ip.length() == 0 || GrinderClient::isValidIPv4(ip.c_str())) {
      grinderIp = ip;
      writeStringToEEPROM(GRINDER_IP_ADDR, grinderIp);
      updated = true;
      grinderConfigChanged = true;
    } else {
      Serial.println("grinder: invalid IP address ignored");
    }
    writeStringToCharacteristic(grinderIpCharacteristic, grinderIp);
  }
  if (grinderPairingIdCharacteristic.written()) {
    if (grinderPairingIdCharacteristic.valueLength() == 4) {
      const uint8_t* b = grinderPairingIdCharacteristic.value();
      grinderPairingId = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
      writePairingIdToEEPROM(grinderPairingId);
      updated = true;
      grinderConfigChanged = true;
    } else {
      Serial.println("grinder: pairingId must be 4 bytes");
      writePairingIdToCharacteristic(grinderPairingId);
    }
  }
  if (updated) {
    EEPROM.commit();
  }
  if (grinderConfigChanged) {
    sendGrinderConfig();
  }
}

void setup() {
#if LOW_VOLTAGE
  setCpuFrequencyMhz(80);
#endif
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  Serial.setTxBufferSize(2048);  // long log lines overflowed the default 256-byte USB CDC buffer
#endif
  Serial.begin(9600);

  // If eeprom isn't initialized default to 36g/1.5g
  loadOrInitEEPROM();

  // initialize the GPIO hardware
  pinMode(in, INPUT_PULLUP);
  pinMode(OUT, OUTPUT);
  digitalWrite(OUT, LOW);
#ifdef LED_RED // ESP32-WROOM-DA has no onboard RGB LED
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
#endif
  setColor(OFF);

  // Start the grinder client on core 0, away from shot control
  grinder.begin();
  grinder.setDebug(GRINDER_DEBUG);
  sendGrinderConfig();
  updateGrinderActive();
  xTaskCreatePinnedToCore(GrinderClient::taskMain, "grinder", GrinderClient::kTaskStackBytes, &grinder,
                          GrinderClient::kTaskPriority, NULL, GrinderClient::kTaskCore);

  // initialize the BLE hardware
  initializeBLE();
}



void loop() {
  // Retry BLE startup if it failed in setup
  if (!bleReady && millis() - lastBleInitAttemptMs > 3000) {
    lastBleInitAttemptMs = millis();
    Serial.println("Retrying BLE init...");
    initializeBLE();
  }

  // Check for setpoint updates
  pollAndReadBLE();
  updateBLEShotStatus(shot.brewing);
  updateBLEWifiIp();
  updateGrinderActive();  // before the inactive branch below ends a shot
  updateBLEGrinderStatus();
  // If shotStopper is disabled or OTA mode is requested, stop the shot and disconnect from the scale
  if (!enabled || checkOTAMode(otaModeRequested, wifiSsid, wifiPass)) {
    if(shot.brewing){
      shot.end = ENDTYPE::TIME;
      shot.brewing = false;
      setBrewingState(false);
    }
    if (scale.isConnected()) {
      scale.disconnect();
      updateScaleStatus(STATUS_DISCONNECTED);
      setColor(RED);
    }
    return;
  }

  // Report a shot stopped by weight once the drip delay has passed, with the final weight
  if (pendingGrinderSuccess && millis() - pendingGrinderAtMs >= (unsigned long)dripDelayS * 1000UL) {
    pendingGrinderSuccess = false;
    float massMg = currentWeight * 1000.0f;
    uint32_t mass = massMg <= 0 ? 0 : (massMg >= 5000000.0f ? 5000000 : (uint32_t)lroundf(massMg));
    if (GRINDER_DEBUG) {
      GrinderClient::log("loop", "shot result to grinder: Success, %lu ms, %lu mg (%.1f g after drip delay)",
                         (unsigned long)pendingGrinderDurationMs, (unsigned long)mass, currentWeight);
    }
    grinder.shotFinished(ShotOutcome::Success, pendingGrinderDurationMs, mass);
  }

  // Connect to scale
  if (!bleReady) {
    setColor(RED);
    return;
  }

  if (!scale.isConnected()) {
    updateScaleStatus(STATUS_DISCONNECTED);
    setColor(RED);
    scale.init(); 
    currentWeight = 0;
    if(shot.brewing){
      setBrewingState(false);
    }
    if(scale.isConnected()){
      setColor(YELLOW);
      updateScaleStatus(STATUS_CONNECTED);
    } else {
      return;
    }
  } else {
    updateScaleStatus(STATUS_CONNECTED);
  }

  // Send a heartbeat message to the scale periodically to maintain connection
  if(scale.heartbeatRequired()){
    scale.heartbeat();
  }

  // always call newWeightAvailable to actually receive the datapoint from the scale,
  // otherwise getWeight() will return stale data
  if(scale.newWeightAvailable()){
    currentWeight = scale.getWeight();
    if (WEIGHT_DEBUG) {
      Serial.print(currentWeight);
    }

    if(!shot.brewing){
      setColor(GREEN);
    }

    // update shot trajectory
    if(shot.brewing){
      if (shot.datapoints < SHOT_ARRAY_CAPACITY) {
        shot.time_s[shot.datapoints] = seconds_f()-shot.start_timestamp_s;
        shot.weight[shot.datapoints] = currentWeight;
        shot.shotTimer = shot.time_s[shot.datapoints];
        shot.datapoints++;
      } else {
        // Prevent buffer overflow if brewing somehow runs longer than expected.
        shot.shotTimer = seconds_f()-shot.start_timestamp_s;
      }

      //get the likely end time of the shot
      calculateEndTime(&shot);
      if (WEIGHT_DEBUG) {
        Serial.print(" ");
        Serial.print(shot.shotTimer);
        Serial.print(" ");
        Serial.print(shot.expected_end_s);
      }
    }
    if (WEIGHT_DEBUG) {
      Serial.println();
    }
  }

  // Read button every period
  if(millis() > (lastButtonRead_ms + BUTTON_READ_PERIOD_MS) ){
    lastButtonRead_ms = millis();

    //push back for new entry
    for(int i = BUTTON_STATE_ARRAY_LENGTH - 2;i>=0;i--){
      buttonArr[i+1] = buttonArr[i];
    }
    buttonArr[0] = !digitalRead(in); //Active Low

    //only return 1 if contains 1
    // Also assume the button is off for a few milliseconds
    // after the shot is done, there can be residual noise
    // from the reed switch
    newButtonState = 0;
    for(int i=0; i<BUTTON_STATE_ARRAY_LENGTH; i++){
      if(buttonArr[i]){
        newButtonState = 1;          
      }
      //Serial.print(buttonArr[i]);
    }
    //Serial.println();
    if(reedSwitch && !shot.brewing && seconds_f() < (shot.start_timestamp_s + shot.end_s + 0.5)){
      newButtonState = 0;
    }
  }
  
  //button just pressed
  if(newButtonState && buttonPressed == false ){
    Serial.println("ButtonPressed");
    buttonPressed = true;
    if(!momentary){
      shot.brewing = true;
      setBrewingState(shot.brewing);
    }
  }
    
  // button held. Take over for the rest of the shot.
  else if(!momentary 
  && shot.brewing 
  && !buttonLatched 
  && (shot.shotTimer > minShotDurationS) 
  ){
    buttonLatched = true;
    Serial.println("Button Latched");
    digitalWrite(OUT,HIGH); Serial.println("wrote high");
    // Get the scale to beep to inform user.
    if(autoTare){
      scale.tare();
    }
  }

  //button released
  else if(!buttonLatched 
  && !newButtonState 
  && buttonPressed == true 
  ){
    Serial.println("Button Released");
    buttonPressed = false;
    shot.brewing = !shot.brewing;
    if(!shot.brewing){
      shot.end = ENDTYPE::BUTTON;
    }
    setBrewingState(shot.brewing);
  }

  //Max duration reached
  else if(shot.brewing && shot.shotTimer > maxShotDurationS ){
    shot.brewing = false;
    Serial.println("Max brew duration reached");
    shot.end = ENDTYPE::TIME;
    setBrewingState(shot.brewing);
  }

  //Blink LED while brewing
  if(shot.brewing){
    setColor( (millis()/1000)%2 ? GREEN : BLUE );
  }

  //End shot
  if(shot.brewing 
  && shot.shotTimer >= shot.expected_end_s
  && shot.shotTimer >  minShotDurationS
  ){
    Serial.println("weight achieved");
    shot.brewing = false;
    shot.end = ENDTYPE::WEIGHT;
    setBrewingState(shot.brewing); 
  }

  //Detect error of shot
  if(shot.start_timestamp_s
  && shot.end_s
  && currentWeight >= (goalWeight - weightOffset)
  && seconds_f() > shot.start_timestamp_s + shot.end_s + dripDelayS){
    shot.start_timestamp_s = 0;
    shot.end_s = 0;

    Serial.print("I detected a final weight of ");
    Serial.print(currentWeight);
    Serial.print("g. The goal was ");
    Serial.print(goalWeight);
    Serial.print("g with a negative offset of ");
    Serial.print(weightOffset);

    if( abs(currentWeight - goalWeight + weightOffset) > MAX_OFFSET ){
      Serial.print("g. Error assumed. Offset unchanged. ");
    }else{
      Serial.print("g. Next time I'll create an offset of ");
      weightOffset += currentWeight - goalWeight;
      Serial.print(weightOffset);
      EEPROM.write(OFFSET_ADDR, weightOffset*10); //1 byte, 0-255
      EEPROM.commit();
    }
    Serial.println();
  }
}

void setBrewingState(bool brewing){
  if(brewing){
    Serial.println("shot started");
    pendingGrinderSuccess = false;
    if (GRINDER_DEBUG) {
      GrinderClient::log("loop", "shot started: grinder keeps the job taken until it's reported");
    }
    grinder.shotStarted();
    shot.start_timestamp_s = seconds_f();
    shot.shotTimer = 0;
    shot.datapoints = 0;
    scale.resetTimer();
    scale.startTimer();
    if(autoTare){
      scale.tare();
    }
    Serial.println("Weight Timer End");
  }else{
    Serial.print("ShotEnded by ");
    switch (shot.end) {
      case ENDTYPE::TIME:
      Serial.println("time");
      break;
    case ENDTYPE::WEIGHT:
    Serial.println("weight");
      break;
    case ENDTYPE::BUTTON:
    Serial.println("button");
      break;
    case ENDTYPE::UNDEF:
      Serial.println("undef");
      break;
    }

    shot.end_s = seconds_f() - shot.start_timestamp_s;
    // Tell the grinder how the shot went. Only shots stopped by weight teach it;
    // a flush (shorter than minShotDurationS) keeps the job for the real shot.
    if (shot.end == ENDTYPE::WEIGHT) {
      pendingGrinderSuccess = true;
      pendingGrinderAtMs = millis();
      pendingGrinderDurationMs = (uint32_t)lroundf(shot.end_s * 1000.0f);
      if (GRINDER_DEBUG) {
        GrinderClient::log("loop", "shot stopped by weight after %.2f s; reporting to grinder after %u s drip delay",
                           shot.end_s, dripDelayS);
      }
    } else if (shot.end == ENDTYPE::BUTTON && shot.end_s < minShotDurationS) {
      if (GRINDER_DEBUG) {
        GrinderClient::log("loop", "flush (%.2f s < %u s): grinder job kept for the real shot", shot.end_s,
                           minShotDurationS);
      }
      grinder.shotFinished(ShotOutcome::Flush);
    } else {
      if (GRINDER_DEBUG) {
        GrinderClient::log("loop", "shot ended after %.2f s, not by weight: grinder told Canceled", shot.end_s);
      }
      grinder.shotFinished(ShotOutcome::Canceled);
    }
    scale.stopTimer();
    if(momentary &&
      (ENDTYPE::WEIGHT == shot.end || ENDTYPE::TIME == shot.end)){
      //Pulse button to stop brewing
      digitalWrite(OUT,HIGH);Serial.println("wrote high");
      delay(300);
      digitalWrite(OUT,LOW);Serial.println("wrote low");
    }else if(!momentary){
      buttonLatched = false;
      buttonPressed = false;
      Serial.println("Button Unlatched and not pressed");
      digitalWrite(OUT,LOW); Serial.println("wrote low");
    }
  } 

  // Reset
  shot.end = ENDTYPE::UNDEF;
}
void calculateEndTime(Shot* s){
  
  // Do not  predict end time if there aren't enough espresso measurements yet
  if ((s->datapoints < N) || (s->weight[s->datapoints-1] < 10)) {
    s->expected_end_s = maxShotDurationS;
  }
  else{
    //Get line of best fit (y=mx+b) from the last 10 measurements 
    float sumXY = 0, sumX = 0, sumY = 0, sumSquaredX = 0, m = 0, b = 0, meanX = 0, meanY = 0;

    for(int i = s->datapoints - N; i < s->datapoints; i++){
      sumXY+=s->time_s[i]*s->weight[i];
      sumX+=s->time_s[i];
      sumY+=s->weight[i];
      sumSquaredX += ( s->time_s[i] * s->time_s[i] );
    }

    float denominator = (N * sumSquaredX - (sumX * sumX));
    if (fabs(denominator) < 1e-6f) {
      s->expected_end_s = maxShotDurationS;
      return;
    }

    m = (N * sumXY - (sumX * sumY)) / denominator;
    if (fabs(m) < 1e-6f) {
      s->expected_end_s = maxShotDurationS;
      return;
    }

    meanX = sumX/N;
    meanY = sumY/N;
    b = meanY-m*meanX;

    //Calculate time at which goal weight will be reached (x = (y-b)/m)
    s->expected_end_s = (goalWeight - weightOffset - b)/m;
    if (!isfinite(s->expected_end_s) || s->expected_end_s < 0) {
      s->expected_end_s = maxShotDurationS;
    }
  }
}

float seconds_f(){
  return millis()/1000.0;
}

void setColor(int rgb[3]){
#ifdef LED_RED
  analogWrite(LED_RED,   255-rgb[0] );
  analogWrite(LED_GREEN, 255-rgb[1] );
  analogWrite(LED_BLUE,  255-rgb[2] );
#endif
  currentColor[0] = rgb[0];
  currentColor[1] = rgb[1];
  currentColor[2] = rgb[2];
}
