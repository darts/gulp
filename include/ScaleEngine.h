#pragma once

#include <cstdint>

enum class ScaleState {
  kWaitBaseline,
  kTracking,
  kRemoved,
};

struct ScaleEngineConfig {
  float present_threshold_g;
  float removed_threshold_g;
  std::uint32_t stable_duration_ms;
  std::uint32_t removed_duration_ms;
};

struct ScaleSample {
  float grams;
  std::uint32_t timestamp_ms;
  bool stable;
};

struct ScaleViewModel {
  float live_weight_g;
  float baseline_g;
  float previous_baseline_g;
  float latched_delta_g;
  ScaleState state;
  bool has_baseline;
  bool has_latched_delta;
  bool display_dirty;
};

class ScaleEngine {
 public:
  explicit ScaleEngine(ScaleEngineConfig config);

  void Reset();
  ScaleViewModel Update(const ScaleSample& sample);
  ScaleViewModel UpdateFromIdleWake(const ScaleSample& sample,
                                    float min_delta_g);
  ScaleViewModel view() const;

 private:
  static bool NearlyEqual(float left, float right, float tolerance);
  bool ViewChanged(const ScaleViewModel& current) const;
  ScaleViewModel BuildView() const;
  ScaleViewModel FinalizeView();
  void ResetCandidates();

  ScaleEngineConfig config_;
  float live_weight_g_ = 0.0f;
  float baseline_g_ = 0.0f;
  float previous_baseline_g_ = 0.0f;
  float latched_delta_g_ = 0.0f;
  bool has_baseline_ = false;
  bool has_latched_delta_ = false;
  ScaleState state_ = ScaleState::kWaitBaseline;
  bool baseline_candidate_active_ = false;
  bool replacement_candidate_active_ = false;
  bool removal_candidate_active_ = false;
  std::uint32_t baseline_candidate_since_ms_ = 0;
  std::uint32_t replacement_candidate_since_ms_ = 0;
  std::uint32_t removal_candidate_since_ms_ = 0;
  mutable ScaleViewModel last_view_{};
  mutable bool has_last_view_ = false;
};
