## 1. Harness groundwork

- [x] 1.1 Vendor ArduinoJson v7.2.0's single header as `examples/shotStopper/test/vendor/ArduinoJson.h`, with `vendor/VERSION.md` (version and source URL), and add `-Ivendor` to the Makefile. Verify a scratch test that parses `{"status":"Ok","n":28000.0}` and reads `n` as 28000 passes under `make -C examples/shotStopper/test`.
- [x] 1.2 Add FreeRTOS fakes reachable through the fake `Arduino.h`: `xTaskCreatePinnedToCore` that records the task without running it, `xQueueCreate`/`xQueueSend`/`xQueueReceive` with real queue semantics and capacity, `vTaskDelay`, and `pdMS_TO_TICKS`. Add `esp_random()` (deterministic) and `WiFi.mode()`. Verify with harness self-tests: queue FIFO and full-queue rejection, the task registry records name, core, stack and priority, and `esp_random` repeats across runs.
- [x] 1.3 Add `fakes/WiFiClientSecure.h` (`setInsecure`) and `fakes/HTTPClient.h` (`begin`, `setReuse`, `setTimeout`, `setConnectTimeout`, `addHeader`, `POST`, `getSize`, `getString`, `end`). `POST` routes by host and port to a registered fake server and records the URL, headers, body, simulated time and caller context (loop or grinder). It returns a connection error when no server is registered. Verify with self-tests: a request to an unregistered host fails, and a registered echo server sees the body and headers.
- [x] 1.4 Implement `support/fake_grinder.{h,cpp}`:
  - `brewJobs`, `brewAccept` and `brewDone` with the documented replies;
  - 403 for an unknown `pairingId`;
  - `addJob(grindUuid, groupHeadId?, doseTypeExpected, durationExpectedMs, brewWeightExpectedMg)`;
  - in simulated time: drop untaken jobs after 60 s, re-list taken jobs every 50 s, and `ClientErrorOngoingJobNotExisting` for a job that isn't taken;
  - fault injection: timeout, connection error, forced `ClientError`, forced 403;
  - a parsed request log.

  Verify with fake self-tests covering each lifecycle rule, driven by raw requests.

## 2. GrinderClient

- [x] 2.1 Create `examples/shotStopper/GrinderClient.h/.cpp` with the `GrinderEvent` types, the event queue, the atomic status, `runOnce()`, and the HTTP helper (new TLS connection per request, insecure, 8 s timeouts, no redirects, 64 KiB reply limit, JSON body with `version: 1`). Add `../GrinderClient.cpp` to both test binaries in the Makefile. Verify `make -C examples/shotStopper/test` builds, and that a library test sees the exact `brewJobs` URL and body from the spec's "Request shape" scenario.
- [x] 2.2 Implement configuration and the WiFi link:
  - `NOT_CONFIGURED` until an IP is set, with no WiFi calls;
  - `WiFi.begin` at most every 10 s while down, status "connecting";
  - never `WiFi.disconnect`;
  - `Active(false)` stops all requests and WiFi calls.

  Verify with library tests: no grinder → no WiFi calls; a dropped connection → reconnect, then polling resumes; inactive → zero requests.
- [x] 2.3 Implement polling and failure handling: `brewJobs` every 2 s in IDLE/JOB_TAKEN, backoff 2 s doubling to 60 s with reset on success, one 403 tolerated, three in a row → "pairing rejected" at a 60 s interval, reset on success or `Config`. Verify with library tests for the "Idle polling", "Grinder unreachable", "Recovery resets the backoff", "Pairing cleared on the grinder" and "A single 403" scenarios.
- [x] 2.4 Implement taking jobs:
  - take the newest unseen job (last in the array when several are new), with `groupHeadId` 0 when missing and `doseType` echoed (`null` when absent);
  - accept float numbers;
  - re-accept on re-list;
  - a newer job → `Canceled` for the old one, then take the new one;
  - expiry 5 min after first taken → `Canceled`.

  Verify with library tests for "Take a new job", "Job listed again", "Grinding again before pulling" and "Job expires unused".
- [x] 2.5 Implement shot handling and report delivery:
  - `ShotStarted` pauses polling with or without a job;
  - `Flush` returns to JOB_TAKEN with the expiry clock still running;
  - `Success`/`Canceled` → REPORTING with one UUID v4 `brewEventUuid` (from `esp_random`), retried on every cycle;
  - `ClientErrorOngoingJobNotExisting` → `brewAccept`, then the same `brewDone`;
  - windows of 120 s (Success) and 20 s (Canceled), then drop;
  - no job → nothing sent.

  Verify with library tests for "Flush keeps the job", "Shot without a grind", "Job was listed again during a long shot", "Grinder briefly unreachable after the shot" and "Report abandoned".
- [x] 2.6 Implement the link status values 0–6 and their transitions, and log free heap on each state change. Verify with a library test that a configured client going through connect, job, shot and report produces the status sequence 1, 4, 5, 6, 4.

## 3. Sketch integration

