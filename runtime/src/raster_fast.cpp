#include "raster_fast.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace psp {
namespace ge {
namespace fast {

namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline int64_t fdiv(int64_t a, int64_t b) {  // b > 0
  int64_t q = a / b;
  if (a % b != 0 && a < 0) --q;
  return q;
}
inline int64_t cdiv(int64_t a, int64_t b) { return -fdiv(-a, b); }

inline bool compare(int func, uint32_t a, uint32_t b) {
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

inline uint32_t pack_px(int fmt, uint32_t c) {
  uint32_t r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
  switch (fmt) {
    case 0: return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    case 1: return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
    case 2: return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
    default: return c;
  }
}

// ---------------------------------------------------------------------------------------------
// texture cache

struct CacheEntry {
  uint32_t addr = 0;
  int fmt = 0, bufW = 0, w = 0, h = 0;
  bool swz = false;
  uint64_t hash = 0;
  uint64_t used = 0;
  std::vector<uint32_t> rgba;
  std::vector<uint8_t> idx;
};

std::vector<CacheEntry> g_cache;
uint64_t g_tick = 0;
constexpr size_t kMaxEntries = 96;
constexpr size_t kMaxBytes = 96u << 20;

uint64_t hash_bytes(const uint8_t* p, size_t n) {
  uint64_t h0 = 0x9E3779B97F4A7C15ull, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x85EBCA77C2B2AE63ull;
  constexpr uint64_t m = 0x100000001B3ull;
  size_t i = 0;
  for (; i + 32 <= n; i += 32) {
    uint64_t w[4];
    std::memcpy(w, p + i, 32);
    h0 = (h0 ^ w[0]) * m; h0 ^= h0 >> 29;
    h1 = (h1 ^ w[1]) * m; h1 ^= h1 >> 29;
    h2 = (h2 ^ w[2]) * m; h2 ^= h2 >> 29;
    h3 = (h3 ^ w[3]) * m; h3 ^= h3 >> 29;
  }
  for (; i < n; ++i) h0 = (h0 ^ p[i]) * m;
  return h0 ^ (h1 * 31) ^ (h2 * 131) ^ (h3 * 1031) ^ n;
}

void decode(const TexSource& s, CacheEntry& e) {
  const int bits = s.fmt == 3 ? 32 : (s.fmt == 4 ? 4 : (s.fmt == 5 ? 8 : 16));
  const uint32_t rowBytes = static_cast<uint32_t>(s.bufW) * bits / 8;
  const bool idx = s.fmt >= 4;
  if (idx) e.idx.assign(static_cast<size_t>(s.w) * s.h, 0);
  else e.rgba.assign(static_cast<size_t>(s.w) * s.h, 0);
  for (int y = 0; y < s.h; ++y) {
    for (int x = 0; x < s.w; ++x) {
      uint32_t xb = static_cast<uint32_t>(x) * bits / 8;
      size_t off = s.swizzled ? (static_cast<size_t>(y) / 8) * (static_cast<size_t>(rowBytes) * 8) + (xb / 16) * 128 + (y % 8) * 16 + (xb % 16)
                              : static_cast<size_t>(y) * rowBytes + xb;
      const uint8_t* p = s.mem + off;
      size_t o = static_cast<size_t>(y) * s.w + x;
      switch (s.fmt) {
        case 0: case 1: case 2: {
          uint16_t v;
          std::memcpy(&v, p, 2);
          e.rgba[o] = unpack_px(s.fmt, v);
          break;
        }
        case 3: std::memcpy(&e.rgba[o], p, 4); break;
        case 4: e.idx[o] = (x & 1) ? (*p >> 4) : (*p & 15); break;
        default: e.idx[o] = *p; break;
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------
// worker threads

class Pool {
 public:
  explicit Pool(int n) {
    for (int i = 0; i < n; ++i) workers_.emplace_back([this] { loop(); });
  }
  ~Pool() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }

  // Runs fn(0..bands-1) on the workers and the calling thread, returns when every band is done.
  void run(int bands, void (*fn)(void*, int), void* arg) {
    {
      std::lock_guard<std::mutex> lk(m_);
      fn_ = fn;
      arg_ = arg;
      bands_ = bands;
      next_ = 0;
      ++epoch_;
    }
    cv_.notify_all();
    work();
    // every band is claimed once next_ >= bands_; wait for the ones other threads are still running
    for (int spin = 0;; ++spin) {
      if (inflight_.load(std::memory_order_acquire) == 0) break;
      if (spin < 4000) cpu_pause();
      else std::this_thread::yield();
    }
  }

 private:
  static void cpu_pause() {
#if defined(_MSC_VER)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
  }

  void work() {
    for (;;) {
      void (*fn)(void*, int);
      void* arg;
      int b;
      {
        std::lock_guard<std::mutex> lk(m_);
        if (next_ >= bands_) return;
        b = next_++;
        fn = fn_;
        arg = arg_;
        inflight_.fetch_add(1, std::memory_order_relaxed);
      }
      fn(arg, b);
      inflight_.fetch_sub(1, std::memory_order_release);
    }
  }

  void loop() {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return stop_ || epoch_ != seen; });
        if (stop_) return;
        seen = epoch_;
      }
      work();
    }
  }

  std::vector<std::thread> workers_;
  std::mutex m_;
  std::condition_variable cv_;
  uint64_t epoch_ = 0;
  bool stop_ = false;
  void (*fn_)(void*, int) = nullptr;
  void* arg_ = nullptr;
  int bands_ = 0, next_ = 0;
  std::atomic<int> inflight_{0};
};

int g_threads = -1;
Pool* g_pool = nullptr;

int default_threads() {
  if (const char* e = std::getenv("MGSPO_GE_THREADS")) return std::max(1, std::atoi(e));
  unsigned hw = std::thread::hardware_concurrency();
  return static_cast<int>(std::clamp<unsigned>(hw / 2, 1, 6));
}

}  // namespace

void set_threads(int n) {
  if (!g_pool) g_threads = std::max(1, n);
}

namespace { int g_mode = -1; }

void set_mode(Mode m) { g_mode = static_cast<int>(m); }

Mode mode() {
  if (g_mode < 0) {
    g_mode = static_cast<int>(Mode::Fast);
    const char* v = std::getenv("MGSPO_GE_REF");
    if (v && v[0] && v[0] != '0') g_mode = static_cast<int>(Mode::Reference);
  }
  return static_cast<Mode>(g_mode);
}

int threads() {
  if (g_threads < 0) g_threads = default_threads();
  return g_threads;
}

bool texture_lookup(const TexSource& s, Texture& out) {
  if (s.fmt < 0 || s.fmt > 5 || s.w < 1 || s.h < 1 || s.bufW < 1) return false;
  const int bits = s.fmt == 3 ? 32 : (s.fmt == 4 ? 4 : (s.fmt == 5 ? 8 : 16));
  const size_t rowBytes = static_cast<size_t>(s.bufW) * bits / 8;
  const size_t rows = s.swizzled ? ((static_cast<size_t>(s.h) + 7) & ~size_t(7)) : static_cast<size_t>(s.h);
  const size_t len = rowBytes * rows;
  if (!rowBytes || len > s.avail) return false;
  if (s.swizzled && (rowBytes & 15)) return false;

  if (s.inVram) sync();
  const uint64_t h = hash_bytes(s.mem, len);
  ++g_tick;
  CacheEntry* hit = nullptr;
  for (CacheEntry& e : g_cache)
    if (e.addr == s.addr && e.fmt == s.fmt && e.swz == s.swizzled && e.bufW == s.bufW && e.w == s.w && e.h == s.h) {
      hit = &e;
      break;
    }
  if (!hit) {
    sync();
    size_t total = 0;
    for (const CacheEntry& e : g_cache) total += e.rgba.size() * 4 + e.idx.size();
    while (!g_cache.empty() && (g_cache.size() >= kMaxEntries || total > kMaxBytes)) {
      auto victim = std::min_element(g_cache.begin(), g_cache.end(), [](const CacheEntry& a, const CacheEntry& b) { return a.used < b.used; });
      total -= victim->rgba.size() * 4 + victim->idx.size();
      g_cache.erase(victim);
    }
    g_cache.emplace_back();
    hit = &g_cache.back();
    hit->addr = s.addr;
    hit->fmt = s.fmt;
    hit->swz = s.swizzled;
    hit->bufW = s.bufW;
    hit->w = s.w;
    hit->h = s.h;
    hit->hash = ~h;
  }
  if (hit->hash != h) {
    sync();
    decode(s, *hit);
    hit->hash = h;
  }
  hit->used = g_tick;
  out.rgba = s.fmt >= 4 ? nullptr : hit->rgba.data();
  out.idx = s.fmt >= 4 ? hit->idx.data() : nullptr;
  return true;
}

// ---------------------------------------------------------------------------------------------
// primitives

struct Batch::Prim {
  int y0, y1;
  int rx0, rx1;  // rectangles only
  bool rect;
  int64_t edx[3], edy[3], ebase[3];
  int tie[3];
  float ox, oy;
  float pl[9][3];  // z, r, g, b, a, u, v, w, fog as (value at origin, d/dx, d/dy)
  bool constCol, persp;
  int cc[4];
};

namespace {

using SpanFn = void (*)(const Ctx&, const Batch::Prim&, int, int, int);

inline uint32_t sample_texel(const Ctx& cx, int x, int y, int kind) {
  x = cx.clampU ? clampi(x, 0, cx.tW - 1) : (x & (cx.tW - 1));
  y = cx.clampV ? clampi(y, 0, cx.tH - 1) : (y & (cx.tH - 1));
  size_t o = static_cast<size_t>(y) * cx.tW + x;
  return kind == 2 ? cx.pal[cx.texIdx[o]] : cx.texRgba[o];
}

inline int ifloor(float f) {
  int i = static_cast<int>(f);
  return f < static_cast<float>(i) ? i - 1 : i;
}

template <int TEX>
inline uint32_t sample(const Ctx& cx, float u, float v) {
  float fx = u * cx.tW, fy = v * cx.tH;
  if (!cx.linear) return sample_texel(cx, ifloor(fx), ifloor(fy), TEX);
  fx -= 0.5f;
  fy -= 0.5f;
  int x0 = ifloor(fx), y0 = ifloor(fy);
  float ax = fx - x0, ay = fy - y0;
  uint32_t t00 = sample_texel(cx, x0, y0, TEX), t10 = sample_texel(cx, x0 + 1, y0, TEX);
  uint32_t t01 = sample_texel(cx, x0, y0 + 1, TEX), t11 = sample_texel(cx, x0 + 1, y0 + 1, TEX);
  uint32_t out = 0;
  for (int s = 0; s < 32; s += 8) {
    float top = (t00 >> s & 255) * (1 - ax) + (t10 >> s & 255) * ax;
    float bot = (t01 >> s & 255) * (1 - ax) + (t11 >> s & 255) * ax;
    out |= static_cast<uint32_t>(top * (1 - ay) + bot * ay + 0.5f) << s;
  }
  return out;
}

inline void tex_combine(const Ctx& cx, int* c, uint32_t t) {
  int tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
  if (!cx.tAlpha) ta = 255;
  const int er = cx.env & 255, eg = (cx.env >> 8) & 255, eb = (cx.env >> 16) & 255;
  switch (cx.tFunc) {
    case 0:
      c[0] = c[0] * tr / 255; c[1] = c[1] * tg / 255; c[2] = c[2] * tb / 255;
      if (cx.tAlpha) c[3] = c[3] * ta / 255;
      break;
    case 1:
      if (cx.tAlpha) {
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
      if (cx.tAlpha) c[3] = c[3] * ta / 255;
      break;
    case 3:
      c[0] = tr; c[1] = tg; c[2] = tb;
      if (cx.tAlpha) c[3] = ta;
      break;
    default:
      c[0] += tr; c[1] += tg; c[2] += tb;
      if (cx.tAlpha) c[3] = c[3] * ta / 255;
      break;
  }
  if (cx.tDouble) {
    c[0] *= 2; c[1] *= 2; c[2] *= 2;
  }
  for (int i = 0; i < 4; ++i) c[i] = clampi(c[i], 0, 255);
}

inline int blend_factor(const Ctx& cx, int f, bool isSrc, int ch, const int* src, const int* dc) {
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
      uint32_t fix = isSrc ? cx.fixA : cx.fixB;
      return ch == 3 ? src[3] : static_cast<int>((fix >> (8 * ch)) & 255);
    }
  }
}

template <int TEX, bool BLEND, bool ZT>
void span(const Ctx& cx, const Batch::Prim& p, int y, int xs, int xe) {
  const float py = y + 0.5f - p.oy;
  const float dxs = xs + 0.5f - p.ox;
  auto at = [&](int i) { return p.pl[i][0] + p.pl[i][1] * dxs + p.pl[i][2] * py; };
  float z = at(0);
  const float dz = p.pl[0][1];
  float col[4] = {0, 0, 0, 0}, dcol[4] = {0, 0, 0, 0};
  if (!p.constCol)
    for (int i = 0; i < 4; ++i) {
      col[i] = at(1 + i);
      dcol[i] = p.pl[1 + i][1];
    }
  float u = 0, v = 0, w = 0, du = 0, dv = 0, dw = 0;
  if (TEX) {
    u = at(5); v = at(6);
    du = p.pl[5][1]; dv = p.pl[6][1];
    if (p.persp) {
      w = at(7);
      dw = p.pl[7][1];
    }
  }
  const bool fb32 = cx.fbFmt == 3;
  const int fmt = cx.fbFmt;
  const uint32_t keep = cx.keep;
  uint8_t* frow = cx.fb + static_cast<size_t>(y) * cx.fbStride * (fb32 ? 4 : 2);
  uint16_t* zrow = ZT ? reinterpret_cast<uint16_t*>(cx.zb + static_cast<size_t>(y) * cx.zStride * 2) : nullptr;

  const bool fogOn = cx.fog;
  float fogv = fogOn ? at(8) : 0, dfog = fogOn ? p.pl[8][1] : 0;
  for (int x = xs; x <= xe; ++x, z += dz, u += du, v += dv, w += dw, fogv += dfog, col[0] += dcol[0], col[1] += dcol[1], col[2] += dcol[2], col[3] += dcol[3]) {
    if (cx.clipZ && (z < cx.zlo || z > cx.zhi)) continue;
    uint16_t zi = 0;
    if (ZT) {
      zi = static_cast<uint16_t>(clampi(static_cast<int>(z + 0.5f), 0, 65535));
      if (!compare(cx.zFunc, zi, zrow[x])) continue;
    }
    int c[4];
    if (p.constCol) {
      c[0] = p.cc[0]; c[1] = p.cc[1]; c[2] = p.cc[2]; c[3] = p.cc[3];
    } else {
      for (int i = 0; i < 4; ++i) c[i] = clampi(static_cast<int>(col[i] + 0.5f), 0, 255);
    }
    if (TEX) {
      float uu = u, vv = v;
      if (p.persp) {
        float iw = w != 0 ? 1.0f / w : 0.0f;
        uu = u * iw;
        vv = v * iw;
      }
      tex_combine(cx, c, sample<TEX>(cx, uu, vv));
    }
    if (fogOn) {
      const float f = std::clamp(fogv, 0.0f, 1.0f);
      for (int i = 0; i < 3; ++i) c[i] = static_cast<int>(c[i] * f + ((cx.fogCol >> (8 * i)) & 255) * (1 - f) + 0.5f);
    }
    if (cx.atest && !compare(cx.aFunc, static_cast<uint32_t>(c[3] & cx.aMask), static_cast<uint32_t>(cx.aRef & cx.aMask))) continue;

    uint32_t* p32 = reinterpret_cast<uint32_t*>(frow) + x;
    uint16_t* p16 = reinterpret_cast<uint16_t*>(frow) + x;
    if (BLEND) {
      uint32_t dst = fb32 ? *p32 : unpack_px(fmt, *p16);
      int src[4] = {c[0], c[1], c[2], c[3]};
      int dc[4] = {static_cast<int>(dst & 255), static_cast<int>((dst >> 8) & 255), static_cast<int>((dst >> 16) & 255), static_cast<int>(dst >> 24)};
      if (fmt == 0) dc[3] = 255;
      if (cx.sfac == 2 && cx.dfac == 3 && cx.bfunc == 0) {
        const int sa = src[3], ia = 255 - sa;
        for (int ch = 0; ch < 3; ++ch) c[ch] = std::min(255, src[ch] * sa / 255 + dc[ch] * ia / 255);
      } else {
        for (int ch = 0; ch < 3; ++ch) {
          int s = src[ch] * blend_factor(cx, cx.sfac, true, ch, src, dc) / 255;
          int d = dc[ch] * blend_factor(cx, cx.dfac, false, ch, src, dc) / 255;
          int o;
          switch (cx.bfunc) {
            case 0: o = s + d; break;
            case 1: o = s - d; break;
            case 2: o = d - s; break;
            case 3: o = std::min(src[ch], dc[ch]); break;
            case 4: o = std::max(src[ch], dc[ch]); break;
            default: o = std::abs(src[ch] - dc[ch]); break;
          }
          c[ch] = clampi(o, 0, 255);
        }
      }
    }
    const uint32_t rgba = static_cast<uint32_t>(c[0] | (c[1] << 8) | (c[2] << 16)) | (static_cast<uint32_t>(c[3]) << 24);
    if (fb32) {
      *p32 = keep ? ((*p32 & keep) | (rgba & ~keep)) : rgba;
    } else {
      uint32_t nv = pack_px(fmt, rgba);
      *p16 = static_cast<uint16_t>(keep ? ((*p16 & keep) | (nv & ~keep)) : nv);
    }
    if (ZT && cx.zWrite) zrow[x] = zi;
  }
}

SpanFn pick_span(const Ctx& cx) {
  const int tex = !cx.tex ? 0 : (cx.texIdx ? 2 : 1);
  const int idx = tex * 4 + (cx.blend ? 2 : 0) + (cx.zTest ? 1 : 0);
  static const SpanFn table[12] = {
      span<0, false, false>, span<0, false, true>, span<0, true, false>, span<0, true, true>,
      span<1, false, false>, span<1, false, true>, span<1, true, false>, span<1, true, true>,
      span<2, false, false>, span<2, false, true>, span<2, true, false>, span<2, true, true>,
  };
  return table[idx];
}

bool row_span(const Batch::Prim& p, const Ctx& cx, int y, int& xs, int& xe) {
  if (p.rect) {
    xs = p.rx0;
    xe = p.rx1;
    return xs <= xe;
  }
  int64_t lo = cx.sx1, hi = cx.sx2;
  for (int k = 0; k < 3; ++k) {
    const int64_t c0 = p.ebase[k] + 16 * p.edx[k] * y - p.tie[k];
    const int64_t s = 16 * p.edy[k];
    if (s > 0) hi = std::min(hi, fdiv(c0, s));
    else if (s < 0) lo = std::max(lo, cdiv(-c0, -s));
    else if (c0 < 0) return false;
  }
  xs = static_cast<int>(lo);
  xe = static_cast<int>(hi);
  return xs <= xe;
}

struct Draw {
  Ctx cx;
  uint32_t pal[256];
  bool hasPal = false;
  SpanFn fn = nullptr;
  size_t p0 = 0, p1 = 0;
  int minRow = 0x7FFFFFFF, maxRow = -1;
  int cat = 1;
};

std::vector<Draw> g_draws;
std::vector<Batch::Prim> g_prims;
int64_t g_area = 0;
int g_minRow = 0x7FFFFFFF, g_maxRow = -1;
uint64_t g_epoch = 0;
uint64_t* g_fragSink = nullptr;
uint64_t* g_nsSink = nullptr;

struct Job {
  int minRow, rowsPerBand, maxRow;
  std::atomic<uint64_t> fragments[4];
};

void run_rows(int r0, int r1, uint64_t* frags) {
  for (Draw& d : g_draws) {
    if (d.maxRow < r0 || d.minRow >= r1) continue;
    const Ctx& cx = d.cx;
    uint64_t n = 0;
    for (size_t i = d.p0; i < d.p1; ++i) {
      const Batch::Prim& p = g_prims[i];
      const int ya = std::max(p.y0, r0), yb = std::min(p.y1, r1 - 1);
      for (int y = ya; y <= yb; ++y) {
        int xs, xe;
        if (!row_span(p, cx, y, xs, xe)) continue;
        n += static_cast<uint64_t>(xe - xs + 1);
        d.fn(cx, p, y, xs, xe);
      }
    }
    frags[d.cat] += n;
  }
}

void band_entry(void* arg, int band) {
  Job* j = static_cast<Job*>(arg);
  const int r0 = j->minRow + band * j->rowsPerBand;
  uint64_t frags[4] = {};
  run_rows(r0, std::min(r0 + j->rowsPerBand, j->maxRow + 1), frags);
  for (int i = 0; i < 4; ++i) j->fragments[i].fetch_add(frags[i], std::memory_order_relaxed);
}

bool same_target(const Ctx& a, const Ctx& b) {
  return a.fb == b.fb && a.fbStride == b.fbStride && a.fbFmt == b.fbFmt && a.zb == b.zb && a.zStride == b.zStride;
}

}  // namespace

