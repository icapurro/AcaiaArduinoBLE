// End-to-end shot scenarios: brew input -> scale weights -> OUT pin.
#include "scenario.h"

// A latching paddle (Linea Mini style): the pump runs while the paddle is on
// or while shotStopper holds OUT high.
static bool latchingPump() { return brewInputActive() || outHigh(); }

TEST(latching_shot_stops_by_weight) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  bootConnected(scale);
  FakePhone phone;

  unsigned long pressedAt = millis();
  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));
  unsigned long latchedAt = outChangedAt(HIGH, pressedAt);
  // Latches on the first weight packet after minShotDurationS (3 s).
  CHECK(latchedAt - pressedAt > 3000);
  CHECK(latchedAt - pressedAt <= 3150);
  releaseBrew();  // paddle back; shotStopper keeps the pump running

  REQUIRE(runUntil(outLow, 40000));
  runFor(10);  // status characteristics update on the next loop()
  unsigned long stoppedAt = outChangedAt(LOW, latchedAt);
  // Auto-tare at the latch zeroes the scale, so at 2 g/s the 34.5 g target
  // (36 g goal - 1.5 g offset) is reached 17.25 s after latching. The sketch
  // stops on the first packet at or past its prediction (100 ms packets).
  CHECK(stoppedAt - latchedAt >= 17250);
  CHECK(stoppedAt - latchedAt <= 17350);
  CHECK_EQ(lastShotEndReason(), std::string("weight"));
  CHECK(fake::serialContains("weight achieved"));
  CHECK_EQ(phone.byteChanges("0xFF20"), (std::vector<int>{0, 1, 0}));  // shot status

  // Shot start and latch both tare; start and stop the scale timer.
  CHECK_EQ(scale.countWrites(acaia::TARE), 2);
  CHECK_EQ(scale.countWrites(acaia::START_TIMER), 1);
  CHECK_EQ(scale.countWrites(acaia::STOP_TIMER), 1);
}

TEST(latching_shot_ended_by_releasing_before_latch) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  bootConnected(scale);

  pressBrew();
  runFor(1000);  // less than minShotDurationS
  releaseBrew();
  REQUIRE(runUntil([] { return !lastShotEndReason().empty(); }, 1000));
  CHECK_EQ(lastShotEndReason(), std::string("button"));
  CHECK(outHistory().size() == 1);  // only the LOW from setup(): never latched
  runFor(5000);
  CHECK(outLow());
}

TEST(stalled_shot_ends_at_max_duration) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  cup.flowGramsPerS = 0.1f;  // choked: never reaches 10 g, so no prediction
  scale.setWeight(cup.weight());
  bootConnected(scale);

  unsigned long pressedAt = millis();
  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));
  releaseBrew();
  REQUIRE(runUntil(outLow, 60000));
  unsigned long stoppedAt = outChangedAt(LOW, pressedAt + 1);
  CHECK(fake::serialContains("Max brew duration reached"));
  CHECK_EQ(lastShotEndReason(), std::string("time"));
  CHECK(stoppedAt - pressedAt > 50000);
  CHECK(stoppedAt - pressedAt <= 50200);
}

TEST(momentary_shot_stops_by_weight_with_a_pulse) {
  StoredConfig config;
  config.momentary = true;
  preloadConfig(config);

  // A GS3-style momentary button: a press starts the pump, and shotStopper's
  // pulse on OUT (another "press") stops it.
  static unsigned long pumpStartedAt = 0;
  FakeScale scale(ScaleType::NEW);
  Espresso cup([] { return pumpStartedAt && outChangedAt(HIGH, pumpStartedAt) == 0; });
  scale.setWeight(cup.weight());
  bootConnected(scale);
  FakePhone phone;

  pumpStartedAt = millis();
  pressBrew();
  runFor(200);
  releaseBrew();  // shotStopper starts the shot on release
  // The pulse rises and falls inside one loop() (delay(300)), so wait on the
  // pin history rather than the current level.
  REQUIRE(runUntil([] { return outChangedAt(HIGH, pumpStartedAt) != 0; }, 40000));
  runFor(1000);

  unsigned long pulseStart = outChangedAt(HIGH, pumpStartedAt);
  unsigned long pulseEnd = outChangedAt(LOW, pulseStart);
  CHECK_EQ(pulseEnd - pulseStart, 300UL);
  CHECK(outLow());
  CHECK_EQ(lastShotEndReason(), std::string("weight"));
  CHECK_EQ(phone.byteChanges("0xFF20"), (std::vector<int>{0, 1, 0}));
  // The shot starts (and tares) once the release is debounced, then the
  // 34.5 g target arrives ~17.25 s later at 2 g/s.
  unsigned long shotStartedAt = 0;
  for (auto& w : scale.writes())
    if (w.bytes == acaia::TARE) shotStartedAt = w.timeMs;
  CHECK(shotStartedAt - pumpStartedAt > 200);
  CHECK(shotStartedAt - pumpStartedAt < 500);
  CHECK(pulseStart - shotStartedAt >= 17250);
  CHECK(pulseStart - shotStartedAt <= 17400);
}

