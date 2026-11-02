// Tests for VFPU decoding and the runtime that executes it.
#include <cmath>
#include <cstdio>
#include <cstring>

#include <psp/memory.h>
#include <psp/vfpu.h>

#include "mips.h"
#include "vfpu.h"

using namespace mgspo;
namespace vf = psp::vfpu;

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

static bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

enum Size { S = 1, P = 2, T = 3, Q = 4 };

static uint32_t sizeBits(int n) { return (n == 2 || n == 4 ? 1u << 7 : 0) | (n >= 3 ? 1u << 15 : 0); }

// major opcode, op bits 25:23, then register fields
static uint32_t enc(uint32_t major, uint32_t sub, int n, uint32_t vd, uint32_t vs, uint32_t vt) {
  return (major << 26) | (sub << 23) | (vt << 16) | (vs << 8) | vd | sizeBits(n);
}
static uint32_t encUnary(uint32_t rs, uint32_t rt, int n, uint32_t vd, uint32_t vs) {
  return (0x34u << 26) | (rs << 21) | (rt << 16) | (vs << 8) | vd | sizeBits(n);
}

static void run(psp::Cpu& c, uint32_t raw) {
  vf::Op op = decodeVfpu(raw);
  if (op == vf::Op::Invalid) {
    std::printf("FAIL: could not decode %08X\n", raw);
    ++g_failures;
    return;
  }
  vf::exec(c, op, raw);
}

static void setVec(psp::Cpu& c, uint32_t reg, int n, std::initializer_list<float> vals) {
  unsigned idx[4];
  vf::vector_slots(reg, n, idx);
  int i = 0;
  for (float v : vals) c.v[idx[i++]] = v;
}
static float getLane(psp::Cpu& c, uint32_t reg, int n, int lane) {
  unsigned idx[4];
  vf::vector_slots(reg, n, idx);
  return c.v[idx[lane]];
}

static void testDecode() {
  CHECK(decodeVfpu(enc(0x18, 0, 4, 0, 4, 8)) == vf::Op::Vadd);
  CHECK(vfpuSize(enc(0x18, 0, 4, 0, 4, 8)) == 4);
  CHECK(vfpuSize(enc(0x18, 0, 1, 0, 4, 8)) == 1);
  CHECK(vfpuSize(enc(0x18, 0, 2, 0, 4, 8)) == 2);
  CHECK(vfpuSize(enc(0x18, 0, 3, 0, 4, 8)) == 3);
  CHECK(decodeVfpu(enc(0x18, 1, 4, 0, 4, 8)) == vf::Op::Vsub);
  CHECK(decodeVfpu(enc(0x19, 1, 4, 0, 4, 8)) == vf::Op::Vdot);
  CHECK(decodeVfpu(enc(0x19, 2, 4, 0, 4, 8)) == vf::Op::Vscl);
  CHECK(decodeVfpu(enc(0x1B, 0, 4, 0, 4, 8)) == vf::Op::Vcmp);
  CHECK(decodeVfpu(enc(0x3C, 0, 4, 0, 4, 8)) == vf::Op::Vmmul);
  CHECK(decodeVfpu(enc(0x3C, 3, 4, 0, 4, 8)) == vf::Op::Vtfm);
  CHECK(decodeVfpu(encUnary(0, 1, 4, 0, 4)) == vf::Op::Vabs);
  CHECK(decodeVfpu(encUnary(0, 18, 1, 0, 4)) == vf::Op::Vsin);
  CHECK(decodeVfpu(encUnary(3, 9, 4, 0, 0)) == vf::Op::Vcst);
  CHECK(decodeVfpu(encUnary(17, 0, 4, 0, 0)) == vf::Op::Vf2iz);
  CHECK(decodeVfpu(encUnary(21, 0, 4, 0, 0)) == vf::Op::Vcmov);
  CHECK(decodeVfpu((0x3Cu << 26) | (28u << 21) | (3u << 16)) == vf::Op::Vmidt);
  CHECK(decodeVfpu((0x3Cu << 26) | (29u << 21)) == vf::Op::Vrot);
  CHECK(decodeVfpu(0x36u << 26) == vf::Op::LvQ);
  CHECK(decodeVfpu((0x35u << 26) | 2) == vf::Op::LvrQ);
  CHECK(decodeVfpu(0x3Eu << 26) == vf::Op::SvQ);
  CHECK(decodeVfpu(0xFFFF0000) == vf::Op::Vnop);
  CHECK(decodeVfpu(0xFFFF0320) == vf::Op::Vsync);
  CHECK(decodeVfpu(0x3F000000) == vf::Op::Invalid);

  // The CC branches go through the normal decoder and know their targets.
  Insn bvt = decode(0x49010003u | (4u << 18), 0x1000);  // bvt CC[4], +3
  CHECK(bvt.op == Op::Bvt && bvt.cond == 4 && bvt.target == 0x1000 + 4 + 12);
  CHECK(hasDelaySlot(bvt.op) && !isLikely(bvt.op));
  Insn bvfl = decode(0x49020000u | (1u << 18), 0x1000);
  CHECK(bvfl.op == Op::Bvfl && isLikely(bvfl.op) && bvfl.cond == 1);
  CHECK(decode(0x49000001, 0).op == Op::Bvf);
  CHECK(decode(0x49030001, 0).op == Op::Bvtl);
  CHECK(decode(0x48600000 | 0, 0).op == Op::Vfpu);  // mfv goes through the generic path
}