void set_stats_sink(uint64_t* fragments, uint64_t* catNs) {
  g_fragSink = fragments;
  g_nsSink = catNs;
}

void sync() {
  if (g_draws.empty()) return;
  const auto t0 = std::chrono::steady_clock::now();
  for (Draw& d : g_draws)
    if (d.hasPal) d.cx.pal = d.pal;
  const int nt = threads();
  const int rows = g_maxRow - g_minRow + 1;
  uint64_t frags[4] = {};
  if (nt > 1 && g_area >= 20000 && rows >= 16) {
    if (!g_pool) g_pool = new Pool(nt - 1);
    Job job;
    for (auto& f : job.fragments) f.store(0);
    job.minRow = g_minRow;
    job.maxRow = g_maxRow;
    job.rowsPerBand = std::max(4, (rows + nt * 3 - 1) / (nt * 3));
    const int bands = (rows + job.rowsPerBand - 1) / job.rowsPerBand;
    g_pool->run(bands, band_entry, &job);
    for (int i = 0; i < 4; ++i) frags[i] = job.fragments[i].load();
  } else {
    run_rows(g_minRow, g_maxRow + 1, frags);
  }
  const uint64_t total = frags[1] + frags[2] + frags[3];
  if (g_fragSink) *g_fragSink += total;
  if (g_nsSink && total) {
    const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    for (int i = 1; i < 4; ++i) g_nsSink[i] += ns * frags[i] / total;
  }
  g_draws.clear();
  g_prims.clear();
  g_area = 0;
  g_minRow = 0x7FFFFFFF;
  g_maxRow = -1;
  ++g_epoch;
}

