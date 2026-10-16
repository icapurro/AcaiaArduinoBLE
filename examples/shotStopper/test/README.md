# shotStopper host tests

Runs the real `shotStopper.ino` and its `AcaiaArduinoBLE` library on your
computer, against fake Arduino, EEPROM and BLE layers. Nothing is flashed;
a full run takes a few seconds.

The Arduino IDE only compiles a sketch's top-level files and `src/`, so this
folder never ends up in the firmware.

## Running

Requirements: a C++17 compiler (clang or gcc) and `make`, on macOS or Linux.

```sh
make -C examples/shotStopper/test        # from the repo root
# or, inside examples/shotStopper/test:
make
```

The run prints one line per test and a summary per binary, and exits non-zero
if anything fails.

| Command | What it does |
| --- | --- |
| `make` | build and run everything |
| `make build` | build only |
| `make SANITIZE=0` | build without AddressSanitizer/UBSan (faster, quieter) |
| `make clean` | remove `build/` |
| `./build/sketch_tests --list` | list test names |
| `./build/sketch_tests NAME` | run one test in-process (use under `lldb` to debug) |
| `TEST_VERBOSE=1 ./build/sketch_tests NAME` | also print the sketch's `Serial` output |
| `TEST_TIMEOUT=30 make` | per-test time limit in seconds (default 10) |

Each test runs in its own forked process, so sketch globals, EEPROM and BLE
state start fresh every time, and a crash or hang fails only that test.

## Layout

```
framework/   test runner: TEST, CHECK, CHECK_EQ, CHECK_NEAR, REQUIRE
fakes/       Arduino.h, EEPROM.h, ArduinoBLE.h, WiFi.h, WebServer.h, Update.h
support/     FakeScale (a BLE scale) and FakePhone (a BLE config client)
library/     tests for ../AcaiaArduinoBLE.cpp, plus self-tests for the fakes
sketch/      tests for ../shotStopper.ino
build/       output (gitignored)
```

- `sketch/sketch_tu.cpp` compiles the unmodified sketch from its real path.
- `sketch/sketch_prototypes.h` lists the function prototypes the Arduino IDE
  normally generates. If you add a function to the sketch that is used before
  it is defined, the build fails with "use of undeclared identifier": add the
  prototype there.
- The build also compiles the sketch with `-DARDUINO_ESP32_WROOM_DA` (compile
  only), so the WROOM-DA pin branch stays buildable.

## Simulated time

`millis()` starts at 0 and only moves when:

- the code calls `delay()` (the sketch's `loop()` does this on every pass,
  through `checkOTAMode`);
- `BLE.available()` finds nothing new (10 ms per call, so the 10 s scale scan
  finishes);
- a test calls `fake::advance(ms)`, `runFor(ms)` or `runUntil(cond, ms)`.

## Writing a scenario

```cpp
#include "scenario.h"

static bool latchingPump() { return brewInputActive() || outHigh(); }

TEST(my_scenario) {
  FakeScale scale(ScaleType::NEW);   // advertises as "PYXIS-FAKE"
  Espresso cup(latchingPump);        // 2 g/s while the pump runs
  scale.setWeight(cup.weight());
  bootConnected(scale);              // setup(), then loop() until connected

  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));  // shotStopper latches the pump on
  releaseBrew();
  REQUIRE(runUntil(outLow, 40000));  // ...and stops it at the target weight

  CHECK_EQ(lastShotEndReason(), std::string("weight"));
}
```

Useful helpers:

- **Sketch** (`sketch/scenario.h`): `boot`, `step`, `runFor`, `runUntil`,
  `pressBrew`/`releaseBrew`, `outHigh`, `outHistory`, `outChangedAt`,
  `lastShotEndReason`, `detectedFinalWeight`, `preloadConfig(StoredConfig)`,
  `eepromFlashString`. Sketch globals and `#define`s are available through
  `sketch/sketch_api.h` (`goalWeight`, `sketch::kPinOut`, ...).
- **Pins, Serial, EEPROM** (`fakes/*.h`, namespace `fake::`): `setInput`,
  `outputLevel`, `outputHistory`, `serialContains`, `eepromPreload`,
  `eepromFlash`, `setWifiStatus`.
- **FakeScale**: `failConnect`, `failDiscover`, `failSubscribe`, `failWrites`,
  `silent`, `visible`, `packetPeriodMs`, `queuePacket`, `writes()`,
  `countWrites`, `events()`. Tare commands zero its reported weight.
- **FakePhone**: `writeByte`/`writeString` by UUID (`"0xFF11"`), `readByte`,
  `history`, `byteChanges`, `disconnect`.
- **BLE stack** (namespace `fakeble::`): `addPeripheral`, `setBeginResults`,
  `beginCallTimes`, `scanCount`, `advertiseCount`.

Put tests that need only the library in `library/test_*.cpp`, and anything
that runs `setup()`/`loop()` in `sketch/test_*.cpp`; the Makefile picks up new
files automatically.

## Known behavior pinned by tests

Some tests document current behavior that looks like a bug, so a fix shows up
as a deliberate test change:

- `rejected_scale_names_including_felicita`: the name filter has no Felicita
  prefix.
- `disconnect_drops_the_link_but_isConnected_stays_true`: the library's
  `disconnect()` doesn't clear `isConnected()`.
- `ssid_of_32_chars_erases_the_stored_password`: a 32-character SSID's
  terminator lands on the first byte of the password.

## Limits

This covers logic and the exact bytes sent over BLE. It does not cover the
ESP32 radio or BLE stack, real timing, boot or memory problems on the board,
or the OTA HTTP upload, so still flash and pull a shot before a release.
