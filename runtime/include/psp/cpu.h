#pragma once
#include <cstdint>

namespace psp {

// Allegrex general state. The VFPU register file is allocated but nothing reads it yet.
struct Cpu {
  uint32_t r[32] = {};
  uint32_t hi = 0;
  uint32_t lo = 0;
  float f[32] = {};
  bool fcc = false;
  uint32_t fcr31 = 0;
  float v[128] = {};
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
