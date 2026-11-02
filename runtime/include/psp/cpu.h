#pragma once
#include <cstdint>

namespace psp {

// Allegrex general state.
// v[] is the VFPU register file laid out as 8 matrices of 4 columns x 4 rows (index = m*16 + col*4 + row).
// vctrl[] holds the VFPU control registers: 0 SPFX, 1 TPFX, 2 DPFX, 3 CC, 4 INF4, 7 REV, 8..15 RCX0-7.
struct Cpu {
  uint32_t r[32] = {};
  uint32_t hi = 0;
  uint32_t lo = 0;
  float f[32] = {};
  bool fcc = false;
  uint32_t fcr31 = 0;
  float v[128] = {};
  uint32_t vctrl[16] = {0xE4, 0xE4, 0, 0, 0, 0, 0, 0x7772CEAB, 1, 2, 4, 8, 0, 0, 0, 0};
};

using HleFn = void (*)(Cpu&);

// A recompiled function, keyed by its guest address.
struct FuncEntry {
  uint32_t addr;
  void (*fn)(Cpu&);
};

// One imported function stub (8 bytes in the guest image) identified by library and NID.
struct ImportEntry {
  uint32_t stub;
  const char* module;
  uint32_t nid;
};

}  // namespace psp
