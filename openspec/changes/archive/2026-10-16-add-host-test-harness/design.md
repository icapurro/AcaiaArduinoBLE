## Context

See proposal.md for the motivation and specs/host-test-harness/spec.md for the required behavior.

These facts about the current code shape the approach:

- **The sketch is a single `.ino` built on global variables.** `examples/shotStopper/shotStopper.ino` keeps all state in globals, so it can only be reset by starting a new process. It also calls functions before they are defined (`setBrewingState`, `calculateEndTime`, `seconds_f`, `setColor`, `blePeripheralDisconnectHandler`). This works in the Arduino IDE only because the IDE generates the missing prototypes.
- **Its quoted includes resolve next to the sketch.** The sketch includes `"AcaiaArduinoBLE.h"` and `"OTASetup.h"` with quotes, so they always resolve to the files in `examples/shotStopper/`, ahead of any include path the test build sets. The fakes therefore can't replace those two files. They have to replace what those files include: `<ArduinoBLE.h>`, `<WiFi.h>`, `<WebServer.h>`, `<Update.h>`, and `Arduino.h`.
- **`OTASetup.h` has no include guard** and defines globals, including `WebServer server(80)`. It must be compiled exactly once, through the sketch.
- **ArduinoBLE handles are passed by value.** `BLEDevice`, `BLECharacteristic` and `BLEService` are copied freely (`BLEDevice peripheral = BLE.available()`, `_write = peripheral.characteristic(...)`). The same class serves both remote scale characteristics and local peripheral characteristics.
- **Some code loops on the clock.** `AcaiaArduinoBLE::init()` busy-waits on `millis()` for up to 10 s. `checkOTAMode()` calls `delay(1)` on every loop. `setBrewingState()` calls `delay(300)` for the momentary pulse.
- **The sketch calls `abs(float)`** in the offset-learning code. On a normal desktop build, `abs` with only `<stdlib.h>` included is the int version and would silently truncate. The Arduino ESP32 core provides a float-correct `abs`.
- **Arduino builds ignore subfolders.** The Arduino IDE and arduino-cli compile only a sketch folder's top-level files and its `src/` subfolder. A `test/` subfolder is invisible to the firmware build, so fake headers there can't shadow the real ones.
- **The sketch has its own copy of the library.** `examples/shotStopper/AcaiaArduinoBLE.{h,cpp}` is currently identical to the root copy. Because the tests target the sketch, they use the sketch's copy, and the root copy is out of scope.
- **Available tools:** clang++ and make. CMake and PlatformIO are not installed.

## Goals / Non-Goals

**Goals:**
- Fake only the API surface the code actually uses. Each fake method is added because a call site needs it.
- Make scenarios read like a timeline: boot, connect, press, ramp, assert.
- Keep the harness small enough to maintain by hand, with zero external dependencies.
- Keep everything inside `examples/shotStopper/test/`, so the sketch folder is self-contained and can be copied out of the repo with its tests.

**Non-Goals:**
- A general-purpose ArduinoBLE or Arduino core emulator.
- Simulating radio timing, packet size limits, or BLE stack faults beyond the injected failures in the spec.
- Exercising the OTA HTTP server. `WebServer`, `Update` and `ESP` are stubbed only far enough to compile and do nothing.
- Testing or syncing the root library copy (`/AcaiaArduinoBLE.{h,cpp}`).
- Refactoring the sketch for testability. That can be a later change if the tests show it's worth it.

## Decisions

### 1. Fake the ArduinoBLE library rather than the scale class
The fakes replace the ArduinoBLE library and leave `AcaiaArduinoBLE` intact. The real `AcaiaArduinoBLE.cpp` then runs in every test, so the bytes on the wire and the scale-type and failure logic are covered both by the library tests and implicitly by the sketch tests.
- *Alternative:* fake `AcaiaArduinoBLE` itself. It's simpler, but only checks that `tare()` was called, not what was sent, and it would leave the library untested.

### 2. Compile the `.ino` as a C++ file, adding prototypes from outside
The sketch test build compiles a small file, `examples/shotStopper/test/sketch/sketch_tu.cpp`, that:
1. includes the fake `Arduino.h`;
2. includes a hand-written `sketch_prototypes.h` declaring the functions the IDE would normally generate prototypes for;
3. contains `#include "../../shotStopper.ino"`.

