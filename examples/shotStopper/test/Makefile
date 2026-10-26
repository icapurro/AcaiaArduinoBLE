# Host-side tests for the shotStopper sketch. See README.md.
#
#   make                 build and run all tests
#   make SANITIZE=0      same, without AddressSanitizer/UBSan
#   make build           build only
#   make clean

SKETCH_DIR := ..
BUILD      := build
OBJ        := $(BUILD)/obj
SANITIZE   ?= 1

# Production code (the sketch and its library copy) is compiled with -Wall only;
# harness code also gets -Wextra.
BASE_FLAGS    := -std=c++17 -g -O0 -MMD -MP -Ifakes -Isupport -Iframework -Isketch -Ivendor
HARNESS_FLAGS := $(BASE_FLAGS) -Wall -Wextra
# Production code builds as C++11, like the ESP32 Arduino 2.x core (gnu++11), so
# C++14/17-only code in the sketch fails here before it fails in the IDE.
PROD_FLAGS    := $(filter-out -std=c++17,$(BASE_FLAGS)) -std=gnu++11 -Wall
# Host-only noise from unmodified sketch code: size_t is 64-bit here (OTASetup's
# printf("%d", size_t)), and clang flags the VLA GCC accepts on the ESP32.
PROD_FLAGS    += -Wno-format -Wno-vla-cxx-extension
LDFLAGS       :=

ifeq ($(SANITIZE),1)
  SAN_FLAGS     := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
  HARNESS_FLAGS += $(SAN_FLAGS)
  PROD_FLAGS    += $(SAN_FLAGS)
  LDFLAGS       += $(SAN_FLAGS)
endif

HARNESS_SRCS := fakes/Arduino.cpp fakes/EEPROM.cpp fakes/ArduinoBLE.cpp fakes/network_stubs.cpp fakes/freertos_fake.cpp fakes/HTTPClient.cpp \
                support/fake_scale.cpp support/fake_phone.cpp support/fake_grinder.cpp framework/test_main.cpp
HARNESS_OBJS := $(HARNESS_SRCS:%.cpp=$(OBJ)/%.o)

# The sketch's own copy of the scale library and the grinder client, as the
# Arduino IDE builds them.
LIBRARY_OBJ := $(OBJ)/prod/AcaiaArduinoBLE.o
GRINDER_OBJ := $(OBJ)/prod/GrinderClient.o

LIBRARY_TEST_OBJS := $(patsubst %.cpp,$(OBJ)/%.o,$(wildcard library/test_*.cpp))
SKETCH_TEST_OBJS  := $(patsubst %.cpp,$(OBJ)/%.o,$(wildcard sketch/test_*.cpp)) $(OBJ)/prod/sketch_tu.o

# Compile-only check that the ESP32-WROOM-DA pin branch still builds.
WROOM_OBJ := $(OBJ)/prod/sketch_tu_wroom_da.o

.PHONY: all test build clean
all: test

build: $(BUILD)/library_tests $(BUILD)/sketch_tests $(WROOM_OBJ)

test: build
	@rc=0; \
	./$(BUILD)/library_tests || rc=1; \
	./$(BUILD)/sketch_tests || rc=1; \
	exit $$rc

$(BUILD)/library_tests: $(LIBRARY_TEST_OBJS) $(LIBRARY_OBJ) $(GRINDER_OBJ) $(HARNESS_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@

$(BUILD)/sketch_tests: $(SKETCH_TEST_OBJS) $(LIBRARY_OBJ) $(GRINDER_OBJ) $(HARNESS_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@

$(OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HARNESS_FLAGS) -c $< -o $@

$(LIBRARY_OBJ): $(SKETCH_DIR)/AcaiaArduinoBLE.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(PROD_FLAGS) -c $< -o $@

$(GRINDER_OBJ): $(SKETCH_DIR)/GrinderClient.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(PROD_FLAGS) -c $< -o $@

$(OBJ)/prod/sketch_tu.o: sketch/sketch_tu.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(PROD_FLAGS) -c $< -o $@

$(WROOM_OBJ): sketch/sketch_tu.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(PROD_FLAGS) -DARDUINO_ESP32_WROOM_DA -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