void Batch::begin(const Ctx* cx, const uint32_t* pal, int cat) {
  cx_ = cx;
  pal_ = pal;
  cat_ = cat;
  epoch_ = ~0ull;
}

void Batch::open() {
  if (epoch_ == g_epoch) return;
  if (!g_draws.empty() && !same_target(g_draws.back().cx, *cx_)) sync();
  g_draws.emplace_back();
  Draw& d = g_draws.back();
  d.cx = *cx_;
  d.hasPal = d.cx.texIdx && pal_;
  if (d.hasPal) std::memcpy(d.pal, pal_, sizeof d.pal);
  d.fn = pick_span(d.cx);
  d.p0 = d.p1 = g_prims.size();
  d.cat = cat_;
  epoch_ = g_epoch;
}
bool Batch::add_tri(const Vert& a0, const Vert& b0, const Vert& c0) {
  const Ctx& cx = *this->cx_;
  auto sane = [](float f) { return std::fabs(f) <= 8192.0f; };
  if (!(sane(a0.x) && sane(a0.y) && sane(b0.x) && sane(b0.y) && sane(c0.x) && sane(c0.y))) return false;
  const Vert* va = &a0;
  const Vert* vb = &b0;
  const Vert* vc = &c0;
  auto snap = [](float f) { return static_cast<int64_t>(std::lrintf(f * 16.0f)); };
  int64_t ax = snap(va->x), ay = snap(va->y), bx = snap(vb->x), by = snap(vb->y), ccx = snap(vc->x), cy = snap(vc->y);
  int64_t area2 = (bx - ax) * (cy - ay) - (by - ay) * (ccx - ax);
  if (area2 == 0) return true;
  if (area2 < 0) {
    std::swap(vb, vc);
    std::swap(bx, ccx);
    std::swap(by, cy);
    area2 = -area2;
  }
  const int64_t minY = std::min({ay, by, cy}), maxY = std::max({ay, by, cy});
  const int64_t minX = std::min({ax, bx, ccx}), maxX = std::max({ax, bx, ccx});
  int ya = static_cast<int>(std::max<int64_t>(cx.sy1, cdiv(minY - 8, 16)));
  int yb = static_cast<int>(std::min<int64_t>(cx.sy2, fdiv(maxY - 8, 16)));
  if (ya > yb) return true;
  int64_t xa = std::max<int64_t>(cx.sx1, cdiv(minX - 8, 16)), xb = std::min<int64_t>(cx.sx2, fdiv(maxX - 8, 16));
  if (xa > xb) return true;

  Prim p;
  p.rect = false;
  p.y0 = ya;
  p.y1 = yb;
  p.rx0 = p.rx1 = 0;
  const int64_t px[3] = {bx, ccx, ax}, py[3] = {by, cy, ay}, qx[3] = {ccx, ax, bx}, qy[3] = {cy, ay, by};
  for (int k = 0; k < 3; ++k) {
    const int64_t dx = qx[k] - px[k], dy = qy[k] - py[k];
    p.edx[k] = dx;
    p.edy[k] = dy;
    p.ebase[k] = dx * (8 - py[k]) - dy * (8 - px[k]);
    const bool topLeft = dy < 0 || (dy == 0 && dx > 0);
    p.tie[k] = topLeft ? 0 : 1;
  }
  const float fax = ax / 16.0f, fay = ay / 16.0f;
  const float bxx = bx / 16.0f - fax, byy = by / 16.0f - fay, cxx = ccx / 16.0f - fax, cyy = cy / 16.0f - fay;
  const float invD = 1.0f / (bxx * cyy - cxx * byy);
  p.ox = fax;
  p.oy = fay;
  auto plane = [&](int i, float fa, float fb, float fc) {
    const float db = fb - fa, dc = fc - fa;
    p.pl[i][0] = fa;
    p.pl[i][1] = (db * cyy - dc * byy) * invD;
    p.pl[i][2] = (dc * bxx - db * cxx) * invD;
  };
  plane(0, va->z, vb->z, vc->z);
  if (cx.fog) plane(8, va->fog, vb->fog, vc->fog);
  p.constCol = true;
  for (int i = 0; i < 4; ++i)
    if (va->col[i] != vb->col[i] || vb->col[i] != vc->col[i]) p.constCol = false;
  if (p.constCol) {
    for (int i = 0; i < 4; ++i) p.cc[i] = clampi(static_cast<int>(va->col[i] * 255.0f + 0.5f), 0, 255);
  } else {
    for (int i = 0; i < 4; ++i) plane(1 + i, va->col[i] * 255.0f, vb->col[i] * 255.0f, vc->col[i] * 255.0f);
  }
  p.persp = cx.tex && !(va->invw == vb->invw && vb->invw == vc->invw);
  if (cx.tex) {
    if (p.persp) {
      plane(5, va->u * va->invw, vb->u * vb->invw, vc->u * vc->invw);
      plane(6, va->v * va->invw, vb->v * vb->invw, vc->v * vc->invw);
      plane(7, va->invw, vb->invw, vc->invw);
    } else {
      plane(5, va->u, vb->u, vc->u);
      plane(6, va->v, vb->v, vc->v);
    }
  }
  open();
  g_prims.push_back(p);
  Draw& dr = g_draws.back();
  dr.p1 = g_prims.size();
  dr.minRow = std::min(dr.minRow, ya);
  dr.maxRow = std::max(dr.maxRow, yb);
  g_area += (xb - xa + 1) * (yb - ya + 1);
  g_minRow = std::min(g_minRow, ya);
  g_maxRow = std::max(g_maxRow, yb);
  return true;
}

