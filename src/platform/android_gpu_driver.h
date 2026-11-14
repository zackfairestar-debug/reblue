/**
 * @file    platform/android_gpu_driver.h
 * @brief   Custom Vulkan driver (Mesa Turnip) chosen on the app's GPU driver
 *          page, loaded through libadrenotools.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

namespace bd::platform::android {

#if defined(__ANDROID__)

// Points volk at the chosen driver package, before plume's VulkanInterface
// initialises it. Does nothing when none is chosen or it cannot be loaded, so
// plume falls back to the system driver. Only the first call does anything.
void LoadCustomVulkanDriver();

// The device came up: removes the boot marker, so the app keeps the driver
// for the next start.
void ConfirmVulkanDriverStarted();

bool UsingCustomVulkanDriver();

#else

inline void LoadCustomVulkanDriver() {}
inline void ConfirmVulkanDriverStarted() {}
inline bool UsingCustomVulkanDriver() { return false; }

#endif

} // namespace bd::platform::android
