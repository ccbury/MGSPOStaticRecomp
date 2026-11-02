#include <psp/vfpu.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <psp/memory.h>

namespace psp {
namespace vfpu {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline uint32_t bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
inline float flt(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline int32_t sbits(float f) { return static_cast<int32_t>(bits(f)); }

// Row/column/transpose breakdown shared by vector and matrix register numbers.
struct RegPos {
  unsigned mtx, col, row;
  bool transposed;
};

RegPos regPos(uint32_t reg, unsigned n) {
  RegPos p;
  p.mtx = (reg >> 2) & 7;
  p.col = reg & 3;
  if (n == 1) {
    p.transposed = false;
    p.row = (reg >> 5) & 3;
  } else {
    p.transposed = (reg >> 5) & 1;
    p.row = n == 3 ? (reg >> 6) & 1 : (reg >> 5) & 2;
  }
  return p;
}

// Slots of an n x n matrix, stored as idx[col * 4 + row].
void matrixSlots(uint32_t reg, unsigned n, unsigned idx[16]) {
  RegPos p = regPos(reg, n);
  for (unsigned j = 0; j < n; ++j)
    for (unsigned i = 0; i < n; ++i) {
      unsigned a = (p.row + i) & 3, b = (p.col + j) & 3;
      idx[j * 4 + i] = p.mtx * 16 + (p.transposed ? a * 4 + b : b * 4 + a);
    }
}

double quarterTurn(double x, bool cosine) {
  if (!std::isfinite(x)) return std::nan("");
  double r = std::fmod(x + (cosine ? 1.0 : 0.0), 4.0);
  if (r < 0) r += 4.0;
  if (r == 0.0 || r == 2.0) return 0.0;
  if (r == 1.0) return 1.0;
  if (r == 3.0) return -1.0;
  return std::sin(r * kPi / 2.0);
}

const float kConstants[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};

// S/T prefix: swizzle, abs, constant substitution and negate, per lane.
void applyPrefix(uint32_t p, unsigned n, float* v) {
  float in[4] = {v[0], v[1], v[2], v[3]};
  for (unsigned i = 0; i < n; ++i) {
    unsigned sw = (p >> (2 * i)) & 3;
    bool abs = (p >> (8 + i)) & 1, cst = (p >> (12 + i)) & 1, neg = (p >> (16 + i)) & 1;
    float x;
    if (cst) {
      x = kConstants[sw + (abs ? 4 : 0)];
    } else {
      x = sw < n ? in[sw] : 0.f;
      if (abs) x = std::fabs(x);
    }
    v[i] = neg ? -x : x;
  }
}

float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

float halfToFloat(uint32_t h) {
  uint32_t sign = (h & 0x8000) << 16, exp = (h >> 10) & 31, mant = h & 0x3FF;
  if (exp == 0) return flt(sign);
  if (exp == 31) return flt(sign | 0x7F800000u | mant);
  return flt(sign | ((exp + 112) << 23) | (mant << 13));
}

uint32_t floatToHalf(float f) {
  uint32_t b = bits(f), sign = (b >> 16) & 0x8000;
  int exp = static_cast<int>((b >> 23) & 0xFF);
  uint32_t mant = b & 0x7FFFFF;
  if (exp == 255) return sign | 0x7C00 | (mant >> 13);
  exp -= 112;
  if (exp <= 0) return sign;
  if (exp >= 31) return sign | 0x7C00;
  return sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
}

uint32_t randomStep(uint32_t* rcx) {
  uint32_t A = (rcx[0] & 0xFFFF) | (rcx[4] << 16);
  uint32_t B = (rcx[1] & 0xFFFF) | (rcx[5] << 16);
  uint32_t C = (rcx[2] & 0xFFFF) | (rcx[6] << 16);
  uint32_t D = (rcx[3] & 0xFFFF) | (rcx[7] << 16);
  uint32_t E = 0;
  for (int i = 0; i < 8; ++i) E |= ((rcx[i] >> 16) & 0xF) << (4 * i);

  A = 69069u * A + 1u;
  B ^= B << 13;
  B ^= B >> 17;
  B ^= B << 5;
  uint32_t t = 2u * D + C + E;
  E = static_cast<uint32_t>((uint64_t(C) + uint64_t(D >> 1) + uint64_t(E)) >> 32);
  C = D;
  D = t;
  uint32_t lo[4] = {A, B, C, D}, hi[4] = {A >> 16, B >> 16, C >> 16, D >> 16};
  for (int i = 0; i < 4; ++i) {
    rcx[i] = 0x3F800000u | (((E >> (4 * i)) & 0xF) << 16) | (lo[i] & 0xFFFF);
    rcx[i + 4] = 0x3F800000u | (((E >> (4 * (i + 4))) & 0xF) << 16) | (hi[i] & 0xFFFF);
  }
  return A + B + D;
}

void seedRandom(uint32_t seed, uint32_t* rcx) {
  for (int i = 0; i < 8; ++i)
    rcx[i] = 0x3F800000u | ((seed >> ((i / 4) * 16)) & 0xFFFF) | (((seed >> (4 * i)) & 0xF) << 16);
}

void setControl(Cpu& c, unsigned reg, uint32_t value) {
  switch (reg) {
    case 0: case 1: c.vctrl[reg] = value & 0xFFFFF; break;
    case 2: c.vctrl[2] = value & 0xFFF; break;
    case 3: c.vctrl[3] = value & 0x3F; break;
    case 4: c.vctrl[4] = value; break;
    case 7: break;  // revision register is read-only
    default:
      if (reg >= 8 && reg < 16) c.vctrl[reg] = value & (0xFFFFF | 0x3F800000u);
      break;
  }
}

const double kVcst[20] = {0.0,
                          3.4028234663852886e+38,
                          1.4142135623730951,
                          0.7071067811865476,
                          1.1283791670955126,
                          0.6366197723675814,
                          0.3183098861837907,
                          0.7853981633974483,
                          1.5707963267948966,
                          3.141592653589793,
                          2.718281828459045,
                          1.4426950408889634,
                          0.4342944819032518,
                          0.6931471805599453,
                          2.302585092994046,
                          6.283185307179586,
                          0.5235987755982988,
                          0.3010299956639812,
                          3.321928094887362,
                          0.8660254037844386};

struct Ctx {
  Cpu& c;
  uint32_t raw;
  unsigned n;
  uint32_t vd, vs, vt;

  Ctx(Cpu& cpu, uint32_t r)
      : c(cpu), raw(r), n(1 + ((r >> 7) & 1) + ((r >> 14) & 2)), vd(r & 127), vs((r >> 8) & 127), vt((r >> 16) & 127) {}

  void read(uint32_t reg, unsigned cnt, float* out) const {
    unsigned idx[4];
    vector_slots(reg, cnt, idx);
    for (unsigned i = 0; i < cnt; ++i) out[i] = c.v[idx[i]];
    for (unsigned i = cnt; i < 4; ++i) out[i] = 0.f;
  }
  void readS(unsigned cnt, float* out, uint32_t reg) const {
    read(reg, cnt, out);
    applyPrefix(c.vctrl[0], cnt, out);
  }
  void readS(unsigned cnt, float* out) const { readS(cnt, out, vs); }
  void readT(unsigned cnt, float* out) const {
    read(vt, cnt, out);
    applyPrefix(c.vctrl[1], cnt, out);
  }

  void write(uint32_t reg, unsigned cnt, const float* r, bool saturate) const {
    unsigned idx[4];
    vector_slots(reg, cnt, idx);
    uint32_t dp = c.vctrl[2];
    for (unsigned i = 0; i < cnt; ++i) {
      if ((dp >> (8 + i)) & 1) continue;
      float x = r[i];
      if (saturate) {
        unsigned s = (dp >> (2 * i)) & 3;
        if (s == 1) x = clampf(x, 0.f, 1.f);
        else if (s == 3) x = clampf(x, -1.f, 1.f);
      }
      c.v[idx[i]] = x;
    }
  }
  void writeD(unsigned cnt, const float* r) const { write(vd, cnt, r, true); }
  void writeBits(unsigned cnt, const uint32_t* r) const {
    float f[4];
    for (unsigned i = 0; i < 4; ++i) f[i] = flt(i < cnt ? r[i] : 0);
    write(vd, cnt, f, false);
  }
};

float unaryFn(Op op, float s) {
  switch (op) {
    case Op::Vrcp: return 1.0f / s;
    case Op::Vnrcp: return -1.0f / s;
    case Op::Vrsq: return 1.0f / std::sqrt(s);
    case Op::Vsqrt: return std::sqrt(s);
    case Op::Vsin: return static_cast<float>(quarterTurn(s, false));
    case Op::Vnsin: return static_cast<float>(-quarterTurn(s, false));
    case Op::Vcos: return static_cast<float>(quarterTurn(s, true));
    case Op::Vasin: return static_cast<float>(std::asin(static_cast<double>(s)) * 2.0 / kPi);
    case Op::Vexp2: return std::exp2(s);
    case Op::Vrexp2: return std::exp2(-s);
    case Op::Vlog2: return std::log2(s);
    default: return s;
  }
}

bool compare(unsigned cond, float s, float t) {
  switch (cond) {
    case 0: return false;
    case 1: return s == t;
    case 2: return s < t;
    case 3: return s <= t;
    case 4: return true;
    case 5: return s != t;
    case 6: return s >= t;
    case 7: return s > t;
    case 8: return s == 0.f;
    case 9: return std::isnan(s);
    case 10: return std::isinf(s);
    case 11: return std::isnan(s) || std::isinf(s);
    case 12: return s != 0.f;
    case 13: return !std::isnan(s);
    case 14: return !std::isinf(s);
    default: return !(std::isnan(s) || std::isinf(s));
  }
}

int32_t toInt(float s, unsigned mode, unsigned scale) {
  if (std::isnan(s)) return 0x7FFFFFFF;
  double x = std::ldexp(static_cast<double>(s), static_cast<int>(scale));
  switch (mode) {
    case 0: x = std::nearbyint(x); break;
    case 1: x = std::trunc(x); break;
    case 2: x = std::ceil(x); break;
    default: x = std::floor(x); break;
  }
  if (x >= 2147483647.0) return 0x7FFFFFFF;
  if (x <= -2147483648.0) return static_cast<int32_t>(0x80000000u);
  return static_cast<int32_t>(x);
}

uint32_t packColor(Op op, uint32_t s) {
  switch (op) {
    case Op::Vt4444: return ((s >> 4) & 0xF) | ((s >> 8) & 0xF0) | ((s >> 12) & 0xF00) | ((s >> 16) & 0xF000);
    case Op::Vt5551: return ((s >> 3) & 0x1F) | ((s >> 6) & 0x3E0) | ((s >> 9) & 0x7C00) | ((s >> 16) & 0x8000);
    default: return ((s >> 3) & 0x1F) | ((s >> 5) & 0x7E0) | ((s >> 8) & 0xF800);
  }
}

void execMatrix(Ctx& x, Op op) {
  Cpu& c = x.c;
  unsigned n = x.n;
  unsigned ds[16], ss[16], ts[16];
  float s[16] = {}, t[16] = {}, d[16] = {};

  switch (op) {
    case Op::Vmmov: case Op::Vmidt: case Op::Vmzero: case Op::Vmone: {
      matrixSlots(x.vd, n, ds);
      if (op == Op::Vmmov) {
        matrixSlots(x.vs, n, ss);
        for (unsigned j = 0; j < n; ++j)
          for (unsigned i = 0; i < n; ++i) d[j * 4 + i] = c.v[ss[j * 4 + i]];
      } else {
        for (unsigned j = 0; j < n; ++j)
          for (unsigned i = 0; i < n; ++i)
            d[j * 4 + i] = op == Op::Vmzero ? 0.f : op == Op::Vmone ? 1.f : (i == j ? 1.f : 0.f);
      }
      break;
    }
    case Op::Vmmul: {
      matrixSlots(x.vd, n, ds);
      matrixSlots(x.vs, n, ss);
      matrixSlots(x.vt, n, ts);
      for (unsigned j = 0; j < n; ++j)
        for (unsigned i = 0; i < n; ++i) {
          s[j * 4 + i] = c.v[ss[j * 4 + i]];
          t[j * 4 + i] = c.v[ts[j * 4 + i]];
        }
      for (unsigned a = 0; a < n; ++a)
        for (unsigned b = 0; b < n; ++b) {
          double sum = 0;
          for (unsigned k = 0; k < n; ++k) sum += static_cast<double>(s[b * 4 + k]) * t[a * 4 + k];
          d[a * 4 + b] = static_cast<float>(sum);
        }
      break;
    }
    case Op::Vmscl: {
      matrixSlots(x.vd, n, ds);
      matrixSlots(x.vs, n, ss);
      float k;
      unsigned ti[4];
      vector_slots(x.vt, 1, ti);
      k = c.v[ti[0]];
      for (unsigned j = 0; j < n; ++j)
        for (unsigned i = 0; i < n; ++i) d[j * 4 + i] = c.v[ss[j * 4 + i]] * k;
      break;
    }
    default: break;
  }
  for (unsigned j = 0; j < n; ++j)
    for (unsigned i = 0; i < n; ++i) c.v[ds[j * 4 + i]] = d[j * 4 + i];
}

}  // namespace

void vector_slots(uint32_t reg, unsigned n, unsigned idx[4]) {
  RegPos p = regPos(reg, n);
  for (unsigned i = 0; i < n; ++i) {
    unsigned r = (p.row + i) & 3;
    idx[i] = p.mtx * 16 + (p.transposed ? p.col + r * 4 : p.col * 4 + r);
  }
}

void exec(Cpu& c, Op op, uint32_t raw) {
  Ctx x(c, raw);
  const unsigned n = x.n;
  float s[4], t[4], d[4] = {};
  bool keepPrefix = false;

  switch (op) {
    case Op::Vpfxs: c.vctrl[0] = raw & 0xFFFFF; keepPrefix = true; break;
    case Op::Vpfxt: c.vctrl[1] = raw & 0xFFFFF; keepPrefix = true; break;
    case Op::Vpfxd: c.vctrl[2] = raw & 0xFFF; keepPrefix = true; break;
    case Op::Vnop: case Op::Vsync: case Op::Vflush: keepPrefix = true; break;

    case Op::Viim:
    case Op::Vfim: {
      unsigned idx[4];
      vector_slots(x.vt, 1, idx);
      if (op == Op::Viim) {
        c.v[idx[0]] = static_cast<float>(static_cast<int16_t>(raw & 0xFFFF));
      } else {
        c.v[idx[0]] = halfToFloat(raw & 0xFFFF);
      }
      keepPrefix = true;
      break;
    }

    case Op::LvS:
    case Op::SvS: {
      uint32_t addr = c.r[(raw >> 21) & 31] + static_cast<int16_t>(raw & 0xFFFC);
      unsigned idx[4];
      vector_slots(((raw >> 16) & 31) | ((raw & 3) << 5), 1, idx);
      if (op == Op::LvS) c.v[idx[0]] = flt(lw(addr));
      else sw(addr, bits(c.v[idx[0]]));
      keepPrefix = true;
      break;
    }
    case Op::LvQ: case Op::SvQ: case Op::LvlQ: case Op::LvrQ: case Op::SvlQ: case Op::SvrQ: {
      uint32_t addr = c.r[(raw >> 21) & 31] + static_cast<int16_t>(raw & 0xFFFC);
      unsigned idx[4];
      vector_slots(((raw >> 16) & 31) | ((raw & 1) << 5), 4, idx);
      unsigned off = (addr >> 2) & 3;
      switch (op) {
        case Op::LvQ:
          for (unsigned i = 0; i < 4; ++i) c.v[idx[i]] = flt(lw(addr + 4 * i));
          break;
        case Op::SvQ:
          for (unsigned i = 0; i < 4; ++i) sw(addr + 4 * i, bits(c.v[idx[i]]));
          break;
        case Op::LvlQ:
          for (unsigned i = 0; i <= off; ++i) c.v[idx[3 - i]] = flt(lw(addr - 4 * i));
          break;
        case Op::LvrQ:
          for (unsigned i = 0; i <= 3 - off; ++i) c.v[idx[i]] = flt(lw(addr + 4 * i));
          break;
        case Op::SvlQ:
          for (unsigned i = 0; i <= off; ++i) sw(addr - 4 * i, bits(c.v[idx[3 - i]]));
          break;
        default:
          for (unsigned i = 0; i <= 3 - off; ++i) sw(addr + 4 * i, bits(c.v[idx[i]]));
          break;
      }
      keepPrefix = true;
      break;
    }

    case Op::Mfv: {
      unsigned rt = (raw >> 16) & 31, imm = raw & 0xFF;
      uint32_t v = 0;
      if (imm < 128) {
        unsigned idx[4];
        vector_slots(imm, 1, idx);
        v = bits(c.v[idx[0]]);
      } else if (imm < 144) {
        v = c.vctrl[imm - 128];
      }
      if (rt) c.r[rt] = v;
      keepPrefix = true;
      break;
    }
    case Op::Mtv: {
      unsigned rt = (raw >> 16) & 31, imm = raw & 0xFF;
      uint32_t v = c.r[rt];
      if (imm < 128) {
        unsigned idx[4];
        vector_slots(imm, 1, idx);
        c.v[idx[0]] = flt(v);
      } else if (imm < 144) {
        setControl(c, imm - 128, v);
      }
      keepPrefix = true;
      break;
    }
    case Op::Vmfvc: {
      unsigned ctl = (raw >> 8) & 0x7F;
      d[0] = flt(ctl < 16 ? c.vctrl[ctl] : 0);
      x.write(x.vd, 1, d, false);
      break;
    }
    case Op::Vmtvc: {
      unsigned ctl = raw & 0x7F;
      unsigned idx[4];
      vector_slots(x.vs, 1, idx);
      if (ctl < 16) setControl(c, ctl, bits(c.v[idx[0]]));
      break;
    }

    case Op::Vadd: case Op::Vsub: case Op::Vmul: case Op::Vdiv: case Op::Vmin: case Op::Vmax:
    case Op::Vsge: case Op::Vslt: case Op::Vscmp: {
      x.readS(n, s);
      x.readT(n, t);
      for (unsigned i = 0; i < n; ++i) {
        switch (op) {
          case Op::Vadd: d[i] = s[i] + t[i]; break;
          case Op::Vsub: d[i] = s[i] - t[i]; break;
          case Op::Vmul: d[i] = s[i] * t[i]; break;
          case Op::Vdiv: d[i] = s[i] / t[i]; break;
          case Op::Vmin: d[i] = s[i] < t[i] ? s[i] : t[i]; break;
          case Op::Vmax: d[i] = s[i] > t[i] ? s[i] : t[i]; break;
          case Op::Vsge: d[i] = s[i] >= t[i] ? 1.f : 0.f; break;
          case Op::Vslt: d[i] = s[i] < t[i] ? 1.f : 0.f; break;
          default: d[i] = static_cast<float>((s[i] > t[i]) - (s[i] < t[i])); break;
        }
      }
      x.writeD(n, d);
      break;
    }
    case Op::Vscl: {
      x.readS(n, s);
      x.read(x.vt, 1, t);
      applyPrefix(c.vctrl[1], 1, t);
      for (unsigned i = 0; i < n; ++i) d[i] = s[i] * t[0];
      x.writeD(n, d);
      break;
    }
    case Op::Vdot: case Op::Vhdp: {
      x.readS(n, s);
      x.readT(n, t);
      if (op == Op::Vhdp) s[n - 1] = 1.f;
      double sum = 0;
      for (unsigned i = 0; i < n; ++i) sum += static_cast<double>(s[i]) * t[i];
      d[0] = static_cast<float>(sum);
      x.writeD(1, d);
      break;
    }
    case Op::Vdet: {
      x.readS(2, s);
      x.readT(2, t);
      d[0] = s[0] * t[1] - s[1] * t[0];
      x.writeD(1, d);
      break;
    }
    case Op::Vcrs: {
      x.readS(3, s);
      x.readT(3, t);
      d[0] = s[1] * t[2];
      d[1] = s[2] * t[0];
      d[2] = s[0] * t[1];
      x.writeD(3, d);
      break;
    }
    case Op::Vfad: case Op::Vavg: {
      x.readS(n, s);
      double sum = 0;
      for (unsigned i = 0; i < n; ++i) sum += s[i];
      d[0] = static_cast<float>(op == Op::Vavg ? sum / n : sum);
      x.writeD(1, d);
      break;
    }
    case Op::Vcmp: {
      x.readS(n, s);
      x.readT(n, t);
      unsigned cond = raw & 15, cc = 0, any = 0, all = 1;
      for (unsigned i = 0; i < n; ++i) {
        unsigned r = compare(cond, s[i], t[i]) ? 1u : 0u;
        cc |= r << i;
        any |= r;
        all &= r;
      }
      cc |= (any << 4) | (all << 5);
      uint32_t affected = (1u << 4) | (1u << 5) | ((1u << n) - 1);
      c.vctrl[3] = (c.vctrl[3] & ~affected) | (cc & affected);
      break;
    }
    case Op::Vcmov: {
      x.readS(n, s);
      bool tf = (raw >> 19) & 1;
      unsigned imm3 = (raw >> 16) & 7;
      x.read(x.vd, n, d);
      uint32_t cc = c.vctrl[3];
      if (imm3 < 6 || imm3 == 7) {
        if (((cc >> imm3) & 1) == static_cast<uint32_t>(!tf))
          for (unsigned i = 0; i < n; ++i) d[i] = s[i];
      } else {
        for (unsigned i = 0; i < n; ++i)
          if (((cc >> i) & 1) == static_cast<uint32_t>(!tf)) d[i] = s[i];
      }
      x.writeD(n, d);
      break;
    }

    case Op::Vmov: case Op::Vsat0: case Op::Vsat1: case Op::Vabs: case Op::Vneg:
    case Op::Vrcp: case Op::Vrsq: case Op::Vsin: case Op::Vcos: case Op::Vexp2: case Op::Vlog2:
    case Op::Vsqrt: case Op::Vasin: case Op::Vnrcp: case Op::Vnsin: case Op::Vrexp2: case Op::Vsgn:
    case Op::Vocp: {
      x.read(x.vs, n, s);
      uint32_t pfx = c.vctrl[0];
      if (op == Op::Vabs) pfx |= 0xF00;
      if (op == Op::Vneg) pfx |= 0xF0000;
      applyPrefix(pfx, n, s);
      for (unsigned i = 0; i < n; ++i) {
        switch (op) {
          case Op::Vsat0: d[i] = clampf(s[i], 0.f, 1.f); break;
          case Op::Vsat1: d[i] = clampf(s[i], -1.f, 1.f); break;
          case Op::Vsgn: d[i] = static_cast<float>((s[i] > 0.f) - (s[i] < 0.f)); break;
          case Op::Vocp: d[i] = 1.f - s[i]; break;
          default: d[i] = unaryFn(op, s[i]); break;
        }
      }
      x.writeD(n, d);
      break;
    }
    case Op::Vsocp: {
      x.readS(n, s);
      for (unsigned i = 0; i < n; ++i) {
        d[2 * i] = clampf(1.f - s[i], 0.f, 1.f);
        d[2 * i + 1] = clampf(s[i], 0.f, 1.f);
      }
      x.writeD(2 * n, d);
      break;
    }
    case Op::Vidt: {
      unsigned row = x.vd & (n == 2 ? 1 : 3);
      for (unsigned i = 0; i < n; ++i) d[i] = i == row ? 1.f : 0.f;
      x.writeD(n, d);
      break;
    }
    case Op::Vzero: case Op::Vone:
      for (unsigned i = 0; i < n; ++i) d[i] = op == Op::Vone ? 1.f : 0.f;
      x.writeD(n, d);
      break;
    case Op::Vcst: {
      unsigned k = (raw >> 16) & 31;
      for (unsigned i = 0; i < n; ++i) d[i] = k < 20 ? static_cast<float>(kVcst[k]) : 0.f;
      x.writeD(n, d);
      break;
    }
    case Op::Vrot: {
      x.read(x.vs, 1, s);
      applyPrefix(c.vctrl[0], 1, s);
      unsigned imm = (raw >> 16) & 31, sl = (imm >> 2) & 3, cl = imm & 3;
      float sn = static_cast<float>(quarterTurn(s[0], false)), cs = static_cast<float>(quarterTurn(s[0], true));
      if (imm & 0x10) sn = -sn;
      if (sl == cl) {
        for (unsigned i = 0; i < n; ++i) d[i] = sn;
      } else {
        d[sl] = sn;
        d[cl] = cs;
      }
      x.writeD(n, d);
      break;
    }

    case Op::Vbfy1: case Op::Vbfy2: case Op::Vsrt1: case Op::Vsrt2: case Op::Vsrt3: case Op::Vsrt4: {
      x.readS(n, s);
      auto lo = [](float a, float b) { return a < b ? a : b; };
      auto hi = [](float a, float b) { return a > b ? a : b; };
      switch (op) {
        case Op::Vbfy1:
          d[0] = s[0] + s[1]; d[1] = s[0] - s[1];
          d[2] = s[2] + s[3]; d[3] = s[2] - s[3];
          break;
        case Op::Vbfy2:
          d[0] = s[0] + s[2]; d[1] = s[1] + s[3];
          d[2] = s[0] - s[2]; d[3] = s[1] - s[3];
          break;
        case Op::Vsrt1:
          d[0] = lo(s[0], s[1]); d[1] = hi(s[0], s[1]); d[2] = lo(s[2], s[3]); d[3] = hi(s[2], s[3]);
          break;
        case Op::Vsrt2:
          d[0] = lo(s[0], s[3]); d[1] = lo(s[1], s[2]); d[2] = hi(s[1], s[2]); d[3] = hi(s[0], s[3]);
          break;
        case Op::Vsrt3:
          d[0] = hi(s[0], s[1]); d[1] = lo(s[0], s[1]); d[2] = hi(s[2], s[3]); d[3] = lo(s[2], s[3]);
          break;
        default:
          d[0] = hi(s[0], s[3]); d[1] = hi(s[1], s[2]); d[2] = lo(s[1], s[2]); d[3] = lo(s[0], s[3]);
          break;
      }
      x.writeD(n, d);
      break;
    }

    case Op::Vf2in: case Op::Vf2iz: case Op::Vf2iu: case Op::Vf2id: {
      x.readS(n, s);
      unsigned mode = (raw >> 21) & 3, scale = (raw >> 16) & 31;
      uint32_t r[4] = {};
      for (unsigned i = 0; i < n; ++i) r[i] = static_cast<uint32_t>(toInt(s[i], mode, scale));
      x.writeBits(n, r);
      break;
    }
    case Op::Vi2f: {
      x.readS(n, s);
      int scale = static_cast<int>((raw >> 16) & 31);
      for (unsigned i = 0; i < n; ++i) d[i] = std::ldexp(static_cast<float>(sbits(s[i])), -scale);
      x.writeD(n, d);
      break;
    }
    case Op::Vh2f: {
      x.readS(n, s);
      for (unsigned i = 0; i < n; ++i) {
        uint32_t w = bits(s[i]);
        d[2 * i] = halfToFloat(w & 0xFFFF);
        d[2 * i + 1] = halfToFloat(w >> 16);
      }
      x.writeD(n == 1 ? 2 : 4, d);
      break;
    }
    case Op::Vf2h: {
      x.readS(n, s);
      uint32_t r[2] = {};
      unsigned outs = n <= 2 ? 1 : 2;
      for (unsigned i = 0; i < outs; ++i) r[i] = floatToHalf(s[2 * i]) | (floatToHalf(s[2 * i + 1]) << 16);
      if (n == 1) r[0] = floatToHalf(s[0]);
      x.writeBits(outs, r);
      break;
    }
    case Op::Vuc2i: case Op::Vc2i: {
      x.readS(1, s);
      uint32_t w = bits(s[0]), r[4];
      for (unsigned i = 0; i < 4; ++i) {
        uint32_t b = (w >> (8 * i)) & 0xFF;
        r[i] = op == Op::Vuc2i ? (b * 0x01010101u) >> 1 : b << 24;
      }
      x.writeBits(4, r);
      break;
    }
    case Op::Vus2i: case Op::Vs2i: {
      x.readS(n, s);
      uint32_t r[4];
      for (unsigned i = 0; i < n; ++i) {
        uint32_t w = bits(s[i]);
        uint32_t lo = w & 0xFFFF, hi = w >> 16;
        if (op == Op::Vus2i) {
          r[2 * i] = lo << 15;
          r[2 * i + 1] = hi << 15;
        } else {
          r[2 * i] = lo << 16;
          r[2 * i + 1] = hi << 16;
        }
      }
      x.writeBits(n == 1 ? 2 : 4, r);
      break;
    }
    case Op::Vi2uc: case Op::Vi2c: {
      x.readS(n, s);
      uint32_t r[4] = {};
      unsigned outs = n < 4 ? 1 : n / 4;
      for (unsigned o = 0; o < outs; ++o) {
        uint32_t w = 0;
        for (unsigned i = 0; i < 4 && o * 4 + i < n; ++i) {
          int32_t v = sbits(s[o * 4 + i]);
          uint32_t b = op == Op::Vi2uc ? (v < 0 ? 0u : static_cast<uint32_t>(v) >> 23) : static_cast<uint32_t>(v) >> 24;
          w |= (b & 0xFF) << (8 * i);
        }
        r[o] = w;
      }
      x.writeBits(outs, r);
      break;
    }
    case Op::Vi2us: case Op::Vi2s: {
      x.readS(n, s);
      uint32_t r[2] = {};
      unsigned outs = n < 2 ? 1 : n / 2;
      for (unsigned o = 0; o < outs; ++o) {
        uint32_t w = 0;
        for (unsigned i = 0; i < 2 && o * 2 + i < n; ++i) {
          int32_t v = sbits(s[o * 2 + i]);
          uint32_t h = op == Op::Vi2us ? (v < 0 ? 0u : static_cast<uint32_t>(v) >> 15) : static_cast<uint32_t>(v) >> 16;
          w |= (h & 0xFFFF) << (16 * i);
        }
        r[o] = w;
      }
      x.writeBits(outs, r);
      break;
    }
    case Op::Vt4444: case Op::Vt5551: case Op::Vt5650: {
      x.readS(4, s);
      uint32_t r[2];
      for (unsigned i = 0; i < 2; ++i) r[i] = packColor(op, bits(s[2 * i])) | (packColor(op, bits(s[2 * i + 1])) << 16);
      x.writeBits(2, r);
      break;
    }

    case Op::Vrnds: {
      x.read(x.vs, 1, s);
      applyPrefix(c.vctrl[0], 1, s);
      seedRandom(bits(s[0]), &c.vctrl[8]);
      break;
    }
    case Op::Vrndi: case Op::Vrndf1: case Op::Vrndf2: {
      uint32_t r[4] = {};
      for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
        uint32_t g = randomStep(&c.vctrl[8]);
        r[i] = op == Op::Vrndi ? g : (op == Op::Vrndf1 ? 0x3F800000u : 0x40000000u) | (g & 0x7FFFFF);
      }
      x.writeBits(n, r);
      break;
    }

    case Op::Vmmul: case Op::Vmscl: case Op::Vmmov: case Op::Vmidt: case Op::Vmzero: case Op::Vmone:
      execMatrix(x, op);
      break;
    case Op::Vtfm: {
      unsigned ins = (raw >> 23) & 3, side = ins + 1;
      unsigned ms[16];
      matrixSlots(x.vs, side, ms);
      unsigned tn = n < side ? n : side;
      x.read(x.vt, tn, t);
      float t2[4] = {};
      for (unsigned i = 0; i < side; ++i) t2[i] = i < tn ? t[i] : (i == ins ? 1.f : 0.f);
      for (unsigned i = 0; i < side; ++i) {
        double sum = 0;
        for (unsigned k = 0; k < side; ++k) sum += static_cast<double>(c.v[ms[i * 4 + k]]) * t2[k];
        d[i] = static_cast<float>(sum);
      }
      x.write(x.vd, side, d, true);
      break;
    }
    case Op::Vcrsp: {
      x.read(x.vs, n, s);
      x.read(x.vt, n, t);
      if (n == 3) {
        d[0] = s[1] * t[2] - s[2] * t[1];
        d[1] = s[2] * t[0] - s[0] * t[2];
        d[2] = s[0] * t[1] - s[1] * t[0];
      } else {
        d[0] = s[0] * t[3] + s[3] * t[0] + s[1] * t[2] - s[2] * t[1];
        d[1] = s[1] * t[3] + s[3] * t[1] + s[2] * t[0] - s[0] * t[2];
        d[2] = s[2] * t[3] + s[3] * t[2] + s[0] * t[1] - s[1] * t[0];
        d[3] = s[3] * t[3] - s[0] * t[0] - s[1] * t[1] - s[2] * t[2];
      }
      x.write(x.vd, n, d, false);
      break;
    }

    default: {
      char buf[96];
      std::snprintf(buf, sizeof buf, "vfpu %s (%08X) is not implemented", op_name(op), raw);
      throw std::runtime_error(buf);
    }
  }

  if (!keepPrefix) {
    c.vctrl[0] = 0xE4;
    c.vctrl[1] = 0xE4;
    c.vctrl[2] = 0;
  }
}

}  // namespace vfpu
}  // namespace psp
