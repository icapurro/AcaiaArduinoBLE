## MODIFIED Requirements

### Requirement: One-command test run on the development machine
The shotStopper sketch SHALL provide a single command, run from the repository root, that builds and runs the whole test suite on macOS or Linux. All harness files SHALL live under `examples/shotStopper/test/`. It SHALL need only a C++17 compiler and `make`. It SHALL NOT need an ESP32 toolchain, an attached device, network access, or installing any package. Third-party code the sketch depends on MAY be vendored into `examples/shotStopper/test/` as a pinned copy whose version is recorded next to it.

#### Scenario: Clean checkout runs the suite
- **WHEN** a developer runs `make -C examples/shotStopper/test` on a fresh clone with only a C++17 compiler and make installed
- **THEN** the library tests and sketch tests are compiled and executed, and a summary of passed and failed tests is printed

#### Scenario: Harness is self-contained in the sketch folder
- **WHEN** the harness is added or extended
- **THEN** every harness file, vendored dependencies included, is under `examples/shotStopper/test/`

#### Scenario: Vendored dependency is pinned
- **WHEN** a third-party header is vendored for the tests
- **THEN** its version and source are recorded in `examples/shotStopper/test/`, and the build uses only that copy

#### Scenario: Failures are visible to scripts
- **WHEN** at least one test fails
- **THEN** the command exits with a non-zero status, and the output names each failing test and the source location of the failed check

#### Scenario: All tests pass
- **WHEN** every test passes
- **THEN** the command exits with status 0

### Requirement: Production code is tested unmodified
The harness SHALL compile `examples/shotStopper/shotStopper.ino`, `examples/shotStopper/OTASetup.h`, `examples/shotStopper/AcaiaArduinoBLE.{h,cpp}` and `examples/shotStopper/GrinderClient.{h,cpp}` exactly as they are in the repository. The presence of the harness SHALL NOT change how the Arduino IDE builds the sketch. It SHALL NOT require test-only flags, hooks or edits in those files.

#### Scenario: Sketch built from its real path
- **WHEN** the sketch tests are built
- **THEN** they compile `examples/shotStopper/shotStopper.ino` directly from its location in the repository, together with the `AcaiaArduinoBLE` and `GrinderClient` sources next to it

#### Scenario: Library built from the sketch's copy
- **WHEN** the library tests are built
- **THEN** they compile `examples/shotStopper/AcaiaArduinoBLE.cpp`, the same copy the sketch is built with

#### Scenario: Arduino IDE build unaffected
- **WHEN** the shotStopper sketch is compiled with the Arduino IDE or arduino-cli
- **THEN** no file under `examples/shotStopper/test/` is compiled into the firmware, and the fake headers do not shadow the real Arduino, EEPROM, ArduinoBLE, WiFi, HTTPClient or FreeRTOS headers

### Requirement: Scriptable fake phone for the config service
The harness SHALL provide a fake BLE client that writes and reads the shotStopper config characteristics by UUID (`0xFF10`-`0xFF27`) and observes the values the sketch publishes on them. It SHALL also be able to trigger a client disconnect.

#### Scenario: Phone writes a characteristic
- **WHEN** the fake phone writes a value to a config characteristic
- **THEN** the sketch sees that characteristic as written on its next BLE poll, with the written value

#### Scenario: Phone observes status
- **WHEN** the sketch updates the shot status, scale status or grinder link status characteristic
- **THEN** the fake phone can read the latest value and the history of published values

## ADDED Requirements

### Requirement: Scriptable fake grinder
The harness SHALL provide a fake Mahlkönig GbS grinder that serves the Generic WiFi API (`brewJobs`, `brewAccept`, `brewDone`) at a configured IP address, through fakes of the HTTPS client APIs the sketch uses. Tests SHALL be able to:
- add brew jobs (as a grind would);
- have the grinder drop untaken jobs after about 60 s and re-list taken jobs about every 50 s, using simulated time;
- answer with HTTP 403 for an unknown `pairingId`;
- inject timeouts, connection errors and `ClientError` replies;
- inspect every request received, with its path, parsed body and simulated time.

#### Scenario: Grind creates a job
- **WHEN** a test adds a job to the fake grinder
- **THEN** the next `brewJobs` request from the sketch lists it, with the fields the test chose

#### Scenario: Grinder job lifecycle in simulated time
- **WHEN** a job is taken and no `brewDone` follows
- **THEN** the fake grinder lists it again about 50 s later, and `brewDone` for it is answered with `ClientErrorOngoingJobNotExisting` until it is taken again

#### Scenario: Requests are observable
- **WHEN** the sketch sends `brewDone`
- **THEN** the test can read its parsed `grindUuid`, `durationFinalMs`, `massFinalMg`, `brewEventUuid` and `result`

### Requirement: Fake task scheduling for the grinder client
The harness SHALL provide fakes for the FreeRTOS task, queue and delay calls the sketch uses. Creating a task SHALL NOT start a thread. Instead, tests SHALL run the grinder client deterministically, one iteration at a time, interleaved with `loop()` on the same simulated clock. The harness SHALL record whether each grinder request was made from the grinder client or from `loop()`.

#### Scenario: Grinder client driven by the scenario helpers
- **WHEN** a sketch test runs for a period of simulated time with grinder sync active
- **THEN** the grinder client's iterations run between `loop()` iterations in a fixed, repeatable order

#### Scenario: Request made from the control loop
- **WHEN** code running inside `loop()` sends a grinder request
- **THEN** the harness records it as made from the control loop, so a test can fail on it
