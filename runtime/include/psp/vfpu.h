#pragma once
#include <cstdint>

#include "cpu.h"
#include "vfpu_ops.h"

namespace psp {
namespace vfpu {

// Runs one VFPU instruction (anything except the CC branches). `op` is what the lifter decoded from
// `raw`; it is passed along so the runtime does not have to decode again.
void exec(Cpu& c, Op op, uint32_t raw);

// Value of CC bit `bit` (0..7) for the bvf/bvt family. Bits 4 and 5 are the "any" and "all" results of
// the last vcmp; bits 6 and 7 always read as clear.
inline bool cc_bit(const Cpu& c, unsigned bit) { return (c.vctrl[3] >> bit) & 1; }

// Maps a VFPU register number to the position in Cpu::v, for the given lane count (1..4).
// idx[i] is the slot of element i.
void vector_slots(uint32_t reg, unsigned n, unsigned idx[4]);

}  // namespace vfpu
}  // namespace psp
