// Drives the software GE with hand-built command lists and checks the pixels that end up in VRAM.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <psp/kernel.h>
#include <psp/runtime.h>

#include "gpu.h"
#include "raster_fast.h"
#include "render_surface.h"
#include "hardware_renderer.h"
#include <psp/video_settings.h>

using namespace psp;

static int g_failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

constexpr uint32_t kList = 0x08800000;
constexpr uint32_t kVerts = 0x08900000;
constexpr uint32_t kTex = 0x08910000;
constexpr uint32_t kVram = 0x04000000;

constexpr uint32_t kThroughCol = 0x800000 | (7 << 2) | (3 << 7);  // through, ABGR8888 colour, float position

static uint32_t cmdf(uint32_t op, float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return (op << 24) | (b >> 8);
}
static uint32_t cmd(uint32_t op, uint32_t arg = 0) { return (op << 24) | (arg & 0xFFFFFF); }

struct Builder {
  std::vector<uint32_t> w;
  Builder& add(uint32_t c) {
    w.push_back(c);
    return *this;
  }
  Builder& f(uint32_t op, float v) { return add(cmdf(op, v)); }
  // framebuffer 480x272 at the start of VRAM with a full-screen scissor
  Builder& setup(uint32_t fmt = 3) {
    add(cmd(0x10, 0x080000));
    add(cmd(0x9C, 0)).add(cmd(0x9D, 0x040000 | 512)).add(cmd(0xD2, fmt));
    add(cmd(0xD4, 0)).add(cmd(0xD5, (271 << 10) | 479));
    add(cmd(0xE7, 1));  // depth writes off unless a test asks for them
    return *this;
  }
  Builder& vertex(uint32_t addr_, uint32_t vtype) { return add(cmd(0x12, vtype)).add(cmd(0x01, addr_ & 0xFFFFFF)); }
  Builder& prim(int type, int count) { return add(cmd(0x04, (type << 16) | count)); }
  Builder& finish() { return add(cmd(0x0F)).add(cmd(0x0C)); }
  void write(uint32_t addr = kList) { mem_write(addr, w.data(), w.size() * 4); }
};

struct V {
  uint32_t col;
  float x, y, z;
};

static void put_verts(uint32_t addr, const std::vector<V>& v) {
  for (size_t i = 0; i < v.size(); ++i) {
    sw(addr + i * 16, v[i].col);
    float f[3] = {v[i].x, v[i].y, v[i].z};
    std::memcpy(ptr(addr + i * 16 + 4), f, 12);
  }
}

struct NullHost : ge::Host {
  int signals = 0, finishes = 0;
  uint32_t lastSignal = 0, lastFinish = 0;
  void on_signal(uint32_t d) override { ++signals; lastSignal = d; }
  void on_finish(uint32_t d) override { ++finishes; lastFinish = d; }
};

static ge::Stop run(ge::Gpu& g, ge::ListContext& l, NullHost* h = nullptr) {
  static NullHost dummy;
  return g.run(l, h ? h : &dummy);
}

static void exec(ge::Gpu& g, Builder& b, NullHost* h = nullptr) {
  b.write();
  ge::ListContext l;
  l.pc = kList;
  ge::Stop s = run(g, l, h);
  CHECK(s == ge::Stop::Ended);
}

static uint32_t px32(int x, int y) { return lw(kVram + (y * 512 + x) * 4); }
static uint16_t px16(int x, int y) { return lhu(kVram + (y * 512 + x) * 2); }

static void clear_vram() { std::memset(ptr(kVram), 0, 512 * 272 * 4 * 2); }

static void fill_rect(ge::Gpu& g, uint32_t col, float x0, float y0, float x1, float y1) {
  put_verts(kVerts, {{col, x0, y0, 0}, {col, x1, y1, 0}});
  Builder b;
  b.setup().vertex(kVerts, kThroughCol).prim(6, 2).finish();
  exec(g, b);
}

static void test_clear_and_sprite() {
  clear_vram();
  ge::Gpu g;
  put_verts(kVerts, {{0xFF0000FF, 0, 0, 0}, {0xFF0000FF, 480, 272, 0}});
  Builder b;
  b.setup().add(cmd(0xD3, 0x301)).vertex(kVerts, kThroughCol).prim(6, 2).add(cmd(0xD3, 0)).finish();
  exec(g, b);
  CHECK(px32(0, 0) == 0xFF0000FF);
  CHECK(px32(479, 271) == 0xFF0000FF);

  fill_rect(g, 0xFF00FF00, 10, 20, 30, 40);
  CHECK(px32(10, 20) == 0xFF00FF00);
  CHECK(px32(29, 39) == 0xFF00FF00);
  CHECK(px32(30, 39) == 0xFF0000FF);  // right edge excluded
  CHECK(px32(9, 20) == 0xFF0000FF);
}

static void test_triangle_and_scissor() {
  clear_vram();
  ge::Gpu g;
  put_verts(kVerts, {{0xFFFFFFFF, 10, 10, 0}, {0xFFFFFFFF, 50, 10, 0}, {0xFFFFFFFF, 10, 50, 0}});
  Builder b;
  b.setup().add(cmd(0xD4, (0 << 10) | 0)).add(cmd(0xD5, (30 << 10) | 29));
  b.vertex(kVerts, kThroughCol).prim(3, 3).finish();
  exec(g, b);
  CHECK(px32(15, 15) == 0xFFFFFFFF);
  CHECK(px32(11, 11) == 0xFFFFFFFF);
  CHECK(px32(45, 12) == 0);   // inside the triangle but outside the scissor
  CHECK(px32(40, 40) == 0);   // outside the triangle
  CHECK(px32(8, 8) == 0);
}

static void test_gouraud() {
  clear_vram();
  ge::Gpu g;
  put_verts(kVerts, {{0xFF0000FF, 0, 0, 0}, {0xFF00FF00, 100, 0, 0}, {0xFFFF0000, 0, 100, 0}});
  Builder b;
  b.setup().add(cmd(0x50, 1)).vertex(kVerts, kThroughCol).prim(3, 3).finish();
  exec(g, b);
  uint32_t p = px32(2, 2);
  CHECK((p & 255) > 200 && ((p >> 8) & 255) < 20 && ((p >> 16) & 255) < 20);
  p = px32(50, 20);
  CHECK(((p >> 8) & 255) > 80);
}

static void test_565() {
  clear_vram();
  ge::Gpu g;
  put_verts(kVerts, {{0xFFFF0000 | 0x00FF, 0, 0, 0}, {0xFFFF00FF, 20, 20, 0}});  // 0xFFFF00FF: red + blue
  Builder b;
  b.setup(0).vertex(kVerts, kThroughCol).prim(6, 2).finish();
  exec(g, b);
  CHECK(px16(5, 5) == (31 | (31 << 11)));
}

