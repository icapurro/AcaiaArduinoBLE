// Checks that the vendored ArduinoJson builds and behaves as the grinder
// client expects with the host toolchain.
#include <ArduinoJson.h>

#include "test.h"

TEST(arduinojson_is_the_pinned_version) { CHECK_EQ(std::string(ARDUINOJSON_VERSION), std::string("7.2.0")); }

TEST(arduinojson_reads_float_numbers_as_numbers) {
  JsonDocument doc;
  REQUIRE(deserializeJson(doc, "{\"status\":\"Ok\",\"n\":28000.0}") == DeserializationError::Ok);
  CHECK_EQ(std::string(doc["status"].as<const char*>()), std::string("Ok"));
  CHECK_EQ(static_cast<long>(doc["n"].as<double>()), 28000L);
  CHECK(doc["missing"].isNull());
}

TEST(arduinojson_serializes_to_a_char_buffer) {
  JsonDocument doc;
  doc["version"] = 1;
  doc["pairingId"] = 42u;
  char out[64];
  size_t n = serializeJson(doc, out, sizeof(out));
  CHECK_EQ(std::string(out, n), std::string("{\"version\":1,\"pairingId\":42}"));
}
