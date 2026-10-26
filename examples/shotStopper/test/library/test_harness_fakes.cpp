// Self-tests for the fakes the other suites rely on.
#include <Arduino.h>
#include <ArduinoBLE.h>
#include <EEPROM.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"  // host size_t is 64-bit; see Makefile
#include "../../OTASetup.h"  // must compile unchanged against the stubs
#pragma GCC diagnostic pop
#include "fake_phone.h"
#include "fake_scale.h"
#include "test.h"

// ---------------------------------------------------------------- clock / GPIO / Serial

TEST(clock_starts_at_zero_and_delay_advances_it) {
  CHECK_EQ(millis(), 0UL);
  fake::advance(1000);
  delay(300);
  CHECK_EQ(millis(), 1300UL);
  CHECK_EQ(micros(), 1300000UL);
}

TEST(output_pin_history_records_changes_with_timestamps) {
  pinMode(11, OUTPUT);
  digitalWrite(11, LOW);
  fake::advance(50);
  digitalWrite(11, HIGH);
  digitalWrite(11, HIGH);  // no change, not recorded
  fake::advance(25);
  digitalWrite(11, LOW);
  const auto& h = fake::outputHistory(11);
  REQUIRE(h.size() == 3);
  CHECK_EQ(h[1].timeMs, 50UL);
  CHECK_EQ(h[1].value, HIGH);
  CHECK_EQ(h[2].timeMs, 75UL);
  CHECK_EQ(fake::outputLevel(11), LOW);
}

TEST(inputs_read_pullup_high_until_driven) {
  pinMode(10, INPUT_PULLUP);
  CHECK_EQ(digitalRead(10), HIGH);
  fake::setInput(10, LOW);
  CHECK_EQ(digitalRead(10), LOW);
}

TEST(abs_keeps_float_fraction) {
  CHECK_EQ(abs(-1.5f), 1.5f);
  CHECK_EQ(abs(-2), 2);
}

TEST(serial_output_is_captured) {
  Serial.print("weight ");
  Serial.println(36.5f);
  Serial.print(255, HEX);
  Serial.println((uint8_t)7);
  Serial.printf("n=%d\n", 3);
  CHECK_EQ(fake::serialOutput(), std::string("weight 36.50\r\nFF7\r\nn=3\n"));
  CHECK(fake::serialContains("36.50"));
  fake::clearSerial();
  CHECK(fake::serialOutput().empty());
}

// ---------------------------------------------------------------- String

TEST(string_construction_and_comparison) {
  CHECK(String("abc") == "abc");
  CHECK(String() == "");
  CHECK(String("abc") != String("abd"));
  CHECK_EQ(String(42).str(), std::string("42"));
  CHECK_EQ(String(-7L).str(), std::string("-7"));
  CHECK_EQ(String(255UL, HEX).str(), std::string("FF"));
  CHECK_EQ(String(1.5f).str(), std::string("1.50"));
}

TEST(string_from_buffer_with_length_is_not_null_terminated) {
  const uint8_t buf[] = {'h', 'o', 'm', 'e', 'X', 'X'};
  String s((const char*)buf, 4);
  CHECK_EQ(s.length(), 4u);
  CHECK(s == "home");
}

TEST(string_substring_index_and_concatenation) {
  String s("PYXIS-1234");
  CHECK(s.substring(0, 5) == "PYXIS");
  CHECK(s.substring(6) == "1234");
  CHECK(String("ab").substring(0, 5) == "ab");
  CHECK_EQ(s[0], 'P');
  CHECK_EQ(s[100], '\0');
  String joined = "ip " + String("1.2.3.4") + ", heap " + 100u + "}";
  CHECK(joined == "ip 1.2.3.4, heap 100}");
  CHECK(IPAddress(10, 0, 0, 7).toString() == "10.0.0.7");
}

// ---------------------------------------------------------------- EEPROM

TEST(eeprom_writes_reach_flash_only_on_commit) {
  fake::eepromPreload(2, 42);
  EEPROM.begin(75);
  CHECK_EQ(EEPROM.read(2), (uint8_t)42);
  CHECK_EQ(EEPROM.read(3), (uint8_t)0xFF);  // erased
  EEPROM.write(2, 40);
  CHECK_EQ(fake::eepromFlash(2), (uint8_t)42);
  EEPROM.commit();
  CHECK_EQ(fake::eepromFlash(2), (uint8_t)40);
  CHECK_EQ(fake::eepromCommitCount(), 1);
  CHECK_EQ(fake::eepromBeginSize(), (size_t)75);
  EEPROM.write(80, 1);  // out of range: ignored
  CHECK_EQ(EEPROM.read(80), (uint8_t)0);
}

