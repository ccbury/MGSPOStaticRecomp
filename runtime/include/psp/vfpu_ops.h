#pragma once
#include <cstdint>

namespace psp {
namespace vfpu {

// Every VFPU operation the lifter can emit. The lifter decodes an instruction into one of these plus
// its raw register fields and the runtime (vfpu.cpp) executes it, so both sides share this list.
#define PSP_VFPU_OPS(X)                                                                                         \
  X(Invalid)                                                                                                    \
  X(Vadd) X(Vsub) X(Vsbn) X(Vdiv) X(Vmul) X(Vdot) X(Vscl) X(Vhdp) X(Vcrs) X(Vdet)                              \
  X(Vcmp) X(Vmin) X(Vmax) X(Vscmp) X(Vsge) X(Vslt)                                                              \
  X(Vmov) X(Vabs) X(Vneg) X(Vidt) X(Vsat0) X(Vsat1) X(Vzero) X(Vone)                                            \
  X(Vrcp) X(Vrsq) X(Vsin) X(Vcos) X(Vexp2) X(Vlog2) X(Vsqrt) X(Vasin) X(Vnrcp) X(Vnsin) X(Vrexp2)               \
  X(Vrnds) X(Vrndi) X(Vrndf1) X(Vrndf2) X(Vf2h) X(Vh2f) X(Vsbz) X(Vlgb)                                         \
  X(Vuc2i) X(Vc2i) X(Vus2i) X(Vs2i) X(Vi2uc) X(Vi2c) X(Vi2us) X(Vi2s)                                           \
  X(Vsrt1) X(Vsrt2) X(Vsrt3) X(Vsrt4) X(Vbfy1) X(Vbfy2) X(Vocp) X(Vsocp) X(Vfad) X(Vavg) X(Vsgn)                \
  X(Vt4444) X(Vt5551) X(Vt5650) X(Vmfvc) X(Vmtvc)                                                               \
  X(Vcst) X(Vf2in) X(Vf2iz) X(Vf2iu) X(Vf2id) X(Vi2f) X(Vcmov) X(Vwbn)                                          \
  X(Vmmul) X(Vtfm) X(Vmscl) X(Vcrsp) X(Vmmov) X(Vmidt) X(Vmzero) X(Vmone) X(Vrot)                               \
  X(Vpfxs) X(Vpfxt) X(Vpfxd) X(Viim) X(Vfim)                                                                    \
  X(LvS) X(SvS) X(LvQ) X(SvQ) X(LvlQ) X(LvrQ) X(SvlQ) X(SvrQ)                                                   \
  X(Mfv) X(Mtv) X(Bvf) X(Bvt) X(Bvfl) X(Bvtl) X(Vnop) X(Vsync) X(Vflush)

enum class Op : uint8_t {
#define X(n) n,
  PSP_VFPU_OPS(X)
#undef X
};

inline const char* op_name(Op op) {
  static const char* names[] = {
#define X(n) #n,
      PSP_VFPU_OPS(X)
#undef X
  };
  return names[static_cast<int>(op)];
}

}  // namespace vfpu
}  // namespace psp
