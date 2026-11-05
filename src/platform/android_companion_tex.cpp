/**
 * @file    platform/android_companion_tex.cpp
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/android_companion_tex.h"

#if defined(__ANDROID__)

#include <algorithm>
#include <cstring>
#include <string>

#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "vfs/vfs.h"

namespace bd::platform::android {
namespace {

namespace tu = rex::graphics::texture_util;
namespace tc = rex::graphics::texture_conversion;
namespace xe = rex::graphics::xenos;

// The game's texture files: a 2048-byte header whose D3D texture carries its
// fetch constant at +0x20, then the payload, based at zero.
constexpr size_t kFileHeaderBytes = 2048;
constexpr size_t kFileFetchOffset = 0x20;

// Anything bigger is not UI art or a minimap, and is refused rather than
// decoded on a panel request.
constexpr u32 kMaxEdge = 2048;

enum : u32 {
  kFmt8 = 2,
  kFmt565 = 4,
  kFmt8888 = 6,
  kFmt88 = 10,
  kFmt4444 = 15,
  kFmtDXT1 = 18,
  kFmtDXT2_3 = 19,
  kFmtDXT4_5 = 20,
};

struct Layout {
  u32 edge = 1;     // texels per block edge
  u32 bpb = 0;      // bytes per block
  u32 bpbLog2 = 0;
  u32 blocksW = 0;
  u32 blocksH = 0;
  u32 pitchBlocks = 0; // tiled: padded to 32 blocks
  u32 rowsBlocks = 0;
};

bool MakeLayout(const XenosFetch &f, Layout &L) {
  switch (f.Format()) {
  case kFmt8:
    L = {1, 1, 0};
    break;
  case kFmt565:
  case kFmt88:
  case kFmt4444:
    L = {1, 2, 1};
    break;
  case kFmt8888:
    L = {1, 4, 2};
    break;
  case kFmtDXT1:
    L = {4, 8, 3};
    break;
  case kFmtDXT2_3:
  case kFmtDXT4_5:
    L = {4, 16, 4};
    break;
  default:
    return false;
  }
  const u32 w = f.Width();
  const u32 h = f.Height();
  if (w > kMaxEdge || h > kMaxEdge)
    return false;
  L.blocksW = (w + L.edge - 1) / L.edge;
  L.blocksH = (h + L.edge - 1) / L.edge;
  if (f.Tiled()) {
    L.pitchBlocks = (L.blocksW + 31) & ~31u;
    L.rowsBlocks = (L.blocksH + 31) & ~31u;
  } else {
    const u32 pitchTexels = ((f.dword[0] >> 22) & 0x1FF) * 32;
    L.pitchBlocks = std::max(L.blocksW, pitchTexels / L.edge);
    L.rowsBlocks = L.blocksH;
  }
  return true;
}

void Rgb565(u16 c, u8 *out) {
  out[0] = static_cast<u8>(((c >> 11) & 31) * 255 / 31);
  out[1] = static_cast<u8>(((c >> 5) & 63) * 255 / 63);
  out[2] = static_cast<u8>((c & 31) * 255 / 31);
}

// A BC1 color block (little-endian after the swap) into 16 RGBA texels.
void DecodeColorBlock(const u8 *b, bool punchThrough, u8 out[16][4]) {
  const u16 c0 = static_cast<u16>(b[0] | (b[1] << 8));
  const u16 c1 = static_cast<u16>(b[2] | (b[3] << 8));
  const u32 idx = b[4] | (b[5] << 8) | (b[6] << 16) | (u32(b[7]) << 24);
  u8 pal[4][4];
  Rgb565(c0, pal[0]);
  Rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || !punchThrough) {
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = static_cast<u8>((2 * pal[0][i] + pal[1][i]) / 3);
      pal[3][i] = static_cast<u8>((pal[0][i] + 2 * pal[1][i]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i)
      pal[2][i] = static_cast<u8>((pal[0][i] + pal[1][i]) / 2);
    pal[2][3] = 255;
    pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
  }
  for (int t = 0; t < 16; ++t)
    std::memcpy(out[t], pal[(idx >> (2 * t)) & 3], 4);
}

void DecodeBlock(u32 format, const u8 *b, u8 out[16][4]) {
  switch (format) {
  case kFmtDXT1:
    DecodeColorBlock(b, true, out);
    return;
  case kFmtDXT2_3:
    DecodeColorBlock(b + 8, false, out);
    for (int t = 0; t < 16; ++t) {
      const u8 nib = (b[t / 2] >> ((t & 1) * 4)) & 15;
      out[t][3] = static_cast<u8>(nib * 17);
    }
    return;
  case kFmtDXT4_5: {
    DecodeColorBlock(b + 8, false, out);
    const u32 a0 = b[0];
    const u32 a1 = b[1];
    u8 pal[8];
    pal[0] = static_cast<u8>(a0);
    pal[1] = static_cast<u8>(a1);
    if (a0 > a1) {
      for (u32 i = 0; i < 6; ++i)
        pal[2 + i] = static_cast<u8>(((6 - i) * a0 + (i + 1) * a1) / 7);
    } else {
      for (u32 i = 0; i < 4; ++i)
        pal[2 + i] = static_cast<u8>(((4 - i) * a0 + (i + 1) * a1) / 5);
      pal[6] = 0;
      pal[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; ++i)
      bits |= u64(b[2 + i]) << (8 * i);
    for (int t = 0; t < 16; ++t)
      out[t][3] = pal[(bits >> (3 * t)) & 7];
    return;
  }
  default:
    return;
  }
}

void DecodeTexel(u32 format, const u8 *b, u8 *px) {
  switch (format) {
  case kFmt8:
    px[0] = px[1] = px[2] = b[0];
    px[3] = 255;
    return;
  case kFmt88:
    px[0] = px[1] = px[2] = b[0];
    px[3] = b[1];
    return;
  case kFmt565:
    Rgb565(static_cast<u16>(b[0] | (b[1] << 8)), px);
    px[3] = 255;
    return;
  case kFmt4444: {
    const u16 v = static_cast<u16>(b[0] | (b[1] << 8));
    px[0] = static_cast<u8>(((v >> 8) & 15) * 17);
    px[1] = static_cast<u8>(((v >> 4) & 15) * 17);
    px[2] = static_cast<u8>((v & 15) * 17);
    px[3] = static_cast<u8>((v >> 12) * 17);
    return;
  }
  case kFmt8888:
    // ARGB stored big-endian: after the swap the bytes run B, G, R, A.
    px[0] = b[2];
    px[1] = b[1];
    px[2] = b[0];
    px[3] = b[3];
    return;
  default:
    return;
  }
}

} // namespace

size_t XenosMip0Bytes(const XenosFetch &fetch) {
  Layout L;
  if (!MakeLayout(fetch, L))
    return 0;
  return size_t(L.pitchBlocks) * L.rowsBlocks * L.bpb;
}

bool DecodeXenosTexture(const XenosFetch &fetch, const u8 *payload,
                        size_t payloadBytes, DecodedImage &out) {
  Layout L;
  if (!payload || !MakeLayout(fetch, L))
    return false;
  const size_t need = size_t(L.pitchBlocks) * L.rowsBlocks * L.bpb;
  if (payloadBytes < need)
    return false;

  const u32 w = fetch.Width();
  const u32 h = fetch.Height();
  out.width = w;
  out.height = h;
  out.rgba.assign(size_t(w) * h * 4, 0);
  const u32 format = fetch.Format();
  const auto endian = static_cast<xe::Endian>(fetch.Endian());

  u8 block[16];
  u8 texels[16][4];
  for (u32 by = 0; by < L.blocksH; ++by) {
    for (u32 bx = 0; bx < L.blocksW; ++bx) {
      size_t offset;
      if (fetch.Tiled()) {
        const i32 o = tu::GetTiledOffset2D(i32(bx), i32(by), L.pitchBlocks,
                                           L.bpbLog2);
        if (o < 0)
          continue;
        offset = size_t(o);
      } else {
        offset = (size_t(by) * L.pitchBlocks + bx) * L.bpb;
      }
      if (offset + L.bpb > payloadBytes)
        continue;
      tc::CopySwapBlock(endian, block, payload + offset, L.bpb);

      if (L.edge == 1) {
        DecodeTexel(format, block, &out.rgba[(size_t(by) * w + bx) * 4]);
        continue;
      }
      DecodeBlock(format, block, texels);
      for (u32 t = 0; t < 16; ++t) {
        const u32 x = bx * 4 + (t & 3);
        const u32 y = by * 4 + (t >> 2);
        if (x < w && y < h)
          std::memcpy(&out.rgba[(size_t(y) * w + x) * 4], texels[t], 4);
      }
    }
  }
  return true;
}

std::vector<u8> EncodePng(const DecodedImage &image) {
  std::vector<u8> png;
  if (!image.width || !image.height ||
      image.rgba.size() < size_t(image.width) * image.height * 4)
    return png;
  size_t len = 0;
  void *data = tdefl_write_image_to_png_file_in_memory_ex(
      image.rgba.data(), int(image.width), int(image.height), 4,
      int(image.width * 4), &len, 1, MZ_FALSE, nullptr, 0, nullptr, 0);
  if (data) {
    png.assign(static_cast<u8 *>(data), static_cast<u8 *>(data) + len);
    mz_free(data);
  }
  return png;
}

std::vector<u8> GameTexturePng(std::string_view path, std::string_view locale) {
  if (path.empty() || path.size() > 128 || path.find("..") != path.npos)
    return {};
  // Key::Localized would read the locale out of guest memory, which is not
  // safe from a WebView thread, so the caller hands it in.
  std::string localized(path);
  if (auto xx = localized.find("_xx."); xx != std::string::npos) {
    if (locale.empty())
      return {};
    localized.replace(xx, 3, locale);
  }
  const auto key = vfs::Key::FromRelative(localized);
  auto hit = vfs::VFS::Get().Files().Read(key, vfs::Tier::Fallback);
  if (!hit || hit->bytes.size() < kFileHeaderBytes)
    return {};
  const std::vector<u8> &blob = hit->bytes;

  XenosFetch fetch;
  for (int i = 0; i < 6; ++i) {
    const u8 *p = blob.data() + kFileFetchOffset + i * 4;
    fetch.dword[i] = (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) |
                     u32(p[3]);
  }
  DecodedImage image;
  if (!DecodeXenosTexture(fetch, blob.data() + kFileHeaderBytes,
                          blob.size() - kFileHeaderBytes, image))
    return {};
  return EncodePng(image);
}

} // namespace bd::platform::android

#endif // __ANDROID__
