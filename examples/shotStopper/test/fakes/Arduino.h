// Host-side fake of the Arduino ESP32 core, covering only what shotStopper,
// OTASetup.h and AcaiaArduinoBLE use. Test-side controls live in `fake::`.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

#include "freertos_fake.h"  // the ESP32 core's Arduino.h pulls in FreeRTOS too

// The ESP32 core's abs() keeps the fractional part of floats; the C library
// int abs() would silently truncate the sketch's offset-error check.
using std::abs;

typedef uint8_t byte;
typedef bool boolean;

#define PROGMEM
#define F(x) (x)

#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05

#define DEC 10
#define HEX 16

// Board LEDs as the Arduino Nano ESP32 core defines them. The sketch defines
// its own for ESP32S3_DEV, and the ESP32-WROOM-DA module has none.
#if !defined(ARDUINO_ESP32S3_DEV) && !defined(ARDUINO_ESP32_WROOM_DA)
#define LED_BUILTIN 13
#define LED_RED 14
#define LED_GREEN 15
#define LED_BLUE 16
#endif

// ---------------------------------------------------------------- String

class String {
 public:
  String() = default;
  String(const char* cstr) : s_(cstr ? cstr : "") {}
  String(const char* cstr, unsigned int length) : s_(cstr ? std::string(cstr, length) : std::string()) {}
  String(const std::string& str) : s_(str) {}
  explicit String(char c) : s_(1, c) {}
  explicit String(unsigned char value, unsigned char base = 10);
  explicit String(int value, unsigned char base = 10);
  explicit String(unsigned int value, unsigned char base = 10);
  explicit String(long value, unsigned char base = 10);
  explicit String(unsigned long value, unsigned char base = 10);
  explicit String(float value, unsigned int decimalPlaces = 2);
  explicit String(double value, unsigned int decimalPlaces = 2);

  unsigned int length() const { return static_cast<unsigned int>(s_.size()); }
  const char* c_str() const { return s_.c_str(); }
  String substring(unsigned int from) const { return substring(from, length()); }
  String substring(unsigned int from, unsigned int to) const;

  char operator[](unsigned int index) const { return index < s_.size() ? s_[index] : '\0'; }
  char& operator[](unsigned int index) {
    static char dummy;  // like Arduino's String: out-of-range writes go nowhere
    if (index >= s_.size()) return dummy = '\0';
    return s_[index];
  }

  bool operator==(const String& other) const { return s_ == other.s_; }
  bool operator==(const char* other) const { return s_ == (other ? other : ""); }
  bool operator!=(const String& other) const { return !(*this == other); }
  bool operator!=(const char* other) const { return !(*this == other); }

  String& operator+=(const String& other) { s_ += other.s_; return *this; }
  String& operator+=(const char* other) { s_ += (other ? other : ""); return *this; }
  String& operator+=(char c) { s_ += c; return *this; }

  const std::string& str() const { return s_; }

 private:
  std::string s_;
};

String operator+(const String& a, const String& b);
String operator+(const String& a, const char* b);
String operator+(const char* a, const String& b);
String operator+(const String& a, char b);
String operator+(const String& a, int b);
String operator+(const String& a, unsigned int b);
String operator+(const String& a, long b);
String operator+(const String& a, unsigned long b);
String operator+(const String& a, double b);
std::ostream& operator<<(std::ostream& out, const String& s);

// ---------------------------------------------------------------- IPAddress

class IPAddress {
 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : octets_{a, b, c, d} {}
  String toString() const;
  bool operator==(const IPAddress& o) const { return std::memcmp(octets_, o.octets_, 4) == 0; }

 private:
  uint8_t octets_[4] = {0, 0, 0, 0};
};

// ---------------------------------------------------------------- Print / Serial

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(const uint8_t* data, size_t size) = 0;

  size_t print(const char* s);
  size_t print(const String& s) { return print(s.c_str()); }
  size_t print(char c);
  size_t print(unsigned char value, int base = DEC) { return print(static_cast<unsigned long>(value), base); }
  size_t print(int value, int base = DEC) { return print(static_cast<long>(value), base); }
  size_t print(unsigned int value, int base = DEC) { return print(static_cast<unsigned long>(value), base); }
  size_t print(long value, int base = DEC);
  size_t print(unsigned long value, int base = DEC);
  size_t print(double value, int digits = 2);
  size_t print(const IPAddress& ip) { return print(ip.toString()); }

  size_t println() { return print("\r\n"); }
  template <class T>
  size_t println(const T& value) { size_t n = print(value); return n + println(); }
  template <class T>
  size_t println(const T& value, int format) { size_t n = print(value, format); return n + println(); }

  size_t printf(const char* format, ...) __attribute__((format(printf, 2, 3)));
};

class HardwareSerial : public Print {
 public:
  void begin(unsigned long) {}
  size_t setTxBufferSize(size_t size) { return size; }
  explicit operator bool() const { return true; }
  size_t write(const uint8_t* data, size_t size) override;
};

extern HardwareSerial Serial;

// ---------------------------------------------------------------- ESP

class EspClass {
 public:
  uint32_t getFreeHeap() { return 200000; }
  void restart() {}
};

extern EspClass ESP;

// ---------------------------------------------------------------- time / GPIO

unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield();

void pinMode(uint8_t pin, uint8_t mode);
int digitalRead(uint8_t pin);
void digitalWrite(uint8_t pin, uint8_t value);
void analogWrite(uint8_t pin, int value);
bool setCpuFrequencyMhz(uint32_t mhz);

// Hardware RNG on the ESP32; a fixed-seed xorshift here so runs repeat.
uint32_t esp_random();

// ---------------------------------------------------------------- test controls

namespace fake {

// Simulated clock. Only advances through delay(), blocking fakes, or advance().
unsigned long now();
void advance(unsigned long ms);

struct PinEvent {
  unsigned long timeMs;
  int value;
};

// Drive a digital input (HIGH/LOW). Unset inputs read HIGH with INPUT_PULLUP.
void setInput(uint8_t pin, int level);
int pinModeOf(uint8_t pin);   // -1 if pinMode() was never called
int outputLevel(uint8_t pin); // -1 if never written
// Every change of a digitalWrite() output, with its simulated time.
const std::vector<PinEvent>& outputHistory(uint8_t pin);
int analogValue(uint8_t pin); // -1 if never written
uint32_t cpuFrequencyMhz();   // 0 if never set

// Everything printed to Serial since start (or the last clearSerial()).
const std::string& serialOutput();
bool serialContains(const std::string& text);
void clearSerial();

}  // namespace fake
