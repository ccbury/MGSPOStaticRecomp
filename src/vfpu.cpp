#include "vfpu.h"

namespace mgspo {

using psp::vfpu::Op;

namespace {

Op unaryOp(uint32_t rt) {
  switch (rt) {
    case 0: return Op::Vmov;
    case 1: return Op::Vabs;
    case 2: return Op::Vneg;
    case 3: return Op::Vidt;
    case 4: return Op::Vsat0;
    case 5: return Op::Vsat1;
    case 6: return Op::Vzero;
    case 7: return Op::Vone;
    case 16: return Op::Vrcp;
    case 17: return Op::Vrsq;
    case 18: return Op::Vsin;
    case 19: return Op::Vcos;
    case 20: return Op::Vexp2;
    case 21: return Op::Vlog2;
    case 22: return Op::Vsqrt;
    case 23: return Op::Vasin;
    case 24: return Op::Vnrcp;
    case 26: return Op::Vnsin;
    case 28: return Op::Vrexp2;
    default: return Op::Invalid;
  }
}

Op convertOp(uint32_t rt) {
  switch (rt) {
    case 0: return Op::Vrnds;
    case 1: return Op::Vrndi;
    case 2: return Op::Vrndf1;
    case 3: return Op::Vrndf2;
    case 18: return Op::Vf2h;
    case 19: return Op::Vh2f;
    case 22: return Op::Vsbz;
    case 23: return Op::Vlgb;
    case 24: return Op::Vuc2i;
    case 25: return Op::Vc2i;
    case 26: return Op::Vus2i;
    case 27: return Op::Vs2i;
    case 28: return Op::Vi2uc;
    case 29: return Op::Vi2c;
    case 30: return Op::Vi2us;
    case 31: return Op::Vi2s;
    default: return Op::Invalid;
  }
}

Op shuffleOp(uint32_t rt) {
  switch (rt) {
    case 0: return Op::Vsrt1;
    case 1: return Op::Vsrt2;
    case 2: return Op::Vbfy1;
    case 3: return Op::Vbfy2;
    case 4: return Op::Vocp;
    case 5: return Op::Vsocp;
    case 6: return Op::Vfad;
    case 7: return Op::Vavg;
    case 8: return Op::Vsrt3;
    case 9: return Op::Vsrt4;
    case 10: return Op::Vsgn;
    case 16: return Op::Vmfvc;
    case 17: return Op::Vmtvc;
    case 25: return Op::Vt4444;
    case 26: return Op::Vt5551;
    case 27: return Op::Vt5650;
    default: return Op::Invalid;
  }
}

Op vfpu4(uint32_t raw) {
  uint32_t rs = (raw >> 21) & 31, rt = (raw >> 16) & 31;
  switch (rs) {
    case 0: return unaryOp(rt);
    case 1: return convertOp(rt);
    case 2: return shuffleOp(rt);
    case 3: return Op::Vcst;
    case 16: return Op::Vf2in;
    case 17: return Op::Vf2iz;
    case 18: return Op::Vf2iu;
    case 19: return Op::Vf2id;
    case 20: return Op::Vi2f;
    case 21: return Op::Vcmov;
    default: return rs >= 24 ? Op::Vwbn : Op::Invalid;
  }
}

Op vfpu6(uint32_t raw) {
  switch ((raw >> 23) & 7) {
    case 0: return Op::Vmmul;
    case 1: case 2: case 3: return Op::Vtfm;
    case 4: return Op::Vmscl;
    case 5: return Op::Vcrsp;
    case 7:
      switch ((raw >> 21) & 31) {
        case 28:
          switch ((raw >> 16) & 15) {
            case 0: return Op::Vmmov;
            case 3: return Op::Vmidt;
            case 6: return Op::Vmzero;
            case 7: return Op::Vmone;
            default: return Op::Invalid;
          }
        case 29: return Op::Vrot;
        default: return Op::Invalid;
      }
    default: return Op::Invalid;
  }
}

}  // namespace

Op decodeVfpu(uint32_t raw) {
  switch (raw >> 26) {
    case 0x12:
      switch ((raw >> 21) & 31) {
        case 3: return Op::Mfv;
        case 7: return Op::Mtv;
        case 8:
          switch ((raw >> 16) & 3) {
            case 0: return Op::Bvf;
            case 1: return Op::Bvt;
            case 2: return Op::Bvfl;
            default: return Op::Bvtl;
          }
        default: return Op::Invalid;
      }
    case 0x18:
      switch ((raw >> 23) & 7) {
        case 0: return Op::Vadd;
        case 1: return Op::Vsub;
        case 2: return Op::Vsbn;
        case 7: return Op::Vdiv;
        default: return Op::Invalid;
      }
    case 0x19:
      switch ((raw >> 23) & 7) {
        case 0: return Op::Vmul;
        case 1: return Op::Vdot;
        case 2: return Op::Vscl;
        case 4: return Op::Vhdp;
        case 5: return Op::Vcrs;
        case 6: return Op::Vdet;
        default: return Op::Invalid;
      }
    case 0x1B:
      switch ((raw >> 23) & 7) {
        case 0: return Op::Vcmp;
        case 2: return Op::Vmin;
        case 3: return Op::Vmax;
        case 5: return Op::Vscmp;
        case 6: return Op::Vsge;
        case 7: return Op::Vslt;
        default: return Op::Invalid;
      }
    case 0x32: return Op::LvS;
    case 0x3A: return Op::SvS;
    case 0x34: return vfpu4(raw);
    case 0x35: return (raw & 2) ? Op::LvrQ : Op::LvlQ;
    case 0x36: return Op::LvQ;
    case 0x37:
      switch ((raw >> 23) & 7) {
        case 0: case 1: return Op::Vpfxs;
        case 2: case 3: return Op::Vpfxt;
        case 4: case 5: return Op::Vpfxd;
        case 6: return Op::Viim;
        default: return Op::Vfim;
      }
    case 0x3C: return vfpu6(raw);
    case 0x3D: return (raw & 2) ? Op::SvrQ : Op::SvlQ;
    case 0x3E: return Op::SvQ;
    case 0x3F:
      if (raw == 0xFFFF0000u) return Op::Vnop;
      if (raw == 0xFFFF040Eu) return Op::Vflush;
      // vsync comes in a couple of encodings (0x0320, 0x040D); none of them touch any state we model
      if ((raw >> 16) == 0xFFFFu) return Op::Vsync;
      return Op::Invalid;
    default: return Op::Invalid;
  }
}

}  // namespace mgspo
