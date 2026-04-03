#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <HX711.h>
#include <SPI.h>
#include <esp_sleep.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "AppConfig.h"
#include "RateTracker.h"
#include "ScaleEngine.h"

namespace {

using AppConfig::kActiveSampleIntervalMs;
using AppConfig::kButton1Pin;
using AppConfig::kButton2Pin;
using AppConfig::kButtonDebounceMs;
using AppConfig::kCalibrationMode;
using AppConfig::kFilterWindowSize;
using AppConfig::kHx711ClockPin;
using AppConfig::kHx711DataPin;
using AppConfig::kIdleSampleIntervalMs;
using AppConfig::kIdleToDeepSleepMs;
using AppConfig::kIdleTimeoutMs;
using AppConfig::kIdleWakeDeltaG;
using AppConfig::kPresentThresholdG;
using AppConfig::kReadSamplesPerUpdate;
using AppConfig::kReferenceMassG;
using AppConfig::kRemovedDurationMs;
using AppConfig::kRemovedThresholdG;
using AppConfig::kScaleFactor;
using AppConfig::kSerialBaudRate;
using AppConfig::kStableDurationMs;
using AppConfig::kStableSpanThresholdG;
using AppConfig::kBatteryAdcPin;
using AppConfig::kBatteryDividerRatio;
using AppConfig::kBatteryReadIntervalMs;
using AppConfig::kBatterySampleCount;
using AppConfig::kBatterySampleDelayUs;
using AppConfig::kBatterySensePowerPin;
using AppConfig::kBatterySenseSettleUs;
using AppConfig::kBatteryTrimCount;
using AppConfig::kBatteryVoltageDisplayDeltaV;
using AppConfig::kBatteryVoltageCalibrationFactor;
using AppConfig::kBootSplashDurationMs;
using AppConfig::kBrightnessLevels;
using AppConfig::kCalibrationReadAverageSamples;
using AppConfig::kCpuActiveFrequencyMhz;
using AppConfig::kCpuIdleFrequencyMhz;
using AppConfig::kDeepSleepMessageDurationMs;
using AppConfig::kDisplayHeightPx;
using AppConfig::kDisplayRotation;
using AppConfig::kDisplayWidthPx;
using AppConfig::kBacklightPwmChannel;
using AppConfig::kBacklightPwmFrequencyHz;
using AppConfig::kBacklightPwmResolutionBits;
using AppConfig::kHx711DiagnosticReadyTimeoutMs;
using AppConfig::kTftBacklightPin;
using AppConfig::kTftChipSelectPin;
using AppConfig::kTftDataCommandPin;
using AppConfig::kTftResetPin;
using AppConfig::kHx711PowerUpDelayMs;
using AppConfig::kHx711ReadTimeoutMs;
using AppConfig::kHx711SampleRateProbeSamples;
using AppConfig::kHx711SetupTimeoutMs;
using AppConfig::kLiveWeightDisplayZeroThresholdG;
using AppConfig::kRateDisplayMinElapsedMs;
using AppConfig::kRateDisplayUpdateIntervalMs;
using AppConfig::kUsbPlugDetectThresholdV;
using AppConfig::kUsbUnplugDetectThresholdV;

constexpr std::uint16_t kBackgroundColor = ST77XX_BLACK;
constexpr std::uint16_t kPrimaryTextColor = ST77XX_WHITE;
constexpr std::uint16_t kAccentTextColor = ST77XX_CYAN;
constexpr std::uint16_t kStatusTextColor = ST77XX_GREEN;

constexpr int kGridColumnWidth = 120;
constexpr int kGridLeftX = 0;
constexpr int kGridRightX = 120;
constexpr int kGridTopValueY = 92;
constexpr int kGridTopLabelY = 108;
constexpr int kGridBottomY = 127;

enum class ButtonEvent {
  kNone,
  kShortPress,
  kLongPress,
};

struct DebouncedButton {
  explicit DebouncedButton(int pin) : pin(pin) {}

  int pin;
  bool stable_level = HIGH;
  bool last_raw_level = HIGH;
  std::uint32_t last_edge_ms = 0;
  std::uint32_t press_start_ms = 0;
  bool is_held = false;
  bool long_fired = false;
  ButtonEvent pending_event = ButtonEvent::kNone;

  void Begin() {
    pinMode(pin, INPUT_PULLUP);
    stable_level = digitalRead(pin);
    last_raw_level = stable_level;
    last_edge_ms = millis();
    is_held = false;
    long_fired = false;
    pending_event = ButtonEvent::kNone;
  }

  void Update(std::uint32_t now_ms) {
    const bool raw = digitalRead(pin);
    if (raw != last_raw_level) {
      last_raw_level = raw;
      last_edge_ms = now_ms;
    }
    if ((now_ms - last_edge_ms) >= kButtonDebounceMs && raw != stable_level) {
      const bool prev = stable_level;
      stable_level = raw;
      if (prev == HIGH && stable_level == LOW) {
        press_start_ms = now_ms;
        is_held = true;
        long_fired = false;
      } else if (prev == LOW && stable_level == HIGH) {
        if (is_held && !long_fired) {
          pending_event = ButtonEvent::kShortPress;
        }
        is_held = false;
      }
    }
    if (is_held && !long_fired &&
        (now_ms - press_start_ms) >= AppConfig::kLongPressMs) {
      long_fired = true;
      pending_event = ButtonEvent::kLongPress;
    }
  }

  ButtonEvent ConsumeEvent() {
    const ButtonEvent ev = pending_event;
    pending_event = ButtonEvent::kNone;
    return ev;
  }
};

struct WeightFilter {
  std::array<float, kFilterWindowSize> values{};
  std::size_t count = 0;
  std::size_t next_index = 0;
  float last_filtered_g = 0.0f;

  void Push(float grams) {
    values[next_index] = grams;
    next_index = (next_index + 1) % values.size();
    count = std::min(values.size(), count + 1);
    last_filtered_g = Filtered();
  }

  void Seed(float grams) {
    values.fill(grams);
    count = values.size();
    next_index = 0;
    last_filtered_g = grams;
  }