static void testRegisterMapping() {
  unsigned idx[4];
  vf::vector_slots(0, 4, idx);  // C000
  CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2 && idx[3] == 3);
  vf::vector_slots(1, 4, idx);  // C010
  CHECK(idx[0] == 4 && idx[3] == 7);
  vf::vector_slots(32, 4, idx);  // R000, the transposed view of the same matrix
  CHECK(idx[0] == 0 && idx[1] == 4 && idx[2] == 8 && idx[3] == 12);
  vf::vector_slots(4, 4, idx);  // C100
  CHECK(idx[0] == 16);
  vf::vector_slots(64, 2, idx);  // C002 (pair starting at row 2)
  CHECK(idx[0] == 2 && idx[1] == 3);
  vf::vector_slots(64, 3, idx);  // triple at row 1
  CHECK(idx[0] == 1 && idx[2] == 3);
  vf::vector_slots(0x3 | (3 << 5), 1, idx);  // single S033
  CHECK(idx[0] == 3 * 4 + 3);
}

static void testArithmetic() {
  psp::Cpu c;
  setVec(c, 0, 4, {1, 2, 3, 4});
  setVec(c, 4, 4, {10, 20, 30, 40});
  run(c, enc(0x18, 0, 4, 8, 0, 4));  // vadd.q C200, C000, C100
  CHECK(getLane(c, 8, 4, 0) == 11 && getLane(c, 8, 4, 3) == 44);
  run(c, enc(0x18, 1, 4, 8, 4, 0));  // vsub.q
  CHECK(getLane(c, 8, 4, 2) == 27);
  run(c, enc(0x19, 0, 2, 8, 0, 4));  // vmul.p
  CHECK(getLane(c, 8, 2, 1) == 40);

  run(c, enc(0x19, 1, 4, 8, 0, 4));  // vdot.q C200, C000, C100
  CHECK(getLane(c, 8, 1, 0) == 300);
  run(c, enc(0x19, 4, 4, 8, 0, 4));  // vhdp.q: last element of s counts as 1
  CHECK(getLane(c, 8, 1, 0) == 10 + 40 + 90 + 40);

  setVec(c, 0, 3, {1, 0, 0});
  setVec(c, 4, 3, {0, 1, 0});
  run(c, enc(0x3C, 5, 3, 8, 0, 4));  // vcrsp.t
  CHECK(getLane(c, 8, 3, 0) == 0 && getLane(c, 8, 3, 1) == 0 && getLane(c, 8, 3, 2) == 1);

  setVec(c, 0, 4, {1, 2, 3, 4});
  setVec(c, 4, 1, {3});
  run(c, enc(0x19, 2, 4, 8, 0, 4));  // vscl.q by the single in C100
  CHECK(getLane(c, 8, 4, 3) == 12);

  run(c, encUnary(0, 2, 4, 8, 0));  // vneg.q
  CHECK(getLane(c, 8, 4, 1) == -2);
  setVec(c, 0, 4, {4, 9, 16, 25});
  run(c, encUnary(0, 22, 4, 8, 0));  // vsqrt.q
  CHECK(getLane(c, 8, 4, 3) == 5);
  run(c, encUnary(0, 16, 1, 8, 0));  // vrcp.s
  CHECK(near(getLane(c, 8, 1, 0), 0.25f));
  run(c, encUnary(0, 17, 1, 8, 0));  // vrsq.s
  CHECK(near(getLane(c, 8, 1, 0), 0.5f));

  setVec(c, 0, 1, {1.0f});
  run(c, encUnary(0, 18, 1, 8, 0));  // vsin.s: one quarter turn
  CHECK(getLane(c, 8, 1, 0) == 1.0f);
  run(c, encUnary(0, 19, 1, 8, 0));  // vcos.s
  CHECK(getLane(c, 8, 1, 0) == 0.0f);
  setVec(c, 0, 1, {2.0f});
  run(c, encUnary(0, 18, 1, 8, 0));
  CHECK(getLane(c, 8, 1, 0) == 0.0f);

  run(c, encUnary(0, 6, 4, 8, 0));  // vzero.q
  CHECK(getLane(c, 8, 4, 2) == 0);
  run(c, encUnary(0, 7, 2, 8, 0));  // vone.p
  CHECK(getLane(c, 8, 2, 1) == 1);
  run(c, encUnary(0, 3, 4, 9, 0));  // vidt.q C210
  CHECK(getLane(c, 9, 4, 1) == 1 && getLane(c, 9, 4, 0) == 0);

  run(c, encUnary(3, 9, 1, 8, 0));  // vcst.s: pi
  CHECK(near(getLane(c, 8, 1, 0), 3.14159265f));
}

