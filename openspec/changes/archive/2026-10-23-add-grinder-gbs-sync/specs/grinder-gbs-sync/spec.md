## Purpose

Lets the shotStopper stand in for the espresso machine in a Mahlkönig E65/E80 GbS grinder's Grind-by-Sync loop. It takes the brew job each grind creates and reports the shot's brew time and weight back over the grinder's local Generic WiFi API, so the grinder can adjust its grind setting.

## ADDED Requirements

### Requirement: Grinder configuration over BLE
The shotStopper SHALL expose three BLE config characteristics:
- the grinder's IP address (`0xFF25`, read/write, a dotted IPv4 string of up to 15 characters);
- the `pairingId` (`0xFF26`, read/write, 4 bytes, unsigned 32-bit little-endian);
- the grinder link status (`0xFF27`, read/notify).

The IP address and `pairingId` SHALL be persisted in EEPROM and restored at boot. Grinder sync SHALL be active only while a grinder IP address is configured; writing an empty IP address SHALL turn it off.

#### Scenario: Configure the grinder from the phone
- **WHEN** the phone writes `192.168.1.30` to `0xFF25` and pairingId 42 to `0xFF26`
- **THEN** both values are applied, persisted to EEPROM, and readable back from the characteristics

#### Scenario: Configuration survives a reboot
- **WHEN** the shotStopper boots with a grinder IP address and pairingId stored in EEPROM
- **THEN** grinder sync starts with those values without any phone interaction

#### Scenario: Upgrade from firmware without grinder support
- **WHEN** the shotStopper boots with EEPROM written by the previous firmware, which has a valid signature but no grinder fields
- **THEN** all existing settings are kept, and grinder sync is off with no grinder configured

#### Scenario: Turning grinder sync off
- **WHEN** the phone writes an empty string to `0xFF25`
- **THEN** grinder sync stops, no further grinder requests are sent, and the link status reports "not configured"

#### Scenario: Invalid address rejected
- **WHEN** the phone writes a value to `0xFF25` that is not a dotted IPv4 address
- **THEN** the previous configuration is kept, and nothing is persisted

### Requirement: WiFi link for grinder sync
While grinder sync is active, the shotStopper SHALL keep its WiFi station connected, using the WiFi SSID and password already configured through `0xFF22`/`0xFF23`. It SHALL reconnect after a lost connection. While grinder sync is not active, it SHALL NOT start WiFi for this feature.

#### Scenario: Connect at boot
- **WHEN** the shotStopper boots with WiFi credentials and a grinder configured
- **THEN** it connects to WiFi without OTA mode being requested

#### Scenario: Reconnect after a drop
- **WHEN** the WiFi connection drops while grinder sync is active
- **THEN** the shotStopper reconnects, and resumes polling once connected

#### Scenario: No grinder, no WiFi
- **WHEN** no grinder IP address is configured and OTA mode is not requested
- **THEN** the shotStopper does not start WiFi