// ---------------------------------------------------------------- WiFi / OTA stubs

TEST(ota_helpers_run_against_stubs) {
  CHECK(getWifiIp() == "disconnected");
  CHECK(checkOTAMode(true, String("home"), String("pw")));
  CHECK_EQ(fake::wifiBeginCount(), 1);
  CHECK(fake::wifiLastSsid() == "home");
  fake::setWifiStatus(WL_CONNECTED);
  checkOTAMode(true, String("home"), String("pw"));
  CHECK(getWifiIp() == "192.168.1.50");
  CHECK(!checkOTAMode(false, String("home"), String("pw")));
  CHECK(getWifiIp() == "disconnected");
}

// ---------------------------------------------------------------- fake BLE

TEST(local_characteristics_written_by_a_central) {
  BLE.begin();
  BLEService service("0x0FFE");
  BLEByteCharacteristic weight("0xFF11", BLEWrite | BLERead);
  BLECharacteristic ssid("0xFF22", BLEWrite | BLERead, 4);
  BLEByteCharacteristic status("0xFF19", BLERead | BLENotify);
  service.addCharacteristic(weight);
  service.addCharacteristic(ssid);
  service.addCharacteristic(status);
  BLE.addService(service);

  FakePhone phone;
  CHECK(phone.exists("0xff11"));
  CHECK(!phone.exists("0xFF99"));
  CHECK(!weight.written());
  CHECK(phone.writeByte("0xFF11", 40));
  CHECK(weight.written());
  CHECK(!weight.written());  // cleared once read
  CHECK_EQ(weight.value(), (uint8_t)40);

  CHECK(phone.writeString("0xFF22", "toolong"));  // truncated to 4 bytes
  CHECK_EQ(phone.readString("0xFF22"), std::string("tool"));
  CHECK(!phone.writeByte("0xFF19", 1));  // read-only

  status.writeValue(0);
  fake::advance(10);
  status.writeValue(1);
  status.writeValue(1);
  CHECK_EQ(phone.history("0xFF19").size(), (size_t)3);
  CHECK_EQ(phone.byteChanges("0xFF19"), (std::vector<int>{0, 1}));
  CHECK_EQ(fakeble::valueHistory(status)[1].timeMs, 10UL);
}

static int g_disconnects = 0;
static void onDisconnect(BLEDevice) { g_disconnects++; }

TEST(phone_disconnect_fires_the_event_handler) {
  FakePhone phone;
  CHECK(!phone.disconnect());
  BLE.setEventHandler(BLEDisconnected, onDisconnect);
  CHECK(phone.disconnect());
  CHECK_EQ(g_disconnects, 1);
}

TEST(begin_results_are_scriptable) {
  fakeble::setBeginResults({false, true});
  CHECK_EQ(BLE.begin(), 0);
  fake::advance(5);
  CHECK_EQ(BLE.begin(), 1);
  CHECK_EQ(BLE.begin(), 1);
  CHECK_EQ(fakeble::beginCallTimes(), (std::vector<unsigned long>{0, 5, 5}));
}

TEST(scan_reports_each_advertising_scale_once_and_waits_otherwise) {
  FakeScale a(ScaleType::NEW, "PYXIS-A", "aa:00:00:00:00:01");
  FakeScale b(ScaleType::OLD, "LUNAR-B", "AA:00:00:00:00:02");
  fakeble::addPeripheral(&a);
  fakeble::addPeripheral(&b);
  BLE.begin();
  CHECK(!BLE.available());  // not scanning yet
  BLE.scan();
  CHECK(BLE.available().localName() == "PYXIS-A");
  CHECK(BLE.available().localName() == "LUNAR-B");
  unsigned long before = millis();
  CHECK(!BLE.available());
  CHECK_EQ(millis() - before, 10UL);
  BLE.scanForAddress(String("aa:00:00:00:00:02"));
  CHECK(BLE.available().localName() == "LUNAR-B");
  CHECK(!BLE.available());
}

TEST(remote_characteristics_exist_only_after_discovery) {
  FakeScale scale(ScaleType::NEW);
  fakeble::addPeripheral(&scale);
  BLE.begin();
  BLE.scan();
  BLEDevice device = BLE.available();
  REQUIRE(static_cast<bool>(device));
  CHECK(!device.characteristic(FakeScale::readUuid(ScaleType::NEW).c_str()));
  REQUIRE(device.connect());
  REQUIRE(device.discoverAttributes());
  BLEDevice copy = device;  // handles share state
  BLECharacteristic read = copy.characteristic("49535343-1E4D-4BD9-BA61-23C647249616");
  CHECK(read.canSubscribe());
  CHECK(!copy.characteristic("2a80"));
  CHECK(!scale.advertising());  // connected scales stop advertising
}

