# shotStopper host tests

Runs the real `shotStopper.ino`, its `AcaiaArduinoBLE` library and the
`GrinderClient` on your computer, against fake Arduino, EEPROM, BLE, WiFi,
HTTPS and FreeRTOS layers. Nothing is flashed;
a full run takes a few seconds.

The Arduino IDE only compiles a sketch's top-level files and `src/`, so this
folder never ends up in the firmware.

## Running

Requirements: a C++17 compiler (clang or gcc) and `make`, on macOS or Linux.
No network or package install: ArduinoJson is vendored in `vendor/` (see
`vendor/VERSION.md`).

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
fakes/       Arduino.h, EEPROM.h, ArduinoBLE.h, WiFi.h, WiFiClientSecure.h,
             HTTPClient.h, WebServer.h, Update.h, freertos/*.h
support/     FakeScale (a BLE scale), FakePhone (a BLE config client),
             FakeGrinder (a Mahlkönig GbS grinder's Generic WiFi API)
vendor/      pinned third-party headers (ArduinoJson)
library/     tests for ../AcaiaArduinoBLE.cpp and ../GrinderClient.cpp, plus
             self-tests for the fakes
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
- Production files (the sketch, `AcaiaArduinoBLE.cpp`, `GrinderClient.cpp`)
  build as `gnu++11`, like the ESP32 Arduino 2.x core, so C++14/17-only code
  fails here before it fails in the IDE. Harness code is C++17.

## Simulated time

`millis()` starts at 0 and only moves when:

- the code calls `delay()` (the sketch's `loop()` does this on every pass,
  through `checkOTAMode`);
- `BLE.available()` finds nothing new (10 ms per call, so the library's
  100 ms scale scan finishes);
- a test calls `fake::advance(ms)`, `runFor(ms)` or `runUntil(cond, ms)`.

## The grinder client's task

On the device, `GrinderClient` runs in its own FreeRTOS task on core 0. Here
`xTaskCreatePinnedToCore` only records the task, and the scenario helper
`step()` runs one `loop()` iteration, then one `grinder.runOnce()`, on the same
clock. Every HTTP request is tagged with who sent it (`"loop"` or `"grinder"`),
so tests can check that `loop()` never talks to the grinder.

Two consequences:

- Requests take no simulated time unless a test says so.
  `runLoopDuringRequests(grinder, ms)` makes each request "hang" for `ms` while
  `loop()` keeps running, the way core 1 does while core 0 waits. Use it for
  timing-isolation tests, or to let the phone see a status that only exists
  while a request is in flight.
- While `loop()` blocks (a scale scan, ~100 ms, or the momentary 300 ms
  pulse), the grinder client doesn't run, though on the device it would.
  Grinder scenarios usually include a `FakeScale` anyway.

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
- **FakeGrinder** (registers itself at `192.168.1.30:30001`, pairingId 42 by
  default): `addJob`, `failNext(count, Fault, operation)`, `unreachable`,
  `onRequest`, `received(operation)`, `count`, `dones()`, `isTaken`, `exists`.
  It drops untaken jobs after 60 s and re-lists taken jobs every 50 s, in
  simulated time. Sketch helpers: `preloadGrinder(g)`, `bootWithGrinder(scale, g)`,
  `runLoopDuringRequests(g, ms)`.
- **HTTP / WiFi / tasks**: `fakehttp::requests()` (with URL, headers, body,
  TLS settings and sender context), `fake::setWifiConnectsOnBegin`,
  `fake::setWifiStatus`, `fake::tasks()`.
- **BLE stack** (namespace `fakeble::`): `addPeripheral`, `setBeginResults`,
  `beginCallTimes`, `scanCount`, `advertiseCount`.

Put tests that need only the library in `library/test_*.cpp`, and anything
that runs `setup()`/`loop()` in `sketch/test_*.cpp`; the Makefile picks up new
files automatically.

## Known behavior pinned by tests

Some tests document current behavior that looks like a bug, so a fix shows up
as a deliberate test change:

- `disconnect_drops_the_link_but_isConnected_stays_true`: the library's
  `disconnect()` doesn't clear `isConnected()`.
- `ssid_of_32_chars_erases_the_stored_password`: a 32-character SSID's
  terminator lands on the first byte of the password.

## Limits

This covers logic and the exact bytes sent over BLE. It does not cover the
ESP32 radio or BLE stack, real timing, boot or memory problems on the board,
or the OTA HTTP upload, so still flash and pull a shot before a release.
