// Drives the software GE with hand-built command lists and checks the pixels that end up in VRAM.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <psp/kernel.h>
#include <psp/runtime.h>

#include "gpu.h"

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
  b.setup().add(cmd(0x21, 1)).add(cmd(0xDF, 0 | (2 << 4) | (3 << 8)));  // add, src alpha, 1 - src alpha
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
  TV tv[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0}, {1, 1, 0xFFFFFFFF, 16, 16, 0}};
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
  TV tv[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0}, {1, 1, 0xFFFFFFFF, 32, 8, 0}};
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
  TV tv[2] = {{0, 0, 0xFFFFFFFF, 0, 0, 0}, {1, 1, 0xFFFFFFFF, 4, 4, 0}};
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

int main() {
  init_memory();
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
  if (g_failures == 0) std::printf("ge tests passed\n");
  return g_failures ? 1 : 0;
}