bool Batch::add_rect(const Vert& a, const Vert& b) {
  const Ctx& cx = *cx_;
  float x0 = a.x, x1 = b.x, y0 = a.y, y1 = b.y;
  float u0 = a.u, u1 = b.u, v0 = a.v, v1 = b.v;
  if (!(std::fabs(x0) <= 1e6f && std::fabs(x1) <= 1e6f && std::fabs(y0) <= 1e6f && std::fabs(y1) <= 1e6f)) return false;
  if (x1 < x0) {
    std::swap(x0, x1);
    std::swap(u0, u1);
  }
  if (y1 < y0) {
    std::swap(y0, y1);
    std::swap(v0, v1);
  }
  const float w = x1 - x0, h = y1 - y0;
  if (w <= 0 || h <= 0) return true;
  const int ix0 = std::max(cx.sx1, static_cast<int>(std::ceil(x0 - 0.5f)));
  const int ix1 = std::min(cx.sx2, static_cast<int>(std::ceil(x1 - 0.5f)) - 1);
  const int iy0 = std::max(cx.sy1, static_cast<int>(std::ceil(y0 - 0.5f)));
  const int iy1 = std::min(cx.sy2, static_cast<int>(std::ceil(y1 - 0.5f)) - 1);
  if (ix0 > ix1 || iy0 > iy1) return true;
  Prim p;
  p.rect = true;
  p.y0 = iy0;
  p.y1 = iy1;
  p.rx0 = ix0;
  p.rx1 = ix1;
  p.ox = x0;
  p.oy = y0;
  std::memset(p.pl, 0, sizeof p.pl);
  p.pl[0][0] = std::clamp(b.z, 0.0f, 65535.0f);
  p.pl[8][0] = b.fog;
  p.constCol = true;
  for (int i = 0; i < 4; ++i) p.cc[i] = clampi(static_cast<int>(b.col[i] * 255.0f + 0.5f), 0, 255);
  p.persp = false;
  p.pl[5][0] = u0;
  p.pl[5][1] = (u1 - u0) / w;
  p.pl[6][0] = v0;
  p.pl[6][2] = (v1 - v0) / h;
  open();
  g_prims.push_back(p);
  Draw& dr = g_draws.back();
  dr.p1 = g_prims.size();
  dr.minRow = std::min(dr.minRow, iy0);
  dr.maxRow = std::max(dr.maxRow, iy1);
  g_area += static_cast<int64_t>(ix1 - ix0 + 1) * (iy1 - iy0 + 1);
  g_minRow = std::min(g_minRow, iy0);
  g_maxRow = std::max(g_maxRow, iy1);
  return true;
}

