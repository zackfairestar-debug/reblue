/**
 * @file    platform/android_companion_tex.h
 * @brief   Guest texture to PNG for the dual-screen panel: the game's own UI
 *          art read from the installed game, and the live minimap texture.
 *
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Nothing here ships game art: every image is decoded on the device from the
 * player's own installed files or from what the game has loaded.
 */
#pragma once

#include <string_view>
#include <vector>

#include <rex/types.h>

namespace bd::platform::android {

#if defined(__ANDROID__)

struct DecodedImage {
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> rgba; // width * height * 4
};

// The six dwords of a Xenos texture fetch constant, host order.
struct XenosFetch {
  u32 dword[6] = {};

  bool Tiled() const { return (dword[0] >> 31) & 1; }
  u32 Format() const { return dword[1] & 0x3F; }
  u32 Endian() const { return (dword[1] >> 6) & 3; }
  u32 BaseAddress() const { return dword[1] & 0xFFFFF000u; }
  u32 Width() const { return (dword[2] & 0x1FFF) + 1; }
  u32 Height() const { return ((dword[2] >> 13) & 0x1FFF) + 1; }
};

// Bytes mip 0 occupies, tiled padding included. Zero for a format this
// decoder does not handle.
size_t XenosMip0Bytes(const XenosFetch &fetch);

// Mip 0 to RGBA8. False for an unsupported format or a short payload.
bool DecodeXenosTexture(const XenosFetch &fetch, const u8 *payload,
                        size_t payloadBytes, DecodedImage &out);

// Empty on failure.
std::vector<u8> EncodePng(const DecodedImage &image);

// A texture of the installed game, by its path under the disc root (for
// example "d2anime/res/cmn_fc.dds" or "bd_system/wnd/bt_01_of.dds"; "_xx"
// becomes 'locale', what Key::Localized puts there), as PNG. Empty when it is not
// there. Reads only the game files, never guest memory, so it is safe off the
// game thread; but the VFS has to be up, which it is once the game loop runs.
std::vector<u8> GameTexturePng(std::string_view path, std::string_view locale);

#endif

} // namespace bd::platform::android
