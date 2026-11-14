/**
 * @file    platform/android_gpu_driver.cpp
 * @brief   Custom Vulkan driver (Mesa Turnip) loaded through libadrenotools
 *          before plume initialises volk.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * The app's "GPU driver" page installs a driver package (a zip with meta.json
 * and the driver .so, the format the Android emulators share) and passes the
 * choice in the environment before the native library starts:
 *
 *   REBLUE_VK_DRIVER_DIR   folder holding the package's .so (app-private)
 *   REBLUE_VK_DRIVER_LIB   the .so's file name (meta.json "libraryName")
 *   REBLUE_VK_HOOK_DIR     nativeLibraryDir, where libadrenotools' hooks are
 *   REBLUE_VK_BOOT_MARKER  a file the app wrote before this start; removed
 *                          once the device is up, so a driver that crashes
 *                          the start is dropped on the next one
 *
 * libadrenotools swaps the driver in under the system Vulkan loader, which
 * keeps providing the Android surface and swap chain. When anything fails the
 * system driver is used, as if nothing had been chosen.
 */
#include "platform/android_gpu_driver.h"

#if defined(__ANDROID__)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dlfcn.h>
#include <unistd.h>

#include <volk.h>

#include "core/logging.h"

#if defined(REBLUE_HAS_ADRENOTOOLS)
#include <adrenotools/driver.h>
#endif

namespace bd::platform::android {
namespace {

bool g_custom = false;
std::string g_name;

const char *Env(const char *name) {
  const char *value = getenv(name);
  return value && *value ? value : nullptr;
}

} // namespace

void LoadCustomVulkanDriver() {
  static bool tried = false;
  if (tried)
    return;
  tried = true;

  const char *lib = Env("REBLUE_VK_DRIVER_LIB");
  if (!lib)
    return;
#if !defined(REBLUE_HAS_ADRENOTOOLS)
  BD_WARN("[gpu] driver '{}' chosen, but this build has no libadrenotools; "
          "using the system driver",
          lib);
#else
  const char *dir = Env("REBLUE_VK_DRIVER_DIR");
  const char *hooks = Env("REBLUE_VK_HOOK_DIR");
  if (!dir || !hooks) {
    BD_WARN("[gpu] driver '{}' chosen without its folders; using the system "
            "driver",
            lib);
    return;
  }

  void *handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM,
                                            nullptr, hooks, dir, lib, nullptr,
                                            nullptr);
  if (!handle) {
    BD_ERROR("[gpu] driver '{}' failed to load from '{}' (hooks '{}'); using "
             "the system driver",
             lib, dir, hooks);
    return;
  }
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(handle, "vkGetInstanceProcAddr"));
  if (!gipa) {
    BD_ERROR("[gpu] driver '{}' has no vkGetInstanceProcAddr; using the "
             "system driver",
             lib);
    return;
  }
  volkInitializeCustom(gipa);
  if (volkGetInstanceVersion() == 0) {
    BD_ERROR("[gpu] driver '{}' reports no instance version; using the "
             "system driver",
             lib);
    // Leave volk unloaded so plume opens the system libvulkan itself.
    volkFinalize();
    return;
  }
  g_custom = true;
  g_name = lib;
  const uint32_t v = volkGetInstanceVersion();
  BD_INFO("[gpu] custom driver '{}' loaded through libadrenotools, instance "
          "Vulkan {}.{}.{}",
          lib, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
          VK_API_VERSION_PATCH(v));
#endif
}

void ConfirmVulkanDriverStarted() {
  if (const char *marker = Env("REBLUE_VK_BOOT_MARKER")) {
    if (unlink(marker) == 0)
      BD_INFO("[gpu] {} driver started; boot marker cleared",
              g_custom ? g_name : std::string("system"));
  }
}

bool UsingCustomVulkanDriver() { return g_custom; }

} // namespace bd::platform::android

#endif // __ANDROID__
