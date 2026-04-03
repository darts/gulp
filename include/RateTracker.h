#pragma once

#include <cstdint>

struct RateTrackerConfig {
  std::uint32_t min_elapsed_ms;
  std::uint32_t refresh_interval_ms;
};

struct RateViewModel {
  bool has_rate = false;
  float grams_per_hour = 0.0f;
  bool display_dirty = false;
};

class RateTracker {
 public:
  explicit RateTracker(RateTrackerConfig config);

  void Reset();
  void Start(std::uint32_t start_ms);
  void RecordDelta(float delta_g);
  RateViewModel Update(std::uint32_t now_ms, bool force_refresh = false);
  RateViewModel view() const;
  bool active() const;
  float cumulative_lost_g() const;
  std::uint32_t start_ms() const;

 private:
  static bool NearlyEqual(float left, float right, float tolerance);
  RateViewModel BuildView() const;
  RateViewModel ComputeView(std::uint32_t now_ms) const;

  RateTrackerConfig config_;
  float cumulative_lost_g_ = 0.0f;
  std::uint32_t start_ms_ = 0;
  std::uint32_t last_refresh_ms_ = 0;
  bool active_ = false;
  bool has_rate_ = false;
  float grams_per_hour_ = 0.0f;
};