static void test_depth() {
  clear_vram();
  ge::Gpu g;
  Builder b;
  b.setup().add(cmd(0x9E, 0)).add(cmd(0x9F, (0x04 << 16) | 512));
  // depth buffer after the colour buffer
  b.add(cmd(0x9E, 0x088000)).add(cmd(0x9F, 0x040000 | 512));
  b.add(cmd(0xD3, 0x401)).add(cmd(0xE7, 0));
  put_verts(kVerts, {{0, 0, 0, 40000}, {0, 480, 272, 40000}});
  b.vertex(kVerts, kThroughCol).prim(6, 2).add(cmd(0xD3, 0));
  b.add(cmd(0x23, 1)).add(cmd(0xDE, 4));  // depth test less
  put_verts(kVerts + 64, {{0xFF0000FF, 0, 0, 30000}, {0xFF0000FF, 20, 20, 30000}});
  put_verts(kVerts + 128, {{0xFF00FF00, 0, 0, 50000}, {0xFF00FF00, 20, 20, 50000}});
  b.vertex(kVerts + 64, kThroughCol).prim(6, 2);
  b.vertex(kVerts + 128, kThroughCol).prim(6, 2);  // farther: must be rejected
  b.finish();
  exec(g, b);
  CHECK(px32(5, 5) == 0xFF0000FF);
  CHECK(lhu(kVram + 0x88000 + (5 * 512 + 5) * 2) == 30000);
}

static void test_blend() {
  clear_vram();
  ge::Gpu g;
  fill_rect(g, 0xFF000000 | 0xC8, 0, 0, 20, 20);  // dest red = 200
  put_verts(kVerts, {{0x80FF0000, 0, 0, 0}, {0x80FF0000, 20, 20, 0}});
  Builder b;
  b.setup().add(cmd(0x21, 1)).add(cmd(0xDF, 2 | (3 << 4) | (0 << 8)));  // add, src alpha, 1 - src alpha
  b.vertex(kVerts, kThroughCol).prim(6, 2).finish();
  exec(g, b);
  uint32_t p = px32(5, 5);
  int r = p & 255, bl = (p >> 16) & 255;
  CHECK(std::abs(r - 100) <= 2);
  CHECK(std::abs(bl - 128) <= 2);
}

static void test_alpha_test() {
  clear_vram();
  ge::Gpu g;
  put_verts(kVerts, {{0x10FFFFFF, 0, 0, 0}, {0x10FFFFFF, 20, 20, 0}});
  Builder b;
  b.setup().add(cmd(0x22, 1)).add(cmd(0xDB, 6 | (0x80 << 8) | (0xFF << 16)));  // greater than 0x80
  b.vertex(kVerts, kThroughCol).prim(6, 2).finish();
  exec(g, b);
  CHECK(px32(5, 5) == 0);
}

static void test_stencil() {
  clear_vram();
  ge::Gpu g;
  Builder b;
  b.setup();
  put_verts(kVerts, {{0, 0, 0, 0}, {0, 40, 40, 0}});
  b.add(cmd(0xD3, 0x201)).vertex(kVerts, kThroughCol).prim(6, 2).add(cmd(0xD3, 0));  // stencil = 0
  b.add(cmd(0x24, 1)).add(cmd(0xDC, 1 | (0x01 << 8) | (0xFF << 16))).add(cmd(0xDD, 2 << 16));  // always, replace on pass
  put_verts(kVerts + 64, {{0xFF123456, 0, 0, 0}, {0xFF123456, 20, 20, 0}});
  b.vertex(kVerts + 64, kThroughCol).prim(6, 2);
  b.finish();
  exec(g, b);
  CHECK((px32(5, 5) >> 24) == 1);
  CHECK((px32(30, 30) >> 24) == 0);
}

static void write_tex_8888(uint32_t addr, int w, int h) {
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) sw(addr + (y * w + x) * 4, 0xFF000000 | (x * 16) | ((y * 16) << 8));
}

static Builder textured_sprite(uint32_t fmt, int w, int h) {
  Builder b;
  b.setup();
  b.add(cmd(0x1E, 1)).add(cmd(0xA0, kTex & 0xFFFFFF)).add(cmd(0xA8, ((kTex >> 8) & 0xFF0000) | w));
  b.add(cmd(0xB8, 4 | (4 << 8))).add(cmd(0xC3, fmt)).add(cmd(0xC9, 3 | (1 << 8)));  // replace, RGBA
  return b;
}

static void test_texture_8888() {
  clear_vram();
  ge::Gpu g;
  write_tex_8888(kTex, 16, 16);
  // through mode, ABGR colour + float uv + float pos
  uint32_t vt = 0x800000 | (7 << 2) | (3 << 7) | 3;
  struct TV {
    float u, v;
    uint32_t col;
    float x, y, z;
  };
  TV tv[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0}, {16, 16, 0xFFFFFFFF, 16, 16, 0}};
  std::memcpy(ptr(kVerts), tv, sizeof tv);
  Builder b = textured_sprite(3, 16, 16);
  b.vertex(kVerts, vt).prim(6, 2).finish();
  exec(g, b);
  // texel (3, 5) is red = 48, green = 80
  CHECK(px32(3, 5) == (0xFF000000u | 48 | (80 << 8)));
  CHECK(px32(15, 15) == (0xFF000000u | 240 | (240 << 8)));
}

