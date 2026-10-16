## 1. Skeleton and test runner

- [x] 1.1 Create the `examples/shotStopper/test/` layout (`framework/`, `fakes/`, `support/`, `library/`, `sketch/`) with a local `.gitignore` containing `build/`. Verify `git status` does not show `build/` after a build, and that nothing outside `examples/shotStopper/test/` is touched.
- [x] 1.2 Write `framework/test.h`: `TEST()` registration, `CHECK`/`CHECK_EQ`/`CHECK_NEAR`/`REQUIRE` reporting file:line, and a `main()` that forks each test with an `alarm()` time limit and prints a pass/fail summary. Running a binary with a test name runs that test in-process. Verify with a throwaway self-test binary that has one passing test, one failing check, one `raise(SIGSEGV)` and one infinite loop: all four are reported correctly, and the exit code is non-zero.
- [x] 1.3 Write `Makefile` with the `library_tests` and `sketch_tests` targets (both linking `../AcaiaArduinoBLE.cpp`), a default target that builds and runs both binaries, and the `SANITIZE=0` option. Verify `make -C examples/shotStopper/test` from the repo root, and `make` from inside `examples/shotStopper/test/`, both run the empty suites and exit 0.

## 2. Platform fakes

- [x] 2.1 Implement the fake `Arduino.h` and core: deterministic clock (`millis`, `micros`, `delay`, `fake::advance`), GPIO with per-pin output history and settable inputs, `analogWrite`, `setCpuFrequencyMhz`, `byte`/`PROGMEM`/`F()`, board LED macros, `using std::abs` with `<cmath>`, and `Serial` capture with `TEST_VERBOSE` echo. Verify with tests: delay advances the clock, pin history records timestamps, `abs(-1.5f) == 1.5f`, and serial text can be asserted.
- [x] 2.2 Implement the fake `String`, covering only what the code uses: the constructors, `+`, `==`, `substring`, `length`, `c_str`, `[]` and number formatting. Verify with unit tests for each operation, including `String((const char*)buf, len)` with non-null-terminated input.
- [x] 2.3 Implement the fake `EEPROM`: an array initialized to `0xFF`, `begin`/`read`/`write`/`commit`, a commit counter, and test-side pre-load and inspect helpers. Verify with a unit test.
- [x] 2.4 Add compile-only stubs for `WiFi.h` (status always disconnected unless set by a test), `WebServer.h` (`on`, `send`, `send_P`, `begin`, `stop`, `handleClient`, `upload()`, `HTTPUpload`), `Update.h` and the `ESP` object. Verify that `examples/shotStopper/OTASetup.h` compiles in a scratch file that includes it.

## 3. Fake BLE stack, fake scale and fake phone

