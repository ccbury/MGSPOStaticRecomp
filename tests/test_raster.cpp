#include "raster_fast.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace fast = psp::ge::fast;

static int failures = 0;
#define CHECK(condition)                                                        \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);           \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static fast::Vert vertex(float x, float y, uint32_t colour = 0xFFFFFFFF) {
  fast::Vert v{};
  v.x = x;
  v.y = y;
  v.invw = 1;
  for (int i = 0; i < 4; ++i) v.col[i] = ((colour >> (i * 8)) & 255) / 255.0f;
  return v;
}

static fast::Ctx context(uint32_t* pixels, int width, int height) {
  fast::Ctx cx;
  cx.fb = reinterpret_cast<uint8_t*>(pixels);
  cx.fbStride = width;
  cx.sx2 = width - 1;
  cx.sy2 = height - 1;
  return cx;
}

static void primitive_limit() {
  uint32_t pixel = 0;
  fast::Ctx cx = context(&pixel, 1, 1);
  uint64_t fragments = 0;
  fast::set_stats_sink(&fragments, nullptr);
  fast::Batch batch;
  batch.begin(&cx, nullptr, 1);
  // These are one-pixel draws: exercise the exact capacity boundary, not raster throughput.
  for (int i = 0; i < 4096; ++i) CHECK(batch.add_rect(vertex(0, 0), vertex(1, 1)));
  CHECK(fragments == 0);
  CHECK(pixel == 0);
  CHECK(batch.add_rect(vertex(0, 0), vertex(1, 1, 0xFF010203)));
  CHECK(fragments == 4096);
  CHECK(pixel == 0xFFFFFFFF);
  fast::sync();
  CHECK(fragments == 4097);
  CHECK(pixel == 0xFF010203);

  fragments = 0;
  pixel = 0;
  batch.begin(&cx, nullptr, 1);
  for (int i = 0; i < 4097; ++i)
    CHECK(batch.add_tri(vertex(0, 0), vertex(2, 0), vertex(0, 2)));
  CHECK(fragments == 4096);
  fast::sync();
  CHECK(fragments == 4097);
  CHECK(pixel == 0xFFFFFFFF);
  fast::set_stats_sink(nullptr, nullptr);
}

static void draw_limit_and_palette() {
  std::vector<uint32_t> pixels(1025);
  fast::Ctx cx = context(pixels.data(), 1025, 1);
  const uint8_t index = 0;
  uint32_t palette[256] = {};
  cx.tex = true;
  cx.texIdx = &index;
  cx.tFunc = 3;
  uint64_t fragments = 0;
  fast::set_stats_sink(&fragments, nullptr);
  fast::Batch batch;
  for (int i = 0; i < 1025; ++i) {
    palette[0] = 0xFF000000u | static_cast<uint32_t>(i + 1);
    batch.begin(&cx, palette, 1);
    CHECK(batch.add_rect(vertex(static_cast<float>(i), 0), vertex(static_cast<float>(i + 1), 1)));
    if (i == 1023) CHECK(fragments == 0);
  }
  CHECK(fragments == 1024);
  CHECK(pixels.back() == 0);
  fast::sync();
  CHECK(fragments == pixels.size());
  for (size_t i = 0; i < pixels.size(); ++i)
    CHECK(pixels[i] == (0xFF000000u | static_cast<uint32_t>(i + 1)));
  fast::set_stats_sink(nullptr, nullptr);
}

static void cache_eviction_and_rewrite() {
  uint32_t pixel = 0, texel = 0;
  fast::Ctx cx = context(&pixel, 1, 1);
  cx.tex = true;
  cx.tFunc = 3;
  fast::TexSource src;
  src.mem = reinterpret_cast<const uint8_t*>(&texel);
  src.avail = sizeof texel;
  src.fmt = 3;
  src.bufW = src.w = src.h = 1;
  fast::Texture tex;
  fast::Batch batch;
  // Rewrites and eviction must finish the draw that still holds the old decoded pointer.
  for (int i = 0; i < 98; ++i) {
    texel = 0xFF000000u | static_cast<uint32_t>(i + 1);
    src.addr = static_cast<uint32_t>(i);
    CHECK(fast::texture_lookup(src, tex));
    if (i) CHECK(pixel == (0xFF000000u | static_cast<uint32_t>(i)));
    cx.texRgba = tex.rgba;
    batch.begin(&cx, nullptr, 1);
    CHECK(batch.add_rect(vertex(0, 0), vertex(1, 1)));
  }
  texel = 0xFF123456;
  CHECK(fast::texture_lookup(src, tex));
  CHECK(pixel == 0xFF000062);
  CHECK(tex.rgba[0] == texel);
  fast::sync();
  src.w = src.h = INT_MAX;
  CHECK(!fast::texture_lookup(src, tex));
}

static std::vector<uint32_t> ordered_bands(int threads) {
  fast::set_threads(threads);
  std::vector<uint32_t> pixels(128 * 192);
  fast::Ctx cx = context(pixels.data(), 128, 192);
  fast::Batch batch;
  batch.begin(&cx, nullptr, 1);
  CHECK(batch.add_rect(vertex(0, 0), vertex(128, 192, 0xFF123456)));
  CHECK(batch.add_rect(vertex(5, 7), vertex(100, 180, 0xFFABCDEF)));
  fast::sync();
  CHECK(pixels[0] == 0xFF123456);
  CHECK(pixels[7 * 128 + 5] == 0xFFABCDEF);
  CHECK(pixels.back() == 0xFF123456);
  return pixels;
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--threads") == 0) {
    const char* expected = std::getenv("MGSPO_TEST_THREADS");
    CHECK(expected != nullptr);
    if (expected) CHECK(fast::threads() == std::atoi(expected));
  } else {
    fast::set_threads(INT_MAX);
    CHECK(fast::threads() == 6);
    fast::set_threads(0);
    CHECK(fast::threads() == 1);
    primitive_limit();
    draw_limit_and_palette();
    cache_eviction_and_rewrite();
    const auto single = ordered_bands(1);
    CHECK(single == ordered_bands(2));
  }
  if (!failures) std::puts("Raster resource regression checks passed.");
  return failures ? 1 : 0;
}
