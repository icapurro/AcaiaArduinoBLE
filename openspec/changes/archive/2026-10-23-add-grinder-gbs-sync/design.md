## Context

See proposal.md for motivation and specs/grinder-gbs-sync/spec.md for the required behavior. The grinder API is the "Grind-by-Sync Generic WiFi API" v0.2.0 (written from gbs-anywhere, tested on E65W firmware V01.36 build #2).

Current state that shapes the design:

- **One control loop.** `loop()` in `shotStopper.ino` reads the brew input every 5 ms, handles a weight packet about every 100 ms, and decides when to stop the shot, all in one task on core 1. Anything blocking there delays the stop.
- **Grinder requests are slow and can hang.** The grinder closes reused connections, so every request is a fresh TLS handshake, roughly 0.5–1.5 s on an ESP32-S3. A request can hang until its 8 s timeout.
- **WiFi is OTA-only today.** `OTASetup.h` starts WiFi only while OTA is requested (`checkOTAMode()` → `connectToWifi()`). It disconnects only when OTA goes from on to off. During OTA, `loop()` returns early, so OTA is already an exclusive mode.
- **The shot end is decided in `setBrewingState(false)`.** It knows `shot.end` (WEIGHT/BUTTON/TIME) and computes `shot.end_s`, then resets `shot.end` to UNDEF.
- **The existing final-weight check doesn't always run.** It ("Detect error of shot") only runs when `currentWeight >= goalWeight - weightOffset`, so it can't double as the brewDone measurement.
- **EEPROM is full.** `EEPROM_SIZE` is 75, and the password slot ends at byte 74.
- **The host harness is single-threaded.** It compiles the sketch unmodified against fakes in `examples/shotStopper/test/fakes/`, and scenario helpers drive `loop()` on a simulated clock.

## Goals / Non-Goals

**Goals:**
- Network work can never add latency to shot control.
- Keep the grinder logic in its own unit (`GrinderClient`) with a small message interface, so the sketch changes stay small and the client can be tested in isolation.
- Make the whole job lifecycle (grind, take, re-list, expire, report, retry) testable on the host with simulated time.

**Non-Goals:**
- Pairing, grinder discovery, brew progress, multiple group heads.
- Running OTA and grinder sync at the same time.
- Fixing the existing SSID/password EEPROM overlap.
- Measuring real-world BLE/WiFi coexistence timing on the host. That needs hardware.

## Decisions

### 1. A `GrinderClient` driven by a FreeRTOS task on core 0
- **Files.** New `examples/shotStopper/GrinderClient.h/.cpp`. The Arduino IDE compiles top-level `.cpp` files in a sketch folder.
- **Starting the task.** `setup()` creates the task with `xTaskCreatePinnedToCore(grinderTask, "grinder", 16384, &grinder, 1, nullptr, 0)`. The 16 KB stack covers an mbedTLS handshake on the task stack; 8 KB is a known crash.
- **Task body.**
  ```cpp
  static void grinderTask(void* arg) {
    auto* client = static_cast<GrinderClient*>(arg);
    for (;;) { client->runOnce(); vTaskDelay(pdMS_TO_TICKS(20)); }
  }
  ```
- **What `runOnce()` does.** Each call:
  1. drains the event queue;
  2. advances the state machine by at most one step (for example, one HTTP request);
  3. returns.

  All timing comes from `millis()` compared against stored deadlines. There are no sleeps inside `runOnce()`.
- **Why core 0 at priority 1.** The WiFi and BT stacks run on core 0 at higher priorities, so the client only uses leftover time. A roughly 1 s CPU-bound handshake stays well under the 5 s task watchdog. On core 1, the handshake would time-slice against `loop()`.
- *Alternative:* a non-blocking HTTP state machine inside `loop()`. Rejected: the TLS handshake itself blocks for about a second, and the ESP32 core has no async HTTPS client.
- *Alternative:* `esp_http_client` (IDF). It offers no advantage here, and `HTTPClient`/`WiFiClientSecure` are already used by Arduino sketches and easier to fake.

### 2. Messages in, a status value out
- **Into the client: a FreeRTOS queue.** `xQueueCreate(8, sizeof(GrinderEvent))`. `loop()` sends with a 0 timeout, so it never blocks. `GrinderEvent` is plain data:
  ```cpp
  enum class GrinderEventType : uint8_t { Config, Active, ShotStarted, ShotFinished };
  enum class ShotOutcome : uint8_t { Success, Canceled, Flush };
  struct GrinderEvent {
    GrinderEventType type;
    bool active;                        // Active
    ShotOutcome outcome;                // ShotFinished
    uint32_t durationMs, massMg;        // ShotFinished (Success)
    char ip[16]; uint32_t pairingId;    // Config
    char ssid[33]; char pass[33];       // Config
  };
  ```
- **Out of the client: one status byte.** `std::atomic<uint8_t>` holds the link status (0–6 per spec). `loop()` reads it each iteration and writes `0xFF27` only when it changes. That's the only state shared directly, and it's lock-free.
- **The client touches nothing else.** No sketch globals, no BLE, no EEPROM. Configuration arrives as copies in `Config` events.
- **When `loop()` sends events:**
  - `Config`: at boot, and when the phone writes `0xFF22`, `0xFF23`, `0xFF25` or `0xFF26`.
  - `Active`: whenever the shotStopper's active state changes (enabled and not in OTA mode).
  - `ShotStarted`: in `setBrewingState(true)`.
  - `ShotFinished`: see Decision 3.
- **If the queue is full** (8 events, which shouldn't happen), the event is dropped and logged to Serial. The client recovers on the next `Config`.

### 3. Classifying shot outcomes in `loop()`
In `setBrewingState(false)`, before `shot.end` is reset, the sketch decides:
- `WEIGHT` → **Success**. It stores a pending report: `durationMs = round(shot.end_s * 1000)` and a deadline of `end + dripDelayS`. A new, separate check in `loop()` sends `ShotFinished{Success, durationMs, massMg}` once the deadline passes, with `massMg = clamp(round(currentWeight * 1000), 0, 5000000)`. It doesn't depend on the offset-learning condition.
- `BUTTON` with `shot.end_s < minShotDurationS` → **Flush**, sent immediately.
- `BUTTON` otherwise, or `TIME` → **Canceled**, sent immediately.

A shot ended by disabling or OTA (`TIME` reached through the inactive path) sends `Active(false)` first. The client then ignores the `ShotFinished`, as the spec requires. The sketch sends `Active(false)` before calling `setBrewingState(false)` in that branch.

### 4. Client state machine
```
           Config(ip set) + Active + WiFi up
 [NOT_CONFIGURED] ------------------------------> [IDLE]  poll brewJobs every 2 s
        ^                                          |  new job (newest unseen)
        | Config(ip "")                            v
        +---------------------------------- [JOB_TAKEN] poll; re-accept on re-list;
                                                   |  newer job -> report old Canceled, take new
                                       ShotStarted |  5 min since first taken -> report Canceled
                                                   v
                                              [IN_SHOT]  keep polling; re-take the job if
                                                         re-listed; take no new job
                         ShotFinished(Flush) /     | ShotFinished(Success|Canceled)
                    back to JOB_TAKEN <------+     v
                                              [REPORTING] brewDone (same brewEventUuid);
                                                   |  OngoingJobNotExisting -> brewAccept, retry
                                                   |  Ok, or window (120 s / 20 s) exceeded
                                                   v
                                                [IDLE]
```
- **ShotStarted in IDLE** (no job): the client moves to `IN_SHOT` with no job and keeps polling, but takes no new job. On `ShotFinished`, it returns to IDLE without sending anything.
- **Newest job.** The order of jobs within a reply isn't specified. The client keeps the `grindUuid`s it has seen with their first-seen time. "Newest" is a job not seen before; if one reply has several unseen jobs, the last one in the array wins.
- **Re-listing.** A taken job showing up in `brewJobs` again triggers `brewAccept` for it.
- **403 handling.** One counter of consecutive 403s. At 3, status becomes "pairing rejected" and the poll interval becomes 60 s. A success or a new `Config` resets it.
- **Backoff.** On a transport failure, the next request waits 2 s, doubling up to a 60 s cap. A success resets it.
- **Report windows.** `brewEventUuid` is generated once per report from `esp_random()` as a UUID v4 string. `Canceled` reports for replaced or expired jobs use the same delivery logic, with the 20 s window.

### 5. HTTP transport
- **One helper per request:**
  1. `WiFiClientSecure tls; tls.setInsecure();`
  2. `HTTPClient http; http.setReuse(false); http.setTimeout(8000); http.setConnectTimeout(8000);`
  3. Leave redirects at the default (`HTTPC_DISABLE_FOLLOW_REDIRECTS`).
  4. `http.begin(tls, url)`, then `addHeader("Content-Type", "application/json")`.
  5. `POST(body)`. Reject replies whose `getSize()` is over 65536 or unknown and too large. Read with `getString()`, then `end()`.
- **Header casing.** The ESP32 core's `HTTPClient` writes `Content-Length` with that exact casing, which the grinder requires. This is confirmed on hardware (see Risks).
- **Error mapping.** HTTP 200 + `status: "Ok"` → ok. HTTP 400 → the `ClientError…` name. HTTP 403 → `Forbidden`. Anything else, or negative codes → transport failure.

### 6. JSON with ArduinoJson 7
- **The library.** The sketch uses ArduinoJson 7 (`JsonDocument`), installed from the Arduino Library Manager. The version is noted in the sketch header comment.
- **Parsing.** Replies are parsed as JSON. Numbers are read with `as<double>()` and rounded, since the grinder may send `28000.0`. A missing `groupHeadId` becomes 0. `doseTypeExpected` is echoed as-is, including `null`.
- **For the host tests:** the single-header release `ArduinoJson-v7.2.0.h` is vendored as `examples/shotStopper/test/vendor/ArduinoJson.h`, with `vendor/VERSION.md` recording the version and source URL. The Makefile adds `-Ivendor`.
- *Alternative:* hand-rolled parsing. Rejected in exploration: more code to maintain, for no real gain.

### 7. WiFi ownership
- **The client owns WiFi while active and configured.** When WiFi is down, it calls `WiFi.mode(WIFI_STA)` and `WiFi.begin(ssid, pass)`, at most every 10 s. Meanwhile status is "connecting".
- **It never calls `WiFi.disconnect()`.** Turning grinder sync off just stops requests, and WiFi stays up until reboot or OTA ends.
- **OTA.** When OTA is requested, the client receives `Active(false)` and goes quiet. `OTASetup.h` runs unchanged: its `WiFi.begin` re-associates, and leaving OTA disconnects. Once `Active(true)` arrives again, the client reconnects on its next cycle.
- **No overlapping WiFi calls.** `OTASetup.h` and the client only touch WiFi in mutually exclusive modes, so the two cores never call WiFi APIs at the same time.
- **Amended after hardware testing.** Calling `WiFi.begin()` while the station is still connecting fails on the ESP32 ("sta is connecting, cannot set config"). That broke OTA when grinder sync had an attempt in progress. So:
  - the client calls `WiFi.begin()` once, with auto-reconnect on, and only aborts (`WiFi.disconnect()`) and restarts an attempt after 30 s without a connection;
  - it starts afresh when it becomes active again;
  - it logs the WiFi status on each retry.
- **`OTASetup.h` now has a small change.** It reuses an existing connection, and stops any attempt in progress before its own `begin()`.

### 8. EEPROM layout and migration
| Address | Field |
|---|---|
| 75 | grinder fields signature (`0xA5`) |
| 76–91 | grinder IP, null-terminated, up to 15 characters |
| 92–95 | `pairingId`, little-endian uint32 |

- **Size.** `EEPROM_SIZE` grows from 75 to 128.
- **First boot.** The default-initialization path also writes empty grinder fields and the signature.
- **Upgrade.** When the main signature is valid but byte 75 isn't `0xA5`, the sketch writes empty grinder fields plus the signature and commits. The bytes beyond the old 75-byte region may read as 0x00 or 0xFF after the resize, so the explicit signature avoids depending on either.
- **Rollback.** Older firmware calls `EEPROM.begin(75)` and ignores bytes 75+.
- **IP validation.** Accepted values are four dot-separated decimal octets, 0–255 each. Anything else is rejected without persisting.

### 9. BLE characteristics
- `0xFF25`: `BLECharacteristic`, `BLEWrite | BLERead`, 16 bytes, the IP string.
- `0xFF26`: `BLECharacteristic`, `BLEWrite | BLERead`, 4 bytes, little-endian `pairingId`.
- `0xFF27`: `BLEByteCharacteristic`, `BLERead | BLENotify`, the status value.

They're added to the existing service, so they follow the existing pattern in `initializeBLE()` and `pollAndReadBLE()`.

### 10. Host harness extensions
- **New fakes.**
  - `fakes/WiFiClientSecure.h` and `fakes/HTTPClient.h`. `HTTPClient::POST` routes to whichever fake grinder is registered for the URL's host and port; with none, it returns a connection error.
  - FreeRTOS fakes (`xTaskCreatePinnedToCore`, `xQueueCreate`/`Send`/`Receive`, `vTaskDelay`, `pdMS_TO_TICKS`), reached through the fake `Arduino.h`, the same way the real core includes them.
  - `esp_random()`, seeded deterministically.
  - `WiFi.mode()`.
- **Tasks don't run.** `xTaskCreatePinnedToCore` only records the task (name, core, stack, priority). The scenario helper `step()` calls `loop()` and then `grinder.runOnce()` (through `sketch_api.h`), so they interleave in a fixed order on the same clock. A context flag set around each call lets the fake `HTTPClient` record whether a request came from `loop()`.
- **Network calls take no simulated time.** A request "takes" zero simulated time, and an injected timeout just fails the request. That keeps the interleaving deterministic. Real concurrency, and the fact that a hanging request doesn't stall `loop()`, can't be simulated by a single-threaded harness. The structural check that no request comes from `loop()` stands in for it, and hardware testing covers the rest.
- **`support/fake_grinder.{h,cpp}`.** It implements the three operations with ArduinoJson (the vendored copy) and simulated-time job lifecycle rules (drop untaken after 60 s, re-list taken every 50 s). It records requests and supports injected faults.
- **Build changes.** The Makefile compiles `../GrinderClient.cpp` with `PROD_FLAGS` into both binaries. Library-level client tests (`library/test_grinder_client.cpp`) drive a `GrinderClient` directly with events and no sketch.

## Risks / Trade-offs

- [BLE + WiFi + TLS together exhaust heap or cause BLE drops on the S3] → Polling continues during shots (amended: the grinder re-lists a taken job ~50 s after the last take, shot or not; see the "Amended" note in Decision 7's section and tasks 4.6). If hardware shows scale dropouts mid-shot, poll every 5 s during shots: still well inside the ~60 s before a re-listed job is dropped. Free heap is logged at each grinder state change. Hardware verification is the last task, and it includes a 30-minute soak with the scale connected and repeated grinds. If heap is tight, the fallback is to lower the TLS buffer sizes (an mbedTLS config) or poll less often.
- [Current draw rises with WiFi on (the `LOW_VOLTAGE` comment says V3 is power-sensitive)] → WiFi only starts when a grinder is configured. Measure on the V3 board during hardware verification. The `LOW_VOLTAGE` 80 MHz option makes handshakes about 3× slower, which is acceptable on core 0.
- [`HTTPClient` header casing or keep-alive behavior differs from what the grinder needs] → Verify with the real grinder: a first `brewJobs` with a correct pairingId returns `Ok`. `setReuse(false)` sends `Connection: close`.
- [ArduinoBLE isn't thread-safe] → Only `loop()` calls BLE, enforced by design. The client gets copies through the queue.
- [The host tests can't prove timing isolation, only structure] → The no-requests-from-loop check, plus the same-shot-timing scenario with a failing grinder, plus hardware verification.
- [Job order in `brewJobs` isn't specified, so "newest" could be wrong when two grinds happen within one poll interval] → Rare: two grinds within 2 s. The last-in-array rule is documented. A wrong pick only affects that shot's learning.
- [The existing SSID/password overlap bug: a 32-character SSID erases the stored password] → Out of scope, but more visible now. Called out in the test README and proposal. A follow-up change should fix it.
- [The grinder's handling of long shots is unknown (the E64 rejects shots over 80 s)] → `maxShotDurationS` defaults to 50 s, and only `Success` (stopped by weight) reports a duration.

## Migration Plan

1. Flash the new firmware over USB or OTA. On first boot, the EEPROM migration adds the empty grinder fields, and all existing settings are kept.
2. Pair the grinder outside the shotStopper, then write the grinder IP and pairingId from the companion app or LightBlue. Make sure WiFi credentials are set.
3. **Rollback:** flash the previous firmware. It ignores the extra EEPROM bytes.

## Open Questions

- What `doseTypeExpected` values mean. We echo them back unchanged, so this doesn't affect the implementation.
- Whether the grinder answers `brewAccept`/`brewDone` with 403 for an unknown pairingId. Either way it's treated as a failed request, and the backoff and report windows apply.