- [x] 3.1 Implement the fake `ArduinoBLE.h` core: `BLE` (`begin` with a scriptable result, `scan`, `scanForAddress`, `available` advancing the clock 10 ms when there's nothing to return, `stopScan`, `poll`, `advertise`, the setters, `addService`, `setEventHandler`), plus `BLEDevice`, `BLEService`, `BLECharacteristic`, `BLEByteCharacteristic` and `BLEDescriptor` as handles sharing state through `shared_ptr`. Verify that `examples/shotStopper/AcaiaArduinoBLE.cpp` and `shotStopper.ino` compile and link against it.
- [x] 3.2 Implement `support/fake_scale` with the OLD, NEW and GENERIC characteristic sets, recorded writes with timestamps, failure flags, an explicit packet queue, weight-function packet generation per type format, and tare zeroing. Verify with unit tests that the encoder output for each type round-trips through the library decoder for a few weights, including a negative one.
- [x] 3.3 Implement `support/fake_phone`: write and read by UUID (`0xFF10`-`0xFF24`), per-characteristic history of values the sketch writes, `written()` set by phone writes and cleared on read, and triggering the disconnect handler. Verify with a unit test against a local characteristic.
- [x] 3.4 Add the scenario helpers `boot()` (calls `setup()`), `runFor(ms)` (repeated `loop()` with guaranteed clock progress), `pressBrew()`/`releaseBrew()` and `connectScale(type)`. Verify a smoke test: boot, connect a NEW scale, run 1 s, and the sketch reports the scale connected.

## 4. Library tests (`test/library/`, the sketch's `AcaiaArduinoBLE.cpp`)

- [x] 4.1 Connect sequence: for each scale type, check subscribe, then `IDENTIFY` (20 bytes), then `NOTIFICATION_REQUEST` (14 bytes), and that `isConnected()` is true. Verify with `make -C examples/shotStopper/test`.
- [x] 4.2 Scale type detection and name filter: each characteristic set maps to OLD, NEW or GENERIC, connection fails when none is present, accepted and rejected name prefixes are checked, and a test pins the current behavior that Felicita names are rejected. Verify with `make -C examples/shotStopper/test`.
- [x] 4.3 Failure paths: connect, discover and subscribe failures return false, a scan with no scale times out after 10 s of simulated time, and a failed tare, timer or heartbeat write sets `isConnected()` false. Verify with `make -C examples/shotStopper/test`.
- [x] 4.4 Command bytes: tare, start, stop and reset timer for Acaia and for generic scales match the protocol constants and lengths. Verify with `make -C examples/shotStopper/test`.
- [x] 4.5 Heartbeat and timeout: `heartbeatRequired()` becomes true after more than 2750 ms for OLD and NEW and is never true for GENERIC, and a gap of more than 5000 ms after the first packet disconnects. Verify with `make -C examples/shotStopper/test`.
- [x] 4.6 Weight decoding: table-driven packets for NEW (13 and 17 bytes, decimal scales 1 and 2, the sign bit), OLD (10 and 14 bytes) and GENERIC (20 bytes, sign `'-'`), and packets with the wrong length or type produce no new weight. Verify with `make -C examples/shotStopper/test`.

## 5. Sketch tests (`test/sketch/`, unmodified `shotStopper.ino`)

- [x] 5.1 Create `sketch_tu.cpp` and `sketch_prototypes.h` to compile the sketch from its real path, plus a compile-only build with `-DARDUINO_ESP32_WROOM_DA`. Verify that `make -C examples/shotStopper/test` builds both variants with no warnings from the harness files.
- [x] 5.2 Boot and EEPROM: a blank EEPROM gets the defaults (36 g, 1.5 g as byte 15, signature `0xAA`), and a pre-seeded EEPROM loads its values. Verify with `make -C examples/shotStopper/test`.
- [x] 5.3 Latching shot by weight: a rising ramp leads to OUT high after the minimum shot duration, OUT low near the predicted stop time (within one packet period of the expected time), "ended by weight" in the serial output, and shot status 1 then 0 seen by the phone. Verify with `make -C examples/shotStopper/test`.
- [x] 5.4 Momentary shot by weight: press and release starts the shot, and at the target OUT pulses high for about 300 ms and returns low. Verify with `make -C examples/shotStopper/test`.
- [x] 5.5 Max duration and button-release endings: a stalled ramp ends by time after `maxShotDurationS`, and releasing the button before latching ends by button. Verify with `make -C examples/shotStopper/test`.
- [x] 5.6 Offset learning: a final weight within `MAX_OFFSET` updates the offset and EEPROM byte 3, and an outlier leaves it unchanged. Add a comment noting the possible byte wrap for negative offsets. Verify with `make -C examples/shotStopper/test`.
- [x] 5.7 BLE config persistence: phone writes to `0xFF10`-`0xFF17`, `0xFF22` and `0xFF23` are applied and persisted at the documented EEPROM addresses, `0xFF12` switches the input pin to `REED_IN`, and an over-long SSID is truncated safely. Verify with `make -C examples/shotStopper/test`.
- [x] 5.8 Disabled and OTA modes: writing disabled during a shot ends it and disconnects the scale with no reconnect while disabled, and an OTA request with no WiFi credentials keeps the scale disconnected. Verify with `make -C examples/shotStopper/test`.
- [x] 5.9 Scale status and reconnect: a scale timeout leads to status 0 and a reconnect when the scale comes back, and the phone sees 0, then 1, then 0, then 1. Verify with `make -C examples/shotStopper/test`.
- [x] 5.10 BLE init retry: `BLE.begin()` fails twice and then succeeds, giving no scale scans while it's failing, retries about every 3 s, and a connection after it succeeds. Verify with `make -C examples/shotStopper/test`.

## 6. Docs and final check

- [x] 6.1 Write `examples/shotStopper/test/README.md`: prerequisites, `make -C examples/shotStopper/test`, running a single test, `TEST_VERBOSE=1`, `SANITIZE=0`, and how to add a scenario. Verify by following the steps from a clean clone.
- [x] 6.2 Final integration check: run `git status` and confirm every new or changed file is under `examples/shotStopper/test/`. If arduino-cli or the IDE is available, compile the shotStopper sketch for the Nano ESP32 and confirm it builds as before, with the test folder ignored. Then run `make -C examples/shotStopper/test` from a clean `examples/shotStopper/test/build/` and confirm it passes in under 30 s and exits 0. Run `openspec validate add-host-test-harness` and confirm it passes.
