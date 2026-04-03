#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "RateTracker.h"
#include "ScaleEngine.h"

namespace {

ScaleEngineConfig TestConfig() {
  return ScaleEngineConfig{
      .present_threshold_g = 5.0f,
      .removed_threshold_g = 2.0f,
      .stable_duration_ms = 800,
      .removed_duration_ms = 500,
  };
}

ScaleViewModel Feed(ScaleEngine& engine, float grams, std::uint32_t timestamp_ms,
                    bool stable = true) {
  ScaleSample sample{};
  sample.grams = grams;
  sample.timestamp_ms = timestamp_ms;
  sample.stable = stable;
  return engine.Update(sample);
}

bool NearlyEqual(float left, float right, float tolerance = 0.05f) {
  return std::fabs(left - right) <= tolerance;
}

void TestInitialBaselineAcquisition() {
  ScaleEngine engine(TestConfig());
  Feed(engine, 10.0f, 0);
  const ScaleViewModel view = Feed(engine, 10.0f, 900);
  assert(view.state == ScaleState::kTracking);
  assert(view.has_baseline);
  assert(NearlyEqual(view.baseline_g, 10.0f));
  assert(!view.has_latched_delta);
}

void TestPositiveDelta() {
  ScaleEngine engine(TestConfig());
  Feed(engine, 20.0f, 0);
  Feed(engine, 20.0f, 900);
  Feed(engine, 0.0f, 1000);
  Feed(engine, 0.0f, 1600);
  Feed(engine, 25.0f, 2500);
  const ScaleViewModel view = Feed(engine, 25.0f, 3400);
  assert(view.state == ScaleState::kTracking);
  assert(view.has_latched_delta);
  assert(NearlyEqual(view.latched_delta_g, 5.0f));
  assert(NearlyEqual(view.baseline_g, 25.0f));
}

void TestNegativeDelta() {
  ScaleEngine engine(TestConfig());
  Feed(engine, 30.0f, 0);
  Feed(engine, 30.0f, 900);
  Feed(engine, 0.0f, 1000);
  Feed(engine, 0.0f, 1600);
  Feed(engine, 18.5f, 2600);
  const ScaleViewModel view = Feed(engine, 18.5f, 3500);
  assert(view.has_latched_delta);
  assert(NearlyEqual(view.latched_delta_g, -11.5f));
  assert(NearlyEqual(view.baseline_g, 18.5f));
}

void TestNoiseDoesNotAcquireBaseline() {
  ScaleEngine engine(TestConfig());
  Feed(engine, 9.0f, 0, false);
  Feed(engine, 10.0f, 500, false);
  const ScaleViewModel view = Feed(engine, 12.0f, 1200, false);
  assert(view.state == ScaleState::kWaitBaseline);
  assert(!view.has_baseline);
}

void TestDriftWithoutRemovalDoesNotLatchDelta() {
  ScaleEngine engine(TestConfig());
  Feed(engine, 15.0f, 0);
  Feed(engine, 15.0f, 900);
  const ScaleViewModel view = Feed(engine, 16.5f, 2500);
  assert(view.state == ScaleState::kTracking);
  assert(!view.has_latched_delta);
  assert(NearlyEqual(view.baseline_g, 15.0f));
}

void TestRepeatedCycles() {
  ScaleEngine engine(TestConfig());
  Feed(engine, 12.0f, 0);
  Feed(engine, 12.0f, 900);
  Feed(engine, 0.0f, 1000);
  Feed(engine, 0.0f, 1600);
  Feed(engine, 15.0f, 2500);
  Feed(engine, 15.0f, 3400);
  Feed(engine, 0.0f, 3600);
  Feed(engine, 0.0f, 4200);
  Feed(engine, 11.0f, 5200);
  const ScaleViewModel view = Feed(engine, 11.0f, 6100);
  assert(view.has_latched_delta);
  assert(NearlyEqual(view.latched_delta_g, -4.0f));
  assert(NearlyEqual(view.baseline_g, 11.0f));
}

void TestIdleWakeCapturesMissedDelta() {
  ScaleEngine engine(TestConfig());
  Feed(engine, 20.0f, 0);
  Feed(engine, 20.0f, 900);

  ScaleSample wake_sample{};
  wake_sample.grams = 15.0f;
  wake_sample.timestamp_ms = 5000;
  wake_sample.stable = true;

  const ScaleViewModel view = engine.UpdateFromIdleWake(wake_sample, 3.0f);
  assert(view.state == ScaleState::kTracking);
  assert(view.has_latched_delta);
  assert(NearlyEqual(view.previous_baseline_g, 20.0f));
  assert(NearlyEqual(view.latched_delta_g, -5.0f));
  assert(NearlyEqual(view.baseline_g, 15.0f));
}

void TestIdleWakeIgnoresSmallChanges() {
  ScaleEngine engine(TestConfig());
  Feed(engine, 20.0f, 0);
  Feed(engine, 20.0f, 900);

  ScaleSample wake_sample{};
  wake_sample.grams = 21.5f;
  wake_sample.timestamp_ms = 5000;
  wake_sample.stable = true;

  const ScaleViewModel view = engine.UpdateFromIdleWake(wake_sample, 3.0f);
  assert(view.state == ScaleState::kTracking);
  assert(!view.has_latched_delta);
  assert(NearlyEqual(view.baseline_g, 20.0f));
}

void TestIdleWakeDoesNotSkipRemovedState() {
  ScaleEngine engine(TestConfig());
  Feed(engine, 20.0f, 0);
  Feed(engine, 20.0f, 900);
  Feed(engine, 0.0f, 1000);
  Feed(engine, 0.0f, 1600);

  ScaleSample wake_sample{};
  wake_sample.grams = 15.0f;
  wake_sample.timestamp_ms = 2500;
  wake_sample.stable = true;

  const ScaleViewModel view = engine.UpdateFromIdleWake(wake_sample, 3.0f);
  assert(view.state == ScaleState::kRemoved);
  assert(!view.has_latched_delta);
  assert(NearlyEqual(view.baseline_g, 20.0f));
}

RateTrackerConfig TestRateConfig(std::uint32_t min_elapsed_ms = 5000,
                                 std::uint32_t refresh_interval_ms = 5000) {
  return RateTrackerConfig{
      .min_elapsed_ms = min_elapsed_ms,
      .refresh_interval_ms = refresh_interval_ms,
  };
}

void TestRateTrackerWaitsForMinElapsed() {
  RateTracker tracker(TestRateConfig());
  tracker.Start(0);
  tracker.RecordDelta(-10.0f);

  const RateViewModel view = tracker.Update(4000, true);
  assert(!view.has_rate);
  assert(!view.display_dirty);
}

void TestRateTrackerHoldsBetweenIntervals() {
  RateTracker tracker(TestRateConfig());
  tracker.Start(0);
  tracker.RecordDelta(-5.0f);

  const RateViewModel refreshed = tracker.Update(5000, true);
  assert(refreshed.has_rate);
  assert(refreshed.display_dirty);
  assert(NearlyEqual(refreshed.grams_per_hour, 3600.0f));

  const RateViewModel held = tracker.Update(8000);
  assert(held.has_rate);
  assert(!held.display_dirty);
  assert(NearlyEqual(held.grams_per_hour, 3600.0f));
}

void TestRateTrackerRefreshesAtInterval() {
  RateTracker tracker(TestRateConfig());
  tracker.Start(0);
  tracker.RecordDelta(-5.0f);
  tracker.Update(5000, true);
  tracker.RecordDelta(-15.0f);

  const RateViewModel refreshed = tracker.Update(10000);
  assert(refreshed.has_rate);
  assert(refreshed.display_dirty);
  assert(NearlyEqual(refreshed.grams_per_hour, 7200.0f));
}

void TestRateTrackerForceRefreshBypassesInterval() {
  RateTracker tracker(TestRateConfig(1000, 5000));
  tracker.Start(0);
  tracker.RecordDelta(-5.0f);
  tracker.Update(5000, true);
  tracker.RecordDelta(-5.0f);

  const RateViewModel refreshed = tracker.Update(7000, true);
  assert(refreshed.has_rate);
  assert(refreshed.display_dirty);
  assert(NearlyEqual(refreshed.grams_per_hour, 5142.857f, 0.1f));
}

void TestRateTrackerIgnoresPositiveDeltas() {
  RateTracker tracker(TestRateConfig(1000, 5000));
  tracker.Start(0);
  tracker.RecordDelta(-5.0f);
  tracker.Update(5000, true);

  tracker.RecordDelta(10.0f);
  const RateViewModel unchanged = tracker.Update(5000, true);
  assert(unchanged.has_rate);
  assert(!unchanged.display_dirty);
  assert(NearlyEqual(unchanged.grams_per_hour, 3600.0f));
}

void TestRateTrackerStartClearsCachedRate() {
  RateTracker tracker(TestRateConfig(1000, 5000));
  tracker.Start(0);
  tracker.RecordDelta(-5.0f);
  tracker.Update(5000, true);

  tracker.Start(6000);
  const RateViewModel cleared = tracker.view();
  assert(!cleared.has_rate);
  assert(!cleared.display_dirty);
}

}  // namespace

int main() {
  TestInitialBaselineAcquisition();
  TestPositiveDelta();
  TestNegativeDelta();
  TestNoiseDoesNotAcquireBaseline();
  TestDriftWithoutRemovalDoesNotLatchDelta();
  TestRepeatedCycles();
  TestIdleWakeCapturesMissedDelta();
  TestIdleWakeIgnoresSmallChanges();
  TestIdleWakeDoesNotSkipRemovedState();
  TestRateTrackerWaitsForMinElapsed();
  TestRateTrackerHoldsBetweenIntervals();
  TestRateTrackerRefreshesAtInterval();
  TestRateTrackerForceRefreshBypassesInterval();
  TestRateTrackerIgnoresPositiveDeltas();
  TestRateTrackerStartClearsCachedRate();
  std::cout << "ScaleEngine and rate tracker tests passed\n";
  return 0;
}
