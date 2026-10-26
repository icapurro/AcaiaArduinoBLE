#include "Arduino.h"

#include <cstdarg>
#include <cstdio>
#include <map>

HardwareSerial Serial;
EspClass ESP;

namespace {

unsigned long g_nowMs = 0;

struct PinState {
  int mode = -1;
  int input = -1;
  int output = -1;
  int analog = -1;
  std::vector<fake::PinEvent> history;
};

std::map<uint8_t, PinState>& pins() {
  static std::map<uint8_t, PinState> state;
  return state;
}

uint32_t g_cpuMhz = 0;

std::string& serialBuffer() {
  static std::string buffer;
  return buffer;
}

std::string formatInteger(unsigned long value, int base) {
  if (base != HEX && base != DEC) base = DEC;
  char buf[32];
  std::snprintf(buf, sizeof(buf), base == HEX ? "%lX" : "%lu", value);
  return buf;
}

}  // namespace

// ---------------------------------------------------------------- String

String::String(unsigned char value, unsigned char base) : String(static_cast<unsigned long>(value), base) {}
String::String(int value, unsigned char base) : String(static_cast<long>(value), base) {}
String::String(unsigned int value, unsigned char base) : String(static_cast<unsigned long>(value), base) {}
String::String(long value, unsigned char base) {
  s_ = value < 0 && base == DEC ? "-" + formatInteger(static_cast<unsigned long>(-value), base)
                                : formatInteger(static_cast<unsigned long>(value), base);
}
String::String(unsigned long value, unsigned char base) : s_(formatInteger(value, base)) {}
String::String(float value, unsigned int decimalPlaces) : String(static_cast<double>(value), decimalPlaces) {}
String::String(double value, unsigned int decimalPlaces) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", static_cast<int>(decimalPlaces), value);
  s_ = buf;
}

String String::substring(unsigned int from, unsigned int to) const {
  if (from > to) std::swap(from, to);
  if (from >= s_.size()) return String();
  if (to > s_.size()) to = static_cast<unsigned int>(s_.size());
  return String(s_.substr(from, to - from));
}

String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
String operator+(const String& a, const char* b) { String r(a); r += b; return r; }
String operator+(const char* a, const String& b) { String r(a); r += b; return r; }
String operator+(const String& a, char b) { String r(a); r += b; return r; }
String operator+(const String& a, int b) { return a + String(b); }
String operator+(const String& a, unsigned int b) { return a + String(b); }
String operator+(const String& a, long b) { return a + String(b); }
String operator+(const String& a, unsigned long b) { return a + String(b); }
String operator+(const String& a, double b) { return a + String(b); }
std::ostream& operator<<(std::ostream& out, const String& s) { return out << '"' << s.str() << '"'; }

String IPAddress::toString() const {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", octets_[0], octets_[1], octets_[2], octets_[3]);
  return String(buf);
}

// ---------------------------------------------------------------- Print / Serial

size_t Print::print(const char* s) { return write(reinterpret_cast<const uint8_t*>(s), std::strlen(s)); }
size_t Print::print(char c) { return write(reinterpret_cast<const uint8_t*>(&c), 1); }
size_t Print::print(long value, int base) { return print(String(value, static_cast<unsigned char>(base))); }
size_t Print::print(unsigned long value, int base) { return print(String(value, static_cast<unsigned char>(base))); }
size_t Print::print(double value, int digits) { return print(String(value, static_cast<unsigned int>(digits))); }

size_t Print::printf(const char* format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  int n = std::vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (n < 0) return 0;
  return write(reinterpret_cast<const uint8_t*>(buf), std::strlen(buf));
}

size_t HardwareSerial::write(const uint8_t* data, size_t size) {
  serialBuffer().append(reinterpret_cast<const char*>(data), size);
  static const bool echo = std::getenv("TEST_VERBOSE") != nullptr;
  if (echo) std::fwrite(data, 1, size, stderr);
  return size;
}

// ---------------------------------------------------------------- time / GPIO

unsigned long millis() { return g_nowMs; }
unsigned long micros() { return g_nowMs * 1000UL; }
void delay(unsigned long ms) { g_nowMs += ms; }
void delayMicroseconds(unsigned int us) { g_nowMs += us / 1000; }
void yield() {}

void pinMode(uint8_t pin, uint8_t mode) { pins()[pin].mode = mode; }

int digitalRead(uint8_t pin) {
  PinState& p = pins()[pin];
  if (p.input != -1) return p.input;
  return p.mode == INPUT_PULLUP ? HIGH : LOW;
}

void digitalWrite(uint8_t pin, uint8_t value) {
  PinState& p = pins()[pin];
  int level = value ? HIGH : LOW;
  if (p.output != level) p.history.push_back({g_nowMs, level});
  p.output = level;
}

void analogWrite(uint8_t pin, int value) { pins()[pin].analog = value; }

uint32_t esp_random() {
  static uint32_t state = 0x2545F491u;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

bool setCpuFrequencyMhz(uint32_t mhz) {
  g_cpuMhz = mhz;
  return true;
}

// ---------------------------------------------------------------- test controls

namespace fake {

unsigned long now() { return g_nowMs; }
void advance(unsigned long ms) { g_nowMs += ms; }

void setInput(uint8_t pin, int level) { pins()[pin].input = level ? HIGH : LOW; }
int pinModeOf(uint8_t pin) { return pins()[pin].mode; }
int outputLevel(uint8_t pin) { return pins()[pin].output; }
const std::vector<PinEvent>& outputHistory(uint8_t pin) { return pins()[pin].history; }
int analogValue(uint8_t pin) { return pins()[pin].analog; }
uint32_t cpuFrequencyMhz() { return g_cpuMhz; }

const std::string& serialOutput() { return serialBuffer(); }
bool serialContains(const std::string& text) { return serialBuffer().find(text) != std::string::npos; }
void clearSerial() { serialBuffer().clear(); }

}  // namespace fake