  float Filtered() const {
    if (count == 0) {
      return 0.0f;
    }
    std::array<float, kFilterWindowSize> sorted{};
    for (std::size_t index = 0; index < count; ++index) {
      sorted[index] = values[index];
    }
    std::sort(sorted.begin(), sorted.begin() + count);
    if (count < 3) {
      float total = 0.0f;
      for (std::size_t index = 0; index < count; ++index) {
        total += sorted[index];
      }
      return total / static_cast<float>(count);
    }
    float total = 0.0f;
    for (std::size_t index = 1; index + 1 < count; ++index) {
      total += sorted[index];
    }
    return total / static_cast<float>(count - 2);
  }

  bool Stable(float span_threshold_g) const {
    if (count < values.size()) {
      return false;
    }
    float minimum = values[0];
    float maximum = values[0];
    for (std::size_t index = 1; index < count; ++index) {
      minimum = std::min(minimum, values[index]);
      maximum = std::max(maximum, values[index]);
    }
    return (maximum - minimum) <= span_threshold_g;
  }
};

HX711 g_scale;
Adafruit_ST7789 g_display(kTftChipSelectPin, kTftDataCommandPin, kTftResetPin);
ScaleEngine g_engine({kPresentThresholdG, kRemovedThresholdG, kStableDurationMs,
                      kRemovedDurationMs});
DebouncedButton g_button1{kButton1Pin};
DebouncedButton g_button2{kButton2Pin};
WeightFilter g_filter;
RateTracker g_rate_tracker(
    {kRateDisplayMinElapsedMs, kRateDisplayUpdateIntervalMs});

enum class DisplayMode {
  kBottle,
  kGlass,
  kTarget,
  kLiveWeight,
};

constexpr int kDisplayModeCount =
    static_cast<int>(DisplayMode::kLiveWeight) + 1;

DisplayMode g_display_mode = DisplayMode::kBottle;
bool g_idle_mode = false;

std::size_t g_brightness_index = 0;
float g_target_weight_g = 0.0f;
bool g_has_target = false;
float g_last_activity_weight_g = 0.0f;
std::uint32_t g_last_activity_ms = 0;
std::uint32_t g_idle_started_ms = 0;
std::uint32_t g_last_sample_ms = 0;
bool g_has_rendered_view = false;
std::uint32_t g_last_elapsed_render_ms = 0;
bool g_help_showing = false;
const char* g_tier_label = "";
float g_tier_delta_g = NAN;
float g_cached_batt_v = 0.0f;
std::uint32_t g_last_batt_read_ms = 0;
bool g_usb_powered = false;
bool g_view_needs_full_draw = true;

struct MainScreenRenderState {
  String header_mode_text;
  String header_status_text;
  String header_voltage_text;
  String battery_status_text;
  int main_y = 32;
  int main_text_size = 3;
  String main_text;
  int secondary_y = 72;
  int secondary_text_size = 2;
  bool show_secondary = false;
  String secondary_text;
  bool show_grid = false;
  String grid_rate_value;
  String grid_total_value;
  String grid_live_value;
  String grid_elapsed_value;
};

MainScreenRenderState g_last_render_state{};

struct BatterySocPoint {
  float voltage;
  int percent;
};

// Typical single-cell LiPo open-circuit curve, interpolated for smoother % output.
constexpr std::array<BatterySocPoint, 21> kBatterySocCurve = {{
    {3.27f, 0},   {3.61f, 5},   {3.69f, 10}, {3.71f, 15}, {3.73f, 20},
    {3.75f, 25},  {3.77f, 30},  {3.79f, 35}, {3.80f, 40}, {3.82f, 45},
    {3.84f, 50},  {3.85f, 55},  {3.87f, 60}, {3.91f, 65}, {3.95f, 70},
    {3.98f, 75},  {4.02f, 80},  {4.08f, 85}, {4.11f, 90}, {4.15f, 95},
    {4.20f, 100},
}};

const char* StateLabel(ScaleState state) {
  switch (state) {
    case ScaleState::kWaitBaseline:
      return "Place weight";
    case ScaleState::kTracking:
      return "Ready";
    case ScaleState::kRemoved:
      return "Removed";
  }
  return "Unknown";
}

const char* ModeLabel(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::kBottle:
      return "Bottle";
    case DisplayMode::kGlass:
      return "Glass";
    case DisplayMode::kTarget:
      return "Target";
    case DisplayMode::kLiveWeight:
      return "Live";
  }
  return "";
}

void SetBacklight(std::uint8_t brightness) {
  ledcWrite(kBacklightPwmChannel, brightness);
}

void SetBatterySensePower(bool enabled) {
  if (kBatterySensePowerPin < 0) {
    return;
  }
  digitalWrite(kBatterySensePowerPin, enabled ? HIGH : LOW);
}

void SuspendPeripherals() {
  SetBacklight(0);
  g_display.enableSleep(true);
  g_scale.power_down();
  SetBatterySensePower(false);
}

void ResumePeripherals() {
  SetBatterySensePower(true);
  delayMicroseconds(kBatterySenseSettleUs);
  g_scale.power_up();
  g_display.enableSleep(false);
  SetBacklight(kBrightnessLevels[g_brightness_index]);
}

void ConfigureDisplay() {
  ledcSetup(kBacklightPwmChannel, kBacklightPwmFrequencyHz,
            kBacklightPwmResolutionBits);
  ledcAttachPin(kTftBacklightPin, kBacklightPwmChannel);
  SPI.begin(AppConfig::kTftSclkPin, -1, AppConfig::kTftMosiPin,
            kTftChipSelectPin);
  g_display.init(kDisplayHeightPx, kDisplayWidthPx);
  g_display.setRotation(kDisplayRotation);
  g_display.fillScreen(kBackgroundColor);
  g_display.setTextWrap(false);
  SetBacklight(kBrightnessLevels[0]);
}

std::uint32_t ReadBatterySenseMilliVolts() {
  static_assert(kBatteryTrimCount < kBatterySampleCount / 2,
                "kBatteryTrimCount must leave at least one sample after trimming");
  std::array<std::uint32_t, kBatterySampleCount> samples{};
  for (std::size_t index = 0; index < samples.size(); ++index) {
    samples[index] = analogReadMilliVolts(kBatteryAdcPin);
    if (index + 1 < samples.size()) {
      delayMicroseconds(kBatterySampleDelayUs);
    }
  }

  std::sort(samples.begin(), samples.end());
  const std::size_t trim = std::min(kBatteryTrimCount, samples.size() / 2);
  const std::size_t first = trim;
  const std::size_t last = samples.size() - trim;

  std::uint32_t total_mv = 0;
  for (std::size_t index = first; index < last; ++index) {
    total_mv += samples[index];
  }
  return total_mv / static_cast<std::uint32_t>(last - first);
}

