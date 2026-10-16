# host-test-harness Specification

## Purpose

Lets developers check the Acaia scale library and the shotStopper sketch on their development machine, without flashing an ESP32. The real source runs against fake time, GPIO, EEPROM and BLE, so shot behavior and the exact BLE traffic can be checked deterministically in seconds.

## Requirements

### Requirement: One-command test run on the development machine
The shotStopper sketch SHALL provide a single command, run from the repository root, that builds and runs the whole test suite on macOS or Linux. All harness files SHALL live under `examples/shotStopper/test/`. It SHALL need only a C++17 compiler and `make`. It SHALL NOT need an ESP32 toolchain, an attached device, network access, or any third-party package.

#### Scenario: Clean checkout runs the suite
- **WHEN** a developer runs `make -C examples/shotStopper/test` on a fresh clone with only a C++17 compiler and make installed
- **THEN** the library tests and sketch tests are compiled and executed, and a summary of passed and failed tests is printed

#### Scenario: Harness is self-contained in the sketch folder
- **WHEN** the harness is added
- **THEN** every new file is under `examples/shotStopper/test/`, and no file outside `examples/shotStopper/test/` is created or modified

#### Scenario: Failures are visible to scripts
- **WHEN** at least one test fails
- **THEN** the command exits with a non-zero status, and the output names each failing test and the source location of the failed check

#### Scenario: All tests pass
- **WHEN** every test passes
- **THEN** the command exits with status 0

### Requirement: Production code is tested unmodified
The harness SHALL compile `examples/shotStopper/shotStopper.ino`, `examples/shotStopper/OTASetup.h` and `examples/shotStopper/AcaiaArduinoBLE.{h,cpp}` exactly as they are in the repository. The presence of the harness SHALL NOT change how the Arduino IDE builds the sketch. It SHALL NOT require test-only flags, hooks or edits in those files.

#### Scenario: Sketch built from its real path
- **WHEN** the sketch tests are built
- **THEN** they compile `examples/shotStopper/shotStopper.ino` directly from its location in the repository, together with the `AcaiaArduinoBLE` sources next to it

#### Scenario: Library built from the sketch's copy
- **WHEN** the library tests are built
- **THEN** they compile `examples/shotStopper/AcaiaArduinoBLE.cpp`, the same copy the sketch is built with

#### Scenario: Arduino IDE build unaffected
- **WHEN** the shotStopper sketch is compiled with the Arduino IDE or arduino-cli
- **THEN** no file under `examples/shotStopper/test/` is compiled into the firmware, and the fake headers do not shadow the real Arduino, EEPROM or ArduinoBLE headers

### Requirement: Tests are isolated from each other
Each test SHALL start with fresh sketch, library and fake state: clock at zero, blank EEPROM, no BLE devices, and pins at their power-on defaults. A test that crashes or hangs SHALL be reported as failed without stopping the remaining tests.

#### Scenario: State does not leak between tests
- **WHEN** one test changes the goal weight to 40 g and a later test boots the sketch on blank EEPROM
- **THEN** the later test sees the default goal weight of 36 g

#### Scenario: Crash is contained
- **WHEN** a test triggers a segmentation fault
- **THEN** that test is reported as failed with the crash reason, and the other tests still run and report

#### Scenario: Hang is contained
- **WHEN** a test does not finish within its time limit
- **THEN** that test is reported as failed due to a timeout, and the other tests still run

### Requirement: Deterministic simulated time
The fake platform SHALL provide a clock that starts at zero and advances only when the test advances it or the code under test blocks (for example a delay, or a BLE scan waiting for devices). Waiting loops that depend on elapsed time SHALL therefore always finish.

#### Scenario: Delay advances the clock
- **WHEN** code under test calls a 300 ms delay at simulated time 1000 ms
- **THEN** the simulated time is 1300 ms afterwards

#### Scenario: Scale search times out in simulated time
- **WHEN** the library searches for a scale and no matching device is advertising
- **THEN** the search gives up after 10 s of simulated time, and the test finishes in well under one second of real time

### Requirement: Observable fake GPIO, serial and EEPROM
The fake platform SHALL let tests drive digital inputs and inspect digital and PWM outputs, including a time-stamped history of changes to each output pin. It SHALL capture serial output. It SHALL provide an EEPROM that tests can pre-load and inspect, and that records each commit.

#### Scenario: Output pin history
- **WHEN** the sketch drives the brew output pin high and then low
- **THEN** the test can read both transitions with their simulated timestamps

#### Scenario: Pre-seeded EEPROM
- **WHEN** a test pre-loads EEPROM with a valid signature and a goal weight of 42 g before booting the sketch
- **THEN** the sketch uses 42 g as its goal weight

#### Scenario: Serial output is captured
- **WHEN** the sketch prints "weight achieved"
- **THEN** the test can assert that this text appears in the captured serial output

### Requirement: Scriptable fake scale
The harness SHALL provide a fake BLE scale that tests configure as an old-protocol Acaia, new-protocol Acaia/Pyxis, or generic/Felicita scale, with a chosen advertised name and address. It SHALL record every write the code under test makes to it, in order and with timestamps. It SHALL deliver weight packets supplied by the test or generated from a weight-over-time profile. It SHALL respond to tare commands by zeroing the weight it reports. It SHALL let tests inject failures in connect, attribute discovery, subscribe and write, and stop sending packets on demand.

