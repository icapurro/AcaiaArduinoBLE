// Minimal test framework for the host-side shotStopper tests.
//
// Every TEST runs in its own fork()ed process (see test_main.cpp), so the
// sketch's globals, the library and all fakes start fresh for each test and a
// crash or hang only fails that one test.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace testfw {

struct TestCase {
  const char* name;
  void (*fn)();
  const char* file;
  int line;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*fn)(), const char* file, int line) {
    registry().push_back({name, fn, file, line});
  }
};

inline int& failureCount() {
  static int count = 0;
  return count;
}

inline void reportFailure(const char* file, int line, const std::string& message) {
  std::fprintf(stderr, "  %s:%d: %s\n", file, line, message.c_str());
  std::fflush(stderr);
  failureCount()++;
}

// Ends the current test immediately (used by REQUIRE).
[[noreturn]] inline void abortTest() {
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(1);
}

template <class T, class = void>
struct is_streamable : std::false_type {};
template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

inline std::string show(const std::vector<uint8_t>& bytes) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < bytes.size(); i++) {
    char hex[4];
    std::snprintf(hex, sizeof(hex), "%02x", bytes[i]);
    out << (i ? " " : "") << hex;
  }
  out << "]";
  return out.str();
}

template <class T>
std::string show(const T& value);

template <class T>
std::string show(const std::vector<T>& values) {
  std::string out = "{";
  for (size_t i = 0; i < values.size(); i++) out += (i ? ", " : "") + show(values[i]);
  return out + "}";
}

template <class T>
std::string show(const T& value) {
  if constexpr (std::is_integral<T>::value && sizeof(T) == 1 && !std::is_same<T, bool>::value) {
    return std::to_string(static_cast<int>(value));
  } else if constexpr (is_streamable<T>::value) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else {
    return "<unprintable>";
  }
}

}  // namespace testfw

#define TEST(name)                                                                  \
  static void name();                                                               \
  static testfw::Registrar name##_registrar(#name, name, __FILE__, __LINE__);       \
  static void name()

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) testfw::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ") failed"); \
  } while (0)

#define REQUIRE(cond)                                                               \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      testfw::reportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ") failed");       \
      testfw::abortTest();                                                          \
    }                                                                               \
  } while (0)

#define CHECK_EQ(actual, expected)                                                  \
  do {                                                                              \
    const auto check_a_ = (actual);                                                 \
    const auto check_e_ = (expected);                                               \
    if (!(check_a_ == check_e_))                                                    \
      testfw::reportFailure(__FILE__, __LINE__,                                     \
                            "CHECK_EQ(" #actual ", " #expected "): got " +          \
                                testfw::show(check_a_) + ", expected " +            \
                                testfw::show(check_e_));                            \
  } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                     \
  do {                                                                              \
    const double check_a_ = (actual);                                               \
    const double check_e_ = (expected);                                             \
    if (!(std::fabs(check_a_ - check_e_) <= (tolerance)))                           \
      testfw::reportFailure(__FILE__, __LINE__,                                     \
                            "CHECK_NEAR(" #actual ", " #expected ", " #tolerance    \
                            "): got " + testfw::show(check_a_) + ", expected " +    \
                                testfw::show(check_e_));                            \
  } while (0)