The quoted includes inside the sketch then resolve to the real `examples/shotStopper/AcaiaArduinoBLE.h` and `OTASetup.h`. `examples/shotStopper/AcaiaArduinoBLE.cpp` is compiled separately and linked in, matching what the Arduino IDE builds.
- *Alternative:* copy the sketch into a `.cpp` at build time with generated prototypes, as `arduino-cli` does. That needs a prototype generator, which is fragile in a Makefile. The hand-written prototypes header is about 10 lines. If it falls out of date, the build fails with an "undeclared identifier" error, so drift is caught at compile time.

### 3. One process per test, using a small in-repo runner
Global state in the sketch, the library and the fakes rules out resetting within a single process. `examples/shotStopper/test/framework/test.h` provides:
- a `TEST(name)` registration macro;
- `CHECK`, `CHECK_EQ`, `CHECK_NEAR` and `REQUIRE` macros that report file and line;
- a `main()` that runs each registered test in a `fork()`ed child with an `alarm()` time limit (default 10 s), and maps the exit status, signal or timeout to pass/fail.

The `fork()` also contains crashes and hangs, as the spec requires. The parent prints a summary and exits non-zero on any failure. `./binary <name>` runs one test without forking, so it can be debugged under lldb.
- *Alternative:* vendor doctest or Catch2. They are richer, but they're large third-party files, they don't fork per test, and they would still need a reset story for the sketch's globals.
- *Alternative:* add a `resetSketch()` that reassigns every global. It breaks silently every time a global is added to the sketch.

### 4. Fake BLE: handles that share one state object
Every fake `BLEDevice`, `BLECharacteristic` and `BLEService` holds a `std::shared_ptr` to its state, so copies stay connected and the by-value usage in the library works unchanged. A single registry, the fake `BLE` object, owns:
- **the local (peripheral) side**: services and characteristics added by `BLE.addService`, looked up by UUID, plus the disconnect event handler and the `begin()` result (scriptable, to test the init-retry path);
- **the remote (central) side**: a list of `FakeScale` objects that become visible to `BLE.scan()` and `BLE.available()`.

`BLECharacteristic` state records which side it belongs to:
- **Remote** characteristics forward `writeValue` to the owning `FakeScale` (recorded, and failure can be injected), and expose `valueUpdated`, `valueLength` and `readValue` from the packet queue.
- **Local** characteristics store the value. `written()` is set by `FakePhone::write` and cleared once read, and every `writeValue` from the sketch is appended to a history the phone can read.

`BLEByteCharacteristic` is a thin typed wrapper over the same state.

### 5. Fake scale behavior
`FakeScale` is configured with a type (OLD/NEW/GENERIC), a name, an address, failure flags (`failConnect`, `failDiscover`, `failSubscribe`, `failWrites`) and either explicit packets or a weight function `w(t)`.
- **Characteristics:** it exposes the characteristic UUIDs for its type: `2a80`, the `49535343-…` pair, or `ff11`/`ff12`.
- **Packets:** with a weight function, it emits one packet per configured period (default 100 ms) of simulated time, encoded in its type's format with a matching packet length (13 for NEW, 10 for OLD, 20 for GENERIC). Packets for the current time are produced on demand when the library polls `valueUpdated()`, so the fake needs no background thread.
- **Tare:** a write that matches its type's tare bytes stores the current raw weight as the zero point.
- **Name:** OLD and NEW scales advertise names the library's name filter accepts (`PYXIS…`, `LUNAR…`). The filter in `isScaleName` has no Felicita prefix, so GENERIC scales in tests advertise an accepted name such as `PROCH…`. A test documents this current filter behavior rather than hiding it.

### 6. Deterministic clock
`millis()` reads a global `fake::now_ms`, which only changes in three ways:
- `delay(ms)` advances it by `ms`;
- `BLE.available()` advances it by 10 ms when it returns no device, so `init()`'s 10 s wait ends after about 1000 iterations;
- tests call `fake::advance(ms)` or the helper `runFor(ms)`, which calls `loop()` repeatedly. Each loop iteration already advances 1 ms through `checkOTAMode`'s `delay(1)`. The helper adds 1 ms per iteration only if the loop didn't move time, so it can never spin forever.

`micros()` is derived from the same clock.