float BatteryVoltageFromSenseMilliVolts(std::uint32_t sense_mv) {
  const float battery_mv = static_cast<float>(sense_mv) * kBatteryDividerRatio *
                           kBatteryVoltageCalibrationFactor;
  return battery_mv / 1000.0f;
}

float ReadBatteryVoltage() {
  return BatteryVoltageFromSenseMilliVolts(ReadBatterySenseMilliVolts());
}

bool UsbPowerStateFromVoltage(float voltage, bool current_state) {
  return current_state ? voltage >= kUsbUnplugDetectThresholdV
                       : voltage >= kUsbPlugDetectThresholdV;
}

bool UpdateBatteryVoltage(std::uint32_t now_ms) {
  if ((now_ms - g_last_batt_read_ms) < kBatteryReadIntervalMs) {
    return false;
  }
  const float voltage = ReadBatteryVoltage();
  const bool prev_usb = g_usb_powered;
  g_usb_powered = UsbPowerStateFromVoltage(voltage, g_usb_powered);
  const bool changed = std::fabs(voltage - g_cached_batt_v) >=
                           kBatteryVoltageDisplayDeltaV ||
                       (g_usb_powered != prev_usb);
  g_cached_batt_v = voltage;
  g_last_batt_read_ms = now_ms;
  return changed;
}

int BatteryPercent(float voltage) {
  if (voltage <= kBatterySocCurve.front().voltage) {
    return kBatterySocCurve.front().percent;
  }
  if (voltage >= kBatterySocCurve.back().voltage) {
    return kBatterySocCurve.back().percent;
  }

  for (std::size_t index = 1; index < kBatterySocCurve.size(); ++index) {
    const BatterySocPoint& lower = kBatterySocCurve[index - 1];
    const BatterySocPoint& upper = kBatterySocCurve[index];
    if (voltage <= upper.voltage) {
      const float span_v = upper.voltage - lower.voltage;
      const float position =
          span_v > 0.0f ? (voltage - lower.voltage) / span_v : 0.0f;
      const float percent = lower.percent +
                            position * static_cast<float>(upper.percent - lower.percent);
      return std::max(0, std::min(100, static_cast<int>(std::lround(percent))));
    }
  }

  return kBatterySocCurve.back().percent;
}

String BatteryString(float voltage) {
  if (g_usb_powered) {
    return "USB " + String(voltage, 2) + "V";
  }
  return String(BatteryPercent(voltage)) + "% " + String(voltage, 2) + "V";
}

int TextLineHeight(int text_size) {
  return 8 * text_size;
}

void DrawCenteredLine(int y, int text_size, std::uint16_t color,
                      const String& text) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  g_display.setTextSize(text_size);
  g_display.getTextBounds(text, 0, y, &x1, &y1, &width, &height);
  const int x = (kDisplayWidthPx - static_cast<int>(width)) / 2;
  g_display.setTextColor(color, kBackgroundColor);
  g_display.setCursor(std::max(0, x), y);
  g_display.print(text);
}

void DrawRightAlignedLine(int y, int text_size, std::uint16_t color,
                          const String& text, int min_chars = 0) {
  g_display.setTextSize(text_size);
  const int char_width = 6 * text_size;
  const int text_len = static_cast<int>(text.length());
  const int total = std::max(min_chars, text_len);
  const int start_x = kDisplayWidthPx - total * char_width;
  const int pad = total - text_len;
  g_display.setCursor(std::max(0, start_x), y);
  g_display.setTextColor(color, kBackgroundColor);
  for (int i = 0; i < pad; i++) g_display.print(' ');
  g_display.print(text);
}

void DrawPaddedLine(int y, int text_size, std::uint16_t color,
                    const String& text, bool center = false) {
  g_display.setTextSize(text_size);
  const int char_width = 6 * text_size;
  const int line_height = 8 * text_size;
  const int max_chars = kDisplayWidthPx / char_width;
  String padded;
  padded.reserve(max_chars + 1);
  if (center) {
    const int text_len = static_cast<int>(text.length());
    const int total_pad = std::max(0, max_chars - text_len);
    const int left_pad = total_pad / 2;
    for (int i = 0; i < left_pad; i++) padded += ' ';
    padded += text;
  } else {
    padded = text;
  }
  while (static_cast<int>(padded.length()) < max_chars) {
    padded += ' ';
  }
  g_display.setTextColor(color, kBackgroundColor);
  g_display.setCursor(0, y);
  g_display.print(padded);
  const int covered = max_chars * char_width;
  if (covered < kDisplayWidthPx) {
    g_display.fillRect(covered, y, kDisplayWidthPx - covered, line_height,
                       kBackgroundColor);
  }
}

void DrawCellText(int x, int y, int width, int text_size,
                  std::uint16_t color, const String& text) {
  const int char_width = 6 * text_size;
  const int line_height = 8 * text_size;
  const int max_chars = width / char_width;
  const int text_len = static_cast<int>(text.length());
  const int total_pad = std::max(0, max_chars - text_len);
  const int left_pad = total_pad / 2;
  const int right_pad = max_chars - left_pad - text_len;
  g_display.setTextSize(text_size);
  g_display.setTextColor(color, kBackgroundColor);
  g_display.setCursor(x, y);
  for (int i = 0; i < left_pad; i++) g_display.print(' ');
  g_display.print(text);
  for (int i = 0; i < right_pad; i++) g_display.print(' ');
  const int covered = max_chars * char_width;
  if (covered < width) {
    g_display.fillRect(x + covered, y, width - covered, line_height,
                       kBackgroundColor);
  }
}