static void test_texture_clut4_swizzled() {
  clear_vram();
  ge::Gpu g;
  // 32x8 texture, 4 bit indices. Swizzle blocks are 16 bytes x 8 rows, so one 16 byte block covers 32 texels.
  const int w = 32, h = 8;
  uint8_t lin[16 * 8];
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t idx = static_cast<uint8_t>((x + y) & 15);
      uint8_t& dst = lin[y * 16 + x / 2];
      dst = (x & 1) ? static_cast<uint8_t>((dst & 0x0F) | (idx << 4)) : static_cast<uint8_t>((dst & 0xF0) | idx);
    }
  for (int y = 0; y < h; ++y)
    std::memcpy(ptr(kTex + y * 16), &lin[y * 16], 16);  // block layout equals linear for a single 16x8 byte block
  // palette: index i -> (i*16, 0, 255-i*16)
  for (int i = 0; i < 16; ++i) sw(kTex + 0x1000 + i * 4, 0xFF000000 | ((255 - i * 16) << 16) | (i * 16));
  uint32_t vt = 0x800000 | (7 << 2) | (3 << 7) | 3;
  struct TV {
    float u, v;
    uint32_t col;
    float x, y, z;
  };
  TV tv[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0}, {32, 8, 0xFFFFFFFF, 32, 8, 0}};
  std::memcpy(ptr(kVerts), tv, sizeof tv);
  Builder b;
  b.setup();
  b.add(cmd(0x1E, 1)).add(cmd(0xA0, kTex & 0xFFFFFF)).add(cmd(0xA8, ((kTex >> 8) & 0xFF0000) | 32));
  b.add(cmd(0xB8, 5 | (3 << 8))).add(cmd(0xC3, 4)).add(cmd(0xC2, 1)).add(cmd(0xC9, 3 | (1 << 8)));
  b.add(cmd(0xB0, (kTex + 0x1000) & 0xFFFFFF)).add(cmd(0xB1, ((kTex + 0x1000) >> 8) & 0xFF0000));
  b.add(cmd(0xC5, 3 | (15 << 8))).add(cmd(0xC4, 2));  // 32 bit entries, mask 15, 16 entries
  b.vertex(kVerts, vt).prim(6, 2).finish();
  exec(g, b);
  for (int x : {0, 5, 20, 31}) {
    int y = 3;
    int idx = (x + y) & 15;
    CHECK(px32(x, y) == (0xFF000000u | ((255 - idx * 16) << 16) | (idx * 16)));
  }
}

static void test_texture_dxt1() {
  clear_vram();
  ge::Gpu g;
  // one block: color0 = pure red (565), color1 = pure blue, indices 0,1,2,3 across each row
  uint16_t c0 = 31, c1 = 31 << 11;
  uint8_t blk[8];
  std::memcpy(blk, &c0, 2);
  std::memcpy(blk + 2, &c1, 2);
  for (int i = 4; i < 8; ++i) blk[i] = 0xE4;  // 3 2 1 0 packed as 11 10 01 00
  mem_write(kTex, blk, 8);
  uint32_t vt = 0x800000 | (7 << 2) | (3 << 7) | 3;
  struct TV {
    float u, v;
    uint32_t col;
    float x, y, z;
  };
  TV tv[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0}, {4, 4, 0xFFFFFFFF, 4, 4, 0}};
  std::memcpy(ptr(kVerts), tv, sizeof tv);
  Builder b;
  b.setup();
  b.add(cmd(0x1E, 1)).add(cmd(0xA0, kTex & 0xFFFFFF)).add(cmd(0xA8, ((kTex >> 8) & 0xFF0000) | 4));
  b.add(cmd(0xB8, 2 | (2 << 8))).add(cmd(0xC3, 8)).add(cmd(0xC9, 3 | (1 << 8)));
  b.vertex(kVerts, vt).prim(6, 2).finish();
  exec(g, b);
  CHECK((px32(0, 0) & 0x00FFFFFF) == 0x0000FF);  // index 0: red
  CHECK((px32(1, 0) & 0x00FFFFFF) == 0xFF0000);  // index 1: blue
  uint32_t mid = px32(2, 0);
  CHECK((mid & 255) > 100 && (mid & 255) < 160 && ((mid >> 16) & 255) > 100 && ((mid >> 16) & 255) < 160);  // 3-colour block midpoint
}

static void test_transform_and_cull() {
  clear_vram();
  ge::Gpu g;
  // vertex type: float position only
  const uint32_t vt = 3 << 7;
  float tri[9] = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, 0};
  std::memcpy(ptr(kVerts), tri, sizeof tri);
  auto build = [&](int cullMode, bool cull) {
    Builder b;
    b.setup();
    // identity matrices are the reset state; the projection only needs a viewport
    b.f(0x42, 240).f(0x43, -136).f(0x44, 32767.5f).f(0x45, 2048).f(0x46, 2048).f(0x47, 32767.5f);
    b.add(cmd(0x4C, (2048 - 240) << 4)).add(cmd(0x4D, (2048 - 136) << 4));
    b.add(cmd(0x55, 0xFFFFFF)).add(cmd(0x58, 0xFF));  // untextured, no vertex colour -> material
    b.add(cmd(0x1D, cull)).add(cmd(0x9B, cullMode));
    b.vertex(kVerts, vt).prim(3, 3).finish();
    return b;
  };
  Builder b = build(0, false);
  exec(g, b);
  CHECK(px32(240, 136) == 0xFFFFFFFF);
  CHECK(px32(240, 60) == 0);  // above the apex

  // the same triangle is front facing in exactly one of the two culling modes
  int drawn = 0;
  for (int mode = 0; mode < 2; ++mode) {
    clear_vram();
    Builder c = build(mode, true);
    exec(g, c);
    if (px32(240, 136) != 0) ++drawn;
  }
  CHECK(drawn == 1);
}

static void test_projection_clip() {
  clear_vram();
  ge::Gpu g;
  const uint32_t vt = 3 << 7;
  // triangle that crosses the near plane (z + w < 0 for one vertex) must not blow up
  float tri[9] = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, -3.0f};
  std::memcpy(ptr(kVerts), tri, sizeof tri);
  Builder b;
  b.setup();
  b.f(0x42, 240).f(0x43, -136).f(0x44, 32767.5f).f(0x45, 2048).f(0x46, 2048).f(0x47, 32767.5f);
  b.add(cmd(0x4C, (2048 - 240) << 4)).add(cmd(0x4D, (2048 - 136) << 4));
  b.add(cmd(0x1C, 1)).add(cmd(0x55, 0xFFFFFF)).add(cmd(0x58, 0xFF));
  b.vertex(kVerts, vt).prim(3, 3).finish();
  exec(g, b);
  CHECK(px32(240, 220) == 0 || px32(240, 220) == 0xFFFFFFFF);  // no crash is the point
}

static void test_transfer() {
  clear_vram();
  ge::Gpu g;
  for (int i = 0; i < 16 * 16; ++i) sw(kTex + i * 4, 0xFF000000 | i);
  Builder b;
  b.add(cmd(0xB2, kTex & 0xFFFFFF)).add(cmd(0xB3, ((kTex >> 8) & 0xFF0000) | 16));
  b.add(cmd(0xB4, 0)).add(cmd(0xB5, 0x040000 | 512));
  b.add(cmd(0xEB, 0)).add(cmd(0xEC, 10 | (20 << 10))).add(cmd(0xEE, 15 | (15 << 10)));
  b.add(cmd(0xEA, 1));
  b.finish();
  exec(g, b);
  CHECK(px32(10, 20) == 0xFF000000);
  CHECK(px32(11, 21) == (0xFF000000u | 17));
  CHECK(px32(25, 35) == (0xFF000000u | 255));
}

