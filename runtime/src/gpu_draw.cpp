// GE vertex decoding, transform & lighting, primitive assembly and the software rasteriser.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <psp/memory.h>

#include "gpu.h"
#include "raster_fast.h"

namespace psp {
namespace ge {

namespace {

float f24(uint32_t arg) {
  uint32_t bits = arg << 8;
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The GE takes addresses without a region byte as offsets into VRAM (the SDK hands over plain offsets).
uint32_t gaddr(uint32_t addr) {
  uint32_t p = addr & 0x3FFFFFFF;
  return p < kVramSize ? p + kVramBase : addr;
}

uint8_t* gptr(uint32_t addr) { return ptr(gaddr(addr)); }

uint32_t region_end(uint32_t addr) {
  uint32_t p = gaddr(addr) & 0x3FFFFFFF;
  if (p - kVramBase < kVramSize) return kVramBase + kVramSize;
  if (p - kRamBase < kRamSize) return kRamBase + kRamSize;
  return 0;
}

struct VType {
  uint32_t raw = 0;
  int tc = 0, col = 0, nrm = 0, pos = 0, wgt = 0, idx = 0, wcount = 0, mcount = 1;
  bool through = false;
  uint32_t wOff = 0, tcOff = 0, colOff = 0, nrmOff = 0, posOff = 0, size = 0;
};

uint32_t comp_size(int fmt) { return fmt == 1 ? 1 : (fmt == 2 ? 2 : 4); }

VType parse_vtype(uint32_t v) {
  VType t;
  t.raw = v;
  t.tc = v & 3;
  t.col = (v >> 2) & 7;
  t.nrm = (v >> 5) & 3;
  t.pos = (v >> 7) & 3;
  t.wgt = (v >> 9) & 3;
  t.idx = (v >> 11) & 3;
  t.wcount = t.wgt ? static_cast<int>(((v >> 14) & 7) + 1) : 0;
  t.mcount = static_cast<int>(((v >> 18) & 7) + 1);
  t.through = (v >> 23) & 1;
  uint32_t size = 0, maxAlign = 1;
  auto place = [&](uint32_t bytes, uint32_t align) {
    size = (size + align - 1) & ~(align - 1);
    uint32_t off = size;
    size += bytes;
    maxAlign = std::max(maxAlign, align);
    return off;
  };
  if (t.wgt) t.wOff = place(comp_size(t.wgt) * t.wcount, comp_size(t.wgt));
  if (t.tc) t.tcOff = place(comp_size(t.tc) * 2, comp_size(t.tc));
  if (t.col >= 4) t.colOff = place(t.col == 7 ? 4 : 2, t.col == 7 ? 4 : 2);
  if (t.nrm) t.nrmOff = place(comp_size(t.nrm) * 3, comp_size(t.nrm));
  if (t.pos) t.posOff = place(comp_size(t.pos) * 3, comp_size(t.pos));
  t.size = (size + maxAlign - 1) & ~(maxAlign - 1);
  return t;
}

struct RawVert {
  float pos[3] = {0, 0, 0};
  float nrm[3] = {0, 0, 0};
  float uv[2] = {0, 0};
  float col[4] = {1, 1, 1, 1};
  float w[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};

// Output of transform: either clip space (normal) or final screen space (through mode).
struct CV {
  float clip[4] = {0, 0, 0, 1};
  float u = 0, v = 0;
  float col[4] = {1, 1, 1, 1};
  float spec[3] = {0, 0, 0};
  float fog = 1;
};
struct SV {
  float x = 0, y = 0, z = 0, invw = 1;
  float u = 0, v = 0;
  float col[4] = {1, 1, 1, 1};
  float spec[3] = {0, 0, 0};
  float fog = 1;
};

float read_comp(const uint8_t* p, int fmt, bool normalise, float norm8, float norm16) {
  switch (fmt) {
    case 1: return normalise ? static_cast<int8_t>(*p) / norm8 : static_cast<float>(static_cast<int8_t>(*p));
    case 2: {
      int16_t s;
      std::memcpy(&s, p, 2);
      return normalise ? s / norm16 : static_cast<float>(s);
    }
    default: {
      float f;
      std::memcpy(&f, p, 4);
      return f;
    }
  }
}

float read_unsigned(const uint8_t* p, int fmt, float norm8, float norm16, bool normalise) {
  switch (fmt) {
    case 1: return normalise ? *p / norm8 : static_cast<float>(*p);
    case 2: {
      uint16_t s;
      std::memcpy(&s, p, 2);
      return normalise ? s / norm16 : static_cast<float>(s);
    }
    default: {
      float f;
      std::memcpy(&f, p, 4);
      return f;
    }
  }
}

void decode_color(const uint8_t* p, int fmt, float* c) {
  if (fmt == 7) {
    for (int i = 0; i < 4; ++i) c[i] = p[i] / 255.0f;
    return;
  }
  uint16_t v;
  std::memcpy(&v, p, 2);
  if (fmt == 4) {
    c[0] = (v & 31) / 31.0f;
    c[1] = ((v >> 5) & 63) / 63.0f;
    c[2] = ((v >> 11) & 31) / 31.0f;
    c[3] = 1;
  } else if (fmt == 5) {
    c[0] = (v & 31) / 31.0f;
    c[1] = ((v >> 5) & 31) / 31.0f;
    c[2] = ((v >> 10) & 31) / 31.0f;
    c[3] = (v >> 15) ? 1.0f : 0.0f;
  } else {
    c[0] = (v & 15) / 15.0f;
    c[1] = ((v >> 4) & 15) / 15.0f;
    c[2] = ((v >> 8) & 15) / 15.0f;
    c[3] = ((v >> 12) & 15) / 15.0f;
  }
}

void decode_vertex(const VType& t, uint32_t addr, RawVert& out, int morphIndex = 0) {
  const uint8_t* p = gptr(addr + morphIndex * t.size);
  const bool thr = t.through;
  if (t.wgt)
    for (int i = 0; i < t.wcount && i < 8; ++i)
      out.w[i] = read_unsigned(p + t.wOff + i * comp_size(t.wgt), t.wgt, 128.0f, 32768.0f, true);
  if (t.tc) {
    for (int i = 0; i < 2; ++i) {
      const uint8_t* q = p + t.tcOff + i * comp_size(t.tc);
      out.uv[i] = read_unsigned(q, t.tc, 128.0f, 32768.0f, !thr);
    }
  }
  if (t.col >= 4) decode_color(p + t.colOff, t.col, out.col);
  if (t.nrm)
    for (int i = 0; i < 3; ++i)
      out.nrm[i] = read_comp(p + t.nrmOff + i * comp_size(t.nrm), t.nrm, true, 128.0f, 32768.0f);
  if (t.pos)
    for (int i = 0; i < 3; ++i) {
      const uint8_t* q = p + t.posOff + i * comp_size(t.pos);
      // fixed point positions are only normalised outside of through mode
      out.pos[i] = (t.pos == 3 || thr) ? read_comp(q, t.pos, false, 1, 1) : read_comp(q, t.pos, true, 128.0f, 32768.0f);
    }
}

void mul43(const float* m, const float* in, float* out) {
  out[0] = m[0] * in[0] + m[3] * in[1] + m[6] * in[2] + m[9];
  out[1] = m[1] * in[0] + m[4] * in[1] + m[7] * in[2] + m[10];
  out[2] = m[2] * in[0] + m[5] * in[1] + m[8] * in[2] + m[11];
}

void mul33(const float* m, const float* in, float* out) {
  out[0] = m[0] * in[0] + m[3] * in[1] + m[6] * in[2];
  out[1] = m[1] * in[0] + m[4] * in[1] + m[7] * in[2];
  out[2] = m[2] * in[0] + m[5] * in[1] + m[8] * in[2];
}

void normalise3(float* v) {
  float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 1e-12f) {
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
  }
}

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// ---- pixel formats ----

uint32_t unpack_px(int fmt, uint32_t v) {
  uint32_t r, g, b, a = 255;
  switch (fmt) {
    case 0:
      r = v & 31; g = (v >> 5) & 63; b = (v >> 11) & 31;
      r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
      break;
    case 1:
      r = v & 31; g = (v >> 5) & 31; b = (v >> 10) & 31;
      r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
      a = (v >> 15) ? 255 : 0;
      break;
    case 2:
      r = (v & 15) * 17; g = ((v >> 4) & 15) * 17; b = ((v >> 8) & 15) * 17; a = ((v >> 12) & 15) * 17;
      break;
    default: return v;
  }
  return r | (g << 8) | (b << 16) | (a << 24);
}

uint32_t pack_px(int fmt, uint32_t c) {
  uint32_t r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
  switch (fmt) {
    case 0: return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    case 1: return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
    case 2: return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
    default: return c;
  }
}

bool compare(int func, uint32_t a, uint32_t b) {
  switch (func) {
    case 0: return false;
    case 1: return true;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
  }
}

// ---- DXT ----

uint32_t col565(uint16_t v) { return unpack_px(0, v); }

void dxt_colors(const uint8_t* b, bool oneBitAlpha, uint32_t out[4]) {
  uint16_t c0, c1;
  std::memcpy(&c0, b, 2);
  std::memcpy(&c1, b + 2, 2);
  uint32_t a = col565(c0), c = col565(c1);
  out[0] = a;
  out[1] = c;
  auto mix = [](uint32_t x, uint32_t y, int wx, int wy, int div) {
    uint32_t res = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) res |= (((x >> s & 255) * wx + (y >> s & 255) * wy) / div) << s;
    return res;
  };
  if (c0 > c1 || !oneBitAlpha) {
    out[2] = mix(a, c, 2, 1, 3);
    out[3] = mix(a, c, 1, 2, 3);
  } else {
    out[2] = mix(a, c, 1, 1, 2);
    out[3] = 0;
  }
}

}  // namespace

struct Raster {
  Gpu& g;
  Stats* st;
  uint32_t* r;

