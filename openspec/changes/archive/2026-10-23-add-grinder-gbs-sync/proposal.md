## Why

Mahlkönig E65/E80 GbS grinders adjust their grind setting from the brew time of each shot (Grind-by-Sync). Today they only learn that time from a compatible espresso machine. The shotStopper already knows exactly when a shot starts and stops, and how much ended up in the cup, so it can stand in for the machine. It reports each shot to the grinder over the grinder's local "Generic WiFi" API, and the grinder dials itself in.

## What Changes

- **Grinder link over WiFi.** The shotStopper talks to a paired GbS grinder at a configured IP address, using the grinder's Generic WiFi API (HTTPS POST to port 30001, self-signed certificate). Pairing happens outside the shotStopper (for example in the companion app). The shotStopper only receives the grinder IP and the `pairingId`.
- **Brew job handling.**
  - While idle, the shotStopper polls the grinder for brew jobs.
  - It takes (`brewAccept`) the newest job as soon as it appears, and takes it again whenever the grinder lists it again.
  - It cancels an older taken job when a newer grind arrives.
  - It gives up on a job no shot has used within 5 minutes.
- **Shot reporting.** Polling continues while a shot is brewing, so a job the grinder lists again mid-shot (~50 s after it was last taken) is taken again in time. No new job is taken during a shot. When the shot ends:
  - A shot stopped by weight is reported as `Success`, with its brew time and the final weight measured after the drip delay.
  - A flush (ended before `minShotDurationS`) leaves the job untouched for the real shot.
  - Every other ending (button, max duration) is reported as `Canceled`, so the grinder doesn't learn from it. A shot cut short by disabling the shotStopper or entering OTA mode sends nothing; the grinder drops the job itself.
  - Reports are retried until the grinder confirms them, re-taking the job first if it was re-listed.
- **Runs on the second core.** The grinder client runs in its own FreeRTOS task on core 0 and exchanges messages with `loop()`. Slow TLS handshakes and network timeouts can never delay button handling, weight processing or the stop decision. All BLE and EEPROM access stays in `loop()`.
- **WiFi always on when a grinder is configured.** The grinder client keeps the WiFi station connected, reusing the existing WiFi SSID and password settings. OTA mode keeps its current behavior as an exclusive maintenance mode: the grinder client idles during OTA and reconnects afterwards. `OTASetup.h` is not changed.
- **New BLE config characteristics.**
  - `0xFF25`: grinder IP address (string).
  - `0xFF26`: `pairingId` (uint32).
  - `0xFF27`: grinder link status (read/notify), so the companion app can show whether the link works.
- **EEPROM layout.** Grows to hold the grinder IP and `pairingId`. Devices upgrading from the current firmware keep their settings and start with no grinder configured.
- **New dependency.** ArduinoJson for building requests and parsing the grinder's replies.
- **Host tests.**
  - A scriptable fake grinder, plus fakes for `WiFiClientSecure`, `HTTPClient` and the FreeRTOS calls used.
  - Sketch scenarios covering the full grind → accept → shot → report timeline, including re-listing, expiry, flushes and cancellations.
  - A pinned copy of ArduinoJson's single header vendored into `examples/shotStopper/test/`, so the suite still runs offline.

No change to scale handling, shot-stop logic, or any existing characteristic or EEPROM value.

## Capabilities

### New Capabilities
- `grinder-gbs-sync`: Reporting shots to a paired Mahlkönig GbS grinder over its Generic WiFi API. Covers configuration, the WiFi link, brew job polling and taking, the mapping from how a shot ended to the reported result, report retries, expiry, link status, and isolation from shot timing.

### Modified Capabilities
- `host-test-harness`:
  - The suite may use a vendored, pinned header kept inside the test folder, while still needing no network or package installation.
  - The production files tested unmodified now include the new grinder client sources.
  - The fake phone covers the new characteristics (`0xFF10`-`0xFF27`).
  - A new requirement adds a scriptable fake grinder.

## Impact

- **Sketch code** (`examples/shotStopper/`):
  - `shotStopper.ino`: new characteristics, EEPROM fields and migration, posting shot events to the grinder task, publishing grinder status.
  - New `GrinderClient.h/.cpp`: the API client, the job state machine and the task entry point.
- **Unchanged**: `AcaiaArduinoBLE.{h,cpp}`, `OTASetup.h`, the root library.
- **Dependencies**: ArduinoJson (Arduino Library Manager; the sketch header documents the version). The ESP32 core's `WiFiClientSecure` and `HTTPClient`, which are already part of the core.
- **Runtime**:
  - BLE (scale and phone), WiFi and TLS run at the same time on the ESP32-S3. This adds heap use (mbedTLS) and current draw, and puts BLE and WiFi on a shared radio.
  - Polling continues during shots (needed to keep the job taken), so the scale's BLE shares the radio with a TLS request every 2 s while brewing.
  - This is verified on hardware at the end. There is no up-front spike.
- **Tests** (`examples/shotStopper/test/`): new fakes, a `FakeGrinder`, a vendored `ArduinoJson.h`, and grinder library and sketch tests.
- **Out of scope**:
  - Pairing with the grinder (`pairingApplication` / `pairingStatus`).
  - Grinder discovery on the network.
  - Live brew progress (`progressSupported` is always `false`).
  - Machines with more than one group head.
  - Changing OTA mode.
  - Fixing the existing SSID/password EEPROM overlap (noted as a risk, since WiFi credentials now matter more).