// ---------------------------------------------------------------- fake scale

TEST(fake_scale_records_writes_and_events) {
  FakeScale scale(ScaleType::GENERIC);
  fakeble::addPeripheral(&scale);
  BLE.begin();
  BLE.scan();
  BLEDevice device = BLE.available();
  REQUIRE(device.connect());
  REQUIRE(device.discoverAttributes());
  BLECharacteristic write = device.characteristic("ff12");
  BLECharacteristic notify = device.characteristic("ff11");
  CHECK(notify.subscribe());
  fake::advance(7);
  CHECK(write.writeValue(acaia::TARE_GENERIC.data(), 6));
  REQUIRE(scale.writes().size() == 1);
  CHECK_EQ(scale.writes()[0].timeMs, 7UL);
  CHECK_EQ(scale.writes()[0].bytes, acaia::TARE_GENERIC);
  CHECK_EQ(scale.events(), (std::vector<std::string>{"connect", "discover", "subscribe ff11", "write ff12"}));
  scale.failWrites = true;
  CHECK(!write.writeValue(acaia::TARE_GENERIC.data(), 6));
  device.disconnect();
  CHECK(!scale.connected());
  CHECK(!write.writeValue(acaia::TARE_GENERIC.data(), 6));
}

TEST(fake_scale_emits_packets_per_period_and_tare_zeroes_weight) {
  FakeScale scale(ScaleType::NEW);
  scale.setWeight([](unsigned long t) { return t / 100.0f; });  // 10 g/s
  fakeble::addPeripheral(&scale);
  BLE.begin();
  BLE.scan();
  BLEDevice device = BLE.available();
  REQUIRE(device.connect());
  REQUIRE(device.discoverAttributes());
  BLECharacteristic notify = device.characteristic(FakeScale::readUuid(ScaleType::NEW).c_str());
  BLECharacteristic write = device.characteristic(FakeScale::writeUuid(ScaleType::NEW).c_str());
  CHECK(!notify.valueUpdated());  // not subscribed yet
  REQUIRE(notify.subscribe());

  fake::advance(1200);  // raw weight 12.0 g
  CHECK(notify.valueUpdated());
  CHECK_EQ(Bytes(notify.value(), notify.value() + notify.valueLength()), FakeScale::encodeWeight(ScaleType::NEW, 12.0f));
  CHECK(!notify.valueUpdated());  // next packet not due yet

  write.writeValue(acaia::TARE.data(), 6);
  CHECK_NEAR(scale.tareOffset(), 12.0, 1e-6);
  fake::advance(100);  // raw 13.0 g, reported 1.0 g
  CHECK(notify.valueUpdated());
  CHECK_EQ(Bytes(notify.value(), notify.value() + notify.valueLength()), FakeScale::encodeWeight(ScaleType::NEW, 1.0f));

  scale.queuePacket({0x01, 0x02});
  CHECK(notify.valueUpdated());
  CHECK_EQ(notify.valueLength(), 2);
  scale.silent = true;
  fake::advance(1000);
  CHECK(!notify.valueUpdated());
}

TEST(fake_scale_encodes_each_protocol) {
  CHECK_EQ(FakeScale::encodeWeight(ScaleType::NEW, 123.4f),
           (Bytes{0xef, 0xdd, 0x0c, 0x08, 0x05, 0xd2, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}));
  CHECK_EQ(FakeScale::encodeWeight(ScaleType::NEW, -2.5f)[10], (uint8_t)0x02);
  CHECK_EQ(FakeScale::encodeWeight(ScaleType::OLD, 123.4f),
           (Bytes{0xef, 0xdd, 0xd2, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}));
  Bytes generic = FakeScale::encodeWeight(ScaleType::GENERIC, -123.45f);
  CHECK_EQ(generic.size(), (size_t)20);
  CHECK_EQ(generic[6], (uint8_t)'-');
  CHECK_EQ((generic[7] << 16) | (generic[8] << 8) | generic[9], 12345);
}

// ---------------------------------------------------------------- FreeRTOS / RNG / WiFi mode

static void dummyTask(void*) {}

TEST(task_creation_is_recorded_not_started) {
  int param = 7;
  TaskHandle_t handle = nullptr;
  CHECK_EQ(xTaskCreatePinnedToCore(dummyTask, "grinder", 16384, &param, 1, &handle, 0), pdPASS);
  CHECK(handle != nullptr);
  REQUIRE(fake::tasks().size() == 1);
  const auto& t = fake::tasks()[0];
  CHECK_EQ(t.name, std::string("grinder"));
  CHECK(t.function == dummyTask);
  CHECK(t.parameters == &param);
  CHECK_EQ(t.stackDepth, 16384u);
  CHECK_EQ(t.priority, 1u);
  CHECK_EQ(t.coreId, 0);
}