  uint8_t* fb = nullptr;
  uint32_t fbStride = 0;
  int fbFmt = 3, fbBpp = 4;
  uint8_t* zb = nullptr;
  uint32_t zStride = 0;
  int sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;

  bool clear = false, clearC = false, clearA = false, clearZ = false;
  bool through = false;
  bool texOn = false, lighting = false, fogOn = false, flat = false, cullOn = false, clipOn = false;
  bool blendOn = false, atestOn = false, ctestOn = false, stestOn = false, ztestOn = false, logicOn = false;
  bool secondarySpec = false;
  float vpScale[3], vpCenter[3], offX = 0, offY = 0;
  int cullMode = 0;

  // texture
  uint32_t tAddr = 0;
  int tBufW = 0, tW = 1, tH = 1, tFmt = 0, tFunc = 0;
  bool tSwz = false, tLinear = false, tClampU = false, tClampV = false, tAlpha = true, tDouble = false;
  int cFmt = 3, cShift = 0, cMask = 0xFF, cStart = 0;
  uint32_t env = 0;

  explicit Raster(Gpu& gpu) : g(gpu), st(gpu.stats_), r(gpu.r_) {}


  bool begin() {
    through = (r[0x12] >> 23) & 1;
    clear = r[0xD3] & 1;
    clearC = r[0xD3] & 0x100;
    clearA = r[0xD3] & 0x200;
    clearZ = r[0xD3] & 0x400;
    fbFmt = r[0xD2] & 3;
    fbBpp = fbFmt == 3 ? 4 : 2;
    uint32_t fbAddr = gaddr((r[0x9C] & 0xFFFFFF) | ((r[0x9D] & 0xFF0000) << 8));
    fbStride = r[0x9D] & 0x7FF;
    sx1 = r[0xD4] & 0x3FF;
    sy1 = (r[0xD4] >> 10) & 0x3FF;
    sx2 = r[0xD5] & 0x3FF;
    sy2 = (r[0xD5] >> 10) & 0x3FF;
    uint32_t end = region_end(fbAddr);
    if (!end || !fbStride) return false;
    int rows = static_cast<int>((end - (fbAddr & 0x3FFFFFFF)) / (fbStride * fbBpp));
    sy2 = std::min(sy2, rows - 1);
    sx2 = std::min<int>(sx2, static_cast<int>(fbStride) - 1);
    if (sy2 < sy1 || sx2 < sx1) return false;
    fb = gptr(fbAddr);

    ztestOn = r[0x23] & 1;
    zb = nullptr;
    if (ztestOn || (clear && clearZ)) {
      uint32_t zAddr = gaddr((r[0x9E] & 0xFFFFFF) | ((r[0x9F] & 0xFF0000) << 8));
      zStride = r[0x9F] & 0x7FF;
      uint32_t zend = region_end(zAddr);
      if (zend && zStride) {
        int zrows = static_cast<int>((zend - (zAddr & 0x3FFFFFFF)) / (zStride * 2));
        // keep the depth rows in range too
        if (zrows > 0) {
          sy2 = std::min(sy2, zrows - 1);
          sx2 = std::min<int>(sx2, static_cast<int>(zStride) - 1);
          zb = gptr(zAddr);
        }
      }
      if (!zb) ztestOn = false;
    }
    if (sy2 < sy1 || sx2 < sx1) return false;

    lighting = !through && (r[0x17] & 1);
    fogOn = !through && (r[0x1F] & 1);
    flat = r[0x50] == 0;
    cullOn = !through && (r[0x1D] & 1);
    clipOn = r[0x1C] & 1;
    blendOn = r[0x21] & 1;
    atestOn = r[0x22] & 1;
    ctestOn = r[0x27] & 1;
    stestOn = r[0x24] & 1;
    logicOn = r[0x28] & 1;
    secondarySpec = r[0x5E] & 1;
    cullMode = r[0x9B] & 1;
    vpScale[0] = f24(r[0x42]);
    vpScale[1] = f24(r[0x43]);
    vpScale[2] = f24(r[0x44]);
    vpCenter[0] = f24(r[0x45]);
    vpCenter[1] = f24(r[0x46]);
    vpCenter[2] = f24(r[0x47]);
    offX = (r[0x4C] & 0xFFFF) / 16.0f;
    offY = (r[0x4D] & 0xFFFF) / 16.0f;

    if (clear) {
      texOn = lighting = fogOn = blendOn = atestOn = ctestOn = stestOn = logicOn = false;
      ztestOn = false;
      cullOn = false;
    }
    texOn = !clear && (r[0x1E] & 1);
    if (texOn) texOn = setup_texture();
    if (texOn) build_palette();
    keepReady = false;
    keepCache = keep_mask();
    keepReady = true;
    decide_fast();
    return true;
  }

  bool setup_texture() {
    tAddr = gaddr((r[0xA0] & 0xFFFFFF) | ((r[0xA8] & 0x0F0000) << 8));
    tBufW = r[0xA8] & 0x7FF;
    tW = 1 << std::min<uint32_t>(r[0xB8] & 0xF, 10);
    tH = 1 << std::min<uint32_t>((r[0xB8] >> 8) & 0xF, 10);
    tFmt = r[0xC3] & 0xF;
    tSwz = r[0xC2] & 1;
    tLinear = (r[0xC6] & 1) || ((r[0xC6] >> 8) & 1);
    tClampU = r[0xC7] & 1;
    tClampV = (r[0xC7] >> 8) & 1;
    tFunc = r[0xC9] & 7;
    tAlpha = (r[0xC9] >> 8) & 1;
    tDouble = (r[0xC9] >> 16) & 1;
    env = r[0xCA] & 0xFFFFFF;
    cFmt = r[0xC5] & 3;
    cShift = (r[0xC5] >> 2) & 31;
    cMask = (r[0xC5] >> 8) & 255;
    cStart = ((r[0xC5] >> 16) & 31) * 16;
    if (tBufW < 1 || tFmt > 10) return false;
    if (st) st->texFormats[static_cast<uint32_t>(tFmt) | (tSwz ? 0x100u : 0)]++;
    uint32_t end = region_end(tAddr);
    uint32_t bits = tFmt == 3 || tFmt == 7 ? 32 : (tFmt >= 8 ? 4 : (tFmt == 4 ? 4 : (tFmt == 5 ? 8 : 16)));
    if (tFmt == 9 || tFmt == 10) bits = 8;
    uint32_t bytes = static_cast<uint32_t>(static_cast<uint64_t>(std::max(tBufW, tW)) * tH * bits / 8);
    return end && (tAddr & 0x3FFFFFFF) + bytes <= end;
  }

  // ---- texture fetch ----

  uint32_t pal[256];
  bool palReady = false;
  uint32_t keepCache = 0;
  bool keepReady = false;

  // fast span path (raster_fast.cpp), used when the draw state is simple enough
  fast::Ctx fx;
  fast::Batch batch;
  bool fastAllowed = false;
  int fastState = 0;  // 0 not resolved yet, 1 usable, 2 not usable for this draw

  static fast::Vert fv(const SV& s) {
    fast::Vert v;
    v.x = s.x; v.y = s.y; v.z = s.z; v.invw = s.invw; v.u = s.u; v.v = s.v; v.fog = s.fog;
    for (int i = 0; i < 4; ++i) v.col[i] = s.col[i];
    return v;
  }