static void test_list_control() {
  ge::Gpu g;
  ge::ListContext l;
  NullHost h;
  // main list: CALL sub; SIGNAL; END; JUMP away over junk; FINISH; END
  uint32_t sub = kList + 0x100;
  uint32_t tail = kList + 0x200;
  Builder sb;
  sb.add(cmd(0x55, 0x112233)).add(cmd(0x0B));
  sb.write(sub);
  Builder tb;
  tb.add(cmd(0x58, 0x44)).add(cmd(0x0F, 0x1234)).add(cmd(0x0C));
  tb.write(tail);
  Builder b;
  b.add(cmd(0x10, 0x080000)).add(cmd(0x0A, sub & 0xFFFFFF));
  b.add(cmd(0x0E, 0x00020007)).add(cmd(0x0C));  // signal 7 with the "continue" behaviour
  b.add(cmd(0x08, tail & 0xFFFFFF));
  b.add(cmd(0x55, 0xFFFFFF));  // skipped by the jump
  b.write();
  l.pc = kList;
  ge::Stop s = run(g, l, &h);
  CHECK(s == ge::Stop::Ended);
  CHECK(h.signals == 1 && (h.lastSignal & 0xFFFF) == 7 && (h.lastSignal >> 16) == 2);
  CHECK(h.finishes == 1 && h.lastFinish == 0x1234);
  CHECK(g.cmd(0x55) == 0x112233);
  CHECK(g.cmd(0x58) == 0x44);
}

static void test_stall_and_base() {
  ge::Gpu g;
  NullHost h;
  Builder b;
  b.add(cmd(0x55, 0xABCDEF)).add(cmd(0x58, 0x33)).add(cmd(0x0F)).add(cmd(0x0C));
  b.write();
  ge::ListContext l;
  l.pc = kList;
  l.stall = kList + 4;  // only the first command is visible so far
  CHECK(run(g, l, &h) == ge::Stop::Stalled);
  CHECK(g.cmd(0x55) == 0xABCDEF && g.cmd(0x58) == 0);
  l.stall = 0;
  CHECK(run(g, l, &h) == ge::Stop::Ended);
  CHECK(g.cmd(0x58) == 0x33 && h.finishes == 1);

  // pause behaviour stops the list until it is resumed
  Builder p;
  p.add(cmd(0x0E, 0x00030001)).add(cmd(0x0C)).add(cmd(0x58, 0x77)).add(cmd(0x0F)).add(cmd(0x0C));
  p.write();
  l = ge::ListContext();
  l.pc = kList;
  CHECK(run(g, l, &h) == ge::Stop::Paused);
  CHECK(g.cmd(0x58) == 0x33);
  CHECK(run(g, l, &h) == ge::Stop::Ended);
  CHECK(g.cmd(0x58) == 0x77);
}

static void test_matrix_and_context() {
  ge::Gpu g;
  g.exec_word(cmd(0x3E, 0));
  for (int i = 0; i < 16; ++i) g.exec_word(cmdf(0x3F, i == 0 || i == 5 || i == 10 || i == 15 ? 2.0f : 0.0f));
  float m[16];
  CHECK(g.get_matrix(10, m));
  CHECK(m[0] == 2.0f && m[5] == 2.0f && m[1] == 0.0f);
  g.exec_word(cmd(0x55, 0x778899));
  uint32_t ctx[ge::Gpu::kContextWords];
  g.save_context(ctx);
  ge::Gpu h;
  h.restore_context(ctx);
  CHECK(h.cmd(0x55) == 0x778899);
  float m2[16];
  h.get_matrix(10, m2);
  CHECK(m2[10] == 2.0f);
}

static void test_bezier() {
  clear_vram();
  ge::Gpu g;
  // flat 4x4 patch covering a square, through-less transform with the viewport mapping used above
  const uint32_t vt = 3 << 7;
  float pts[16 * 3];
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i) {
      pts[(j * 4 + i) * 3 + 0] = (-0.5f + i / 3.0f) * 0.5f;
      pts[(j * 4 + i) * 3 + 1] = (-0.5f + j / 3.0f) * 0.5f;
      pts[(j * 4 + i) * 3 + 2] = 0;
    }
  std::memcpy(ptr(kVerts), pts, sizeof pts);
  Builder b;
  b.setup();
  b.f(0x42, 240).f(0x43, -136).f(0x44, 32767.5f).f(0x45, 2048).f(0x46, 2048).f(0x47, 32767.5f);
  b.add(cmd(0x4C, (2048 - 240) << 4)).add(cmd(0x4D, (2048 - 136) << 4));
  b.add(cmd(0x55, 0xFFFFFF)).add(cmd(0x58, 0xFF)).add(cmd(0x36, 4 | (4 << 8)));
  b.vertex(kVerts, vt).add(cmd(0x05, 4 | (4 << 8))).finish();
  exec(g, b);
  CHECK(px32(240, 136) == 0xFFFFFFFF);
  CHECK(px32(240 + 40, 136) == 0xFFFFFFFF);
  CHECK(px32(240 + 100, 136) == 0);
}

static void test_indexed_strip() {
  clear_vram();
  ge::Gpu g;
  put_verts(kVerts, {{0xFF0000FF, 0, 0, 0}, {0xFF0000FF, 20, 0, 0}, {0xFF0000FF, 0, 20, 0}, {0xFF0000FF, 20, 20, 0}});
  uint16_t idx[4] = {0, 1, 2, 3};
  std::memcpy(ptr(kVerts + 0x100), idx, sizeof idx);
  Builder b;
  b.setup().add(cmd(0x02, (kVerts + 0x100) & 0xFFFFFF)).vertex(kVerts, kThroughCol | (2 << 11)).prim(4, 4).finish();
  exec(g, b);
  CHECK(px32(3, 3) == 0xFF0000FF);
  CHECK(px32(17, 17) == 0xFF0000FF);
  CHECK(px32(25, 25) == 0);
}

// ---- sceGe_user through the import dispatcher ----

enum GeImport : uint32_t { EnQueue, ListSync, DrawSync, GetCmd, SetCb, kGeCount };
static const ImportEntry kGeImports[kGeCount] = {
    {0x08A00000, "sceGe_user", 0xAB49E76A},
    {0x08A00008, "sceGe_user", 0x03444EB4},
    {0x08A00010, "sceGe_user", 0xB287BD61},
    {0x08A00018, "sceGe_user", 0xDC93CFEF},
    {0x08A00020, "sceGe_user", 0xA4FC06A4},
};

static uint32_t call_import(GeImport i, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
  Cpu c;
  c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[7] = a3;
  hle(c, i);
  return c.r[2];
}

