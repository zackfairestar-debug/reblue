/**
 * @file    platform/android_companion.h
 * @brief   Dual-screen companion: the game state the second-screen panel of a
 *          dual-screen handheld (AYN Thor and the like) shows.
 *
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * The panel itself is a WebView in a Presentation on the second display
 * (android/.../DualScreenCompanion.java). It polls a JSON snapshot that is
 * built here on the game thread, once every few engine steps, from the same
 * engine handles the console commands read. Nothing here writes engine state.
 */
#pragma once

#include <rex/types.h>

namespace bd::platform::android {

#if defined(__ANDROID__)

// Called on the game thread right after each bdMainGameStep. Throttled
// internally; cheap when nothing reads the snapshot.
void CompanionSample();

#else

inline void CompanionSample() {}

#endif

} // namespace bd::platform::android