// Runs a full latching shot with `dripGrams` arriving after the stop, then
// waits past the drip delay for the offset check.
static void pullLatchingShot(FakeScale& scale, Espresso& cup) {
  scale.setWeight(cup.weight());
  bootConnected(scale);
  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));
  releaseBrew();
  REQUIRE(runUntil(outLow, 40000));
  REQUIRE(runUntil([] { return fake::serialContains("I detected a final weight"); }, 10000));
}

TEST(offset_learns_from_final_weight) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  cup.dripGrams = 2.0f;
  pullLatchingShot(scale, cup);

  float finalWeight = detectedFinalWeight();
  CHECK(finalWeight > 36.0f && finalWeight < 37.5f);
  CHECK(fake::serialContains("Next time I'll create an offset of"));
  // offset += final - goal
  CHECK_NEAR(weightOffset, 1.5 + (finalWeight - 36.0), 0.011);
  CHECK_EQ(fake::eepromFlash(sketch::kOffsetAddr), (uint8_t)(weightOffset * 10));
  // Note: the offset is stored as (uint8_t)(offset * 10). A negative learned
  // offset (cup consistently under goal) would wrap or be undefined here.
}

TEST(offset_unchanged_when_error_too_large) {
  FakeScale scale(ScaleType::NEW);
  Espresso cup(latchingPump);
  cup.dripGrams = 10.0f;  // e.g. someone touched the cup
  pullLatchingShot(scale, cup);

  CHECK(fake::serialContains("Error assumed. Offset unchanged."));
  CHECK_NEAR(weightOffset, 1.5, 1e-6);
  CHECK_EQ(fake::eepromFlash(sketch::kOffsetAddr), (uint8_t)15);
}

TEST(latching_shot_on_bookoo_beeps_at_latch_instead_of_taring) {
  // Upstream c39a364: the latch signals the user with scale.beep(). On a
  // Bookoo that's a real beep, so the shot keeps its start-of-shot tare and
  // the 34.5 g target comes 17.25 s after the shot starts (2 g/s).
  FakeScale scale(ScaleType::GENERIC);
  Espresso cup(latchingPump);
  scale.setWeight(cup.weight());
  bootConnected(scale);

  unsigned long pressedAt = millis();
  pressBrew();
  REQUIRE(runUntil(outHigh, 5000));
  releaseBrew();
  REQUIRE(runUntil(outLow, 40000));
  CHECK_EQ(scale.countWrites(acaia::TARE_GENERIC), 1);         // shot start only
  CHECK_EQ(scale.countWrites(acaia::BEEP_LEVEL_1_BOOKOO), 1);  // at the latch
  CHECK_EQ(scale.countWrites(acaia::TARE_START_TIMER_BOOKOO), 0);  // CAN_TARE_START_TIMER is off
  unsigned long startedAt = 0;
  for (auto& w : scale.writes())
    if (w.bytes == acaia::TARE_GENERIC && w.timeMs >= pressedAt) startedAt = w.timeMs;
  unsigned long stoppedAt = outChangedAt(LOW, pressedAt + 1);
  CHECK(stoppedAt - startedAt >= 17250);
  CHECK(stoppedAt - startedAt <= 17400);
  CHECK_EQ(lastShotEndReason(), std::string("weight"));
}
