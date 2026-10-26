## Why

Today the only way to check any change to the scale library or the shotStopper sketch is to flash an ESP32 and pull a real shot. That makes regressions in shot-stop timing, button handling, offset learning and the BLE protocol slow and expensive to find. Recent debugging on an ESP32-WROOM-DA board showed how much time that loop costs. A test harness that runs on the development machine lets the logic and the exact BLE traffic be checked in seconds, before any flashing.

## What Changes

- Add an `examples/shotStopper/test/` directory, kept entirely inside the sketch folder, that builds and runs a test suite on the development machine with `make -C examples/shotStopper/test`. It needs only a C++17 compiler and `make`: no ESP32 toolchain, PlatformIO, CMake or network download.
- Add fakes, built for the development machine, for the platform APIs the code uses:
  - Arduino core: a controllable clock, GPIO, `Serial` capture, `String`.
  - `EEPROM`.
  - `ArduinoBLE`, covering both the central role (talking to a scale) and the peripheral role (exposing config characteristics to a phone).
  - Compile-only stubs for `WiFi`, `WebServer` and `Update`, so `OTASetup.h` builds unchanged.
- Add a scriptable **fake scale** that advertises as an OLD Lunar, a NEW Lunar/Pyxis, or a GENERIC (Felicita) scale. It records every write the library sends and can push weight packets and inject failures.
- Add a scriptable **fake phone** that writes and reads the shotStopper config characteristics (`0xFF10`-`0xFF24`) and observes their notifications.
- Add **library tests** that run the sketch's own copy of the scale library (`examples/shotStopper/AcaiaArduinoBLE.cpp`) against the fake BLE stack. They cover the connect sequence and command bytes, scale type detection, failure paths, heartbeat timing, the packet timeout and weight decoding.
- Add **sketch tests** that compile the unmodified `examples/shotStopper/shotStopper.ino` and drive whole shot scenarios. They cover the stop-by-weight timing, momentary and latching switches, the max-duration cutoff, offset learning, BLE config writes persisted to EEPROM, OTA mode and BLE init retry.
- Document how to run the tests in `examples/shotStopper/test/README.md`.

There are no firmware behavior changes. The sketch and library are tested as-is, with no edits to production code and no flags added only for testing.

## Capabilities

### New Capabilities
- `host-test-harness`: Running the scale library and the shotStopper sketch on the development machine against fake Arduino, EEPROM and BLE layers. Includes the fake scale and fake phone used to script scenarios, and test isolation and reporting.

### Modified Capabilities
<!-- None: no existing specs, and no firmware behavior changes. -->

## Impact

- **New files**: all under `examples/shotStopper/test/`: Makefile, test runner, fakes, fake scale and phone helpers, library and sketch test suites, a local `.gitignore` for `build/`, and a `README.md`. Nothing outside `examples/shotStopper/` is added or changed.
- **Arduino IDE**: the IDE builds only the sketch folder's top level and `src/`, so the `test/` subfolder (including its fake `Arduino.h` and `ArduinoBLE.h`) is ignored when the sketch is compiled or flashed.
- **Production code**: none. `shotStopper.ino`, `OTASetup.h` and the sketch's `AcaiaArduinoBLE.{h,cpp}` are unchanged, as are the root library and `examples/bare_minimum/`. If a test exposes a real bug, the fix goes in a separate change.
- **Dependencies**: none added. The test runner is a small in-repo header, not a vendored framework.
- **Out of scope**: running the real ESP32 image (Wokwi or QEMU), real radio and BLE stack behavior, the OTA HTTP upload flow, CI wiring (a natural follow-up once the suite exists), and the root library copy and keeping it in sync with the sketch's copy.