static void test_syscalls() {
  clear_vram();
  KernelConfig cfg;
  cfg.headless = true;
  kernel_init(cfg, 0x08B00000, 0);
  register_all_hle();
  bind_tables(nullptr, 0, kGeImports, kGeCount);

  put_verts(kVerts, {{0xFFFFFFFF, 0, 0, 0}, {0xFFFFFFFF, 8, 8, 0}});
  Builder b;
  b.setup().vertex(kVerts, kThroughCol).prim(6, 2).add(cmd(0x0F, 5)).add(cmd(0x0C));
  b.write();
  uint32_t id = call_import(EnQueue, kList, 0, 0, 0);
  CHECK(static_cast<int32_t>(id) >= 0);
  CHECK(call_import(DrawSync, 1) == 0);  // already drawn
  CHECK(px32(4, 4) == 0xFFFFFFFF);
  CHECK(call_import(ListSync, id, 0) == 0);
  CHECK(call_import(GetCmd, 0x9D) == ((0x9Du << 24) | 0x040200));
  CHECK(call_import(ListSync, id, 0) == 0x80000100);  // synced lists are gone

  // a stalled list reports stalling until the stall address moves
  Builder s;
  s.setup().add(cmd(0x0F)).add(cmd(0x0C));
  s.write();
  uint32_t stalled = call_import(EnQueue, kList, kList + 4, 0, 0);
  CHECK(call_import(ListSync, stalled, 1) == 3);
  CHECK(call_import(DrawSync, 1) == 3);
}

// Renders the same random scene with the span rasteriser and with the reference path and compares VRAM.
static void render_scene(ge::Gpu& g, int variant, uint32_t seed) {
  struct TV {
    float u, v;
    uint32_t col;
    float x, y, z;
  };
  uint32_t s = seed;
  auto rnd = [&]() {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) & 0xFFFF;
  };
  const int n = 60;
  std::vector<TV> tv(n * 3);
  for (int t = 0; t < n; ++t) {
    float cx = rnd() % 480, cy = rnd() % 272, sz = 4 + rnd() % 70;
    float z = 1000 + rnd() % 60000;
    for (int k = 0; k < 3; ++k) {
      TV& v = tv[t * 3 + k];
      v.x = cx + (static_cast<int>(rnd() % 1000) - 500) / 1000.0f * sz;
      v.y = cy + (static_cast<int>(rnd() % 1000) - 500) / 1000.0f * sz;
      v.z = z + k * 3;
      v.u = (rnd() % 1500) / 1000.0f;
      v.v = (rnd() % 1500) / 1000.0f;
      v.col = (variant & 1) ? (0x80000000u | rnd() | (rnd() << 16)) : (0xFF000000u | rnd() | (rnd() << 16));
      if (variant & 8) v.col = (v.col & 0xFF000000u) | 0x00FFFFFF;
    }
  }
  std::memcpy(ptr(kVerts), tv.data(), tv.size() * sizeof(TV));
  const int fmt = (variant & 16) ? 1 : 3;
  Builder b;
  b.setup(fmt);
  b.add(cmd(0x9E, 0x088000)).add(cmd(0x9F, 0x040000 | 512));
  b.add(cmd(0xD3, 0x401)).add(cmd(0xE7, 0));
  b.add(cmd(0xD3, 0));
  if (variant & 2) {
    b.add(cmd(0x23, 1)).add(cmd(0xDE, 4));
    b.add(cmd(0xE7, 0));
  }
  if (variant & 1) b.add(cmd(0x21, 1)).add(cmd(0xDF, 2 | (3 << 4)));
  if (variant & 4) {
    b.add(cmd(0x1E, 1)).add(cmd(0xA0, kTex & 0xFFFFFF)).add(cmd(0xA8, ((kTex >> 8) & 0xFF0000) | 16));
    b.add(cmd(0xB8, 4 | (4 << 8))).add(cmd(0xC3, 3)).add(cmd(0xC9, (variant & 32) ? 0 : 1 | (1 << 8)));
    if (variant & 64) b.add(cmd(0xC6, 1 | (1 << 8)));
  }
  b.vertex(kVerts, 0x800000 | (7 << 2) | (3 << 7) | 3).prim(3, n * 3).finish();
  exec(g, b);
}

static void test_fast_matches_reference() {
  write_tex_8888(kTex, 16, 16);
  int bad = 0;
  for (int variant = 0; variant < 128; ++variant) {
    std::vector<uint8_t> out[2];
    for (int m = 0; m < 2; ++m) {
      ge::fast::set_mode(m ? ge::fast::Mode::Reference : ge::fast::Mode::Fast);
      clear_vram();
      ge::Gpu g;
      render_scene(g, variant, 1234 + variant);
      out[m].assign(ptr(kVram), ptr(kVram) + 0x90000);
    }
    // The span path snaps vertices to 1/16 pixel like the hardware, the reference works in floats, so edge
    // pixels may differ and colours may be off by a rounding step. Anything beyond that is a bug.
    const bool is565 = (variant & 16) != 0;
    const int bpp = is565 ? 2 : 4;
    size_t cover = 0, colour = 0, total = 0;
    int worst = 0;
    for (int i = 0; i < 512 * 272; ++i) {
      uint32_t p[2] = {0, 0};
      for (int m = 0; m < 2; ++m) std::memcpy(&p[m], &out[m][i * bpp], bpp);
      if (p[0] || p[1]) ++total;
      if (p[0] == p[1]) continue;
      if (!p[0] || !p[1]) {
        ++cover;
        continue;
      }
      int d = 0;
      if (is565) {
        for (int sh : {0, 5, 10}) d = std::max(d, std::abs(static_cast<int>((p[0] >> sh) & 31) - static_cast<int>((p[1] >> sh) & 31)) * 8);
      } else {
        for (int sh : {0, 8, 16, 24}) d = std::max(d, std::abs(static_cast<int>((p[0] >> sh) & 255) - static_cast<int>((p[1] >> sh) & 255)));
      }
      worst = std::max(worst, d);
      if (d > 8) ++colour;
    }
    if (colour * 100 > total * 3 || cover * 100 > total * 2) {
      std::printf("variant %d: %zu of %zu pixels differ in coverage, %zu in colour (worst %d)\n", variant, cover, total, colour, worst);
      ++bad;
    }
  }
  ge::fast::set_mode(ge::fast::Mode::Fast);
  CHECK(bad == 0);
}

static uint32_t host_pixel(const std::shared_ptr<ge::surface::Plane>& p, int x, int y) {
  uint32_t pixel = 0;
  if (p) std::memcpy(&pixel, p->pixels.data() + (size_t(y) * p->stride * p->scale + x) * p->bpp, p->bpp);
  return pixel;
}