void DrawLabeledCell(int x, int y, int width, int text_size,
                     std::uint16_t label_color, const char* label,
                     std::uint16_t value_color, const String& value) {
  const int char_width = 6 * text_size;
  const int line_height = 8 * text_size;
  const int max_chars = width / char_width;
  const int label_len = static_cast<int>(strlen(label));
  const int total_chars = label_len + static_cast<int>(value.length());
  const int total_pad = std::max(0, max_chars - total_chars);
  const int left_pad = total_pad / 2;
  const int right_pad = max_chars - left_pad - total_chars;
  g_display.setTextSize(text_size);
  g_display.setCursor(x, y);
  g_display.setTextColor(label_color, kBackgroundColor);
  for (int i = 0; i < left_pad; i++) g_display.print(' ');
  g_display.print(label);
  g_display.setTextColor(value_color, kBackgroundColor);
  g_display.print(value);
  for (int i = 0; i < std::max(0, right_pad); i++) g_display.print(' ');
  const int covered = max_chars * char_width;
  if (covered < width) {
    g_display.fillRect(x + covered, y, width - covered, line_height,
                       kBackgroundColor);
  }
}

const char* DeltaTierLabel(float abs_delta_g, DisplayMode mode) {
  const auto pick = [](const char* const labels[], int count) {
    return labels[random(count)];
  };
  const float low_max = mode == DisplayMode::kGlass
                            ? AppConfig::kGlassTierLowMaxG
                            : AppConfig::kBottleTierLowMaxG;
  const float mid_max = mode == DisplayMode::kGlass
                            ? AppConfig::kGlassTierMidMaxG
                            : AppConfig::kBottleTierMidMaxG;
  const auto* low_labels = mode == DisplayMode::kGlass
                               ? AppConfig::kGlassTierLowLabels
                               : AppConfig::kBottleTierLowLabels;
  const int low_label_count = mode == DisplayMode::kGlass
                                  ? AppConfig::kGlassTierLowLabelCount
                                  : AppConfig::kBottleTierLowLabelCount;
  const auto* mid_labels = mode == DisplayMode::kGlass
                               ? AppConfig::kGlassTierMidLabels
                               : AppConfig::kBottleTierMidLabels;
  const int mid_label_count = mode == DisplayMode::kGlass
                                  ? AppConfig::kGlassTierMidLabelCount
                                  : AppConfig::kBottleTierMidLabelCount;
  const auto* high_labels = mode == DisplayMode::kGlass
                                ? AppConfig::kGlassTierHighLabels
                                : AppConfig::kBottleTierHighLabels;
  const int high_label_count = mode == DisplayMode::kGlass
                                   ? AppConfig::kGlassTierHighLabelCount
                                   : AppConfig::kBottleTierHighLabelCount;
  if (abs_delta_g < low_max) {
    return pick(low_labels, low_label_count);
  }
  if (abs_delta_g < mid_max) {
    return pick(mid_labels, mid_label_count);
  }
  return pick(high_labels, high_label_count);
}

const char* RefillLabel(DisplayMode mode) {
  if (mode == DisplayMode::kGlass) {
    return AppConfig::kGlassRefillLabels[random(AppConfig::kGlassRefillLabelCount)];
  }
  return AppConfig::kBottleRefillLabels[random(AppConfig::kBottleRefillLabelCount)];
}

void InvalidateTierLabel() {
  g_tier_label = "";
  g_tier_delta_g = NAN;
}

void InvalidateMainView() {
  g_view_needs_full_draw = true;
  g_has_rendered_view = false;
}

void EnterIdleMode(std::uint32_t now_ms) {
  g_idle_mode = true;
  g_idle_started_ms = now_ms;
  SuspendPeripherals();
}

void ExitIdleMode() {
  g_idle_mode = false;
  g_idle_started_ms = 0;
  ResumePeripherals();
  InvalidateMainView();
}

void ResetScaleSession() {
  g_engine.Reset();
  g_filter = WeightFilter{};
  g_rate_tracker.Reset();
  InvalidateTierLabel();
}

bool AutoDeepSleepDue(std::uint32_t now_ms) {
  return kIdleToDeepSleepMs > 0 && g_idle_started_ms > 0 &&
         (now_ms - g_idle_started_ms) >= kIdleToDeepSleepMs;
}

bool BaselineChanged(const ScaleViewModel& previous_view,
                     const ScaleViewModel& current_view);

bool DeltaCaptured(const ScaleViewModel& previous_view,
                   const ScaleViewModel& current_view);

void ConfigureLightSleepWakeSources();
void ConfigureDeepSleepWakeSources();

RateViewModel UpdateRateTracking(const ScaleViewModel& previous_view,
                                 const ScaleViewModel& current_view,
                                 std::uint32_t now_ms) {
  if (current_view.has_baseline && !g_rate_tracker.active()) {
    g_rate_tracker.Start(now_ms);
  }

  const bool delta_captured = DeltaCaptured(previous_view, current_view);
  if (delta_captured) {
    g_rate_tracker.RecordDelta(current_view.latched_delta_g);
  }

  return g_rate_tracker.Update(now_ms, delta_captured);
}

float DisplayWeight(float grams) {
  return std::fabs(grams) < kLiveWeightDisplayZeroThresholdG ? 0.0f : grams;
}

bool BaselineChanged(const ScaleViewModel& previous_view,
                     const ScaleViewModel& current_view) {
  return current_view.has_baseline &&
         (!previous_view.has_baseline ||
          current_view.baseline_g != previous_view.baseline_g);
}

bool DeltaCaptured(const ScaleViewModel& previous_view,
                   const ScaleViewModel& current_view) {
  return current_view.has_latched_delta && previous_view.has_baseline &&
         BaselineChanged(previous_view, current_view);
}

bool BothButtonsHeldLong() {
  return g_button1.is_held && g_button1.long_fired && g_button2.is_held &&
         g_button2.long_fired;
}

