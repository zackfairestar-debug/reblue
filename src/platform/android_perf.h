/**
 * @file    platform/android_perf.h
 * @brief   Android Dynamic Performance Framework: the per-frame work report
 *          that keeps the CPU clocked for the frame rate, and the thermal
 *          state the [perf] line carries.
 *
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Without a hint session Android's governor sees a thread that sleeps between
 * frames and keeps it on low clocks or a little core, so a 60 fps cap reads as
 * a light load. Reporting how long each frame's work took against the target
 * lets the scheduler boost exactly enough. Everywhere else both are no-ops.
 */
#pragma once

#include <chrono>
#include <string>

namespace bd::platform::android {

#if defined(__ANDROID__)

// Call once per presented frame from the thread that runs the frame. The
// first call opens the hint session for that thread. target_fps <= 0 means
// uncapped, which reports against 60.
void ReportFrameWork(std::chrono::nanoseconds work, int target_fps);

// " | thermal <status>, headroom <x>" for the [perf] line, or empty when the
// device reports neither.
std::string ThermalSummary();

#else

inline void ReportFrameWork(std::chrono::nanoseconds, int) {}
inline std::string ThermalSummary() { return {}; }

#endif

} // namespace bd::platform::android