static void tiny_triangle(ge::Gpu& g, bool depth = false) {
  put_verts(kVerts, {{0xFFFFFFFF, 10, 10, 100}, {0xFFFFFFFF, 10.75f, 10, 100},
                    {0xFFFFFFFF, 10, 10.75f, 100}});
  Builder b;
  b.setup();
  if (depth)
    b.add(cmd(0x9E, 0x100000)).add(cmd(0x9F, 0x040000 | 512)).add(cmd(0x23, 1))
     .add(cmd(0xDE, 1)).add(cmd(0xE7, 0));
  b.vertex(kVerts, kThroughCol).prim(3, 3).finish();
  exec(g, b);
}

static void test_internal_resolution() {
  namespace s = ge::surface;
  ge::fast::set_stats_sink(nullptr, nullptr);
  for (auto mode : {ge::fast::Mode::Fast, ge::fast::Mode::Reference}) {
    ge::fast::set_mode(mode);
    s::set_scale(2);
    clear_vram();
    std::memset(ptr(kVram + 0x100000), 0xFF, 512 * 272 * 2);
    ge::Gpu g;
    tiny_triangle(g, true);
    auto colour = s::display(kVram, 512, 3);
    CHECK(colour && host_pixel(colour, 20, 20) == 0xFFFFFFFF);
    CHECK(host_pixel(colour, 21, 20) == 0);
    CHECK(host_pixel(colour, 20, 21) == 0);
    {
      s::NativeScope read;
      CHECK(px32(10, 10) == 0);  // the finer sample exists even though the entire native pixel is empty
    }
    // Independent high-resolution depth must occlude a farther primitive at that subpixel only.
    put_verts(kVerts, {{0xFF0000FF, 9, 9, 200}, {0xFF0000FF, 12, 12, 200}});
    Builder b;
    b.setup().add(cmd(0x9E, 0x100000)).add(cmd(0x9F, 0x040000 | 512)).add(cmd(0x23, 1))
     .add(cmd(0xDE, 4)).add(cmd(0xE7, 0));
    b.vertex(kVerts, kThroughCol).prim(6, 2).finish();
    exec(g, b);
    colour = s::display(kVram, 512, 3);
    CHECK(host_pixel(colour, 20, 20) == 0xFFFFFFFF);
    CHECK(host_pixel(colour, 21, 20) == 0xFF0000FF);
    {
      s::NativeScope read;
      CHECK(px32(10, 10) == 0xFF0000FF);
    }
    // A CPU readback is conservative: native authority is never replaced by a downsample.
    CHECK(px32(10, 10) == 0xFF0000FF);
    colour = s::display(kVram, 512, 3);
    CHECK(host_pixel(colour, 20, 20) == 0xFF0000FF);
    // A same-value CPU store through an uncached alias must also invalidate the old host detail.
    s::reset();
    clear_vram();
    tiny_triangle(g);
    sw(kVram | 0x40000000, 0);
    colour = s::display(kVram, 512, 3);
    CHECK(host_pixel(colour, 20, 20) == 0);
    // Copying identical native bytes must still destroy old host samples at the destination.
    s::reset();
    clear_vram();
    tiny_triangle(g);
    sw(kTex, 0);
    Builder copy;
    copy.add(cmd(0xB2, kTex & 0xFFFFFF)).add(cmd(0xB3, 0x080000 | 1))
        .add(cmd(0xB4, 0)).add(cmd(0xB5, 0x040000 | 512))
        .add(cmd(0xEB, 0)).add(cmd(0xEC, (10 << 10) | 10)).add(cmd(0xEE, 0))
        .add(cmd(0xEA, 1)).finish();
    exec(g, copy);
    colour = s::display(kVram, 512, 3);
    CHECK(host_pixel(colour, 20, 20) == 0);
    // Changed movie/HLE pixels via a retained pointer are detected even without a fresh ptr() call.
    uint8_t* retained;
    { s::NativeScope read; retained = ptr(kVram); }
    uint32_t moviePixel = 0xFF2468AC;
    std::memcpy(retained + (10 * 512 + 10) * 4, &moviePixel, 4);
    colour = s::display(kVram, 512, 3);
    CHECK(host_pixel(colour, 20, 20) == moviePixel && host_pixel(colour, 21, 21) == moviePixel);
    s::reset();
    clear_vram();
    tiny_triangle(g);
    struct TV { float u, v; uint32_t colour; float x, y, z; };
    TV tv[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0}, {16, 16, 0xFFFFFFFF, 16, 16, 0}};
    std::memcpy(ptr(kVerts), tv, sizeof tv);
    Builder feedback;
    feedback.setup().add(cmd(0x9C, 0x88000)).add(cmd(0x1E, 1))
            .add(cmd(0xA0, 0)).add(cmd(0xA8, 0x040000 | 512)).add(cmd(0xB8, 4 | (4 << 8)))
            .add(cmd(0xC3, 3)).add(cmd(0xC9, 3 | (1 << 8)))
            .vertex(kVerts, kThroughCol | 3).prim(6, 2).finish();
    exec(g, feedback);
    auto other = s::display(kVram + 0x88000, 512, 3);
    CHECK(other && host_pixel(other, 20, 20) == 0);  // native texture feedback, not the white host-only subpixel
    {
      s::NativeScope read;
      CHECK(lw(kVram + 0x88000 + (10 * 512 + 10) * 4) == 0);
    }
    // Same-target feedback cannot replay safely after the native draw; it explicitly reseeds.
    feedback.w.clear();
    feedback.setup().add(cmd(0x1E, 1)).add(cmd(0xA0, 0)).add(cmd(0xA8, 0x040000 | 512))
            .add(cmd(0xB8, 4 | (4 << 8))).add(cmd(0xC3, 3)).add(cmd(0xC9, 3 | (1 << 8)))
            .vertex(kVerts, kThroughCol | 3).prim(6, 2).finish();
    exec(g, feedback);
    colour = s::display(kVram, 512, 3);
    CHECK(host_pixel(colour, 20, 20) == 0);
    s::reset();
    clear_vram();
    tiny_triangle(g);
    TV subBlock[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0}, {1, 1, 0xFFFFFFFF, 1, 1, 0}};
    std::memcpy(ptr(kVerts), subBlock, sizeof subBlock);
    Builder dxtFeedback;
    dxtFeedback.setup().add(cmd(0x1E, 1)).add(cmd(0xA0, 0)).add(cmd(0xA8, 0x040000 | 1))
               .add(cmd(0xB8, 0)).add(cmd(0xC3, 8)).add(cmd(0xC9, 3 | (1 << 8)))
               .vertex(kVerts, kThroughCol | 3).prim(6, 2).finish();
    exec(g, dxtFeedback);
    colour = s::display(kVram, 512, 3);
    CHECK(host_pixel(colour, 20, 20) == 0);  // 1x1 DXT still reads a whole block: self-feedback fallback
    for (int format : {0, 1, 2}) {
      s::reset();
      clear_vram();
      put_verts(kVerts, {{0xFFFFFFFF, 10, 10, 0}, {0xFFFFFFFF, 10.75f, 10, 0},
                        {0xFFFFFFFF, 10, 10.75f, 0}});
      Builder packed;
      packed.setup(format).add(cmd(0x1E, 0)).add(cmd(0x23, 0))
            .vertex(kVerts, kThroughCol).prim(3, 3).finish();
      exec(g, packed);
      auto packedColour = s::display(kVram, 512, format);
      CHECK(packedColour && host_pixel(packedColour, 20, 20) == 0xFFFF);
      CHECK(host_pixel(packedColour, 21, 20) == 0);
      {
        s::NativeScope read;
        CHECK(px16(10, 10) == 0);
      }
      // A different packed-format view invalidates old detail even when native pixels stay zero.
      tiny_triangle(g);
      packedColour = s::display(kVram, 512, format);
      CHECK(host_pixel(packedColour, 20, 20) == 0);
    }
  }
  // The host pass may not change ANY guest colour/depth bytes, in either raster mode.
  for (auto mode : {ge::fast::Mode::Fast, ge::fast::Mode::Reference}) {
    ge::fast::set_mode(mode);
    write_tex_8888(kTex, 16, 16);
    for (int variant : {0, 1, 2, 4, 7, 15, 16, 31, 64, 127}) {
      std::vector<uint8_t> output[2];
      for (int scale = 1; scale <= 2; ++scale) {
        s::set_scale(scale);
        clear_vram();
        ge::Gpu g;
        render_scene(g, variant, 5678 + variant);
        s::NativeScope read;
        output[scale - 1].assign(ptr(kVram), ptr(kVram) + kVramSize);
      }
      CHECK(output[0] == output[1]);
    }
  }
  s::set_scale(2);
  CHECK(!s::acquire(kVram, 0, 3, 272));
  CHECK(!s::acquire(kVram, UINT32_MAX, 3, 272));
  CHECK(!s::acquire(kVram, 512, 3, INT32_MAX));
  CHECK(!s::acquire(kRamBase, 512, 3, 272));
  CHECK(!s::acquire(kVram + kVramSize - 1, 512, 3, 272));
  CHECK(!s::overlaps(kVram + 1, 0, kVram, 1024));
  for (int i = 0; i < 16; ++i) {
    auto p = s::acquire(kVram + i * 64, 512, 3, 272);
    CHECK(p != nullptr);
  }
  CHECK(s::allocated_bytes() <= s::budget_bytes());
  bool invalid = false;
  try { s::set_scale(9); } catch (const std::invalid_argument&) { invalid = true; }
  CHECK(invalid && s::scale() == 2);
  s::set_scale(1);
  CHECK(s::allocated_bytes() == 0 && !s::display(kVram, 512, 3));
  ge::fast::set_mode(ge::fast::Mode::Fast);
}

