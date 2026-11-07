/**
 * @file    platform/android_perf.cpp
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/android_perf.h"

#include <cstdint>

#include <rex/cvar.h>

#include "core/settings.h" // kCvarGroup

#if defined(__ANDROID__)
constexpr int32_t kDefaultPowerSaving =
    static_cast<int32_t>(bd::platform::android::PowerSaving::Auto);
#else
constexpr int32_t kDefaultPowerSaving =
    static_cast<int32_t>(bd::platform::android::PowerSaving::Off);
#endif

REXCVAR_DEFINE_INT32(bd_power_saving, kDefaultPowerSaving, kCvarGroup,
                     "Android power saving: 0 off, 1 auto (30 fps, the game's "
                     "own rate, while the device runs hot), 2 max (always 30 "
                     "fps).");

#if defined(__ANDROID__)

#include <atomic>
#include <cmath>
#include <format>

#include <SDL3/SDL_video.h>
#include <android/native_window.h>
#include <jni.h>
#include <android/performance_hint.h>
#include <android/thermal.h>
#include <unistd.h>

#include "core/logging.h"
#include "platform/reboot.h" // ConfigFilePath

namespace bd::platform::android {
namespace {

constexpr int kUncappedTargetFPS = 60;
constexpr int kPowerSavingFPS = 30;

int64_t TargetNanos(int target_fps) {
  if (target_fps <= 0)
    target_fps = kUncappedTargetFPS;
  return 1'000'000'000LL / target_fps;
}

const char *ThermalStatusName(int status) {
  switch (status) {
  case ATHERMAL_STATUS_NONE:
    return "none";
  case ATHERMAL_STATUS_LIGHT:
    return "light";
  case ATHERMAL_STATUS_MODERATE:
    return "moderate";
  case ATHERMAL_STATUS_SEVERE:
    return "severe";
  case ATHERMAL_STATUS_CRITICAL:
    return "critical";
  case ATHERMAL_STATUS_EMERGENCY:
    return "emergency";
  case ATHERMAL_STATUS_SHUTDOWN:
    return "shutdown";
  default:
    return nullptr;
  }
}

// The thermal service rate-limits getThermalHeadroom (a second call within a
// second reads NaN), so the cap and the [perf] line share one sample.
struct ThermalSample {
  int status = -1;      // AThermalStatus, -1 unknown
  float headroom = NAN; // now
  float forecast = NAN; // 10 s ahead
};

// Battery, pushed by ReblueActivity's ACTION_BATTERY_CHANGED receiver.
// -1 until the first broadcast.
std::atomic<int> g_battery_level{-1};
std::atomic<bool> g_battery_charging{false};
std::atomic<int> g_battery_decicelsius{0};

// Not every device reports thermal headroom (an AYN Thor gives the status
// only, and it read "none" all session while the console got hot), so the
// battery's own temperature, which every device broadcasts and which tracks
// the skin the player holds, is the other heat signal.
constexpr int kHotBatteryDeci = 400;  // 40.0 C
constexpr int kCoolBatteryDeci = 370; // 37.0 C

int BatteryDeci() {
  return g_battery_decicelsius.load(std::memory_order_relaxed);
}

using Clock = std::chrono::steady_clock;
constexpr auto kSampleEvery = std::chrono::seconds(2);
// Hot when the device is already throttling, or about to: the forecast moves
// before the clocks drop, which is the point of reacting to it.
constexpr float kHotHeadroom = 0.90f;
// Cool enough to go back up, held for a while so a scene that heats the
// device again does not bounce between 30 and 60.
constexpr float kCoolHeadroom = 0.75f;
constexpr auto kCoolHold = std::chrono::seconds(45);

AThermalManager *Thermal() {
  static AThermalManager *manager = AThermal_acquireManager();
  return manager;
}

ThermalSample g_sample;
Clock::time_point g_sampled{};

const ThermalSample &Sample() {
  const auto now = Clock::now();
  if (g_sampled.time_since_epoch().count() != 0 &&
      now - g_sampled < kSampleEvery)
    return g_sample;
  g_sampled = now;
  AThermalManager *manager = Thermal();
  if (!manager)
    return g_sample;
  g_sample.status = static_cast<int>(AThermal_getCurrentThermalStatus(manager));
  g_sample.headroom = AThermal_getThermalHeadroom(manager, 0);
  g_sample.forecast = AThermal_getThermalHeadroom(manager, 10);
  return g_sample;
}

bool SampleHot(const ThermalSample &t) {
  if (t.status >= ATHERMAL_STATUS_MODERATE)
    return true;
  if (BatteryDeci() >= kHotBatteryDeci)
    return true;
  return (std::isfinite(t.headroom) && t.headroom >= kHotHeadroom) ||
         (std::isfinite(t.forecast) && t.forecast >= kHotHeadroom);
}

bool SampleCool(const ThermalSample &t) {
  if (t.status >= ATHERMAL_STATUS_LIGHT)
    return false;
  if (BatteryDeci() > kCoolBatteryDeci)
    return false;
  const float h = std::isfinite(t.forecast) ? t.forecast : t.headroom;
  // No headroom reading at all: the status alone says it is fine.
  return !std::isfinite(h) || h < kCoolHeadroom;
}

std::atomic<int> g_power_cap{0};
bool g_hot = false;
bool g_low = false;
Clock::time_point g_cool_since{};

// Off the charger, Auto runs at 30 fps, the rate Blue Dragon's simulation
// ticks at. An AYN Thor reports neither thermal status nor headroom, and its
// battery stayed under 31 C while the SoC ran hot and the charge fell 1% a
// minute at 60 fps, so neither heat signal ever fired. Plugged in, Auto goes
// to 60 unless the device reports it is hot.

// A mode picked on the second screen, applied on the frame thread so the
// cvar and its config write never race the settings menu. -1 = none.
std::atomic<int> g_pending_mode{-1};

// What the second screen shows, published once a second by the frame thread.
std::atomic<int> g_live_fps{0};
std::atomic<int> g_live_status{-1};
std::atomic<int> g_live_headroom_pct{-1};
std::atomic<bool> g_live_hot{false};

PowerSaving Mode() {
  const int32_t v = REXCVAR_GET(bd_power_saving);
  if (v <= 0)
    return PowerSaving::Off;
  return v >= 2 ? PowerSaving::Max : PowerSaving::Auto;
}

const char *ModeName(PowerSaving mode) {
  switch (mode) {
  case PowerSaving::Off:
    return "off";
  case PowerSaving::Auto:
    return "auto";
  case PowerSaving::Max:
    return "max";
  }
  return "?";
}

ANativeWindow *g_rate_window = nullptr;
int g_rate_fps = -1;

} // namespace

void ReportFrameWork(std::chrono::nanoseconds work, int target_fps) {
  // Only ever touched from the frame thread.
  static APerformanceHintSession *session = nullptr;
  static bool tried = false;
  static int64_t target = 0;
  const int64_t want = TargetNanos(target_fps);
  if (!tried) {
    tried = true;
    APerformanceHintManager *manager = APerformanceHint_getManager();
    const int32_t tid = gettid();
    if (manager)
      session = APerformanceHint_createSession(manager, &tid, 1, want);
    if (session) {
      target = want;
      BD_INFO("[adpf] performance hint session on thread {}, target {:.1f} ms",
              tid, want / 1e6);
    } else {
      BD_WARN("[adpf] no performance hint session, CPU clocks are left to "
              "the governor");
    }
  }
  if (!session)
    return;
  if (want != target) {
    APerformanceHint_updateTargetWorkDuration(session, want);
    target = want;
  }
  const int64_t ns = work.count();
  if (ns > 0)
    APerformanceHint_reportActualWorkDuration(session, ns);
}

std::string ThermalSummary() {
  const ThermalSample &t = Sample();
  const char *status = ThermalStatusName(t.status);
  std::string out;
  if (status || std::isfinite(t.headroom)) {
    // 1.0 is where the device starts throttling.
    out = " | thermal ";
    out += status ? status : "?";
    if (std::isfinite(t.headroom))
      out += std::format(", headroom {:.2f}", t.headroom);
    if (std::isfinite(t.forecast))
      out += std::format(" (10s {:.2f})", t.forecast);
  }
  if (const int level = g_battery_level.load(std::memory_order_relaxed);
      level >= 0)
    out += std::format(" | battery {}% {:.1f} C{}", level, BatteryDeci() / 10.0,
                       g_battery_charging.load(std::memory_order_relaxed)
                           ? " charging"
                           : "");
  out += std::format(" | power {}", ModeName(Mode()));
  if (const int cap = PowerFPSCap())
    out += std::format(", capped {}", cap);
  return out;
}

int PowerFPSCap() { return g_power_cap.load(std::memory_order_relaxed); }

void UpdatePowerSaving() {
  if (const int want = g_pending_mode.exchange(-1, std::memory_order_acq_rel);
      want >= 0) {
    rex::cvar::SetFlagByName("bd_power_saving", std::to_string(want));
    rex::cvar::SaveConfig(bd::platform::ConfigFilePath());
    BD_INFO("[power] mode set to {} from the second screen", want);
  }
  const ThermalSample &t = Sample();
  const auto now = Clock::now();
  {
    // The frame rate the second screen shows, over the last second.
    static Clock::time_point since = now;
    static int frames = 0;
    ++frames;
    if (now - since >= std::chrono::seconds(1)) {
      const double secs = std::chrono::duration<double>(now - since).count();
      g_live_fps.store(static_cast<int>(frames / secs + 0.5),
                       std::memory_order_relaxed);
      frames = 0;
      since = now;
      g_live_status.store(t.status, std::memory_order_relaxed);
      const float h = std::isfinite(t.forecast) ? t.forecast : t.headroom;
      g_live_headroom_pct.store(
          std::isfinite(h) ? static_cast<int>(h * 100) : -1,
          std::memory_order_relaxed);
    }
  }
  const PowerSaving mode = Mode();
  int cap = 0;
  if (mode == PowerSaving::Max) {
    cap = kPowerSavingFPS;
  } else if (mode == PowerSaving::Auto) {
    const int level = g_battery_level.load(std::memory_order_relaxed);
    const bool charging = g_battery_charging.load(std::memory_order_relaxed);
    const bool low = level >= 0 && !charging;
    if (low != g_low) {
      g_low = low;
      BD_INFO("[power] battery {}% {}: {}", level,
              charging ? "charging" : "on battery",
              low ? "30 fps while on battery" : "on the charger, up to 60 fps");
    }
    if (!g_hot && SampleHot(t)) {
      g_hot = true;
      g_cool_since = {};
      BD_INFO("[power] device running hot (thermal {}, headroom {:.2f}, 10s "
              "{:.2f}, battery {:.1f} C): {} fps until it cools",
              t.status, t.headroom, t.forecast, BatteryDeci() / 10.0,
              kPowerSavingFPS);
    } else if (g_hot) {
      if (!SampleCool(t)) {
        g_cool_since = {};
      } else if (g_cool_since.time_since_epoch().count() == 0) {
        g_cool_since = now;
      } else if (now - g_cool_since >= kCoolHold) {
        g_hot = false;
        BD_INFO("[power] cooled down (headroom {:.2f}, battery {:.1f} C): "
                "frame-rate cap lifted",
                t.headroom, BatteryDeci() / 10.0);
      }
    }
    cap = (g_hot || g_low) ? kPowerSavingFPS : 0;
  } else {
    g_hot = false;
    g_low = false;
  }
  g_live_hot.store(g_hot, std::memory_order_relaxed);
  g_power_cap.store(cap, std::memory_order_relaxed);
}

void InvalidateDisplayFrameRate() {
  g_rate_window = nullptr;
  g_rate_fps = -1;
}

void ApplyDisplayFrameRate(int fps) {
  if (fps < 0)
    fps = 0;
  if (fps == g_rate_fps && g_rate_window)
    return;
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *sdl_window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!sdl_window)
    return;
  auto *window = static_cast<ANativeWindow *>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window),
                             SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
  if (!window)
    return;
  // FIXED_SOURCE: the game paces itself, so the display should pick a rate
  // the frames divide into rather than the nearest one. The default change
  // strategy only switches when the panel can do it seamlessly, so this never
  // blanks the screen mid-game; where it cannot, nothing changes.
  const int32_t err = ANativeWindow_setFrameRate(
      window, static_cast<float>(fps),
      fps > 0 ? ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE
              : ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
  if (err != 0) {
    BD_WARN("[power] ANativeWindow_setFrameRate({}) failed: {}", fps, err);
  } else {
    BD_INFO("[power] display frame rate {}",
            fps > 0 ? std::to_string(fps) : std::string("free"));
  }
  g_rate_window = window;
  g_rate_fps = fps;
}

} // namespace bd::platform::android

extern "C" {

JNIEXPORT void JNICALL Java_org_reblue_android_ReblueActivity_nativeBattery(
    JNIEnv *, jclass, jint level, jboolean charging, jint decicelsius) {
  using namespace bd::platform::android;
  g_battery_level.store(level, std::memory_order_relaxed);
  g_battery_charging.store(charging, std::memory_order_relaxed);
  g_battery_decicelsius.store(decicelsius, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_org_reblue_android_DualScreenCompanion_nativeSetPowerSaving(JNIEnv *,
                                                                 jclass,
                                                                 jint mode) {
  using namespace bd::platform::android;
  if (mode >= 0 && mode <= 2)
    g_pending_mode.store(mode, std::memory_order_release);
}

// {"fps":60,"thermal":0,"headroom":42,"mode":1,"cap":0,"hot":false,
//  "battery":81,"charging":false,"temp":36.5} for the second screen.
JNIEXPORT jstring JNICALL
Java_org_reblue_android_DualScreenCompanion_nativePerf(JNIEnv *env, jclass) {
  using namespace bd::platform::android;
  const int pending = g_pending_mode.load(std::memory_order_acquire);
  const int mode = pending >= 0 ? pending : static_cast<int>(Mode());
  const std::string json = std::format(
      R"({{"fps":{},"thermal":{},"headroom":{},"mode":{},"cap":{},"hot":{},"battery":{},"charging":{},"temp":{:.1f}}})",
      g_live_fps.load(std::memory_order_relaxed),
      g_live_status.load(std::memory_order_relaxed),
      g_live_headroom_pct.load(std::memory_order_relaxed), mode, PowerFPSCap(),
      g_live_hot.load(std::memory_order_relaxed),
      g_battery_level.load(std::memory_order_relaxed),
      g_battery_charging.load(std::memory_order_relaxed),
      g_battery_decicelsius.load(std::memory_order_relaxed) / 10.0);
  return env->NewStringUTF(json.c_str());
}

} // extern "C"

#endif // __ANDROID__