static void testPrefixes() {
  psp::Cpu c;
  setVec(c, 0, 4, {1, 2, 3, 4});

  c.vctrl[0] = 0x1B;  // swizzle (w, z, y, x)
  run(c, encUnary(0, 0, 4, 8, 0));  // vmov.q
  CHECK(getLane(c, 8, 4, 0) == 4 && getLane(c, 8, 4, 3) == 1);
  CHECK(c.vctrl[0] == 0xE4);  // consumed by the instruction

  run(c, 0xDC000000u | 0u);  // vpfxs, identity
  CHECK((c.vctrl[0] & 0xFFFFF) == 0);

  // vpfxs [-x, |y|, 1, 0.5]: lane 2 constant "1", lane 3 constant "0.5"
  setVec(c, 0, 4, {5, -6, 0, 0});
  uint32_t data = 0 | (1 << 2) | (1 << 4) | (3 << 6)  // swizzle x y (const 1) (const 0.5)
                  | (1 << 9)                           // abs on lane 1
                  | (1 << 14) | (1 << 15)              // lanes 2 and 3 take constants
                  | (1 << 16);                         // negate lane 0
  data = (0x37u << 26) | (data & 0xFFFFF);
  vf::exec(c, vf::Op::Vpfxs, data);
  run(c, encUnary(0, 0, 4, 8, 0));
  CHECK(getLane(c, 8, 4, 0) == -5 && getLane(c, 8, 4, 1) == 6);
  CHECK(getLane(c, 8, 4, 2) == 1.f && getLane(c, 8, 4, 3) == 0.5f);

  // Destination prefix: saturate lane 0 to [0,1], mask lane 1
  setVec(c, 0, 4, {5, 6, -7, 8});
  setVec(c, 8, 4, {0, 100, 0, 0});
  vf::exec(c, vf::Op::Vpfxd, (0x37u << 26) | (4u << 23) | 1u | (1u << 9));
  run(c, encUnary(0, 0, 4, 8, 0));
  CHECK(getLane(c, 8, 4, 0) == 1.f);
  CHECK(getLane(c, 8, 4, 1) == 100.f);
  CHECK(getLane(c, 8, 4, 2) == -7.f);

  // vscl reads the scale through the T prefix; prefix survives lv.s/mtv
  c.vctrl[1] = 0xE4;
  CHECK(c.vctrl[2] == 0);
}

