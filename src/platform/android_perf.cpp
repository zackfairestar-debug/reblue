/**
 * @file    platform/android_perf.cpp
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/android_perf.h"

#if defined(__ANDROID__)

#include <cmath>
#include <cstdint>
#include <format>

#include <android/performance_hint.h>
#include <android/thermal.h>
#include <unistd.h>

#include "core/logging.h"

namespace bd::platform::android {
namespace {

constexpr int kUncappedTargetFPS = 60;

int64_t TargetNanos(int target_fps) {
  if (target_fps <= 0)
    target_fps = kUncappedTargetFPS;
  return 1'000'000'000LL / target_fps;
}

const char *ThermalStatusName(AThermalStatus status) {
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
  static AThermalManager *manager = AThermal_acquireManager();
  if (!manager)
    return {};
  const char *status = ThermalStatusName(AThermal_getCurrentThermalStatus(manager));
  // 1.0 is where the device starts throttling; NaN when unsupported.
  const float headroom = AThermal_getThermalHeadroom(manager, 0);
  if (!status && !std::isfinite(headroom))
    return {};
  std::string out = " | thermal ";
  out += status ? status : "?";
  if (std::isfinite(headroom))
    out += std::format(", headroom {:.2f}", headroom);
  return out;
}

} // namespace bd::platform::android

#endif // __ANDROID__