MainScreenRenderState BuildMainScreenRenderState(const ScaleViewModel& view,
                                                 std::uint32_t now_ms) {
  MainScreenRenderState state;
  state.header_mode_text = String("gulp - ") + ModeLabel(g_display_mode);
  state.header_status_text =
      g_idle_mode ? String("Sleeping") : String(StateLabel(view.state));
  state.header_voltage_text = String(g_cached_batt_v, 2) + "V";
  state.battery_status_text =
      g_usb_powered ? String("USB")
                    : String(BatteryPercent(g_cached_batt_v)) + "%";

  switch (g_display_mode) {
    case DisplayMode::kBottle:
    case DisplayMode::kGlass:
      state.main_text = view.has_latched_delta
                            ? String(std::fabs(view.latched_delta_g), 1) + " g"
                            : String("---");
      state.main_y = 32;
      state.main_text_size = 3;
      state.secondary_y = 64;
      state.secondary_text_size = 2;
      state.show_secondary = view.has_latched_delta;
      if (view.has_latched_delta) {
        const float abs_delta = std::fabs(view.latched_delta_g);
        if (abs_delta != g_tier_delta_g) {
          g_tier_delta_g = abs_delta;
          if (abs_delta < AppConfig::kNoDeltaThresholdG) {
            g_tier_label = AppConfig::kNoDeltaLabels[
                random(AppConfig::kNoDeltaLabelCount)];
          } else if (view.latched_delta_g > 0) {
            g_tier_label = RefillLabel(g_display_mode);
          } else {
            g_tier_label = DeltaTierLabel(abs_delta, g_display_mode);
          }
        }
        state.secondary_text = String(g_tier_label);
      }

      state.show_grid = true;
      {
        const RateViewModel rate_view = g_rate_tracker.view();
        state.grid_rate_value = rate_view.has_rate
            ? String(rate_view.grams_per_hour, 1)
            : String("---");
        state.grid_total_value = String(g_rate_tracker.cumulative_lost_g(), 1);
        const float live_display = DisplayWeight(view.live_weight_g);
        state.grid_live_value = String(live_display, 1) + " g";
        if (g_rate_tracker.active()) {
          const std::uint32_t elapsed_ms = now_ms - g_rate_tracker.start_ms();
          const std::uint32_t elapsed_s = elapsed_ms / 1000;
          char buf[8];
          if (elapsed_s < 3600) {
            snprintf(buf, sizeof(buf), "%02u:%02u",
                     static_cast<unsigned>(elapsed_s / 60),
                     static_cast<unsigned>(elapsed_s % 60));
          } else {
            snprintf(buf, sizeof(buf), "%u:%02u",
                     static_cast<unsigned>(elapsed_s / 3600),
                     static_cast<unsigned>((elapsed_s % 3600) / 60));
          }
          state.grid_elapsed_value = buf;
        } else {
          state.grid_elapsed_value = "00:00";
        }
      }
      break;

    case DisplayMode::kLiveWeight: {
      const float display_weight = DisplayWeight(view.live_weight_g);
      state.main_text = String(display_weight, 1) + " g";
      state.main_y = 64;
      state.main_text_size = 4;
      state.secondary_y = 96;
      break;
    }

    case DisplayMode::kTarget:
      state.main_text = g_has_target ? String(g_target_weight_g, 1) + " g"
                                     : String("---");
      state.main_y = 32;
      state.main_text_size = 3;
      state.secondary_y = 82;
      state.secondary_text_size = 2;
      state.show_secondary = true;
      if (g_has_target) {
        const float diff = view.live_weight_g - g_target_weight_g;
        state.secondary_text = "Diff " + String(diff, 1) + " g";
      } else {
        state.secondary_text = "No target";
      }
      break;
  }

  return state;
}

void RenderView(MainScreenRenderState state) {
  if (!g_has_rendered_view) {
    g_display.fillScreen(kBackgroundColor);
  }
  const bool full_draw = !g_has_rendered_view;
  g_view_needs_full_draw = false;

  // --- Header (three elements share y=8; padded mode text clears the line) ---
  const bool header_changed = full_draw ||
      state.header_mode_text != g_last_render_state.header_mode_text ||
      state.header_status_text != g_last_render_state.header_status_text ||
      state.header_voltage_text != g_last_render_state.header_voltage_text;
  if (header_changed) {
    DrawPaddedLine(8, 1, kAccentTextColor, state.header_mode_text);
    DrawCenteredLine(8, 1, kStatusTextColor, state.header_status_text);
    DrawRightAlignedLine(8, 1, kStatusTextColor, state.header_voltage_text);
  }

  // --- Battery status (y=16, right-aligned, pad to 4 chars to clear old) ---
  if (full_draw ||
      state.battery_status_text != g_last_render_state.battery_status_text) {
    DrawRightAlignedLine(16, 1, kStatusTextColor,
                         state.battery_status_text, 4);
  }

  // --- Main text: clear old position if it moved ---
  const bool main_moved = !full_draw &&
      (state.main_y != g_last_render_state.main_y ||
       state.main_text_size != g_last_render_state.main_text_size);
  if (main_moved) {
    DrawPaddedLine(g_last_render_state.main_y,
                   g_last_render_state.main_text_size,
                   kPrimaryTextColor, String(""), true);
  }
  if (full_draw || main_moved ||
      state.main_text != g_last_render_state.main_text) {
    DrawPaddedLine(state.main_y, state.main_text_size, kPrimaryTextColor,
                   state.main_text, true);
  }

  // --- Secondary text: clear old position if it moved ---
  const bool sec_moved = !full_draw &&
      (state.secondary_y != g_last_render_state.secondary_y ||
       state.secondary_text_size != g_last_render_state.secondary_text_size);
  if (sec_moved) {
    DrawPaddedLine(g_last_render_state.secondary_y,
                   g_last_render_state.secondary_text_size,
                   kAccentTextColor, String(""), true);
  }
  if (full_draw || sec_moved ||
      state.show_secondary != g_last_render_state.show_secondary ||
      state.secondary_text != g_last_render_state.secondary_text) {
    DrawPaddedLine(state.secondary_y, state.secondary_text_size,
                   kAccentTextColor,
                   state.show_secondary ? state.secondary_text : String(""),
                   true);
  }

  // --- Grid: clear area when transitioning from visible to hidden ---
  if (!full_draw && g_last_render_state.show_grid && !state.show_grid) {
    g_display.fillRect(0, kGridTopValueY, kDisplayWidthPx,
                       kDisplayHeightPx - kGridTopValueY, kBackgroundColor);
  }

  if (state.show_grid) {
    const bool grid_new = full_draw || !g_last_render_state.show_grid;
    if (grid_new ||
        state.grid_rate_value != g_last_render_state.grid_rate_value) {
      DrawCellText(kGridLeftX, kGridTopValueY, kGridColumnWidth, 2,
                   kPrimaryTextColor, state.grid_rate_value);
    }
    if (grid_new ||
        state.grid_total_value != g_last_render_state.grid_total_value) {
      DrawCellText(kGridRightX, kGridTopValueY, kGridColumnWidth, 2,
                   kPrimaryTextColor, state.grid_total_value);
    }
    if (grid_new ||
        state.grid_live_value != g_last_render_state.grid_live_value) {
      DrawLabeledCell(kGridLeftX, kGridBottomY, kGridColumnWidth, 1,
                      kAccentTextColor, "live ",
                      kStatusTextColor, state.grid_live_value);
    }
    if (grid_new ||
        state.grid_elapsed_value != g_last_render_state.grid_elapsed_value) {
      DrawLabeledCell(kGridRightX, kGridBottomY, kGridColumnWidth, 1,
                      kAccentTextColor, "elapsed ",
                      kStatusTextColor, state.grid_elapsed_value);
    }
    if (grid_new) {
      DrawCellText(kGridLeftX, kGridTopLabelY, kGridColumnWidth, 1,
                   kAccentTextColor, "g/hr");
      DrawCellText(kGridRightX, kGridTopLabelY, kGridColumnWidth, 1,
                   kAccentTextColor, "total g");
    }
  }

  g_last_render_state = std::move(state);
  g_has_rendered_view = true;
}