static void testCompare() {
  psp::Cpu c;
  setVec(c, 0, 4, {1, 5, 3, 7});
  setVec(c, 4, 4, {2, 5, 1, 9});
  run(c, enc(0x1B, 0, 4, 0, 0, 4) | 2);  // vcmp.q LT
  CHECK((c.vctrl[3] & 0xF) == 0x9);
  CHECK((c.vctrl[3] >> 4 & 1) == 1);  // any
  CHECK((c.vctrl[3] >> 5 & 1) == 0);  // all
  CHECK(vf::cc_bit(c, 0) && !vf::cc_bit(c, 1) && vf::cc_bit(c, 4) && !vf::cc_bit(c, 5));
  run(c, enc(0x1B, 0, 4, 0, 0, 4) | 3);  // LE
  CHECK((c.vctrl[3] & 0xF) == 0xB);

  // vcmov.q: if CC[imm3] is set move all lanes
  setVec(c, 8, 4, {-1, -1, -1, -1});
  c.vctrl[3] = 1;
  run(c, (0x34u << 26) | (21u << 21) | (0u << 16) | (0u << 8) | 8 | sizeBits(4));  // vcmovt, CC[0]
  CHECK(getLane(c, 8, 4, 3) == 7);
  setVec(c, 8, 4, {-1, -1, -1, -1});
  c.vctrl[3] = 0;
  run(c, (0x34u << 26) | (21u << 21) | (0u << 16) | 8 | sizeBits(4));
  CHECK(getLane(c, 8, 4, 3) == -1);
  // per-element form (imm3 == 6)
  c.vctrl[3] = 0x5;
  run(c, (0x34u << 26) | (21u << 21) | (6u << 16) | 8 | sizeBits(4));
  CHECK(getLane(c, 8, 4, 0) == 1 && getLane(c, 8, 4, 1) == -1 && getLane(c, 8, 4, 2) == 3);
}

static void testMatrix() {
  psp::Cpu c;
  for (int i = 0; i < 16; ++i) c.v[32 + i] = static_cast<float>(i + 1);  // M200
  run(c, (0x3Cu << 26) | (28u << 21) | (3u << 16) | 4 | sizeBits(4));      // vmidt.q M100
  CHECK(c.v[16] == 1 && c.v[17] == 0 && c.v[21] == 1);
  run(c, enc(0x3C, 0, 4, 0, 4, 8));  // vmmul.q M000, M100, M200
  bool same = true;
  for (int i = 0; i < 16; ++i) same = same && c.v[i] == c.v[32 + i];
  CHECK(same);

  // scale matrix times (1,2,3,4)
  for (int i = 0; i < 16; ++i) c.v[16 + i] = (i % 5 == 0) ? 2.f : 0.f;
  setVec(c, 8, 4, {1, 2, 3, 4});
  run(c, enc(0x3C, 3, 4, 0, 4, 8));  // vtfm4.q C000, M100, C200
  CHECK(getLane(c, 0, 4, 0) == 2 && getLane(c, 0, 4, 3) == 8);
  // translation sits in the last element of each register for vhtfm
  c.v[16 + 3] = 10;
  c.v[16 + 7] = 20;
  c.v[16 + 11] = 30;
  c.v[16 + 15] = 1;
  setVec(c, 8, 3, {1, 2, 3});
  run(c, enc(0x3C, 3, 3, 0, 4, 8));  // vhtfm4.t
  CHECK(getLane(c, 0, 3, 0) == 12 && getLane(c, 0, 3, 1) == 24 && getLane(c, 0, 3, 2) == 36);

  // vmmov M300, M100
  run(c, (0x3Cu << 26) | (28u << 21) | (4u << 8) | 12 | sizeBits(4));
  same = true;
  for (int i = 0; i < 16; ++i) same = same && c.v[48 + i] == c.v[16 + i];
  CHECK(same);

  // vrot.q: quarter turn into lane 1
  setVec(c, 4, 1, {1.f});
  run(c, (0x3Cu << 26) | (29u << 21) | (4u << 16) | (4u << 8) | 0 | sizeBits(4));
  CHECK(near(getLane(c, 0, 4, 0), 0.f) && near(getLane(c, 0, 4, 1), 1.f) && near(getLane(c, 0, 4, 2), 0.f));
}

