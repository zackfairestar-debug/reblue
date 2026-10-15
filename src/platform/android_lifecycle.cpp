/**
 * @file    platform/android_lifecycle.cpp
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/android_lifecycle.h"

#if defined(__ANDROID__)

#include <atomic>

#include <SDL3/SDL_events.h>

#include "core/logging.h"

namespace bd::platform::android {
namespace {

std::atomic<bool> g_installed{false};
std::atomic<bool> g_surface_available{true};
std::atomic<bool> g_surface_changed{false};

bool LifecycleWatch(void * /*userdata*/, SDL_Event *event) {
  switch (event->type) {
  case SDL_EVENT_WILL_ENTER_BACKGROUND:
  case SDL_EVENT_DID_ENTER_BACKGROUND:
    if (g_surface_available.exchange(false, std::memory_order_acq_rel))
      BD_INFO("Activity left the foreground, presentation paused");
    break;
  case SDL_EVENT_DID_ENTER_FOREGROUND:
    if (!g_surface_available.load(std::memory_order_acquire)) {
      g_surface_changed.store(true, std::memory_order_release);
      g_surface_available.store(true, std::memory_order_release);
      BD_INFO("Activity back in the foreground, rebuilding the swap chain");
    }
    break;
  default:
    break;
  }
  return true;
}

} // namespace

void InstallLifecycleWatch() {
  if (g_installed.exchange(true, std::memory_order_acq_rel))
    return;
  SDL_AddEventWatch(&LifecycleWatch, nullptr);
}

bool SurfaceAvailable() {
  return g_surface_available.load(std::memory_order_acquire);
}

bool ConsumeSurfaceChanged() {
  return g_surface_changed.exchange(false, std::memory_order_acq_rel);
}

} // namespace bd::platform::android

#endif // __ANDROID__