float ReadWeightGrams() {
  if (!g_scale.wait_ready_timeout(kHx711ReadTimeoutMs)) {
    return g_filter.last_filtered_g;
  }
  return g_scale.get_units(kReadSamplesPerUpdate);
}

void EnterDeepSleep(bool show_message = true) {
  if (show_message) {
    if (g_idle_mode) {
      ResumePeripherals();
    }
    g_display.fillScreen(kBackgroundColor);
    DrawCenteredLine(48, 2, kPrimaryTextColor, "Deep sleep");
    DrawCenteredLine(82, 1, kStatusTextColor, "Press BTN2 to wake");
    delay(kDeepSleepMessageDurationMs);
  }
  SuspendPeripherals();
  ConfigureDeepSleepWakeSources();
  esp_deep_sleep_start();
}

void RenderHelp() {
  g_display.fillScreen(kBackgroundColor);
  DrawCenteredLine(4, 1, kAccentTextColor, "Button Functions");
  DrawPaddedLine(24, 1, kPrimaryTextColor, "BTN_R short: Cycle mode");
  DrawPaddedLine(36, 1, kPrimaryTextColor, "BTN_R long:  Zero/Target");
  DrawPaddedLine(56, 1, kPrimaryTextColor, "BTN_L short: Brightness");
  DrawPaddedLine(68, 1, kPrimaryTextColor, "BTN_L long:  Deep sleep");
  DrawCenteredLine(96, 1, kStatusTextColor, "Release to dismiss");
}

void HandleButtons(std::uint32_t now_ms) {
  const ButtonEvent ev1 = g_button1.ConsumeEvent();
  const ButtonEvent ev2 = g_button2.ConsumeEvent();

  // BTN1 short: cycle display mode
  if (ev1 == ButtonEvent::kShortPress) {
    g_display_mode = static_cast<DisplayMode>(
        (static_cast<int>(g_display_mode) + 1) % kDisplayModeCount);
    InvalidateTierLabel();
    InvalidateMainView();
    g_last_activity_ms = now_ms;
  }

  // BTN1 long: set target (in Target mode) or tare/reset (other modes)
  if (ev1 == ButtonEvent::kLongPress) {
    if (g_display_mode == DisplayMode::kTarget) {
      g_target_weight_g = g_filter.last_filtered_g;
      g_has_target = true;
    } else {
      g_scale.tare();
      Serial.println("Runtime tare offset: " + String(g_scale.get_offset()));
      ResetScaleSession();
    }
    InvalidateMainView();
    g_last_activity_ms = now_ms;
  }

  // BTN2 short: wake from idle / cycle brightness
  if (ev2 == ButtonEvent::kShortPress) {
    if (g_idle_mode) {
      g_brightness_index = 0;
      ExitIdleMode();
      g_last_activity_ms = now_ms;
    } else {
      g_brightness_index =
          (g_brightness_index + 1) % kBrightnessLevels.size();
      SetBacklight(kBrightnessLevels[g_brightness_index]);
      g_last_activity_ms = now_ms;
    }
  }

  // BTN2 long: deep sleep
  if (ev2 == ButtonEvent::kLongPress) {
    EnterDeepSleep();
  }
}

void UpdateIdleMode(float filtered_weight_g, std::uint32_t now_ms) {
  if (std::fabs(filtered_weight_g - g_last_activity_weight_g) >= kIdleWakeDeltaG) {
    g_last_activity_weight_g = filtered_weight_g;
    g_last_activity_ms = now_ms;
  }

  const bool should_idle = (now_ms - g_last_activity_ms) >= kIdleTimeoutMs;
  if (should_idle == g_idle_mode) {
    return;
  }

  if (should_idle) {
    EnterIdleMode(now_ms);
  } else {
    ExitIdleMode();
  }
}

void ProcessSample(const ScaleSample& sample,
                   bool allow_idle_wake_reconcile = false) {
  const ScaleViewModel previous_view = g_engine.view();
  const ScaleViewModel view =
      allow_idle_wake_reconcile
          ? g_engine.UpdateFromIdleWake(sample, kIdleWakeDeltaG)
          : g_engine.Update(sample);
  if (DeltaCaptured(previous_view, view)) {
    InvalidateTierLabel();
  }
  const RateViewModel rate_view =
      UpdateRateTracking(previous_view, view, sample.timestamp_ms);
  const bool battery_dirty = UpdateBatteryVoltage(sample.timestamp_ms);

  if (view.display_dirty || battery_dirty || rate_view.display_dirty ||
      !g_has_rendered_view || g_view_needs_full_draw) {
    RenderView(BuildMainScreenRenderState(view, sample.timestamp_ms));
  }

  if (view.state != previous_view.state) {
    g_last_activity_ms = sample.timestamp_ms;
  }

  UpdateIdleMode(sample.grams, sample.timestamp_ms);
}

void ProcessCurrentFilteredSample(std::uint32_t now_ms,
                                  bool allow_idle_wake_reconcile = false) {
  ScaleSample sample{};
  sample.grams = g_filter.last_filtered_g;
  sample.timestamp_ms = now_ms;
  sample.stable = g_filter.Stable(kStableSpanThresholdG);
  ProcessSample(sample, allow_idle_wake_reconcile);
}

