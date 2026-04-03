#include "RateTracker.h"

#include <cmath>

namespace {

constexpr float kRateToleranceGPerHour = 0.05f;

}  // namespace

RateTracker::RateTracker(RateTrackerConfig config) : config_(config) {}

void RateTracker::Reset() {
  cumulative_lost_g_ = 0.0f;
  start_ms_ = 0;
  last_refresh_ms_ = 0;
  active_ = false;
  has_rate_ = false;
  grams_per_hour_ = 0.0f;
}

void RateTracker::Start(std::uint32_t start_ms) {
  cumulative_lost_g_ = 0.0f;
  start_ms_ = start_ms;
  last_refresh_ms_ = start_ms;
  active_ = true;
  has_rate_ = false;
  grams_per_hour_ = 0.0f;
}

void RateTracker::RecordDelta(float delta_g) {
  if (!active_ || delta_g >= 0.0f) {
    return;
  }

  cumulative_lost_g_ += -delta_g;
}

RateViewModel RateTracker::Update(std::uint32_t now_ms, bool force_refresh) {
  RateViewModel previous = BuildView();
  if (!active_) {
    return previous;
  }

  const bool should_refresh =
      force_refresh || config_.refresh_interval_ms == 0 ||
      (now_ms - last_refresh_ms_) >= config_.refresh_interval_ms;
  if (!should_refresh) {
    return previous;
  }

  last_refresh_ms_ = now_ms;

  const RateViewModel refreshed = ComputeView(now_ms);
  has_rate_ = refreshed.has_rate;
  grams_per_hour_ = refreshed.grams_per_hour;

  RateViewModel current = BuildView();
  current.display_dirty =
      current.has_rate != previous.has_rate ||
      !NearlyEqual(current.grams_per_hour, previous.grams_per_hour,
                   kRateToleranceGPerHour);
  return current;
}

RateViewModel RateTracker::view() const { return BuildView(); }

bool RateTracker::active() const { return active_; }

float RateTracker::cumulative_lost_g() const { return cumulative_lost_g_; }

std::uint32_t RateTracker::start_ms() const { return start_ms_; }

bool RateTracker::NearlyEqual(float left, float right, float tolerance) {
  return std::fabs(left - right) <= tolerance;
}

RateViewModel RateTracker::BuildView() const {
  RateViewModel view;
  view.has_rate = has_rate_;
  view.grams_per_hour = grams_per_hour_;
  return view;
}

RateViewModel RateTracker::ComputeView(std::uint32_t now_ms) const {
  RateViewModel view;
  if (!active_) {
    return view;
  }

  const std::uint32_t elapsed_ms = now_ms - start_ms_;
  if (elapsed_ms == 0 || elapsed_ms < config_.min_elapsed_ms) {
    return view;
  }

  if (cumulative_lost_g_ <= 0.0f) {
    return view;
  }

  const float hours = static_cast<float>(elapsed_ms) / 3600000.0f;
  view.has_rate = true;
  view.grams_per_hour = cumulative_lost_g_ / hours;
  return view;
}
