/**
 * @file    platform/android_lifecycle.h
 * @brief   Tracks whether the Android activity has a surface to present to.
 *
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Android destroys the window surface whenever the activity leaves the
 * foreground and hands out a new ANativeWindow when it returns, so the
 * renderer must stop presenting in between and rebuild its swap chain after.
 * Everywhere else the surface simply always exists.
 */
#pragma once

namespace bd::platform::android {

#if defined(__ANDROID__)

// Idempotent. Registers an SDL event watch for the lifecycle events, which SDL
// delivers on the Java UI thread.
void InstallLifecycleWatch();

// False from the moment the activity starts going to the background until it
// is back in the foreground.
bool SurfaceAvailable();

// True once per return to the foreground: the old ANativeWindow is gone and
// the swap chain has to be created again from the new one.
bool ConsumeSurfaceChanged();

#else

inline void InstallLifecycleWatch() {}
inline bool SurfaceAvailable() { return true; }
inline bool ConsumeSurfaceChanged() { return false; }

#endif

} // namespace bd::platform::android