  void decide_fast() {
    fastAllowed = !clear && fast::mode() != fast::Mode::Reference && !ctestOn && !stestOn && !logicOn && !(secondarySpec && lighting);
    fastState = 0;
  }

  bool use_fast() {
    if (!fastAllowed) return false;
    if (fastState == 0) fastState = setup_fast() ? 1 : 2;
    return fastState == 1;
  }

  bool setup_fast() {
    fx = fast::Ctx();
    fx.fb = fb;
    fx.fbStride = fbStride;
    fx.fbFmt = fbFmt;
    fx.keep = keepCache;
    fx.sx1 = sx1; fx.sy1 = sy1; fx.sx2 = sx2; fx.sy2 = sy2;
    fx.zb = zb;
    fx.zStride = zStride;
    fx.zTest = ztestOn;
    fx.zWrite = !(r[0xE7] & 1);
    fx.zFunc = r[0xDE] & 7;
    fx.clipZ = clipOn && !through;
    fx.zlo = static_cast<float>(r[0xD6] & 0xFFFF);
    fx.zhi = static_cast<float>(r[0xD7] & 0xFFFF);
    fx.blend = blendOn;
    fx.sfac = r[0xDF] & 15;
    fx.dfac = (r[0xDF] >> 4) & 15;
    fx.bfunc = (r[0xDF] >> 8) & 7;
    fx.fixA = r[0xE0];
    fx.fixB = r[0xE1];
    fx.atest = atestOn;
    fx.fog = fogOn;
    fx.fogCol = r[0xCF];
    fx.aMask = (r[0xDB] >> 16) & 0xFF;
    fx.aRef = (r[0xDB] >> 8) & 0xFF;
    fx.aFunc = r[0xDB] & 7;
    if (texOn) {
      if (tFmt > 5 || ((tFmt == 4 || tFmt == 5) && !palReady)) return false;
      fast::TexSource ts;
      ts.mem = gptr(tAddr);
      ts.avail = region_end(tAddr) - (tAddr & 0x3FFFFFFF);
      ts.addr = tAddr;
      ts.fmt = tFmt;
      ts.swizzled = tSwz;
      ts.bufW = tBufW;
      ts.w = tW;
      ts.h = tH;
      ts.inVram = (tAddr & 0x3FFFFFFF) >= 0x04000000 && (tAddr & 0x3FFFFFFF) < 0x04200000;
      fast::Texture t;
      if (!fast::texture_lookup(ts, t)) return false;
      fx.tex = true;
      fx.texRgba = t.rgba;
      fx.texIdx = t.idx;
      fx.pal = pal;
      fx.tW = tW;
      fx.tH = tH;
      fx.clampU = tClampU;
      fx.clampV = tClampV;
      fx.linear = tLinear;
      fx.tFunc = tFunc;
      fx.tAlpha = tAlpha;
      fx.tDouble = tDouble;
      fx.env = env;
    }
    const int fcat = (r[0xD3] & 1) ? 0 : (texOn ? (blendOn ? 3 : 2) : 1);
    batch.begin(&fx, pal, fcat);
    return true;
  }

  uint32_t clut_entry(uint32_t idx) const {
    if (palReady) return pal[idx & 255];
    return clut_slow(idx);
  }

  void build_palette() {
    if (tFmt != 4 && tFmt != 5) return;
    const int n = tFmt == 4 ? 16 : 256;
    for (int i = 0; i < n; ++i) pal[i] = clut_slow(i);
    palReady = true;
  }

  uint32_t clut_slow(uint32_t idx) const {
    idx = ((idx >> cShift) & cMask) + cStart;
    if (cFmt == 3) return g.clut_[idx & 1023] ;
    uint16_t v;
    idx &= 2047;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(g.clut_) + idx * 2, 2);
    return unpack_px(cFmt, v);
  }

  uint32_t texel(int x, int y) const {
    const int bw = tBufW;
    auto addr = [&](uint32_t bitsPerPx) -> const uint8_t* {
      uint32_t rowBytes = bw * bitsPerPx / 8;
      uint32_t xb = x * bitsPerPx / 8;
      if (!tSwz) return gptr(tAddr + y * rowBytes + xb);
      uint32_t off = (y / 8) * (rowBytes * 8) + (xb / 16) * 128 + (y % 8) * 16 + (xb % 16);
      return gptr(tAddr + off);
    };
    switch (tFmt) {
      case 0: case 1: case 2: {
        uint16_t v;
        std::memcpy(&v, addr(16), 2);
        return unpack_px(tFmt, v);
      }
      case 3: {
        uint32_t v;
        std::memcpy(&v, addr(32), 4);
        return v;
      }
      case 4: {
        uint8_t b = *addr(4);
        return clut_entry((x & 1) ? (b >> 4) : (b & 15));
      }
      case 5: return clut_entry(*addr(8));
      case 6: {
        uint16_t v;
        std::memcpy(&v, addr(16), 2);
        return clut_entry(v);
      }
      case 7: {
        uint32_t v;
        std::memcpy(&v, addr(32), 4);
        return clut_entry(v);
      }
      default: break;
    }
    // DXT
    const uint32_t blockSize = tFmt == 8 ? 8 : 16;
    const uint8_t* blk = gptr(tAddr + ((y / 4) * (bw / 4) + (x / 4)) * blockSize);
    int px = x & 3, py = y & 3;
    uint32_t pal[4];
    const uint8_t* cb = tFmt == 8 ? blk : blk + 8;
    dxt_colors(cb, tFmt == 8, pal);
    uint32_t cidx = (cb[4 + py] >> (px * 2)) & 3;
    uint32_t c = pal[cidx];
    if (tFmt == 9) {
      uint16_t row;
      std::memcpy(&row, blk + py * 2, 2);
      uint32_t a = (row >> (px * 4)) & 15;
      c = (c & 0x00FFFFFF) | ((a * 17) << 24);
    } else if (tFmt == 10) {
      uint32_t a0 = blk[0], a1 = blk[1];
      uint64_t bitsAll = 0;
      for (int i = 0; i < 6; ++i) bitsAll |= static_cast<uint64_t>(blk[2 + i]) << (8 * i);
      uint32_t ai = (bitsAll >> ((py * 4 + px) * 3)) & 7;
      uint32_t a;
      if (ai == 0) a = a0;
      else if (ai == 1) a = a1;
      else if (a0 > a1) a = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
      else if (ai == 6) a = 0;
      else if (ai == 7) a = 255;
      else a = ((6 - ai) * a0 + (ai - 1) * a1) / 5;
      c = (c & 0x00FFFFFF) | (a << 24);
    }
    return c;
  }

  uint32_t texel_wrapped(int x, int y) const {
    if (tClampU) x = clampi(x, 0, tW - 1);
    else x &= tW - 1;
    if (tClampV) y = clampi(y, 0, tH - 1);
    else y &= tH - 1;
    return texel(x, y);
  }