### Requirement: Grinder API transport
Every grinder request SHALL be an HTTPS `POST` to `https://<grinder IP>:30001/grinderApi/<operation>` with a JSON body that includes `"version": 1` and `Content-Type: application/json`. Requests SHALL:
- skip certificate verification (the grinder's certificate is self-signed);
- use a new connection each time;
- not follow redirects;
- time out after 8 s.

Replies larger than 64 KiB SHALL be rejected. After a failed request (timeout, connection error, or unparseable reply), the next attempt SHALL wait 2 s, doubling up to 60 s, and the wait SHALL reset after a successful request.

#### Scenario: Request shape
- **WHEN** the shotStopper polls the grinder at `192.168.1.30` with pairingId 42
- **THEN** it sends `POST https://192.168.1.30:30001/grinderApi/brewJobs` with the body `{"version":1,"pairingId":42}`

#### Scenario: Grinder unreachable
- **WHEN** consecutive requests to the grinder fail
- **THEN** the waits between attempts are 2 s, 4 s, 8 s and so on, capped at 60 s, and the link status reports "grinder unreachable"

#### Scenario: Recovery resets the backoff
- **WHEN** a request succeeds after a series of failures
- **THEN** the next poll follows the normal 2 s interval

### Requirement: Brew job polling
While grinder sync is active and WiFi is connected, the shotStopper SHALL poll `brewJobs` every 2 s, including while a shot is brewing: the grinder lists a taken job again about 50 s after its last `brewAccept`, whether or not a shot is running, and the job must be taken again before the shot's report. A single HTTP 403 reply SHALL be tolerated as a grinder that is still starting up. Three consecutive 403 replies SHALL set the link status to "pairing rejected", after which the shotStopper SHALL keep checking at the 60 s backoff interval until a request succeeds or the configuration changes.

#### Scenario: Idle polling
- **WHEN** grinder sync is active, WiFi is connected and no shot is brewing
- **THEN** a `brewJobs` request is sent about every 2 s, and the link status reports "ready"

#### Scenario: Polling continues during a shot
- **WHEN** a shot is brewing
- **THEN** `brewJobs` is still polled about every 2 s

#### Scenario: Pairing cleared on the grinder
- **WHEN** three `brewJobs` requests in a row are answered with HTTP 403
- **THEN** the link status reports "pairing rejected", and polling slows to every 60 s

#### Scenario: A single 403
- **WHEN** one `brewJobs` request is answered with HTTP 403 and the next one with `Ok`
- **THEN** the link status stays "ready"

### Requirement: Taking brew jobs
When `brewJobs` lists a job the shotStopper has not taken, it SHALL take the newest such job immediately with `brewAccept`. That request SHALL carry the job's `grindUuid`, its `groupHeadId` (0 when the job has none), and its `doseTypeExpected` echoed as `doseType` (`null` when absent). Numeric fields SHALL be accepted as integers or floats. Each time the grinder lists a taken job again, the shotStopper SHALL take it again, including during a shot. It SHALL NOT take a job again while it is still taken (the grinder refuses that with `ClientErrorBrewJobNotExisting`). While a shot is brewing, no new job SHALL be taken; a job first listed during the shot is taken once the shot's outcome is settled. While a job is taken and no shot has used it:
- if a newer job appears, the shotStopper SHALL report the older taken job as `Canceled` and take the newer one;
- if 5 minutes pass since the job was first taken, the shotStopper SHALL report it as `Canceled` and wait for a new grind.

#### Scenario: Job listed again during a shot
- **WHEN** job "g1" was taken 25 s before a 28 s shot starts, so the grinder lists it again during the shot
- **THEN** the shotStopper takes "g1" again at its next poll, and the shot's `brewDone` is accepted on the first attempt

#### Scenario: Grind during a shot
- **WHEN** the grinder lists a new job "g2" while a shot for "g1" is brewing
- **THEN** "g2" is not taken until the report for "g1" is settled, and is taken after that

#### Scenario: Take a new job
- **WHEN** the grinder lists a job with `grindUuid` "g1", no `groupHeadId` and `doseTypeExpected` 2
- **THEN** the shotStopper sends `brewAccept` with `grindUuid` "g1", `groupHeadId` 0 and `doseType` 2, and the link status reports "job taken"

#### Scenario: Job listed again
- **WHEN** the taken job "g1" appears again in a `brewJobs` reply about 50 s later
- **THEN** the shotStopper sends `brewAccept` for "g1" again

#### Scenario: Grinding again before pulling
- **WHEN** job "g1" is taken and the grinder lists a newer job "g2"
- **THEN** the shotStopper reports "g1" with `result` `Canceled`, and takes "g2"

#### Scenario: Job expires unused
- **WHEN** job "g1" was first taken more than 5 minutes ago and no shot has used it
- **THEN** the shotStopper reports "g1" as `Canceled`, and the link status returns to "ready"

### Requirement: Shot outcome reporting
When a shot ends while a job is taken, the shotStopper SHALL report the job with `brewDone` according to how the shot ended:
- **Stopped by weight**: `result` `Success`. `durationFinalMs` is the shot's duration from start to stop, in milliseconds. `massFinalMg` is the scale weight measured once the drip delay (`dripDelayS`) has passed, in milligrams, clamped to 0..5,000,000.
- **Flush**, meaning a shot ended by the button before `minShotDurationS`: no report. The job stays taken for the next shot, and its 5-minute expiry keeps running.
- **Any other ending** (button after `minShotDurationS`, or maximum duration): `result` `Canceled`, with `durationFinalMs` and `massFinalMg` set to 0, sent as soon as the shot ends.

A shot that ends with no taken job SHALL send nothing. A shot cut short by disabling the shotStopper or requesting OTA mode SHALL send nothing either; the grinder drops the job on its own (see *Grinder sync idles when the shotStopper is inactive*).

#### Scenario: Successful shot reported with time and weight
- **WHEN** a shot with a taken job "g1" is stopped by weight after 28.4 s, and the scale reads 37.2 g once the 3 s drip delay has passed
- **THEN** the shotStopper sends `brewDone` for "g1" with `durationFinalMs` 28400, `massFinalMg` 37200 and `result` `Success`

#### Scenario: Flush keeps the job
- **WHEN** a job is taken and the user runs a 2 s flush with `minShotDurationS` 3
- **THEN** no `brewDone` is sent, and the next shot stopped by weight is reported against that job

#### Scenario: Manual stop is not learned from
- **WHEN** a shot with a taken job is ended by the button after `minShotDurationS`
- **THEN** the shotStopper sends `brewDone` with `result` `Canceled`

#### Scenario: Max duration is not learned from
- **WHEN** a shot with a taken job is ended by the maximum shot duration
- **THEN** the shotStopper sends `brewDone` with `result` `Canceled`

#### Scenario: Shot without a grind
- **WHEN** a shot is stopped by weight and no job is taken
- **THEN** no `brewDone` is sent

### Requirement: Report delivery
Each report SHALL carry a `brewEventUuid` that is new for every report and the same on every retry of it. When the grinder answers `ClientErrorOngoingJobNotExisting` (the job was listed again and the re-list was missed), the shotStopper SHALL take the job again with `brewAccept` straight away, without waiting to see it in `brewJobs`, then resend the same report. The first attempt at a shot's report SHALL be made as soon as the shot's outcome is known, without waiting out a backoff from earlier failed requests, because a re-listed job that nobody takes is likely dropped about 60 s later. A report SHALL be retried until the grinder answers `Ok`, for up to 120 s for `Success` and 20 s for `Canceled`, then dropped. Polling SHALL resume once the report is confirmed or dropped.

#### Scenario: Job was listed again during a long shot
- **WHEN** `brewDone` is answered with `ClientErrorOngoingJobNotExisting`
- **THEN** the shotStopper sends `brewAccept` for that job, then sends `brewDone` again with the same `brewEventUuid`

#### Scenario: Grinder briefly unreachable after the shot
- **WHEN** the first `brewDone` attempts fail with connection errors and a later attempt within 120 s succeeds
- **THEN** every attempt carries the same `brewEventUuid` and values, and polling resumes after the success

#### Scenario: Report abandoned
- **WHEN** a `Success` report has not been confirmed 120 s after the shot's outcome was settled
- **THEN** the shotStopper stops retrying it and resumes polling

### Requirement: Grinder sync never affects shot control
Grinder communication SHALL run independently of the shot-control loop. Network requests, TLS handshakes, timeouts and retries SHALL NOT be performed from the loop that reads the brew input, processes scale weights and decides when to stop the shot. All BLE and EEPROM access SHALL remain in that loop.

#### Scenario: Unreachable grinder during a shot
- **WHEN** the grinder is configured but every request times out
- **THEN** shots start, latch and stop at the same times and weights as with grinder sync turned off

#### Scenario: No network calls from the control loop
- **WHEN** the shot-control loop runs through a complete grind, shot and report
- **THEN** none of the grinder requests are made from it

### Requirement: Link status reporting
The link status characteristic (`0xFF27`) SHALL publish one of these values, and notify on every change:

| Value | State |
|---|---|
| 0 | not configured |
| 1 | connecting to WiFi |
| 2 | grinder unreachable |
| 3 | pairing rejected |
| 4 | ready, polling with no job |
| 5 | job taken |
| 6 | reporting a shot |

#### Scenario: Status follows the link
- **WHEN** a grinder is configured, WiFi connects, the grinder answers, a job is taken and its shot is reported
- **THEN** the phone observes the status values 1, 4, 5, 6 and 4, in that order

### Requirement: Grinder sync idles when the shotStopper is inactive
While OTA mode is requested or the shotStopper is disabled (`0xFF10` = 0), the grinder client SHALL send no requests, and SHALL NOT start or stop WiFi itself. When the shotStopper becomes active again, grinder sync SHALL resume, reconnecting WiFi if needed. A job taken before going inactive SHALL be left for the grinder to drop.

#### Scenario: OTA mode
- **WHEN** OTA mode is requested while grinder sync is active
- **THEN** no grinder requests are sent until OTA mode ends, after which polling resumes once WiFi is connected again

#### Scenario: Disabled
- **WHEN** the shotStopper is disabled from the phone
- **THEN** no grinder requests are sent, and no new jobs are taken, until it is enabled again

#### Scenario: Disabled during a shot
- **WHEN** the shotStopper is disabled while a shot with a taken job is brewing
- **THEN** no `brewDone` is sent for that job