### 7. Fake Arduino core scope
- **`String`:** a wrapper over `std::string`, providing the constructors and operators the code uses: from `const char*`, from `(const char*, len)`, from numbers, `+`, `==`, `substring`, `length`, `c_str`, `operator[]`, and `toString` support for `IPAddress`.
- **Utilities:** `byte`, `PROGMEM`/`F()` as no-ops, `pinMode`, `digitalRead`/`digitalWrite`, `analogWrite`, `setCpuFrequencyMhz`, `pow`, `isfinite`, and board LED macros matching the Nano ESP32 branch of the sketch.
- **`abs`:** brought in as `using std::abs;` with `<cmath>`, so `abs(float)` keeps its fractional part as it does on the ESP32 core.
- **`Serial`:** appends to a buffer that tests can query, and is echoed to stderr when `TEST_VERBOSE=1`.
- **`EEPROM`:** a fixed 512-byte array starting at `0xFF` (like erased flash), with a commit counter.

### 8. Build layout
```
examples/shotStopper/
  shotStopper.ino  OTASetup.h  AcaiaArduinoBLE.h  AcaiaArduinoBLE.cpp   # unchanged, under test
  test/
    Makefile          # make -C examples/shotStopper/test  -> build/ then run all
    README.md         # how to run, debug and add scenarios
    .gitignore        # build/
    framework/test.h
    fakes/            # Arduino.h ArduinoBLE.h EEPROM.h WiFi.h WebServer.h Update.h + .cpp
    support/          # fake_scale.h/.cpp, fake_phone.h/.cpp, scenario helpers (boot, runFor, rampWeight)
    library/          # test_*.cpp + ../../AcaiaArduinoBLE.cpp  -> build/library_tests
    sketch/           # sketch_tu.cpp, sketch_prototypes.h, test_*.cpp + ../../AcaiaArduinoBLE.cpp -> build/sketch_tests
    build/            # gitignored
```
- **Flags:** `-std=c++17 -Wall -I fakes -I support -g`, plus `-fsanitize=address,undefined` by default, which catches out-of-bounds writes like the ones the recent shot-array and EEPROM fixes addressed. `make SANITIZE=0` turns the sanitizers off.
- **Two binaries:** both link the sketch's `AcaiaArduinoBLE.cpp`. The library tests are kept separate so they don't pull in the sketch's globals (BLE service objects, `setup`/`loop`), and a sketch compile error doesn't block the library tests.
- **Board variant:** the Makefile also compiles `sketch_tu.cpp` with `-DARDUINO_ESP32_WROOM_DA`, compile only, so the WROOM-DA pin branch stays buildable.

## Risks / Trade-offs

- [Fakes diverge from real ArduinoBLE semantics, e.g. whether `written()` clears on read] → Model each method on the ArduinoBLE source and note the source line in a comment. Keep the surface minimal. Treat any mismatch found on hardware as a fake bug with a regression test.
- [The prototypes header goes stale when sketch functions are added or renamed] → This fails at compile time with a clear error. The fix is a one-line edit.
- [`fork()` is POSIX-only, so Windows isn't supported natively] → Acceptable for this project. WSL works. Single-test mode still works without `fork`.
- [Sanitizers slow the run or fail on some macOS setups] → `SANITIZE=0` escape hatch. The suite should still run in a few seconds.
- [Tests fix in place the current behavior, including possible bugs such as the offset byte wrapping when the offset is negative, or Felicita names being rejected] → Tests check current behavior and name the suspected bug in a comment. Fixes are made in separate changes, which then update the test.
- [Fake headers named `Arduino.h`/`ArduinoBLE.h` live inside the sketch folder, and an IDE or tool could pick them up] → They sit two levels down in `test/fakes/`, which Arduino builds never scan. The spec's "Arduino IDE build unaffected" scenario is checked once by compiling the sketch with the real toolchain after the harness lands.
- [The sketch's `delay(5000)` and `ESP.restart()` in the OTA upload handler] → Never reached, because the stubbed `WebServer` never invokes handlers.

## Migration Plan

Additive only, all inside `examples/shotStopper/test/`. Nothing to deploy or roll back. The firmware build is unaffected, because the Arduino IDE doesn't compile the `test/` subfolder and no existing file changes.

## Open Questions

- Default packet period for the fake scale (100 ms vs. a real Pyxis's ~50-60 ms)? This only changes how close the stop-time check can be. It can be tuned once real packet captures exist.