static void testConversions() {
  psp::Cpu c;
  setVec(c, 0, 4, {1.5f, -1.5f, 2.5f, 1e20f});
  run(c, encUnary(17, 0, 4, 8, 0));  // vf2iz.q (truncate)
  auto bitsOf = [&](int lane) { float f = getLane(c, 8, 4, lane); uint32_t u; std::memcpy(&u, &f, 4); return static_cast<int32_t>(u); };
  CHECK(bitsOf(0) == 1 && bitsOf(1) == -1 && bitsOf(2) == 2 && bitsOf(3) == 0x7FFFFFFF);
  run(c, encUnary(16, 0, 4, 8, 0));  // vf2in.q, ties to even
  CHECK(bitsOf(0) == 2 && bitsOf(1) == -2 && bitsOf(2) == 2);
  run(c, encUnary(18, 0, 4, 8, 0));  // vf2iu.q (ceil)
  CHECK(bitsOf(0) == 2 && bitsOf(1) == -1);
  run(c, encUnary(19, 0, 4, 8, 0));  // vf2id.q (floor)
  CHECK(bitsOf(0) == 1 && bitsOf(1) == -2);
  run(c, encUnary(16, 4, 4, 8, 0));  // vf2in with scale 2^4
  CHECK(bitsOf(0) == 24);

  // vi2f with a shift
  uint32_t raw = 1u << 8;
  float f;
  std::memcpy(&f, &raw, 4);
  setVec(c, 0, 1, {f});
  run(c, encUnary(20, 8, 1, 8, 0));  // vi2f.s, scale 2^-8
  CHECK(getLane(c, 8, 1, 0) == 1.0f);

  // half floats: 1.0 and -2.0 packed into one word
  uint32_t packed = 0x3C00u | (0xC000u << 16);
  std::memcpy(&f, &packed, 4);
  setVec(c, 0, 1, {f});
  run(c, encUnary(1, 19, 1, 8, 0));  // vh2f.s -> pair
  CHECK(getLane(c, 8, 2, 0) == 1.0f && getLane(c, 8, 2, 1) == -2.0f);
  run(c, encUnary(1, 18, 2, 12, 8));  // vf2h.p back
  float back = getLane(c, 12, 1, 0);
  uint32_t bb;
  std::memcpy(&bb, &back, 4);
  CHECK(bb == packed);

  // unsigned bytes to ints: 0xFF -> 0x7FFFFFFF
  raw = 0x000080FFu;
  std::memcpy(&f, &raw, 4);
  setVec(c, 0, 1, {f});
  run(c, encUnary(1, 24, 1, 8, 0));  // vuc2i.s
  CHECK(bitsOf(0) == 0x7FFFFFFF);
}

