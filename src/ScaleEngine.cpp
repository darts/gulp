#include "ScaleEngine.h"

#include <cmath>

namespace {

constexpr float kViewToleranceG = 0.05f;

}  // namespace

ScaleEngine::ScaleEngine(ScaleEngineConfig config) : config_(config) {
  Reset();
}

void ScaleEngine::ResetCandidates() {
  baseline_candidate_active_ = false;
  replacement_candidate_active_ = false;
  removal_candidate_active_ = false;
  baseline_candidate_since_ms_ = 0;
  replacement_candidate_since_ms_ = 0;
  removal_candidate_since_ms_ = 0;
}

void ScaleEngine::Reset() {
  live_weight_g_ = 0.0f;
  baseline_g_ = 0.0f;
  previous_baseline_g_ = 0.0f;
  latched_delta_g_ = 0.0f;
  has_baseline_ = false;
  has_latched_delta_ = false;
  state_ = ScaleState::kWaitBaseline;
  ResetCandidates();
  has_last_view_ = false;
}

ScaleViewModel ScaleEngine::Update(const ScaleSample& sample) {
  live_weight_g_ = sample.grams;

  if (!has_baseline_) {
    if (sample.stable && sample.grams >= config_.present_threshold_g) {
      if (!baseline_candidate_active_) {
        baseline_candidate_active_ = true;
        baseline_candidate_since_ms_ = sample.timestamp_ms;
      }
      if (sample.timestamp_ms - baseline_candidate_since_ms_ >=
          config_.stable_duration_ms) {
        baseline_g_ = sample.grams;
        has_baseline_ = true;
        state_ = ScaleState::kTracking;
        baseline_candidate_active_ = false;
      }
    } else {
      baseline_candidate_active_ = false;
    }
  } else if (state_ == ScaleState::kTracking) {
    replacement_candidate_active_ = false;
    if (sample.grams <= config_.removed_threshold_g) {
      if (!removal_candidate_active_) {
        removal_candidate_active_ = true;
        removal_candidate_since_ms_ = sample.timestamp_ms;
      }
      if (sample.timestamp_ms - removal_candidate_since_ms_ >=
          config_.removed_duration_ms) {
        state_ = ScaleState::kRemoved;
        removal_candidate_active_ = false;
      }
    } else {
      removal_candidate_active_ = false;
    }
  } else if (state_ == ScaleState::kRemoved) {
    removal_candidate_active_ = false;
    if (sample.stable && sample.grams >= config_.present_threshold_g) {
      if (!replacement_candidate_active_) {
        replacement_candidate_active_ = true;
        replacement_candidate_since_ms_ = sample.timestamp_ms;
      }
      if (sample.timestamp_ms - replacement_candidate_since_ms_ >=
          config_.stable_duration_ms) {
        latched_delta_g_ = sample.grams - baseline_g_;
        previous_baseline_g_ = baseline_g_;
        baseline_g_ = sample.grams;
        has_latched_delta_ = true;
        state_ = ScaleState::kTracking;
        replacement_candidate_active_ = false;
      }
    } else {
      replacement_candidate_active_ = false;
    }
  }

  return FinalizeView();
}

ScaleViewModel ScaleEngine::UpdateFromIdleWake(const ScaleSample& sample,
                                               float min_delta_g) {
  live_weight_g_ = sample.grams;

  const bool missed_cycle_detected =
      has_baseline_ && state_ == ScaleState::kTracking && sample.stable &&
      sample.grams >= config_.present_threshold_g &&
      std::fabs(sample.grams - baseline_g_) >= min_delta_g;
  if (!missed_cycle_detected) {
    return Update(sample);
  }

  latched_delta_g_ = sample.grams - baseline_g_;
  previous_baseline_g_ = baseline_g_;
  baseline_g_ = sample.grams;
  has_latched_delta_ = true;
  state_ = ScaleState::kTracking;
  ResetCandidates();
  return FinalizeView();
}

ScaleViewModel ScaleEngine::view() const {
  ScaleViewModel current = BuildView();
  current.display_dirty = ViewChanged(current);
  return current;
}

bool ScaleEngine::NearlyEqual(float left, float right, float tolerance) {
  return std::fabs(left - right) <= tolerance;
}

bool ScaleEngine::ViewChanged(const ScaleViewModel& current) const {
  if (!has_last_view_) {
    return true;
  }
  return !NearlyEqual(current.live_weight_g, last_view_.live_weight_g,
                      kViewToleranceG) ||
         !NearlyEqual(current.baseline_g, last_view_.baseline_g,
                      kViewToleranceG) ||
         !NearlyEqual(current.previous_baseline_g,
                      last_view_.previous_baseline_g, kViewToleranceG) ||
         !NearlyEqual(current.latched_delta_g, last_view_.latched_delta_g,
                      kViewToleranceG) ||
         current.state != last_view_.state ||
         current.has_baseline != last_view_.has_baseline ||
         current.has_latched_delta != last_view_.has_latched_delta;
}

ScaleViewModel ScaleEngine::BuildView() const {
  ScaleViewModel view{};
  view.live_weight_g = live_weight_g_;
  view.baseline_g = baseline_g_;
  view.previous_baseline_g = previous_baseline_g_;
  view.latched_delta_g = latched_delta_g_;
  view.state = state_;
  view.has_baseline = has_baseline_;
  view.has_latched_delta = has_latched_delta_;
  view.display_dirty = false;
  return view;
}

ScaleViewModel ScaleEngine::FinalizeView() {
  ScaleViewModel current = BuildView();
  current.display_dirty = ViewChanged(current);
  last_view_ = current;
  has_last_view_ = true;
  return current;
}