void ConfigureLightSleepWakeSources() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
  esp_sleep_enable_timer_wakeup(
      static_cast<std::uint64_t>(kIdleSampleIntervalMs) * 1000ULL);
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(kButton2Pin), LOW);
}

void ConfigureDeepSleepWakeSources() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(kButton2Pin), LOW);
}

void EnterLightSleep() {
  setCpuFrequencyMhz(kCpuIdleFrequencyMhz);
  ConfigureLightSleepWakeSources();
  esp_light_sleep_start();
  setCpuFrequencyMhz(kCpuActiveFrequencyMhz);
}

enum class CalibrationStep {
  kDiag,
  kButtonTest,
  kTare,
  kWeigh,
  kDone,
};

CalibrationStep g_cal_step = CalibrationStep::kDiag;
long g_cal_offset = 0;
float g_cal_scale_factor = 0.0f;

bool g_diag_needs_full_draw = true;
bool g_btn_test_needs_full_draw = true;
String g_btn1_last_event = "---";
String g_btn2_last_event = "---";
std::uint32_t g_btn1_short_count = 0;
std::uint32_t g_btn1_long_count = 0;
std::uint32_t g_btn2_short_count = 0;
std::uint32_t g_btn2_long_count = 0;

void RenderCalibrationDiag() {
  if (g_diag_needs_full_draw) {
    g_display.fillScreen(kBackgroundColor);
    DrawCenteredLine(4, 1, kAccentTextColor, "HX711 Diagnostic");
    DrawCenteredLine(104, 1, kStatusTextColor, "Any button: next");
    g_diag_needs_full_draw = false;
  }

  const bool ready = g_scale.wait_ready_timeout(kHx711DiagnosticReadyTimeoutMs);
  DrawPaddedLine(24, 1, ready ? kStatusTextColor : ST77XX_RED,
                 ready ? "HX711: connected" : "HX711: NOT FOUND");

  if (ready) {
    const long raw = g_scale.read();
    DrawPaddedLine(44, 1, kPrimaryTextColor, "Raw: " + String(raw));

    g_scale.set_scale();
    const float units = g_scale.get_units(3);
    DrawPaddedLine(60, 1, kPrimaryTextColor, "Units: " + String(units, 1));
  }

  DrawPaddedLine(84, 1, kAccentTextColor,
                 "Batt: " + BatteryString(g_cached_batt_v));
}

void RenderButtonTest(ButtonEvent ev1, ButtonEvent ev2) {
  if (ev1 == ButtonEvent::kShortPress) {
    g_btn1_last_event = "SHORT";
    g_btn1_short_count++;
  } else if (ev1 == ButtonEvent::kLongPress) {
    g_btn1_last_event = "LONG";
    g_btn1_long_count++;
  }
  if (ev2 == ButtonEvent::kShortPress) {
    g_btn2_last_event = "SHORT";
    g_btn2_short_count++;
  } else if (ev2 == ButtonEvent::kLongPress) {
    g_btn2_last_event = "LONG";
    g_btn2_long_count++;
  }

  if (g_btn_test_needs_full_draw) {
    g_display.fillScreen(kBackgroundColor);
    DrawCenteredLine(4, 1, kAccentTextColor, "Button Test");
    DrawCenteredLine(104, 1, kStatusTextColor, "Both long-press: next");
    g_btn_test_needs_full_draw = false;
  }

  DrawPaddedLine(24, 1, kPrimaryTextColor,
                 "BTN1: " + g_btn1_last_event +
                 "  S:" + String(g_btn1_short_count) +
                 " L:" + String(g_btn1_long_count));
  DrawPaddedLine(44, 1, kPrimaryTextColor,
                 "BTN2: " + g_btn2_last_event +
                 "  S:" + String(g_btn2_short_count) +
                 " L:" + String(g_btn2_long_count));

  // Show held state live
  DrawPaddedLine(68, 1, kAccentTextColor,
                 String("BTN1: ") + (g_button1.stable_level == LOW ? "HELD" : "---") +
                 "  BTN2: " + (g_button2.stable_level == LOW ? "HELD" : "---"));
}

void RenderCalibrationTare() {
  g_display.fillScreen(kBackgroundColor);
  DrawCenteredLine(12, 1, kAccentTextColor, "Step 1: Tare");
  DrawCenteredLine(36, 2, kPrimaryTextColor, "Remove weight");
  DrawCenteredLine(72, 1, kStatusTextColor, "Then press BTN1");
}

void RenderCalibrationWeigh() {
  g_display.fillScreen(kBackgroundColor);
  DrawCenteredLine(12, 1, kAccentTextColor, "Step 2: Calibrate");
  DrawCenteredLine(36, 2, kPrimaryTextColor,
                   String(kReferenceMassG, 0) + "g");
  DrawCenteredLine(72, 1, kStatusTextColor, "Place weight, BTN1");
}

void RenderCalibrationDone() {
  g_display.fillScreen(kBackgroundColor);
  DrawCenteredLine(4, 1, kAccentTextColor, "Calibration complete");
  DrawCenteredLine(28, 1, kPrimaryTextColor,
                   "Offset: " + String(g_cal_offset));
  DrawCenteredLine(48, 1, kPrimaryTextColor,
                   "Scale: " + String(g_cal_scale_factor, 6));
  DrawCenteredLine(72, 1, kStatusTextColor, "Update AppConfig.h");
  DrawCenteredLine(88, 1, kStatusTextColor, "set kCalibrationMode=false");

  Serial.println();
  Serial.println("Copy these into include/AppConfig.h:");
  Serial.print("constexpr float kScaleFactor = ");
  Serial.print(g_cal_scale_factor, 6);
  Serial.println("f;");
}

