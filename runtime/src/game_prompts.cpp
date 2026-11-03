#include <psp/game_prompts.h>
#include <psp/memory.h>

#include <cstdio>
#include <atomic>
#include <stdexcept>

#include "kernel_internal.h"

namespace psp {
namespace {
uint32_t g_art = 0;
constexpr uint32_t kTileBytes = 256;  // 32x16 swizzled CLUT4, same layout as the native icon.

bool ram(uint32_t address, uint32_t bytes) {
  return (address & 0x3FFFFFFF) - kRamBase <= kRamSize - bytes;
}
}  // namespace

char prompts_label(uint32_t pack, uint32_t sprite) {
  // Title/menu and pause-map resources only. Gameplay action prompts are not positional aliases.
  if (pack != 0x00EB5398 && pack != 0x00A6753D) return 0;
  if (sprite == 0x00C850DF) return 'Y';
  if (sprite == 0x00B450DB) return 'X';
  return 0;
}

bool prompts_pixel(char label, unsigned x, unsigned y) {
  if (x >= 12 || y >= 12 || (label != 'X' && label != 'Y')) return false;
  const int dx = int(x) * 2 - 11, dy = int(y) * 2 - 11;
  const int radius = dx * dx + dy * dy;
  if (radius >= 85 && radius <= 125) return true;
  if (x < 3 || x > 7 || y < 2 || y > 8) return false;
  constexpr unsigned cross[] = {17, 17, 10, 4, 10, 17, 17};
  constexpr unsigned triangle[] = {17, 17, 10, 4, 4, 4, 4};
  return ((label == 'X' ? cross[y - 2] : triangle[y - 2]) >> (7 - x)) & 1;
}

void prompts_init(bool modern) {
  g_art = 0;
  if (!modern) return;
  // Immutable palette-index variants avoid modifying guest textures or queued/cache snapshots.
  g_art = k::sysmem_alloc(2 * 16 * 16 * kTileBytes, 128, true, "controller prompts");
  if (!g_art) throw std::runtime_error("cannot allocate controller prompt artwork");
  for (unsigned label = 0; label < 2; ++label)
    for (unsigned clear = 0; clear < 16; ++clear)
      for (unsigned ink = 0; ink < 16; ++ink) {
        uint8_t* tile = ptr(g_art + ((label * 16 + clear) * 16 + ink) * kTileBytes);
        for (unsigned y = 0; y < 16; ++y)
          for (unsigned x = 0; x < 32; x += 2) {
            unsigned a = prompts_pixel(label ? 'Y' : 'X', x, y) ? ink : clear;
            unsigned b = prompts_pixel(label ? 'Y' : 'X', x + 1, y) ? ink : clear;
            tile[(y / 8) * 128 + (y % 8) * 16 + x / 2] = uint8_t(a | b << 4);
          }
      }
}

uint32_t prompts_texture(uint32_t descriptor, uint32_t original) {
  if (!g_art || !ram(descriptor, 48)) return original;
  const char label = prompts_label(lw(descriptor + 8), lw(descriptor + 4));
  if (!label || lw(descriptor + 12) != 0x000C000C) return original;
  const uint32_t header = lw(descriptor + 20);
  if (!ram(header, 16) || lw(header) != 0x40105020 || lhu(header + 4) != 4 ||
      (lw(descriptor) & 3) || lw(descriptor + 16) != 0)
    return original;
  const uint32_t palette = lw(header + 12) + lhu(descriptor + 24) * 64;
  if (!ram(palette, 64)) return original;
  int clear = -1, ink = -1;
  uint32_t bestAlpha = 127;
  for (unsigned i = 0; i < 16; ++i) {
    uint32_t color = lw(palette + i * 4);
    if (!(color >> 24)) clear = int(i);
    if ((color >> 24) > bestAlpha && (color & 255) >= 224 &&
        ((color >> 8) & 255) >= 224 && ((color >> 16) & 255) >= 224) {
      ink = int(i);
      bestAlpha = color >> 24;
    }
  }
  if (clear < 0 || ink < 0) {
    static std::atomic_flag warned = ATOMIC_FLAG_INIT;
    if (!warned.test_and_set())
      std::fprintf(stderr, "controller prompt palette unsupported; keeping original icons\n");
    return original;
  }
  return g_art + (((label == 'Y' ? 1 : 0) * 16 + clear) * 16 + ink) * kTileBytes;
}
}  // namespace psp