  uint32_t sample(float u, float v) const {
    float fx = u * tW, fy = v * tH;
    if (!tLinear) return texel_wrapped(static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fy)));
    fx -= 0.5f;
    fy -= 0.5f;
    int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    float ax = fx - x0, ay = fy - y0;
    uint32_t t00 = texel_wrapped(x0, y0), t10 = texel_wrapped(x0 + 1, y0);
    uint32_t t01 = texel_wrapped(x0, y0 + 1), t11 = texel_wrapped(x0 + 1, y0 + 1);
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
      float top = (t00 >> s & 255) * (1 - ax) + (t10 >> s & 255) * ax;
      float bot = (t01 >> s & 255) * (1 - ax) + (t11 >> s & 255) * ax;
      out |= static_cast<uint32_t>(top * (1 - ay) + bot * ay + 0.5f) << s;
    }
    return out;
  }

  // ---- framebuffer access ----

  uint32_t fb_read(int x, int y) const {
    const uint8_t* p = fb + (static_cast<size_t>(y) * fbStride + x) * fbBpp;
    if (fbBpp == 4) {
      uint32_t v;
      std::memcpy(&v, p, 4);
      return v;
    }
    uint16_t v;
    std::memcpy(&v, p, 2);
    return unpack_px(fbFmt, v);
  }

  void fb_write(int x, int y, uint32_t rgba, uint32_t keepMask) {
    uint8_t* p = fb + (static_cast<size_t>(y) * fbStride + x) * fbBpp;
    if (fbBpp == 4) {
      uint32_t old;
      std::memcpy(&old, p, 4);
      uint32_t nv = (old & keepMask) | (rgba & ~keepMask);
      std::memcpy(p, &nv, 4);
    } else {
      uint16_t old;
      std::memcpy(&old, p, 2);
      uint32_t nv = (old & keepMask) | (pack_px(fbFmt, rgba) & ~keepMask);
      uint16_t o = static_cast<uint16_t>(nv);
      std::memcpy(p, &o, 2);
    }
  }

  uint16_t* zptr(int x, int y) { return reinterpret_cast<uint16_t*>(zb + (static_cast<size_t>(y) * zStride + x) * 2); }

  // ---- fragment pipeline ----

  uint32_t keep_mask() const {
    if (keepReady) return keepCache;
    uint32_t m = (r[0xE8] & 0xFFFFFF) | ((r[0xE9] & 0xFF) << 24);
    return fbBpp == 4 ? m : pack_px(fbFmt, m);
  }

  void fragment(int x, int y, float z, const float* col, const float* spec, float fog, float u, float v) {
    if (x < sx1 || x > sx2 || y < sy1 || y > sy2) return;
    if (st) st->fragments++;
    int c[4];
    for (int i = 0; i < 4; ++i) c[i] = clampi(static_cast<int>(col[i] * 255.0f + 0.5f), 0, 255);

    if (texOn) {
      uint32_t t = sample(u, v);
      int tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
      if (!tAlpha) ta = 255;
      int er = env & 255, eg = (env >> 8) & 255, eb = (env >> 16) & 255;
      switch (tFunc) {
        case 0:
          c[0] = c[0] * tr / 255; c[1] = c[1] * tg / 255; c[2] = c[2] * tb / 255;
          if (tAlpha) c[3] = c[3] * ta / 255;
          break;
        case 1:
          if (tAlpha) {
            c[0] = (tr * ta + c[0] * (255 - ta)) / 255;
            c[1] = (tg * ta + c[1] * (255 - ta)) / 255;
            c[2] = (tb * ta + c[2] * (255 - ta)) / 255;
          } else {
            c[0] = tr; c[1] = tg; c[2] = tb;
          }
          break;
        case 2:
          c[0] = (c[0] * (255 - tr) + er * tr) / 255;
          c[1] = (c[1] * (255 - tg) + eg * tg) / 255;
          c[2] = (c[2] * (255 - tb) + eb * tb) / 255;
          if (tAlpha) c[3] = c[3] * ta / 255;
          break;
        case 3:
          c[0] = tr; c[1] = tg; c[2] = tb;
          if (tAlpha) c[3] = ta;
          break;
        default:
          c[0] += tr; c[1] += tg; c[2] += tb;
          if (tAlpha) c[3] = c[3] * ta / 255;
          break;
      }
      if (tDouble)
        for (int i = 0; i < 3; ++i) c[i] *= 2;
      for (int i = 0; i < 4; ++i) c[i] = clampi(c[i], 0, 255);
    }
    if (spec && secondarySpec && lighting)
      for (int i = 0; i < 3; ++i) c[i] = clampi(c[i] + static_cast<int>(spec[i] * 255.0f), 0, 255);
    if (fogOn) {
      float f = clamp01(fog);
      uint32_t fc = r[0xCF];
      for (int i = 0; i < 3; ++i) c[i] = static_cast<int>(c[i] * f + ((fc >> (8 * i)) & 255) * (1 - f) + 0.5f);
    }

    if (ctestOn) {
      uint32_t mask = r[0xDA] & 0xFFFFFF, refc = r[0xD9] & mask;
      uint32_t px = (c[0] | c[1] << 8 | c[2] << 16) & mask;
      int func = r[0xD8] & 3;
      if (!compare(func, px, refc)) return;
    }
    if (atestOn) {
      uint32_t mask = (r[0xDB] >> 16) & 0xFF, ref = (r[0xDB] >> 8) & 0xFF;
      if (!compare(r[0xDB] & 7, c[3] & mask, ref & mask)) return;
    }

    uint32_t dst = fb_read(x, y);
    uint32_t newStencil = c[3];
    bool writeStencil = false;
    uint16_t* zp = ztestOn ? zptr(x, y) : nullptr;
    uint16_t zi = static_cast<uint16_t>(clampi(static_cast<int>(z + 0.5f), 0, 65535));
    if (stestOn) {
      uint32_t sval = fbFmt == 0 ? 0 : dst >> 24;
      uint32_t smask = (r[0xDC] >> 16) & 0xFF, sref = (r[0xDC] >> 8) & 0xFF;
      uint32_t sf = r[0xDD] & 0xFF, zf = (r[0xDD] >> 8) & 0xFF, zp_ = (r[0xDD] >> 16) & 0xFF;
      auto apply = [&](uint32_t op) -> uint32_t {
        switch (op) {
          case 1: return 0;
          case 2: return sref;
          case 3: return ~sval & 0xFF;
          case 4: return sval < 255 ? sval + 1 : 255;
          case 5: return sval > 0 ? sval - 1 : 0;
          default: return sval;
        }
      };
      bool spass = compare(r[0xDC] & 7, sval & smask, sref & smask);
      bool zpass = !zp || compare(r[0xDE] & 7, zi, *zp);
      writeStencil = true;
      if (!spass) {
        newStencil = apply(sf);
        write_stencil_only(x, y, dst, newStencil);
        return;
      }
      if (!zpass) {
        newStencil = apply(zf);
        write_stencil_only(x, y, dst, newStencil);
        return;
      }
      newStencil = apply(zp_);
    } else if (zp) {
      if (!compare(r[0xDE] & 7, zi, *zp)) return;
    }

    if (blendOn) {
      uint32_t bm = r[0xDF];
      int sfac = bm & 15, dfac = (bm >> 4) & 15, func = (bm >> 8) & 7;
      int src[4] = {c[0], c[1], c[2], c[3]};
      int dc[4] = {static_cast<int>(dst & 255), static_cast<int>((dst >> 8) & 255), static_cast<int>((dst >> 16) & 255),
                   static_cast<int>(dst >> 24)};
      if (fbFmt == 0) dc[3] = 255;
      auto factor = [&](int f, bool isSrc, int ch) -> int {
        switch (f) {
          case 0: return isSrc ? dc[ch] : src[ch];
          case 1: return 255 - (isSrc ? dc[ch] : src[ch]);
          case 2: return src[3];
          case 3: return 255 - src[3];
          case 4: return dc[3];
          case 5: return 255 - dc[3];
          case 6: return std::min(255, src[3] * 2);
          case 7: return 255 - std::min(255, src[3] * 2);
          case 8: return std::min(255, dc[3] * 2);
          case 9: return 255 - std::min(255, dc[3] * 2);
          default: {
            uint32_t fix = isSrc ? r[0xE0] : r[0xE1];
            return ch == 3 ? src[3] : static_cast<int>((fix >> (8 * ch)) & 255);
          }
        }
      };
      int out[4];
      for (int ch = 0; ch < 3; ++ch) {
        int s = src[ch] * factor(sfac, true, ch) / 255;
        int d = dc[ch] * factor(dfac, false, ch) / 255;
        switch (func) {
          case 0: out[ch] = s + d; break;
          case 1: out[ch] = s - d; break;
          case 2: out[ch] = d - s; break;
          case 3: out[ch] = std::min(src[ch], dc[ch]); break;
          case 4: out[ch] = std::max(src[ch], dc[ch]); break;
          default: out[ch] = std::abs(src[ch] - dc[ch]); break;
        }
        out[ch] = clampi(out[ch], 0, 255);
      }
      // alpha takes the source value, matching what the destination alpha is used for in practice
      out[3] = c[3];
      for (int i = 0; i < 4; ++i) c[i] = out[i];
    }

    uint32_t rgba = c[0] | (c[1] << 8) | (c[2] << 16) | (static_cast<uint32_t>(writeStencil ? newStencil : c[3]) << 24);
    if (logicOn) {
      uint32_t s = rgba, d = dst, o;
      switch (r[0xE6] & 15) {
        case 0: o = 0; break;
        case 1: o = s & d; break;
        case 2: o = s & ~d; break;
        case 3: o = s; break;
        case 4: o = ~s & d; break;
        case 5: o = d; break;
        case 6: o = s ^ d; break;
        case 7: o = s | d; break;
        case 8: o = ~(s | d); break;
        case 9: o = ~(s ^ d); break;
        case 10: o = ~d; break;
        case 11: o = s | ~d; break;
        case 12: o = ~s; break;
        case 13: o = ~s | d; break;
        case 14: o = ~(s & d); break;
        default: o = 0xFFFFFFFF; break;
      }
      rgba = o;
    }
    fb_write(x, y, rgba, keep_mask());
    if (zp && !(r[0xE7] & 1)) *zp = zi;
  }

  void write_stencil_only(int x, int y, uint32_t dst, uint32_t stencil) {
    uint32_t rgba = (dst & 0x00FFFFFF) | (stencil << 24);
    uint32_t keep = keep_mask();
    // only the alpha/stencil channel changes
    uint32_t aMask = fbBpp == 4 ? 0xFF000000u : pack_px(fbFmt, 0xFF000000u);
    fb_write(x, y, rgba, keep | ~aMask);
  }

  void clear_pixel(int x, int y, float z, const float* col) {
    if (x < sx1 || x > sx2 || y < sy1 || y > sy2) return;
    if (clearC || clearA) {
      uint32_t c = 0;
      for (int i = 0; i < 4; ++i) c |= static_cast<uint32_t>(clampi(static_cast<int>(col[i] * 255.0f + 0.5f), 0, 255)) << (8 * i);
      uint32_t keep = 0;
      uint32_t rgbMask = fbBpp == 4 ? 0x00FFFFFFu : pack_px(fbFmt, 0x00FFFFFFu);
      uint32_t aMask = fbBpp == 4 ? 0xFF000000u : pack_px(fbFmt, 0xFF000000u);
      if (fbFmt == 0) aMask = 0;
      if (!clearC) keep |= rgbMask;
      if (!clearA) keep |= aMask;
      if (fbFmt == 0) keep |= 0;  // 565 has no alpha bits
      fb_write(x, y, c, keep);
    }
    if (clearZ && zb) *zptr(x, y) = static_cast<uint16_t>(clampi(static_cast<int>(z + 0.5f), 0, 65535));
  }

  // ---- primitives ----

  static float edge(const SV& a, const SV& b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
  }

  static bool top_left(const SV& a, const SV& b) {
    float dy = b.y - a.y, dx = b.x - a.x;
    return dy < 0 || (dy == 0 && dx > 0);
  }

  void tri(SV a, SV b, SV c) {
    float area = edge(a, b, c.x, c.y);
    if (area == 0) return;
    if (cullOn) {
      bool frontIsPositive = cullMode == 1;
      if ((area > 0) != frontIsPositive) {
        if (st) st->culled++;
        return;
      }
    }
    if (st) st->triangles++;
    if (flat) {
      // the last vertex of the triangle provides the colour
      for (int i = 0; i < 4; ++i) a.col[i] = b.col[i] = c.col[i];
      for (int i = 0; i < 3; ++i) a.spec[i] = b.spec[i] = c.spec[i];
    }
    if (area < 0) {
      std::swap(b, c);
      area = -area;
    }
    if (use_fast()) {
      if (batch.add_tri(fv(a), fv(b), fv(c))) return;
    }
    fast::sync();
    int minX = std::max(sx1, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    int maxX = std::min(sx2, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    int minY = std::max(sy1, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    int maxY = std::min(sy2, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    const bool tl0 = top_left(b, c), tl1 = top_left(c, a), tl2 = top_left(a, b);
    const float invArea = 1.0f / area;
    const bool plainCol = flat || (a.col[0] == b.col[0] && a.col[1] == b.col[1] && a.col[2] == b.col[2] && a.col[3] == b.col[3] &&
                                   b.col[0] == c.col[0] && b.col[1] == c.col[1] && b.col[2] == c.col[2] && b.col[3] == c.col[3]);
    const bool needSpec = secondarySpec && lighting;
    const bool clipZ = clipOn && !through;
    const float zlo = static_cast<float>(r[0xD6] & 0xFFFF), zhi = static_cast<float>(r[0xD7] & 0xFFFF);
    for (int y = minY; y <= maxY; ++y) {
      const float py = y + 0.5f;
      int xs = minX, xe = maxX;
      {
        // narrow the scan to where the three edge functions can be non-negative
        const SV* ea[3] = {&b, &c, &a};
        const SV* eb[3] = {&c, &a, &b};
        float lo = static_cast<float>(minX), hi = static_cast<float>(maxX);
        for (int k = 0; k < 3; ++k) {
          float slope = -(eb[k]->y - ea[k]->y);  // d(edge)/dx
          float base = edge(*ea[k], *eb[k], 0.5f, py);
          if (slope > 0) lo = std::max(lo, std::floor(-base / slope) - 1.0f);
          else if (slope < 0) hi = std::min(hi, std::ceil(-base / slope) + 1.0f);
          else if (base < 0) { lo = 1; hi = 0; }
        }
        xs = static_cast<int>(lo);
        xe = static_cast<int>(hi);
      }
      for (int x = xs; x <= xe; ++x) {
        const float px = x + 0.5f;
        float w0 = edge(b, c, px, py), w1 = edge(c, a, px, py), w2 = edge(a, b, px, py);
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        if ((w0 == 0 && !tl0) || (w1 == 0 && !tl1) || (w2 == 0 && !tl2)) continue;
        w0 *= invArea;
        w1 *= invArea;
        w2 *= invArea;
        float z = a.z * w0 + b.z * w1 + c.z * w2;
        if (clipZ && (z < zlo || z > zhi)) continue;
        float col[4], spec[3] = {0, 0, 0};
        if (plainCol) {
          for (int i = 0; i < 4; ++i) col[i] = a.col[i];
        } else {
          for (int i = 0; i < 4; ++i) col[i] = a.col[i] * w0 + b.col[i] * w1 + c.col[i] * w2;
        }
        if (needSpec)
          for (int i = 0; i < 3; ++i) spec[i] = a.spec[i] * w0 + b.spec[i] * w1 + c.spec[i] * w2;
        float fog = fogOn ? a.fog * w0 + b.fog * w1 + c.fog * w2 : 0.0f;
        float u = 0, v = 0;
        if (texOn) {
          float pw0 = w0 * a.invw, pw1 = w1 * b.invw, pw2 = w2 * c.invw;
          float s = pw0 + pw1 + pw2;
          if (s != 0) {
            u = (a.u * pw0 + b.u * pw1 + c.u * pw2) / s;
            v = (a.v * pw0 + b.v * pw1 + c.v * pw2) / s;
          }
        }
        if (clear) clear_pixel(x, y, std::clamp(z, 0.0f, 65535.0f), col);
        else fragment(x, y, std::clamp(z, 0.0f, 65535.0f), col, spec, fog, u, v);
      }
    }
  }

  void rect(const SV& a, const SV& b) {
    float x0 = a.x, x1 = b.x, y0 = a.y, y1 = b.y;
    float u0 = a.u, u1 = b.u, v0 = a.v, v1 = b.v;
    if (x1 < x0) {
      std::swap(x0, x1);
      std::swap(u0, u1);
    }
    if (y1 < y0) {
      std::swap(y0, y1);
      std::swap(v0, v1);
    }
    if (st) st->triangles += 2;
    const float w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    int ix0 = std::max(sx1, static_cast<int>(std::ceil(x0 - 0.5f))), ix1 = std::min(sx2, static_cast<int>(std::ceil(x1 - 0.5f)) - 1);
    int iy0 = std::max(sy1, static_cast<int>(std::ceil(y0 - 0.5f))), iy1 = std::min(sy2, static_cast<int>(std::ceil(y1 - 0.5f)) - 1);
    if (clear && fast::mode() != fast::Mode::Reference) {
      if (ix0 > ix1 || iy0 > iy1) return;
      fast::sync();
      fast::Ctx cx;
      cx.fb = fb;
      cx.fbStride = fbStride;
      cx.fbFmt = fbFmt;
      cx.zb = zb;
      cx.zStride = zStride;
      uint32_t c = 0;
      for (int i = 0; i < 4; ++i) c |= static_cast<uint32_t>(clampi(static_cast<int>(b.col[i] * 255.0f + 0.5f), 0, 255)) << (8 * i);
      const uint16_t zv = static_cast<uint16_t>(clampi(static_cast<int>(std::clamp(b.z, 0.0f, 65535.0f) + 0.5f), 0, 65535));
      fast::clear_rect(cx, ix0, iy0, ix1, iy1, clearC, clearA, clearZ && zb, c, zv);
      return;
    }
    if (!clear && use_fast() && batch.add_rect(fv(a), fv(b))) return;
    fast::sync();
    for (int y = iy0; y <= iy1; ++y) {
      float ty = (y + 0.5f - y0) / h;
      float v = v0 + (v1 - v0) * ty;
      for (int x = ix0; x <= ix1; ++x) {
        float u = u0 + (u1 - u0) * ((x + 0.5f - x0) / w);
        float z = std::clamp(b.z, 0.0f, 65535.0f);
        if (clear) clear_pixel(x, y, z, b.col);
        else fragment(x, y, z, b.col, b.spec, b.fog, u, v);
      }
    }
  }

  void line(const SV& a, const SV& b) {
    fast::sync();
    float dx = b.x - a.x, dy = b.y - a.y;
    int steps = static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; ++i) {
      float t = static_cast<float>(i) / steps;
      int x = static_cast<int>(std::floor(a.x + dx * t)), y = static_cast<int>(std::floor(a.y + dy * t));
      float col[4];
      for (int k = 0; k < 4; ++k) col[k] = flat ? b.col[k] : a.col[k] + (b.col[k] - a.col[k]) * t;
      float z = a.z + (b.z - a.z) * t;
      float u = a.u + (b.u - a.u) * t, v = a.v + (b.v - a.v) * t;
      if (clear) clear_pixel(x, y, z, col);
      else fragment(x, y, z, col, a.spec, a.fog, u, v);
    }
  }

  void point(const SV& a) {
    fast::sync();
    int x = static_cast<int>(std::floor(a.x)), y = static_cast<int>(std::floor(a.y));
    if (clear) clear_pixel(x, y, a.z, a.col);
    else fragment(x, y, a.z, a.col, a.spec, a.fog, a.u, a.v);
  }

  // ---- transform ----

  SV project(const CV& c) const {
    SV s;
    for (int i = 0; i < 4; ++i) s.col[i] = c.col[i];
    for (int i = 0; i < 3; ++i) s.spec[i] = c.spec[i];
    s.u = c.u;
    s.v = c.v;
    s.fog = c.fog;
    if (through) {
      s.x = c.clip[0];
      s.y = c.clip[1];
      s.z = c.clip[2];
      s.invw = 1;
      return s;
    }
    float iw = 1.0f / c.clip[3];
    s.invw = iw;
    s.x = c.clip[0] * iw * vpScale[0] + vpCenter[0] - offX;
    s.y = c.clip[1] * iw * vpScale[1] + vpCenter[1] - offY;
    s.z = c.clip[2] * iw * vpScale[2] + vpCenter[2];
    return s;
  }

  void light(const RawVert& rv, const float* wpos, const float* wnrm, bool hasCol, CV& out) const {
    auto rgb = [&](uint32_t reg, float* o) {
      for (int i = 0; i < 3; ++i) o[i] = ((reg >> (8 * i)) & 255) / 255.0f;
    };
    uint32_t upd = r[0x53];
    float matAmb[4], matDif[3], matSpec[3], emis[3], sceneAmb[3];
    rgb(r[0x55], matAmb);
    matAmb[3] = (r[0x58] & 255) / 255.0f;
    rgb(r[0x56], matDif);
    rgb(r[0x57], matSpec);
    rgb(r[0x54], emis);
    rgb(r[0x5C], sceneAmb);
    if (hasCol) {
      if (upd & 1)
        for (int i = 0; i < 4; ++i) matAmb[i] = rv.col[i];
      if (upd & 2)
        for (int i = 0; i < 3; ++i) matDif[i] = rv.col[i];
      if (upd & 4)
        for (int i = 0; i < 3; ++i) matSpec[i] = rv.col[i];
    }
    float n[3] = {wnrm[0], wnrm[1], wnrm[2]};
    normalise3(n);
    if (r[0x51] & 1)
      for (float& f : n) f = -f;
    float specCoef = f24(r[0x5B]);
    float sum[3], specSum[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) sum[i] = emis[i] + sceneAmb[i] * matAmb[i];
    for (int li = 0; li < 4; ++li) {
      if (!(r[0x18 + li] & 1)) continue;
      uint32_t type = r[0x5F + li];
      int comp = type & 3, kind = (type >> 8) & 3;
      float lp[3] = {f24(r[0x63 + li * 3]), f24(r[0x64 + li * 3]), f24(r[0x65 + li * 3])};
      float L[3], att = 1.0f;
      if (kind == 0) {
        for (int i = 0; i < 3; ++i) L[i] = lp[i];
        normalise3(L);
      } else {
        for (int i = 0; i < 3; ++i) L[i] = lp[i] - wpos[i];
        float d = std::sqrt(dot3(L, L));
        normalise3(L);
        float ka = f24(r[0x7B + li * 3]), kb = f24(r[0x7C + li * 3]), kc = f24(r[0x7D + li * 3]);
        float den = ka + kb * d + kc * d * d;
        att = den > 1e-9f ? std::min(1.0f, 1.0f / den) : 1.0f;
        if (kind == 2) {
          float sd[3] = {f24(r[0x6F + li * 3]), f24(r[0x70 + li * 3]), f24(r[0x71 + li * 3])};
          normalise3(sd);
          float cs = -dot3(L, sd);
          if (cs < f24(r[0x8B + li])) att = 0;
          else att *= std::pow(std::max(cs, 0.0f), f24(r[0x87 + li]));
        }
      }
      float la[3], ld[3], ls[3];
      rgb(r[0x8F + li * 3], la);
      rgb(r[0x90 + li * 3], ld);
      rgb(r[0x91 + li * 3], ls);
      float ndl = dot3(n, L);
      float df = std::max(ndl, 0.0f);
      if (comp == 2) df = std::pow(df, specCoef);
      for (int i = 0; i < 3; ++i) sum[i] += att * (la[i] * matAmb[i] + df * ld[i] * matDif[i]);
      if (comp >= 1 && ndl > 0) {
        float H[3] = {L[0], L[1], L[2] + 1.0f};
        normalise3(H);
        float sf = std::pow(std::max(dot3(n, H), 0.0f), specCoef);
        for (int i = 0; i < 3; ++i) specSum[i] += att * sf * ls[i] * matSpec[i];
      }
    }
    for (int i = 0; i < 3; ++i) {
      if (secondarySpec) {
        out.col[i] = clamp01(sum[i]);
        out.spec[i] = clamp01(specSum[i]);
      } else {
        out.col[i] = clamp01(sum[i] + specSum[i]);
        out.spec[i] = 0;
      }
    }
    out.col[3] = (hasCol && (upd & 1)) ? rv.col[3] : matAmb[3];
  }

  void transform(const VType& vt, const RawVert& rv, CV& out) const {
    const bool hasCol = vt.col >= 4;
    if (through) {
      out.clip[0] = rv.pos[0];
      out.clip[1] = rv.pos[1];
      out.clip[2] = rv.pos[2];
      out.clip[3] = 1;
      if (vt.tc) {
        out.u = rv.uv[0];
        out.v = rv.uv[1];
        // through mode coordinates are always texels, whatever the vertex format
        out.u /= static_cast<float>(tW);
        out.v /= static_cast<float>(tH);
      }
      if (hasCol) for (int i = 0; i < 4; ++i) out.col[i] = rv.col[i];
      else {
        for (int i = 0; i < 3; ++i) out.col[i] = ((r[0x55] >> (8 * i)) & 255) / 255.0f;
        out.col[3] = (r[0x58] & 255) / 255.0f;
      }
      return;
    }
    float p[3] = {rv.pos[0], rv.pos[1], rv.pos[2]}, nrm[3] = {rv.nrm[0], rv.nrm[1], rv.nrm[2]};
    if (vt.wcount) {
      float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
      for (int i = 0; i < vt.wcount && i < 8; ++i) {
        float tp[3], tn[3];
        mul43(&g.bone_[i * 12], rv.pos, tp);
        mul33(&g.bone_[i * 12], rv.nrm, tn);
        for (int k = 0; k < 3; ++k) {
          sp[k] += tp[k] * rv.w[i];
          sn[k] += tn[k] * rv.w[i];
        }
      }
      std::memcpy(p, sp, sizeof p);
      std::memcpy(nrm, sn, sizeof nrm);
    }
    float wp[3], wn[3], vp[3];
    mul43(g.world_, p, wp);
    mul33(g.world_, nrm, wn);
    mul43(g.view_, wp, vp);
    const float in[4] = {vp[0], vp[1], vp[2], 1.0f};
    for (int row = 0; row < 4; ++row) {
      float s = 0;
      for (int col = 0; col < 4; ++col) s += g.proj_[col * 4 + row] * in[col];
      out.clip[row] = s;
    }

    if (hasCol) for (int i = 0; i < 4; ++i) out.col[i] = rv.col[i];
    else {
      for (int i = 0; i < 3; ++i) out.col[i] = ((r[0x55] >> (8 * i)) & 255) / 255.0f;
      out.col[3] = (r[0x58] & 255) / 255.0f;
    }
    if (lighting) light(rv, wp, wn, hasCol, out);

    // texture coordinates
    int mode = r[0xC0] & 3;
    if (mode == 0) {
      out.u = rv.uv[0] * f24(r[0x48]) + f24(r[0x4A]);
      out.v = rv.uv[1] * f24(r[0x49]) + f24(r[0x4B]);
    } else if (mode == 1) {
      int src = (r[0xC0] >> 8) & 3;
      float s[3];
      if (src == 0) std::memcpy(s, rv.pos, sizeof s);
      else if (src == 1) { s[0] = rv.uv[0]; s[1] = rv.uv[1]; s[2] = 0; }
      else {
        std::memcpy(s, rv.nrm, sizeof s);
        if (src == 2) normalise3(s);
      }
      float t[3];
      mul43(g.tgen_, s, t);
      out.u = t[0];
      out.v = t[1];
      if (src == 0 && std::fabs(t[2]) > 1e-9f) {
        out.u /= t[2];
        out.v /= t[2];
      }
    } else {
      float n[3] = {wn[0], wn[1], wn[2]};
      normalise3(n);
      int lu = r[0xC1] & 3, lv = (r[0xC1] >> 8) & 3;
      float Lu[3] = {f24(r[0x63 + lu * 3]), f24(r[0x64 + lu * 3]), f24(r[0x65 + lu * 3])};
      float Lv[3] = {f24(r[0x63 + lv * 3]), f24(r[0x64 + lv * 3]), f24(r[0x65 + lv * 3])};
      normalise3(Lu);
      normalise3(Lv);
      out.u = 0.5f + 0.5f * dot3(n, Lu);
      out.v = 0.5f + 0.5f * dot3(n, Lv);
    }

    if (fogOn) {
      float end = f24(r[0xCD]), slope = f24(r[0xCE]);
      out.fog = clamp01((end - out.clip[3]) * slope);
    }
  }

  // Clips against the near plane (or w > 0 when plane clipping is off) and rasterises the triangle.
  void emit_tri(const VType&, const CV& a, const CV& b, const CV& c) {
    if (through) {
      tri(project(a), project(b), project(c));
      return;
    }
    CV in[3] = {a, b, c};
    CV outv[4];
    int n = 0;
    auto dist = [&](const CV& v) { return clipOn ? v.clip[2] + v.clip[3] : v.clip[3] - 1e-5f; };
    for (int i = 0; i < 3; ++i) {
      const CV& p = in[i];
      const CV& q = in[(i + 1) % 3];
      float dp = dist(p), dq = dist(q);
      if (dp >= 0 && n < 4) outv[n++] = p;
      if ((dp >= 0) != (dq >= 0) && n < 4) {
        float t = dp / (dp - dq);
        CV m;
        for (int k = 0; k < 4; ++k) m.clip[k] = p.clip[k] + (q.clip[k] - p.clip[k]) * t;
        for (int k = 0; k < 4; ++k) m.col[k] = p.col[k] + (q.col[k] - p.col[k]) * t;
        for (int k = 0; k < 3; ++k) m.spec[k] = p.spec[k] + (q.spec[k] - p.spec[k]) * t;
        m.u = p.u + (q.u - p.u) * t;
        m.v = p.v + (q.v - p.v) * t;
        m.fog = p.fog + (q.fog - p.fog) * t;
        outv[n++] = m;
      }
    }
    if (n < 3) return;
    SV s[4];
    for (int i = 0; i < n; ++i) s[i] = project(outv[i]);
    tri(s[0], s[1], s[2]);
    if (n == 4) tri(s[0], s[2], s[3]);
  }

  void assemble(const VType& vt, const std::vector<CV>& v, int prim) {
    const size_t n = v.size();
    auto sv = [&](size_t i) { return project(v[i]); };
    switch (prim) {
      case 0:
        for (size_t i = 0; i < n; ++i) {
          if (!through && v[i].clip[3] <= 0) continue;
          point(sv(i));
        }
        break;
      case 1:
      case 2:
        for (size_t i = 0; i + 1 < n; i += (prim == 1 ? 2 : 1)) {
          if (!through && (v[i].clip[3] <= 0 || v[i + 1].clip[3] <= 0)) continue;
          line(sv(i), sv(i + 1));
        }
        break;
      case 3:
        for (size_t i = 0; i + 2 < n; i += 3) emit_tri(vt, v[i], v[i + 1], v[i + 2]);
        break;
      case 4:
        for (size_t i = 0; i + 2 < n; ++i) {
          if (i & 1) emit_tri(vt, v[i + 1], v[i], v[i + 2]);
          else emit_tri(vt, v[i], v[i + 1], v[i + 2]);
        }
        break;
      case 5:
        for (size_t i = 1; i + 1 < n; ++i) emit_tri(vt, v[0], v[i], v[i + 1]);
        break;
      case 6:
        for (size_t i = 0; i + 1 < n; i += 2) {
          if (!through && (v[i].clip[3] <= 0 || v[i + 1].clip[3] <= 0)) continue;
          rect(sv(i), sv(i + 1));
        }
        break;
      default: break;
    }
  }
};

namespace {

// Reads vertex #i, honouring the index buffer and morph targets.
void fetch(const Gpu& g, const VType& vt, uint32_t vaddr, uint32_t iaddr, uint32_t i, RawVert& out) {
  uint32_t index = i;
  if (vt.idx) {
    uint32_t sz = comp_size(vt.idx);
    const uint8_t* p = gptr(iaddr + i * sz);
    if (sz == 1) index = *p;
    else if (sz == 2) {
      uint16_t v;
      std::memcpy(&v, p, 2);
      index = v;
    } else std::memcpy(&index, p, 4);
  }
  uint32_t addr = vaddr + index * vt.size * vt.mcount;
  if (vt.mcount == 1) {
    decode_vertex(vt, addr, out);
    return;
  }
  RawVert acc;
  std::memset(&acc, 0, sizeof acc);
  RawVert first;
  for (int m = 0; m < vt.mcount; ++m) {
    RawVert t;
    decode_vertex(vt, addr, t, m);
    float w = f24(g.cmd(0x2C + m));
    if (m == 0) first = t;
    for (int k = 0; k < 3; ++k) {
      acc.pos[k] += t.pos[k] * w;
      acc.nrm[k] += t.nrm[k] * w;
    }
    for (int k = 0; k < 2; ++k) acc.uv[k] += t.uv[k] * w;
  }
  first.pos[0] = acc.pos[0]; first.pos[1] = acc.pos[1]; first.pos[2] = acc.pos[2];
  first.nrm[0] = acc.nrm[0]; first.nrm[1] = acc.nrm[1]; first.nrm[2] = acc.nrm[2];
  first.uv[0] = acc.uv[0]; first.uv[1] = acc.uv[1];
  out = first;
}

}  // namespace

void Gpu::draw_prim(uint32_t arg) {
  const uint32_t count = arg & 0xFFFF;
  const int prim = (arg >> 16) & 7;
  const VType vt = parse_vtype(r_[0x12]);
  if (stats_) {
    stats_->draws++;
    stats_->vertices += count;
    stats_->primCount[prim]++;
    stats_->vertexTypes[vt.raw & 0x00FFFFFF]++;
    if (vt.through) stats_->throughDraws++;
    if (vt.wcount) stats_->skinnedDraws++;
    if (r_[0xD3] & 1) stats_->clears++;
    else {
      if (r_[0x1E] & 1) stats_->texturedDraws++;
      if (r_[0x17] & 1) stats_->lightedDraws++;
      if (r_[0x21] & 1) stats_->blendedDraws++;
    }
    stats_->fbFormats[r_[0xD2] & 3]++;
  }
  if (!count || !vt.pos || prim > 6) return;
  Raster R(*this);
  if (!R.begin()) return;
  std::vector<CV> verts(count);
  try {
    for (uint32_t i = 0; i < count; ++i) {
      RawVert rv;
      fetch(*this, vt, vaddr_, iaddr_, i, rv);
      R.transform(vt, rv, verts[i]);
    }
  } catch (const std::runtime_error&) {
    return;  // vertex data outside of emulated memory
  }
  if (dump_ && std::getenv("MGSPO_VDUMP"))
    for (uint32_t i = 0; i < count && i < 8; ++i)
      std::fprintf(dump_, "    v%u pos %.1f %.1f %.1f uv %.4f %.4f col %.2f %.2f %.2f %.2f\n", i, verts[i].clip[0],
                   verts[i].clip[1], verts[i].clip[2], verts[i].u, verts[i].v, verts[i].col[0], verts[i].col[1],
                   verts[i].col[2], verts[i].col[3]);
  auto t0 = std::chrono::steady_clock::now();
  R.assemble(vt, verts, prim);
  if (stats_) {
    int cat = (r_[0xD3] & 1) ? 0 : (R.texOn ? (R.blendOn ? 3 : 2) : 1);
    stats_->catNs[cat] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
  }
}

// ---- patches ----

namespace {

void bernstein(float t, float* b) {
  float s = 1 - t;
  b[0] = s * s * s;
  b[1] = 3 * t * s * s;
  b[2] = 3 * t * t * s;
  b[3] = t * t * t;
}

float basis(const std::vector<float>& knots, int i, int k, float t) {
  if (k == 0) return (t >= knots[i] && t < knots[i + 1]) ? 1.0f : 0.0f;
  float a = 0, b = 0;
  float d1 = knots[i + k] - knots[i], d2 = knots[i + k + 1] - knots[i + 1];
  if (d1 > 0) a = (t - knots[i]) / d1 * basis(knots, i, k - 1, t);
  if (d2 > 0) b = (knots[i + k + 1] - t) / d2 * basis(knots, i + 1, k - 1, t);
  return a + b;
}

std::vector<float> make_knots(int n, int type) {
  std::vector<float> k(n + 4);
  for (int i = 0; i < n + 4; ++i) {
    float v = static_cast<float>(i);
    if ((type & 1) && i <= 3) v = 3;
    if ((type & 2) && i >= n) v = static_cast<float>(n);
    k[i] = v;
  }
  return k;
}

}  // namespace

void Gpu::draw_patch(uint32_t arg, bool spline) {
  const int ucount = arg & 0xFF, vcount = (arg >> 8) & 0xFF;
  const int utype = (arg >> 16) & 3, vtype = (arg >> 18) & 3;
  const VType vt = parse_vtype(r_[0x12]);
  if (stats_) {
    stats_->draws++;
    stats_->vertices += static_cast<uint64_t>(ucount) * vcount;
    stats_->vertexTypes[vt.raw & 0x00FFFFFF]++;
  }
  if (!vt.pos) return;
  if (spline ? (ucount < 4 || vcount < 4) : (ucount < 4 || vcount < 4 || (ucount - 1) % 3 || (vcount - 1) % 3)) return;
  const int divU = std::clamp<int>(r_[0x36] & 0xFF, 1, 64), divV = std::clamp<int>((r_[0x36] >> 8) & 0xFF, 1, 64);
  Raster R(*this);
  if (!R.begin()) return;

  std::vector<RawVert> cp(static_cast<size_t>(ucount) * vcount);
  try {
    for (size_t i = 0; i < cp.size(); ++i) fetch(*this, vt, vaddr_, iaddr_, static_cast<uint32_t>(i), cp[i]);
  } catch (const std::runtime_error&) {
    return;
  }

  const int spansU = spline ? ucount - 3 : (ucount - 1) / 3;
  const int spansV = spline ? vcount - 3 : (vcount - 1) / 3;
  const int nu = spansU * divU + 1, nv = spansV * divV + 1;
  std::vector<float> ku, kv;
  if (spline) {
    ku = make_knots(ucount, utype);
    kv = make_knots(vcount, vtype);
  }

  auto blend_into = [](RawVert& o, const RawVert& s, float w) {
    for (int k = 0; k < 3; ++k) {
      o.pos[k] += s.pos[k] * w;
      o.nrm[k] += s.nrm[k] * w;
    }
    for (int k = 0; k < 2; ++k) o.uv[k] += s.uv[k] * w;
    for (int k = 0; k < 4; ++k) o.col[k] += s.col[k] * w;
  };

  std::vector<float> wuTab, wvTab;
  if (spline) {
    // the weights only depend on the grid column or row, so compute them once per patch
    wuTab.resize(static_cast<size_t>(nu) * ucount);
    wvTab.resize(static_cast<size_t>(nv) * vcount);
    for (int i = 0; i < nu; ++i) {
      const float gu = static_cast<float>(i) / (nu - 1);
      const float tu = 3 + gu * (ucount - 3) - (gu >= 1 ? 1e-4f : 0);
      for (int a = 0; a < ucount; ++a) wuTab[static_cast<size_t>(i) * ucount + a] = basis(ku, a, 3, tu);
    }
    for (int j = 0; j < nv; ++j) {
      const float gv = static_cast<float>(j) / (nv - 1);
      const float tv = 3 + gv * (vcount - 3) - (gv >= 1 ? 1e-4f : 0);
      for (int b = 0; b < vcount; ++b) wvTab[static_cast<size_t>(j) * vcount + b] = basis(kv, b, 3, tv);
    }
  }

  std::vector<CV> grid(static_cast<size_t>(nu) * nv);
  for (int j = 0; j < nv; ++j) {
    for (int i = 0; i < nu; ++i) {
      RawVert o;
      std::memset(o.pos, 0, sizeof o.pos);
      std::memset(o.nrm, 0, sizeof o.nrm);
      std::memset(o.uv, 0, sizeof o.uv);
      std::memset(o.col, 0, sizeof o.col);
      float gu = static_cast<float>(i) / (nu - 1), gv = static_cast<float>(j) / (nv - 1);
      if (!spline) {
        int pi = std::min(i / divU, spansU - 1), pj = std::min(j / divV, spansV - 1);
        float s = static_cast<float>(i - pi * divU) / divU, t = static_cast<float>(j - pj * divV) / divV;
        float bu[4], bv[4];
        bernstein(s, bu);
        bernstein(t, bv);
        for (int b = 0; b < 4; ++b)
          for (int a = 0; a < 4; ++a)
            blend_into(o, cp[static_cast<size_t>(pj * 3 + b) * ucount + pi * 3 + a], bu[a] * bv[b]);
      } else {
        const float* wuRow = &wuTab[static_cast<size_t>(i) * ucount];
        const float* wvRow = &wvTab[static_cast<size_t>(j) * vcount];
        for (int b = 0; b < vcount; ++b) {
          const float wv = wvRow[b];
          if (wv == 0) continue;
          for (int a = 0; a < ucount; ++a) {
            const float wu = wuRow[a];
            if (wu != 0) blend_into(o, cp[static_cast<size_t>(b) * ucount + a], wu * wv);
          }
        }
      }
      if (!vt.tc) {
        o.uv[0] = gu;
        o.uv[1] = gv;
      }
      if (vt.col < 4) {
        for (float& c : o.col) c = 1.0f;
      }
      R.transform(vt, o, grid[static_cast<size_t>(j) * nu + i]);
    }
  }
  for (int j = 0; j + 1 < nv; ++j) {
    for (int i = 0; i + 1 < nu; ++i) {
      const CV& a = grid[static_cast<size_t>(j) * nu + i];
      const CV& b = grid[static_cast<size_t>(j) * nu + i + 1];
      const CV& c = grid[static_cast<size_t>(j + 1) * nu + i];
      const CV& d = grid[static_cast<size_t>(j + 1) * nu + i + 1];
      R.emit_tri(vt, a, c, b);
      R.emit_tri(vt, b, c, d);
    }
  }
}

void Gpu::bbox(uint32_t arg) {
  const uint32_t count = arg & 0xFFFF;
  const VType vt = parse_vtype(r_[0x12]);
  bboxVisible_ = 1;
  if (!count || !vt.pos || vt.through) return;
  Raster R(*this);
  R.through = false;
  R.lighting = false;
  R.fogOn = false;
  int outside[6] = {0, 0, 0, 0, 0, 0};
  uint32_t n = std::min<uint32_t>(count, 8);
  try {
    for (uint32_t i = 0; i < n; ++i) {
      RawVert rv;
      fetch(*this, vt, vaddr_, iaddr_, i, rv);
      CV c;
      R.transform(vt, rv, c);
      const float w = c.clip[3];
      if (c.clip[0] < -w) outside[0]++;
      if (c.clip[0] > w) outside[1]++;
      if (c.clip[1] < -w) outside[2]++;
      if (c.clip[1] > w) outside[3]++;
      if (c.clip[2] < -w) outside[4]++;
      if (c.clip[2] > w) outside[5]++;
    }
  } catch (const std::runtime_error&) {
    return;
  }
  for (int p = 0; p < 6; ++p)
    if (static_cast<uint32_t>(outside[p]) == n) bboxVisible_ = 0;
}

}  // namespace ge
}  // namespace psp
