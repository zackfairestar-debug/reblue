/**
 * @file    platform/android_perf.h
 * @brief   Android Dynamic Performance Framework: the per-frame work report
 *          that keeps the CPU clocked for the frame rate, the thermal state
 *          the [perf] line carries, the power-saving frame-rate cap and the
 *          display refresh rate that follows the game's.
 *
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Without a hint session Android's governor sees a thread that sleeps between
 * frames and keeps it on low clocks or a little core, so a 60 fps cap reads as
 * a light load. Reporting how long each frame's work took against the target
 * lets the scheduler boost exactly enough. Everywhere else all of it is a
 * no-op.
 *
 * Power saving (bd_power_saving): Blue Dragon simulates at 30 Hz and the frame
 * rate above that is interpolation, so 30 fps is the original game and halves
 * the GPU's work. "Auto" drops to 30 while the device reports it is getting
 * hot and goes back up once it has cooled; "Max" stays at 30. Either way the
 * display is asked to run at the rate the game presents, so a 120 Hz panel
 * does not scan out every frame twice.
 */
#pragma once

#include <chrono>
#include <string>

namespace bd::platform::android {

enum class PowerSaving : int { Off = 0, Auto = 1, Max = 2 };

#if defined(__ANDROID__)

// Call once per presented frame from the thread that runs the frame. The
// first call opens the hint session for that thread. target_fps <= 0 means
// uncapped, which reports against 60.
void ReportFrameWork(std::chrono::nanoseconds work, int target_fps);

// " | thermal <status>, headroom <x>, power <mode>" for the [perf] line, or
// empty when the device reports nothing.
std::string ThermalSummary();

// The power-saving cap in frames per second, 0 for none. A relaxed load, so
// it can be asked many times a frame; it only moves in UpdatePowerSaving.
int PowerFPSCap();

// Once per frame, between frames: re-reads the thermal state (at most every
// couple of seconds) and moves the cap.
void UpdatePowerSaving();

// Asks the compositor to run the game's display at fps (0 = no preference).
// Only talks to the surface when the rate or the surface changes.
void ApplyDisplayFrameRate(int fps);

// The activity came back with a new surface: the next ApplyDisplayFrameRate
// sets the rate on it again.
void InvalidateDisplayFrameRate();

#else

inline void ReportFrameWork(std::chrono::nanoseconds, int) {}
inline std::string ThermalSummary() { return {}; }
inline int PowerFPSCap() { return 0; }
inline void UpdatePowerSaving() {}
inline void ApplyDisplayFrameRate(int) {}
inline void InvalidateDisplayFrameRate() {}

#endif

} // namespace bd::platform::android