TEST(queue_is_fifo_and_rejects_when_full) {
  struct Item { int a; char b[3]; };
  QueueHandle_t q = xQueueCreate(2, sizeof(Item));
  Item one{1, "x"}, two{2, "y"}, three{3, "z"}, out{};
  CHECK_EQ(xQueueSend(q, &one, 0), pdTRUE);
  CHECK_EQ(xQueueSend(q, &two, 0), pdTRUE);
  CHECK_EQ(xQueueSend(q, &three, 0), errQUEUE_FULL);
  CHECK_EQ(uxQueueMessagesWaiting(q), 2u);
  CHECK_EQ(xQueueReceive(q, &out, 0), pdTRUE);
  CHECK_EQ(out.a, 1);
  CHECK_EQ(std::string(out.b), std::string("x"));
  CHECK_EQ(xQueueReceive(q, &out, 0), pdTRUE);
  CHECK_EQ(out.a, 2);
  CHECK_EQ(xQueueReceive(q, &out, 0), pdFALSE);
}

TEST(vtaskdelay_advances_the_clock) {
  vTaskDelay(pdMS_TO_TICKS(20));
  CHECK_EQ(millis(), 20UL);
}

TEST(esp_random_repeats_across_runs) {
  // Fixed seed: the same sequence in every (forked) test process.
  CHECK_EQ(esp_random(), 0xe124b63au);
  CHECK_EQ(esp_random(), 0x8b9a74abu);
}

TEST(wifi_mode_and_connect_on_begin) {
  CHECK_EQ(fake::wifiMode(), WIFI_OFF);
  WiFi.mode(WIFI_STA);
  CHECK_EQ(fake::wifiMode(), WIFI_STA);
  WiFi.begin(String("home"), String("pw"));
  CHECK_EQ(WiFi.status(), WL_DISCONNECTED);
  fake::setWifiConnectsOnBegin(true);
  WiFi.begin(String("home"), String("pw"));  // still connecting: refused, like the ESP32
  CHECK_EQ(fake::wifiBeginRejectedCount(), 1);
  CHECK_EQ(WiFi.status(), WL_DISCONNECTED);
  WiFi.disconnect();
  WiFi.begin(String("home"), String("pw"));
  CHECK_EQ(WiFi.status(), WL_CONNECTED);
}

// ---------------------------------------------------------------- HTTP

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

TEST(http_to_an_unregistered_host_is_refused_but_recorded) {
  WiFiClientSecure tls;
  HTTPClient http;
  REQUIRE(http.begin(tls, String("https://10.0.0.9:30001/grinderApi/brewJobs")));
  CHECK_EQ(http.POST(String("{}")), HTTPC_ERROR_CONNECTION_REFUSED);
  REQUIRE(fakehttp::requests().size() == 1);
  CHECK_EQ(fakehttp::requests()[0].host, std::string("10.0.0.9"));
  CHECK_EQ(fakehttp::requests()[0].port, 30001);
  CHECK_EQ(fakehttp::requests()[0].path, std::string("/grinderApi/brewJobs"));
}

namespace {
struct EchoServer : fakehttp::Server {
  fakehttp::Response handle(const fakehttp::Request& r) override { return {200, "echo:" + r.body}; }
};
}  // namespace

TEST(http_requests_reach_a_registered_server_with_settings) {
  EchoServer echo;
  fakehttp::registerServer("192.168.1.30", 30001, &echo);
  fake::advance(1234);
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  {
    fakehttp::ScopedContext ctx("grinder");
    REQUIRE(http.begin(tls, String("https://192.168.1.30:30001/x")));
    http.setReuse(false);
    http.setTimeout(8000);
    http.setConnectTimeout(8000);
    http.addHeader("Content-Type", "application/json");
    CHECK_EQ(http.POST(String("{\"version\":1}")), 200);
    CHECK(http.getString() == "echo:{\"version\":1}");
    CHECK_EQ(http.getSize(), 18);
    http.end();
  }
  CHECK_EQ(fakehttp::context(), std::string("test"));
  const auto& r = fakehttp::requests().at(0);
  CHECK_EQ(r.timeMs, 1234UL);
  CHECK_EQ(r.context, std::string("grinder"));
  CHECK_EQ(r.scheme, std::string("https"));
  CHECK_EQ(r.headers.at("Content-Type"), std::string("application/json"));
  CHECK(r.tlsInsecure);
  CHECK(!r.reuse);
  CHECK_EQ(r.timeoutMs, (uint16_t)8000);
  CHECK_EQ(r.followRedirects, HTTPC_DISABLE_FOLLOW_REDIRECTS);
}