static void test_video_settings() {
  for (int scale = 1; scale <= 8; ++scale) CHECK(video::render_scale(std::to_string(scale)) == scale);
  CHECK(video::window_size("1920x1088").width == 1920);
  auto fitted = video::fit({1000, 1000}, {960, 544});
  CHECK(fitted.width == 1000 && fitted.height == 566);
  fitted = video::fit({1000, 500}, {480, 272});
  CHECK(fitted.width == 882 && fitted.height == 500);
  CHECK(video::fit({0, 0}, {480, 272}).width == 0);
  CHECK(video::fit({1000, 1000}, {3840, 2176}).height == 566);
  for (const char* value : {"0", "-1", "9", "2x", "08", "999999999999"}) {
    bool invalid = false;
    try { video::render_scale(value); } catch (const std::invalid_argument&) { invalid = true; }
    CHECK(invalid);
  }
  for (const char* value : {"0x0", "-1x272", "960x544junk", "999999999999x99999999"}) {
    bool invalid = false;
    try { video::window_size(value); } catch (const std::invalid_argument&) { invalid = true; }
    CHECK(invalid);
  }
}

static void test_all_resolution_scales() {
  namespace s = ge::surface;
  for (auto mode : {ge::fast::Mode::Fast, ge::fast::Mode::Reference}) {
    ge::fast::set_mode(mode);
    std::vector<uint8_t> nativeColour[4], nativeDepth[4];
    for (int scale = 1; scale <= 8; ++scale) {
      for (int format = 0; format <= 3; ++format) {
        s::set_scale(scale);
        clear_vram();
        std::memset(ptr(kVram + 0x100000), 0xFF, 16 * 16 * 2);
        ge::Gpu g;
        auto setup = [&](Builder& b) -> Builder& {
          return b.setup(format).add(cmd(0x9D, 0x040000 | 16)).add(cmd(0xD5, (15 << 10) | 15))
                  .add(cmd(0x9E, 0x100000)).add(cmd(0x9F, 0x040000 | 16))
                  .add(cmd(0x23, 1)).add(cmd(0xDE, 4)).add(cmd(0xE7, 0));
        };
        put_verts(kVerts, {{0xFFFFFFFF, 2, 2, 100}, {0xFFFFFFFF, 2.75f, 2, 100},
                          {0xFFFFFFFF, 2, 2.75f, 100}});
        Builder tiny;
        setup(tiny).vertex(kVerts, kThroughCol).prim(3, 3).finish();
        exec(g, tiny);
        {
          s::NativeScope read;
          CHECK(lw(kVram + (2 * 16 + 2) * (format == 3 ? 4 : 2)) == 0);
        }
        auto colour = s::acquire(kVram, 16, format, 16);
        if (scale == 1) {
          CHECK(!colour && s::allocated_bytes() == 0);
          put_verts(kVerts, {{0xFF0000FF, 1, 1, 200}, {0xFF0000FF, 4, 4, 200}});
          Builder farther;
          setup(farther).vertex(kVerts, kThroughCol).prim(6, 2).finish();
          exec(g, farther);
          nativeColour[format].assign(ptr(kVram), ptr(kVram) + 16 * 16 * (format == 3 ? 4 : 2));
          nativeDepth[format].assign(ptr(kVram + 0x100000), ptr(kVram + 0x100000) + 16 * 16 * 2);
          continue;
        }
        const uint32_t white = format == 3 ? UINT32_MAX : 0xFFFF;
        CHECK(colour && colour->scale == scale);
        CHECK(host_pixel(colour, 2 * scale, 2 * scale) == white);
        CHECK(host_pixel(colour, 3 * scale - 1, 3 * scale - 1) == 0);
        auto depth = s::acquire(kVram + 0x100000, 16, -1, 16);
        CHECK(host_pixel(depth, 2 * scale, 2 * scale) == 100);
        put_verts(kVerts, {{0xFF0000FF, 1, 1, 200}, {0xFF0000FF, 4, 4, 200}});
        Builder farther;
        setup(farther).vertex(kVerts, kThroughCol).prim(6, 2).finish();
        exec(g, farther);
        CHECK(host_pixel(colour, 2 * scale, 2 * scale) == white);
        CHECK(host_pixel(colour, 3 * scale - 1, 3 * scale - 1) != white);
        CHECK(host_pixel(depth, 2 * scale, 2 * scale) == 100);
        {
          s::NativeScope read;
          CHECK(std::memcmp(ptr(kVram), nativeColour[format].data(), nativeColour[format].size()) == 0);
          CHECK(std::memcmp(ptr(kVram + 0x100000), nativeDepth[format].data(), nativeDepth[format].size()) == 0);
        }
        // Same-value native writes still invalidate high-frequency detail at every scale/format.
        sw(kVram | 0x40000000, 0);
        colour->refresh();
        CHECK(host_pixel(colour, 2 * scale, 2 * scale) != white);
        CHECK(s::allocated_bytes() <= s::budget_bytes());
        CHECK(s::budget_bytes() <= (128u << 20));
      }
      if (scale > 1) {
        s::reset();
        std::vector<std::shared_ptr<s::Plane>> pinned;
        for (int i = 0; i < 8; ++i) pinned.push_back(s::acquire(kVram + i * 1024, 16, 3, 16));
        bool exhausted = false;
        try { s::acquire(kVram + 8192, 16, 3, 16); } catch (const std::runtime_error&) { exhausted = true; }
        CHECK(exhausted);
        pinned.clear();
        CHECK(s::acquire(kVram + 8192, 16, 3, 16) != nullptr);
      }
    }
  }
  s::set_scale(1);
  ge::fast::set_mode(ge::fast::Mode::Fast);
}