void CalibrationLoop() {
  // Buttons already updated by loop(); just consume events here.
  const ButtonEvent ev1 = g_button1.ConsumeEvent();
  const ButtonEvent ev2 = g_button2.ConsumeEvent();

  switch (g_cal_step) {
    case CalibrationStep::kDiag:
      RenderCalibrationDiag();
      if (ev1 != ButtonEvent::kNone || ev2 != ButtonEvent::kNone) {
        g_cal_step = CalibrationStep::kButtonTest;
        g_btn_test_needs_full_draw = true;
      }
      break;

    case CalibrationStep::kButtonTest:
      RenderButtonTest(ev1, ev2);
      if (BothButtonsHeldLong()) {
        g_cal_step = CalibrationStep::kTare;
        RenderCalibrationTare();
      }
      break;

    case CalibrationStep::kTare:
      if (ev1 != ButtonEvent::kNone) {
        g_scale.set_scale();
        g_scale.tare();
        g_cal_offset = g_scale.get_offset();
        g_cal_step = CalibrationStep::kWeigh;
        RenderCalibrationWeigh();
      }
      break;

    case CalibrationStep::kWeigh:
      if (ev1 != ButtonEvent::kNone) {
        const float reading = g_scale.read_average(kCalibrationReadAverageSamples);
        g_cal_scale_factor =
            (reading - static_cast<float>(g_cal_offset)) / kReferenceMassG;
        g_cal_step = CalibrationStep::kDone;
        RenderCalibrationDone();
      }
      break;

    case CalibrationStep::kDone:
      break;
  }
}

void ConfigureScale() {
  g_scale.begin(kHx711DataPin, kHx711ClockPin);
  if (kCalibrationMode) {
    return;
  }
  g_scale.set_scale(kScaleFactor);
}

}  // namespace

void setup() {
  setCpuFrequencyMhz(kCpuActiveFrequencyMhz);
  Serial.begin(kSerialBaudRate);
  randomSeed(esp_random());
  pinMode(kBatteryAdcPin, INPUT);
  analogSetPinAttenuation(kBatteryAdcPin, ADC_11db);
  if (kBatterySensePowerPin >= 0) {
    pinMode(kBatterySensePowerPin, OUTPUT);
    SetBatterySensePower(true);
    delayMicroseconds(kBatterySenseSettleUs);
  }
  ConfigureDisplay();
  g_button1.Begin();
  g_button2.Begin();
  ConfigureScale();

  g_cached_batt_v = ReadBatteryVoltage();
  // Seed from the lower threshold so a board that boots while already on USB
  // does not get stuck in the false side of the hysteresis band.
  g_usb_powered = g_cached_batt_v >= kUsbUnplugDetectThresholdV;
  g_last_batt_read_ms = millis();
  g_last_activity_ms = millis();

  // Measure HX711 sample rate: 10 reads at 80 SPS should take ~125ms
  if (g_scale.wait_ready_timeout(kHx711SetupTimeoutMs)) {
    const std::uint32_t t0 = millis();
    g_scale.get_units(kHx711SampleRateProbeSamples);
    const std::uint32_t elapsed = millis() - t0;
    Serial.println("HX711 10-sample read: " + String(elapsed) + " ms"
                   " (expect ~125 for 80 SPS, ~1000 for 10 SPS)");
  }

  if (kCalibrationMode) {
    RenderCalibrationDiag();
  } else {
    // Auto-tare on boot to compensate for temperature drift
    if (g_scale.wait_ready_timeout(kHx711SetupTimeoutMs)) {
      g_scale.tare();
      Serial.println("Boot tare offset: " + String(g_scale.get_offset()));
    }

    g_display.fillScreen(kBackgroundColor);
    DrawCenteredLine(40, 3, kPrimaryTextColor, "gulp");
    DrawCenteredLine(72, 2, kAccentTextColor, "v1.0");
    delay(kBootSplashDurationMs);
  }
}

void loop() {
  const std::uint32_t now_ms = millis();

  g_button1.Update(now_ms);
  g_button2.Update(now_ms);

  if (kCalibrationMode) {
    CalibrationLoop();
    delay(50);
    return;
  }

  const bool both_held =
      g_button1.stable_level == LOW && g_button2.stable_level == LOW;
  if (both_held) {
    if (!g_help_showing) {
      g_help_showing = true;
      RenderHelp();
    }
    g_button1.ConsumeEvent();
    g_button2.ConsumeEvent();
    delay(50);
    return;
  }
  if (g_help_showing) {
    g_help_showing = false;
    InvalidateMainView();
    g_button1.ConsumeEvent();
    g_button2.ConsumeEvent();
  }

  HandleButtons(now_ms);

  if (g_view_needs_full_draw && !g_idle_mode) {
    ProcessCurrentFilteredSample(now_ms);
  }

  const std::uint32_t sample_interval_ms =
      g_idle_mode ? kIdleSampleIntervalMs : kActiveSampleIntervalMs;
  if ((now_ms - g_last_sample_ms) < sample_interval_ms) {
    delay(5);
    return;
  }
  g_last_sample_ms = now_ms;

  if (g_idle_mode) {
    g_scale.power_up();
    delay(kHx711PowerUpDelayMs);  // HX711 needs up to 400ms after power-up; 500ms has been more reliable in testing.
    // g_scale.get_units(1);  // Discard first reading after power-up (unreliable)
    const float raw_g = ReadWeightGrams();
    UpdateIdleMode(raw_g, now_ms);
    if (g_idle_mode) {
      if (AutoDeepSleepDue(now_ms)) {
        EnterDeepSleep(false);
      }
      g_scale.power_down();
      EnterLightSleep();
    } else {
      g_filter.Seed(raw_g);
      ProcessCurrentFilteredSample(now_ms, true);
    }
    return;
  }

  const float weight_g = ReadWeightGrams();
  g_filter.Push(weight_g);
  ProcessCurrentFilteredSample(now_ms);

  const bool grid_mode = (g_display_mode == DisplayMode::kBottle ||
                          g_display_mode == DisplayMode::kGlass) &&
                         g_has_rendered_view && g_rate_tracker.active();
  if (grid_mode && (now_ms - g_last_elapsed_render_ms) >= 1000) {
    g_last_elapsed_render_ms = now_ms;
    const std::uint32_t elapsed_s = (now_ms - g_rate_tracker.start_ms()) / 1000;
    char buf[8];
    if (elapsed_s < 3600) {
      snprintf(buf, sizeof(buf), "%02u:%02u",
               static_cast<unsigned>(elapsed_s / 60),
               static_cast<unsigned>(elapsed_s % 60));
    } else {
      snprintf(buf, sizeof(buf), "%u:%02u",
               static_cast<unsigned>(elapsed_s / 3600),
               static_cast<unsigned>((elapsed_s % 3600) / 60));
    }
    DrawLabeledCell(kGridRightX, kGridBottomY, kGridColumnWidth, 1,
                    kAccentTextColor, "elapsed ",
                    kStatusTextColor, buf);
  }
}
