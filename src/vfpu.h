#pragma once
#include <cstdint>

#include <psp/vfpu_ops.h>

namespace mgspo {

// Identifies a VFPU instruction word. Returns Invalid for encodings that are not understood.
// The raw word still carries the register fields; the runtime pulls those out itself.
psp::vfpu::Op decodeVfpu(uint32_t raw);

// Number of lanes in the vector size field (1..4).
inline unsigned vfpuSize(uint32_t raw) { return 1 + ((raw >> 7) & 1) + ((raw >> 14) & 2); }

}  // namespace mgspo
