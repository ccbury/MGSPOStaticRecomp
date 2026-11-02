#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "cpu.h"

namespace psp {

inline uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

inline uint32_t rotr(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

inline uint32_t clz(uint32_t v) {
  uint32_t n = 0;
  for (uint32_t m = 0x80000000u; m && !(v & m); m >>= 1) ++n;
  return n;
}
inline uint32_t clo(uint32_t v) { return clz(~v); }

inline uint32_t ext(uint32_t v, uint32_t pos, uint32_t size) {
  uint32_t mask = size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1);
  return (v >> pos) & mask;
}
inline uint32_t ins(uint32_t dst, uint32_t src, uint32_t pos, uint32_t size) {
  uint32_t mask = (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1)) << pos;
  return (dst & ~mask) | ((src << pos) & mask);
}

inline uint32_t wsbh(uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }
inline uint32_t wsbw(uint32_t v) {
  return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
}
inline uint32_t seb(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v))); }
inline uint32_t seh(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); }
inline uint32_t bitrev(uint32_t v) {
  uint32_t r = 0;
  for (int i = 0; i < 32; ++i) r |= ((v >> i) & 1u) << (31 - i);
  return r;
}

inline uint32_t max_s(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a) > static_cast<int32_t>(b) ? a : b;
}
inline uint32_t min_s(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a) < static_cast<int32_t>(b) ? a : b;
}

inline void mult(Cpu& c, uint32_t a, uint32_t b) {
  int64_t p = static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b);
  c.lo = static_cast<uint32_t>(p);
  c.hi = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
}
inline void multu(Cpu& c, uint32_t a, uint32_t b) {
  uint64_t p = static_cast<uint64_t>(a) * b;
  c.lo = static_cast<uint32_t>(p);
  c.hi = static_cast<uint32_t>(p >> 32);
}
inline void div(Cpu& c, uint32_t a, uint32_t b) {
  int32_t x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
  if (y == 0) {
    c.lo = x < 0 ? 1u : 0xFFFFFFFFu;
    c.hi = a;
  } else if (x == INT32_MIN && y == -1) {
    c.lo = 0x80000000u;
    c.hi = 0;
  } else {
    c.lo = static_cast<uint32_t>(x / y);
    c.hi = static_cast<uint32_t>(x % y);
  }
}
inline void divu(Cpu& c, uint32_t a, uint32_t b) {
  if (b == 0) {
    c.lo = 0xFFFFFFFFu;
    c.hi = a;
  } else {
    c.lo = a / b;
    c.hi = a % b;
  }
}

inline uint64_t hilo(const Cpu& c) { return (static_cast<uint64_t>(c.hi) << 32) | c.lo; }
inline void set_hilo(Cpu& c, uint64_t v) {
  c.lo = static_cast<uint32_t>(v);
  c.hi = static_cast<uint32_t>(v >> 32);
}
inline void madd(Cpu& c, uint32_t a, uint32_t b) {
  set_hilo(c, hilo(c) + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b)));
}
inline void maddu(Cpu& c, uint32_t a, uint32_t b) { set_hilo(c, hilo(c) + static_cast<uint64_t>(a) * b); }
inline void msub(Cpu& c, uint32_t a, uint32_t b) {
  set_hilo(c, hilo(c) - static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b)));
}
inline void msubu(Cpu& c, uint32_t a, uint32_t b) { set_hilo(c, hilo(c) - static_cast<uint64_t>(a) * b); }

// Float -> int32 conversions saturate the way the PSP FPU does.
inline uint32_t sat_w(double v) {
  if (v != v || v >= 2147483648.0) return 0x7FFFFFFFu;
  if (v < -2147483648.0) return 0x80000000u;
  return static_cast<uint32_t>(static_cast<int32_t>(v));
}
inline uint32_t round_w(float f) { return sat_w(std::nearbyint(f)); }
inline uint32_t trunc_w(float f) { return sat_w(std::trunc(f)); }
inline uint32_t ceil_w(float f) { return sat_w(std::ceil(f)); }
inline uint32_t floor_w(float f) { return sat_w(std::floor(f)); }
inline uint32_t cvt_w(float f) { return round_w(f); }

inline bool fcmp(float a, float b, uint32_t cond) {
  bool un = (a != a) || (b != b);
  bool eq = !un && a == b;
  bool lt = !un && a < b;
  return ((cond & 1) && un) || ((cond & 2) && eq) || ((cond & 4) && lt);
}

inline uint32_t cfc1(const Cpu& c, uint32_t reg) {
  if (reg == 31) return (c.fcr31 & ~0x00800000u) | (c.fcc ? 0x00800000u : 0u);
  if (reg == 0) return 0x00003351;
  return 0;
}
inline void ctc1(Cpu& c, uint32_t reg, uint32_t v) {
  if (reg != 31) return;
  c.fcr31 = v & 0x0183FFFFu;
  c.fcc = (v >> 23) & 1;
}

}  // namespace psp