- [x] 3.1 Grow `EEPROM_SIZE` to 128 and add the grinder signature at 75, the IP at 76–91 and the pairingId at 92–95. Write them on first boot, and migrate when the main signature is valid but the grinder signature is missing. Verify with sketch tests: blank EEPROM → signature written and grinder empty; EEPROM seeded like the old firmware (75 bytes, valid signature) → old settings kept and grinder off; stored grinder config → loaded at boot.
- [x] 3.2 Add characteristics `0xFF25` (IP, 16 bytes), `0xFF26` (pairingId, 4 bytes LE) and `0xFF27` (status, notify). Apply writes with IPv4 validation, persist them, and send `Config` events at boot and on changes to `0xFF22`/`0xFF23`/`0xFF25`/`0xFF26`. Publish the client status to `0xFF27` on change. Verify with sketch tests for "Configure the grinder from the phone", "Invalid address rejected" and "Turning grinder sync off". Extend `FakePhone`'s documented UUID range to `0xFF27`.
- [x] 3.3 Start the grinder task in `setup()` (core 0, 16384 stack, priority 1) and send `Active` events whenever enabled/OTA state changes, sending `Active(false)` before ending a shot through the disabled or OTA path. Verify with sketch tests: a task registered on core 0, "OTA mode", "Disabled" and "Disabled during a shot".
- [x] 3.4 Post shot events: `ShotStarted` in `setBrewingState(true)`. In `setBrewingState(false)`, classify by `shot.end` before it's reset: WEIGHT → pending Success, sent after `dripDelayS` with the current weight; BUTTON under `minShotDurationS` → Flush; otherwise Canceled. Verify with sketch tests: "Successful shot reported with time and weight" (duration and mass within one packet period and 0.2 g), "Manual stop is not learned from" and "Max duration is not learned from".
- [x] 3.5 Extend the scenario helpers: `step()` calls `loop()` then `grinder.runOnce()` with the caller context set, plus `bootWithGrinder(FakeScale&, FakeGrinder&)`. Expose `grinder` in `sketch_api.h`. Verify that the existing 71 tests still pass unchanged.
- [x] 3.6 Add end-to-end sketch scenarios on the shared clock:
  - grind, take, latching shot stopped by weight, `brewDone` Success with the right values, polling resumes;
  - a flush, then a real shot reported against the same job;
  - a grind more than 50 s before the shot end, where the re-list is handled and the report still succeeds;
  - a newer grind before pulling;
  - expiry;
  - status sequence on the phone.

  Verify with `make -C examples/shotStopper/test`.
- [x] 3.7 Add the isolation scenarios: the fake HTTP log shows no request made from `loop()` across a full grind-shot-report run, and with the grinder configured but every request timing out, the latch and stop times and the stop weight match the same shot with grinder sync off. Verify with `make -C examples/shotStopper/test`.

## 4. Docs and verification

- [x] 4.1 Document the feature: in the `shotStopper.ino` header, the ArduinoJson dependency and version plus the grinder setup steps (pair elsewhere, set WiFi, IP, pairingId); in the root `README.md`, a short section listing `0xFF25`-`0xFF27` and the status values; in `examples/shotStopper/test/README.md`, `FakeGrinder` and the new helpers. Verify the README steps match the spec's characteristics and values.
- [x] 4.2 Run the full suite from a clean `build/` and run `openspec validate add-grinder-gbs-sync --strict`. Verify all tests pass, the suite runs in under 60 s, and validation passes.
- [x] 4.3 Hardware verification on the ESP32-S3 with a paired E65 GbS and the scale. Steps:
  1. Compile in the Arduino IDE with ArduinoJson 7 installed.
  2. Configure WiFi, IP and pairingId from LightBlue.
  3. Confirm that status reaches 4 (ready), that the grinder shows the machine as On, and that a grind reaches 5 (job taken).
  4. Pull a shot stopped by weight and confirm the grinder adjusts its grind setting.
  5. Run a 30-minute soak with repeated grinds and shots, logging free heap, and checking for scale disconnects or shot timing that differs from the host tests.
  6. Measure the current draw on the V3 board with WiFi on.

  Record the results in the change folder as `hardware-notes.md`.

  Closed by decision after the hardware runs on 2026-10-22: on the ESP32-S3 with the E65 GbS and an Acaia scale, the board reached status 4 (ready), took a job (5), stopped a shot by weight, re-took the job mid-shot after the grinder re-listed it, and got `brewDone Ok`. Requests took 1.2-2 s, RSSI was -45 to -53 dBm, and free heap held at 97-107 KB. Not run: the 30-minute soak and the V3 current-draw measurement.
- [x] 4.4 Add flow logging for hardware bring-up: `GRINDER_DEBUG` in the sketch and `GrinderClient::setDebug()` log every grinder request and reply (HTTP code, status, duration, free heap), events, WiFi transitions, job decisions, shot classification and published `0xFF27` status, one timestamped `[ms source]` line per `Serial` write so the two cores' lines don't interleave. Also fixed a boot crash found on hardware: on ESP32 core 3.x the sketch now claims `btInUse()` so the core doesn't free the BT controller memory ArduinoBLE needs. Verified with host tests for the log output and an `arduino-cli` compile for `esp32:esp32:esp32s3` (core 3.3.8).
- [x] 4.5 Fix OTA not starting when grinder sync had a WiFi attempt in progress (`sta is connecting, cannot set config`): one `WiFi.begin()` per 30 s attempt with auto-reconnect and the reason logged on each retry, a fresh connect on resuming after OTA/disabled, and `OTASetup.h` reusing a live connection and aborting a pending attempt before its own `begin()`. The fake WiFi now refuses `begin()` mid-attempt like the ESP32; verified with tests that fail against the old `OTASetup.h`.
- [x] 4.6 Keep polling `brewJobs` during shots (field finding): the grinder re-lists a taken job ~50 s after the last `brewAccept`, shot or not, so a shot that crosses that mark must re-take the job or its `brewDone` is refused. During a shot the client re-takes a re-listed job and takes no new one; the shot's report goes out immediately even after earlier failures (a re-listed job is likely dropped ~60 s later); the existing `OngoingJobNotExisting` → `brewAccept` → same `brewDone` recovery stays as backup. Verified with tests reproducing the field timeline (take, +25 s, 28 s shot), a missed re-list, a grind during a shot, and no premature re-take; a mutation restoring the shot pause fails them.