#### Scenario: Writes are recorded
- **WHEN** the library connects to a fake new-protocol scale and tares it
- **THEN** the test can read the exact byte sequences written, in the order they were sent

#### Scenario: Tare zeroes reported weight
- **WHEN** the fake scale is reporting 12.0 g and receives a tare command
- **THEN** its next weight packets report weight relative to 12.0 g

#### Scenario: Injected write failure
- **WHEN** a test configures the fake scale to reject writes and the code sends a command
- **THEN** the write reports failure to the code under test

### Requirement: Scriptable fake phone for the config service
The harness SHALL provide a fake BLE client that writes and reads the shotStopper config characteristics by UUID (`0xFF10`-`0xFF24`) and observes the values the sketch publishes on them. It SHALL also be able to trigger a client disconnect.

#### Scenario: Phone writes a characteristic
- **WHEN** the fake phone writes a value to a config characteristic
- **THEN** the sketch sees that characteristic as written on its next BLE poll, with the written value

#### Scenario: Phone observes status
- **WHEN** the sketch updates the shot status or scale status characteristic
- **THEN** the fake phone can read the latest value and the history of published values

### Requirement: Library protocol coverage
The library test suite SHALL check the BLE behavior of the scale library bundled with the sketch against the fake scale for every supported scale type. That includes the bytes and order of the connect sequence, the bytes of each command, scale type detection, failure handling, heartbeat timing, packet timeout, and weight decoding.

#### Scenario: Connect sequence
- **WHEN** the library connects to a fake scale of any supported type
- **THEN** it subscribes to the scale's notify characteristic, and then writes the 20-byte identify message followed by the 14-byte notification request

#### Scenario: Command bytes per scale type
- **WHEN** tare, start timer, stop timer and reset timer are each sent to an Acaia scale and to a generic scale
- **THEN** the bytes written match the protocol constants for that scale type, with the 6-byte or 7-byte lengths the library defines

#### Scenario: Scale type detection
- **WHEN** the fake scale exposes only the old-protocol, only the new-protocol, or only the generic characteristics
- **THEN** the library identifies it as old, new or generic respectively, and connection fails when none of them is present

#### Scenario: Failure paths
- **WHEN** connect, attribute discovery or subscribe fails on the fake scale
- **THEN** connecting returns failure and the library reports itself as not connected

#### Scenario: Failed command marks the scale disconnected
- **WHEN** a command write to a connected scale fails
- **THEN** the library reports itself as not connected

#### Scenario: Heartbeat timing
- **WHEN** more than 2750 ms of simulated time pass after the last heartbeat with an Acaia scale
- **THEN** the library reports that a heartbeat is required, and a generic scale never reports that one is required

#### Scenario: Packet timeout
- **WHEN** a connected scale that has sent at least one weight packet then sends nothing for more than 5000 ms of simulated time
- **THEN** the library disconnects and reports itself as not connected

#### Scenario: Weight decoding
- **WHEN** weight packets of each supported length and format are delivered, including negative weights and different decimal scales
- **THEN** the decoded weight matches the value encoded in the packet, and packets of unsupported length or type produce no new weight

### Requirement: Sketch behavior coverage
The sketch test suite SHALL run the shotStopper sketch through its setup and loop against the fake scale, fake phone and fake GPIO. It SHALL cover end-to-end shot scenarios, persisted configuration, and degraded states.

#### Scenario: Stop by weight with a latching switch
- **WHEN** the sketch is configured for a latching switch with a 36 g goal and a 1.5 g offset, the brew input goes active, and the fake scale reports a steadily rising weight
- **THEN** the brew output goes high once the minimum shot duration has passed, goes low when the predicted weight reaches goal minus offset, and the shot is reported as ended by weight

#### Scenario: Stop by weight with a momentary switch
- **WHEN** the sketch is configured for a momentary switch and a shot started by a press-and-release reaches its target
- **THEN** the brew output is pulsed high for about 300 ms of simulated time and then returns low

#### Scenario: Max duration cutoff
- **WHEN** a shot is still brewing after the configured maximum shot duration
- **THEN** the sketch ends the shot as ended by time

#### Scenario: Offset learning
- **WHEN** a shot ends and the final weight, measured after the drip delay, differs from the goal by no more than the maximum allowed error
- **THEN** the weight offset is adjusted by the difference and persisted to EEPROM, and a difference larger than the maximum leaves the offset unchanged

#### Scenario: First boot defaults
- **WHEN** the sketch boots on blank EEPROM
- **THEN** it writes its default configuration (36 g goal, 1.5 g offset) and a valid signature to EEPROM

#### Scenario: Config written over BLE is persisted
- **WHEN** the fake phone writes a new goal weight, switch type, auto-tare, shot durations, drip delay, or WiFi credentials
- **THEN** the sketch applies the value and persists it to the matching EEPROM location

#### Scenario: Disabled stops the shot and drops the scale
- **WHEN** the fake phone writes "disabled" during a shot
- **THEN** the shot ends, the scale is disconnected, and the sketch does not reconnect while disabled

#### Scenario: Shot and scale status notifications
- **WHEN** the scale connects or disconnects, or a shot starts or ends
- **THEN** the fake phone observes the matching scale status and shot status values

#### Scenario: BLE init failure is retried
- **WHEN** BLE initialisation fails at boot and succeeds on a later attempt
- **THEN** the sketch does not try to connect to the scale while BLE is unavailable, retries initialisation about every 3 s of simulated time, and connects to the scale once BLE is up
