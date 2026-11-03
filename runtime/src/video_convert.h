// NV12 to RGBA conversion shared by the video decoders (BT.601, limited range).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace psp {
namespace mpeg {

inline uint8_t clamp_u8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// `y` and `uv` point at the first row of each plane; the pitches are in bytes and independent of
// the picture width (decoders pad rows). `out` receives width * height * 4 bytes (R G B A).
inline void nv12_to_rgba(const uint8_t* y, size_t yPitch, const uint8_t* uv, size_t uvPitch, int width, int height,
                         std::vector<uint8_t>& out) {
  out.resize(static_cast<size_t>(width) * height * 4);
  for (int row = 0; row < height; ++row) {
    uint8_t* o = &out[static_cast<size_t>(row) * width * 4];
    const uint8_t* yr = y + static_cast<size_t>(row) * yPitch;
    const uint8_t* cr = uv + static_cast<size_t>(row / 2) * uvPitch;
    for (int x = 0; x < width; ++x) {
      int Y = yr[x] - 16, U = cr[x & ~1] - 128, V = cr[(x & ~1) + 1] - 128;
      int c = 298 * Y;
      o[0] = clamp_u8((c + 409 * V + 128) >> 8);
      o[1] = clamp_u8((c - 100 * U - 208 * V + 128) >> 8);
      o[2] = clamp_u8((c + 516 * U + 128) >> 8);
      o[3] = 255;
      o += 4;
    }
  }
}

}  // namespace mpeg
}  // namespace psp