static void testMemoryAndMoves() {
  psp::init_memory();
  psp::Cpu c;
  uint32_t base = 0x08900000;
  for (int i = 0; i < 4; ++i) psp::sw(base + 4 * i, 0x3F800000u + i);
  c.r[4] = base;
  // lv.q C100, 0(a0)  /  sv.q C100, 16(a0)
  run(c, (0x36u << 26) | (4u << 21) | (4u << 16) | 0);
  CHECK(c.v[16] == 1.0f);
  run(c, (0x3Eu << 26) | (4u << 21) | (4u << 16) | 16);
  CHECK(psp::lw(base + 16) == 0x3F800000u && psp::lw(base + 28) == 0x3F800003u);

  // lv.s / sv.s use the 7-bit single register split across the word
  run(c, (0x32u << 26) | (4u << 21) | (5u << 16) | 4);  // lv.s S101 (reg 5), 4(a0)
  unsigned idx[4];
  vf::vector_slots(5, 1, idx);
  uint32_t u;
  std::memcpy(&u, &c.v[idx[0]], 4);
  CHECK(u == 0x3F800001u);
  run(c, (0x3Au << 26) | (4u << 21) | (5u << 16) | 32);
  CHECK(psp::lw(base + 32) == 0x3F800001u);

  // mtv / mfv
  c.r[5] = 0x40000000;
  run(c, (0x12u << 26) | (7u << 21) | (5u << 16) | 9);  // mtv a1, S?
  vf::vector_slots(9, 1, idx);
  CHECK(c.v[idx[0]] == 2.0f);
  run(c, (0x12u << 26) | (3u << 21) | (6u << 16) | 9);  // mfv a2
  CHECK(c.r[6] == 0x40000000u);
  // control registers: read the revision, write CC
  run(c, (0x12u << 26) | (3u << 21) | (7u << 16) | (128 + 7));  // mfvc
  CHECK(c.r[7] == 0x7772CEABu);
  c.r[8] = 0xFFFFFFFF;
  run(c, (0x12u << 26) | (7u << 21) | (8u << 16) | (128 + 3));  // mtvc CC
  CHECK(c.vctrl[3] == 0x3F);

  // lvl.q at the third word of a block fills the upper three lanes from the words at -8, -4 and 0
  for (int i = 0; i < 8; ++i) psp::sw(base + 64 + 4 * i, 0x3F800000u + 100 + i);
  psp::Cpu d;
  d.r[4] = base + 64 + 8;
  run(d, (0x35u << 26) | (4u << 21) | (8u << 16) | 0);
  auto lane = [&](int i) { vf::vector_slots(8, 4, idx); std::memcpy(&u, &d.v[idx[i]], 4); return u; };
  CHECK(lane(0) == 0 && lane(1) == 0x3F800000u + 100 && lane(2) == 0x3F800000u + 101 && lane(3) == 0x3F800000u + 102);
}

static void testPrefixLifetime() {
  psp::Cpu c;
  setVec(c, 0, 4, {1, 2, 3, 4});
  vf::exec(c, vf::Op::Vpfxs, (0x37u << 26) | 0x1B);
  psp::sw(0x08900100, 0);
  c.r[4] = 0x08900100;
  run(c, (0x36u << 26) | (4u << 21) | (4u << 16));  // lv.q keeps pending prefixes
  CHECK(c.vctrl[0] == 0x1B);
  run(c, 0xFFFF0000u);  // vnop keeps them too
  CHECK(c.vctrl[0] == 0x1B);
  run(c, encUnary(0, 0, 4, 8, 0));
  CHECK(c.vctrl[0] == 0xE4);
}

int main() {
  testDecode();
  testRegisterMapping();
  testArithmetic();
  testPrefixes();
  testCompare();
  testMatrix();
  testConversions();
  testMemoryAndMoves();
  testPrefixLifetime();
  if (g_failures == 0) std::printf("all vfpu tests passed\n");
  return g_failures == 0 ? 0 : 1;
}
