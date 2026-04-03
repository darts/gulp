#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace AppConfig {

constexpr bool kCalibrationMode = false;
constexpr float kScaleFactor = 1.0f;
constexpr float kReferenceMassG = 190.0f;

constexpr int kHx711DataPin = 26;
constexpr int kHx711ClockPin = 27;
constexpr int kButton1Pin = 32;
constexpr int kButton2Pin = 33;

constexpr int kBatteryAdcPin = 34;
constexpr int kBatterySensePowerPin = 14;  // Set to -1 if the divider is always on.

constexpr int kTftMosiPin = 19;
constexpr int kTftSclkPin = 18;
constexpr int kTftChipSelectPin = 5;
constexpr int kTftDataCommandPin = 16;
constexpr int kTftResetPin = 23;
constexpr int kTftBacklightPin = 4;

constexpr int kDisplayWidthPx = 240;
constexpr int kDisplayHeightPx = 135;
constexpr int kDisplayRotation = 1;
constexpr int kBacklightPwmChannel = 0;
constexpr int kBacklightPwmFrequencyHz = 5000;
constexpr int kBacklightPwmResolutionBits = 8;
constexpr std::array<std::uint8_t, 3> kBrightnessLevels = {255, 128, 48};

constexpr std::uint32_t kSerialBaudRate = 115200;
constexpr std::uint32_t kButtonDebounceMs = 30;
constexpr std::uint32_t kLongPressMs = 600;
constexpr std::uint32_t kStableDurationMs = 800;
constexpr std::uint32_t kRemovedDurationMs = 500;
constexpr std::uint32_t kIdleTimeoutMs = 600000; // 10 minutes
constexpr std::uint32_t kIdleToDeepSleepMs = 6000000; // 60 minutes
constexpr std::uint32_t kActiveSampleIntervalMs = 100;
constexpr std::uint32_t kIdleSampleIntervalMs = 1500;
constexpr std::uint32_t kBatteryReadIntervalMs = 2000;
constexpr std::uint32_t kBatterySenseSettleUs = 300;
constexpr std::uint32_t kBatterySampleDelayUs = 150;
constexpr std::uint32_t kBootSplashDurationMs = 1500;
constexpr std::uint32_t kDeepSleepMessageDurationMs = 1000;
constexpr std::uint32_t kRateDisplayMinElapsedMs = 5000;
constexpr std::uint32_t kRateDisplayUpdateIntervalMs = 5000;
constexpr std::uint32_t kHx711ReadTimeoutMs = 100;
constexpr std::uint32_t kHx711DiagnosticReadyTimeoutMs = 200;
constexpr std::uint32_t kHx711SetupTimeoutMs = 500;
constexpr std::uint32_t kHx711PowerUpDelayMs = 500;

constexpr float kPresentThresholdG = 5.0f;
constexpr float kRemovedThresholdG = 2.0f;
constexpr float kStableSpanThresholdG = 1.5f;
constexpr float kIdleWakeDeltaG = 3.0f;
constexpr float kBatteryDividerRatio = 2.0f;
constexpr float kBatteryVoltageCalibrationFactor = 1.0f;
constexpr float kBatteryVoltageDisplayDeltaV = 0.01f;
constexpr float kLiveWeightDisplayZeroThresholdG = 0.1f;
constexpr float kUsbPlugDetectThresholdV = 4.36f;
constexpr float kUsbUnplugDetectThresholdV = 4.24f;

// -- Bottle mode tier thresholds (weight delta in grams) --
// Low < kBottleTierLowMaxG, Mid < kBottleTierMidMaxG, High >= kBottleTierMidMaxG
constexpr float kBottleTierLowMaxG = 120.0f;
constexpr float kBottleTierMidMaxG = 200.0f;

// -- Glass mode tier thresholds (weight delta in grams) --
// Low < kGlassTierLowMaxG, Mid < kGlassTierMidMaxG, High >= kGlassTierMidMaxG
constexpr float kGlassTierLowMaxG = 7.0f;
constexpr float kGlassTierMidMaxG = 40.0f;

template <typename T, std::size_t N>
constexpr int ArrayCount(const T (&)[N]) {
  return static_cast<int>(N);
}