static void test_hardware_integration(ge::hardware::Backend backend) {
  namespace s = ge::surface;
  using Test = void (*)();
  const Test cases[] = {test_clear_and_sprite, test_triangle_and_scissor, test_gouraud,
    test_565, test_depth, test_blend, test_alpha_test, test_stencil, test_texture_8888,
    test_texture_clut4_swizzled, test_texture_dxt1, test_transform_and_cull, test_projection_clip,
    test_bezier, test_indexed_strip};
  struct Image { std::vector<uint8_t> native, host; };
  std::vector<Image> reference;
  ge::fast::set_mode(ge::fast::Mode::Reference);
  for (int pass = 0; pass < 2; ++pass) {
    ge::hardware::select(pass ? backend : ge::hardware::Backend::Software);
    for (size_t i = 0; i < std::size(cases); ++i) {
      std::fprintf(stderr, "hardware GE pass=%d fixture=%zu\n", pass, i);
      s::set_scale(2); s::reset();
      s::NativeScope internal;
      cases[i]();
      auto plane = s::display(kVram, 512, i == 3 ? 0 : 3);
      Image image;
      if (plane) image.host = plane->pixels;
      image.native.assign(ptr(kVram), ptr(kVram) + 2 * 512 * 272 * 4);
      if (!pass) { reference.push_back(std::move(image)); continue; }
      CHECK(image.native == reference[i].native);
      CHECK(image.host.size() == reference[i].host.size());
      if (!plane || image.host.size() != reference[i].host.size()) continue;
      size_t bad = 0, covered = 0;
      const int bpp = plane->bpp;
      for (size_t pixel = 0; pixel < image.host.size() / bpp; ++pixel) {
        uint32_t a = 0, b = 0;
        std::memcpy(&a, image.host.data() + pixel * bpp, bpp);
        std::memcpy(&b, reference[i].host.data() + pixel * bpp, bpp);
        if (a || b) ++covered;
        int delta = 0;
        if (bpp == 2) {
          for (int shift : {0, 5, 11})
            delta = std::max(delta, std::abs(int((a >> shift) & (shift == 5 ? 63 : 31)) -
                                           int((b >> shift) & (shift == 5 ? 63 : 31))) * 8);
        } else {
          for (int shift : {0, 8, 16, 24})
            delta = std::max(delta, std::abs(int((a >> shift) & 255) - int((b >> shift) & 255)));
        }
        if (delta > 8) ++bad;
      }
      // Floating GPU interpolation may differ at edges/quantization boundaries, never in guest VRAM.
      if (bad * 100 > std::max<size_t>(1, covered) * 2) {
        std::printf("hardware fixture %zu: %zu / %zu pixels exceed 8-channel tolerance\n", i, bad, covered);
        ++g_failures;
      }
    }
  }
  CHECK(ge::hardware::counters().submissions > 0);
  test_internal_resolution();
  ge::hardware::reset(); s::set_scale(1);
}

int main(int argc, char** argv) {
  init_memory();
  if (argc == 2) {
    test_hardware_integration(ge::hardware::parse(argv[1]));
    std::printf("hardware GE integration: %d failures\n", g_failures);
    return g_failures ? 1 : 0;
  }
  test_clear_and_sprite();
  test_triangle_and_scissor();
  test_gouraud();
  test_565();
  test_depth();
  test_blend();
  test_alpha_test();
  test_stencil();
  test_texture_8888();
  test_texture_clut4_swizzled();
  test_texture_dxt1();
  test_transform_and_cull();
  test_projection_clip();
  test_transfer();
  test_list_control();
  test_stall_and_base();
  test_matrix_and_context();
  test_bezier();
  test_indexed_strip();
  test_syscalls();
  test_fast_matches_reference();
  test_internal_resolution();
  test_all_resolution_scales();
  test_video_settings();
  ge::surface::set_scale(2);
  test_clear_and_sprite();
  test_565();
  test_depth();
  test_blend();
  test_alpha_test();
  test_stencil();
  test_texture_8888();
  test_texture_clut4_swizzled();
  test_texture_dxt1();
  test_transform_and_cull();
  test_transfer();
  test_bezier();
  ge::surface::set_scale(1);
  if (g_failures == 0) std::printf("ge tests passed\n");
  return g_failures ? 1 : 0;
}