void clear_rect(const Ctx& cx, int x0, int y0, int x1, int y1, bool clearColour, bool clearAlpha, bool clearDepth, uint32_t rgba, uint16_t z) {
  const bool fb32 = cx.fbFmt == 3;
  if (clearColour || clearAlpha) {
    uint32_t keep = 0;
    const uint32_t rgbMask = fb32 ? 0x00FFFFFFu : pack_px(cx.fbFmt, 0x00FFFFFFu);
    uint32_t aMask = fb32 ? 0xFF000000u : pack_px(cx.fbFmt, 0xFF000000u);
    if (cx.fbFmt == 0) aMask = 0;
    if (!clearColour) keep |= rgbMask;
    if (!clearAlpha) keep |= aMask;
    const uint32_t nv = fb32 ? rgba : pack_px(cx.fbFmt, rgba);
    for (int y = y0; y <= y1; ++y) {
      uint8_t* row = cx.fb + static_cast<size_t>(y) * cx.fbStride * (fb32 ? 4 : 2);
      if (fb32) {
        uint32_t* d = reinterpret_cast<uint32_t*>(row) + x0;
        const int n = x1 - x0 + 1;
        if (keep == 0) std::fill(d, d + n, nv);
        else for (int i = 0; i < n; ++i) d[i] = (d[i] & keep) | (nv & ~keep);
      } else {
        uint16_t* d = reinterpret_cast<uint16_t*>(row) + x0;
        const int n = x1 - x0 + 1;
        const uint16_t v = static_cast<uint16_t>(nv);
        if (keep == 0) std::fill(d, d + n, v);
        else for (int i = 0; i < n; ++i) d[i] = static_cast<uint16_t>((d[i] & keep) | (v & ~keep));
      }
    }
  }
  if (clearDepth && cx.zb) {
    for (int y = y0; y <= y1; ++y) {
      uint16_t* d = reinterpret_cast<uint16_t*>(cx.zb + static_cast<size_t>(y) * cx.zStride * 2) + x0;
      std::fill(d, d + (x1 - x0 + 1), z);
    }
  }
}

}  // namespace fast
}  // namespace ge
}  // namespace psp