// -- Bottle mode tier labels --
constexpr const char* kBottleTierLowLabels[] = {
    "drizzle", "trickle", "taste test", "micro pour",
    "baby pour", "just a splash", "just a drop", "tutorial pour",
    "npc hydration", "stealth pour", "hobbit pour", "mildly moist",
    "goblin teaser", "(._.)", "pour (._.)>", "gentle lick"};
constexpr const char* kBottleTierMidLabels[] = {
    "ooooh a lil pour", "good pour", "mid", "average", "solid pour",
    "decent pour", "less disappointing", "standard", "main quest",
    "training arc", "speedrun pace", "canon event",
    "plot armor", "(o_o)", "pour (^_^)"};
constexpr const char* kBottleTierHighLabels[] = {
    "big pour", "good boy", "beeg pour", "CHONK",
    "absolute unit", "hydro homie", "alco homie",
    "(O_O)!", "(*_*)!!", "'w' key"};
constexpr int kBottleTierLowLabelCount = ArrayCount(kBottleTierLowLabels);
constexpr int kBottleTierMidLabelCount = ArrayCount(kBottleTierMidLabels);
constexpr int kBottleTierHighLabelCount = ArrayCount(kBottleTierHighLabels);

// -- Glass mode tier labels --
constexpr const char* kGlassTierLowLabels[] = {
    "sip", "sippy", "lil sip", "a lil wet", "dainty", "dinky out",
    "smol sip", "bird sip", "just a kiss",
    "elf behaviour", "tea ceremony",
    "tiny violin", "(._.)*", "~(._.)~", "cringe", "kinda cringe",
    "microdose", "I guess?", "gentle lick"};
constexpr const char* kGlassTierMidLabels[] = {
    "gulp", "mid", "average", "proper swig",
    "sommelier", "civilised", "good form",
    "smooth drink", "fancy goblin", "(^_~)", "~(o_o)~"};
constexpr const char* kGlassTierHighLabels[] = {
    "big gulp", "good boy", "beeg gulp", "swallow ;)",
    "bottomless", "feral",
    "goblet goblin", "drained it",
    "chaos gremlin", "so fucking wet",
    "(>_<)!!", "\\(T_T)/", "UwU"};
constexpr int kGlassTierLowLabelCount = ArrayCount(kGlassTierLowLabels);
constexpr int kGlassTierMidLabelCount = ArrayCount(kGlassTierMidLabels);
constexpr int kGlassTierHighLabelCount = ArrayCount(kGlassTierHighLabels);

// -- Positive delta (refill) messages --
constexpr const char* kBottleRefillLabels[] = {
    "refill", "top-up", "reload", "restocked",
    "back in biz", "fresh fill", "juiced up", "mana restored",
    "round two", "respawned", "patch notes", "hydration DLC",
    "second breakfast", "tactical reload", "(^_^)b", "+1 (>_<)"};
constexpr int kBottleRefillLabelCount = ArrayCount(kBottleRefillLabels);
constexpr const char* kGlassRefillLabels[] = {
    "refill", "top-up", "more!", "encore", "another round",
    "freshened up", "replenished", "topped off", "directors cut",
    "prestige pour", "extra scene", "season renewed",
    "plot twist pour", "glass respawn", "(^_^)/", "~refill~"};
constexpr int kGlassRefillLabelCount = ArrayCount(kGlassRefillLabels);

// -- No-change (near-zero delta) threshold and messages --
constexpr float kNoDeltaThresholdG = 0.1f;
constexpr const char* kNoDeltaLabels[] = {
    "did you even", "chat, is this real?", "bro what",
    "ghost sip", "placebo gulp", "error 404",
    "just vibes", "air gulp", "the audacity",
    "literally nothing", "skill issue", "???",
    "(-_-)", "(=_=)", "(._.)?", "(-_-)zzZ"};
constexpr int kNoDeltaLabelCount = ArrayCount(kNoDeltaLabels);

constexpr std::size_t kFilterWindowSize = 12;
constexpr std::size_t kReadSamplesPerUpdate = 1;
constexpr std::size_t kBatterySampleCount = 12;
constexpr std::size_t kBatteryTrimCount = 2;
constexpr std::size_t kHx711SampleRateProbeSamples = 10;
constexpr std::size_t kCalibrationReadAverageSamples = 10;

constexpr std::uint8_t kCpuActiveFrequencyMhz = 80;
constexpr std::uint8_t kCpuIdleFrequencyMhz = 40;

}  // namespace AppConfig
