#include "mips.h"

#include <cstdio>

namespace mgspo {

const char* opName(Op op) {
  static const char* names[] = {
#define X(n) #n,
      MGSPO_OPS(X)
#undef X
  };
  return names[static_cast<int>(op)];
}

namespace {

Op special(uint32_t raw, const Insn& i) {
  switch (raw & 0x3F) {
    case 0x00: return Op::Sll;
    case 0x02: return i.rs == 1 ? Op::Rotr : Op::Srl;
    case 0x03: return Op::Sra;
    case 0x04: return Op::Sllv;
    case 0x06: return i.sa == 1 ? Op::Rotrv : Op::Srlv;
    case 0x07: return Op::Srav;
    case 0x08: return Op::Jr;
    case 0x09: return Op::Jalr;
    case 0x0A: return Op::Movz;
    case 0x0B: return Op::Movn;
    case 0x0C: return Op::Syscall;
    case 0x0D: return Op::Break;
    case 0x0F: return Op::Sync;
    case 0x10: return Op::Mfhi;
    case 0x11: return Op::Mthi;
    case 0x12: return Op::Mflo;
    case 0x13: return Op::Mtlo;
    case 0x16: return Op::Clz;
    case 0x17: return Op::Clo;
    case 0x18: return Op::Mult;
    case 0x19: return Op::Multu;
    case 0x1A: return Op::Div;
    case 0x1B: return Op::Divu;
    case 0x1C: return Op::Madd;
    case 0x1D: return Op::Maddu;
    case 0x20: return Op::Add;
    case 0x21: return Op::Addu;
    case 0x22: return Op::Sub;
    case 0x23: return Op::Subu;
    case 0x24: return Op::And;
    case 0x25: return Op::Or;
    case 0x26: return Op::Xor;
    case 0x27: return Op::Nor;
    case 0x2A: return Op::Slt;
    case 0x2B: return Op::Sltu;
    case 0x2C: return Op::Max;
    case 0x2D: return Op::Min;
    case 0x2E: return Op::Msub;
    case 0x2F: return Op::Msubu;
    default: return Op::Unknown;
  }
}

Op special3(uint32_t raw, const Insn& i) {
  switch (raw & 0x3F) {
    case 0x00: return Op::Ext;
    case 0x04: return Op::Ins;
    case 0x20:
      switch (i.sa) {
        case 0x02: return Op::Wsbh;
        case 0x03: return Op::Wsbw;
        case 0x10: return Op::Seb;
        case 0x14: return Op::Bitrev;
        case 0x18: return Op::Seh;
        default: return Op::Unknown;
      }
    default: return Op::Unknown;
  }
}

Op regimm(const Insn& i) {
  switch (i.rt) {
    case 0x00: return Op::Bltz;
    case 0x01: return Op::Bgez;
    case 0x02: return Op::Bltzl;
    case 0x03: return Op::Bgezl;
    case 0x10: return Op::Bltzal;
    case 0x11: return Op::Bgezal;
    case 0x12: return Op::Bltzall;
    case 0x13: return Op::Bgezall;
    default: return Op::Unknown;
  }
}

Op cop1(uint32_t raw, const Insn& i) {
  switch (i.rs) {
    case 0x00: return Op::Mfc1;
    case 0x02: return Op::Cfc1;
    case 0x04: return Op::Mtc1;
    case 0x06: return Op::Ctc1;
    case 0x08:
      switch (i.rt) {
        case 0: return Op::Bc1f;
        case 1: return Op::Bc1t;
        case 2: return Op::Bc1fl;
        case 3: return Op::Bc1tl;
        default: return Op::Unknown;
      }
    case 0x10: {
      uint32_t f = raw & 0x3F;
      switch (f) {
        case 0x00: return Op::AddS;
        case 0x01: return Op::SubS;
        case 0x02: return Op::MulS;
        case 0x03: return Op::DivS;
        case 0x04: return Op::SqrtS;
        case 0x05: return Op::AbsS;
        case 0x06: return Op::MovS;
        case 0x07: return Op::NegS;
        case 0x0C: return Op::RoundWS;
        case 0x0D: return Op::TruncWS;
        case 0x0E: return Op::CeilWS;
        case 0x0F: return Op::FloorWS;
        case 0x24: return Op::CvtWS;
        default: return f >= 0x30 ? Op::CmpS : Op::Unknown;
      }
    }
    case 0x14: return (raw & 0x3F) == 0x20 ? Op::CvtSW : Op::Unknown;
    default: return Op::Unknown;
  }
}

}  // namespace

Insn decode(uint32_t raw, uint32_t addr) {
  Insn i;
  i.raw = raw;
  i.addr = addr;
  i.rs = (raw >> 21) & 31;
  i.rt = (raw >> 16) & 31;
  i.rd = (raw >> 11) & 31;
  i.sa = (raw >> 6) & 31;
  i.imm = static_cast<int16_t>(raw & 0xFFFF);
  i.target = ((raw & 0x03FFFFFFu) << 2) | ((addr + 4) & 0xF0000000u);
  i.cond = raw & 0xF;

  switch (raw >> 26) {
    case 0x00: i.op = special(raw, i); break;
    case 0x01: i.op = regimm(i); break;
    case 0x02: i.op = Op::J; break;
    case 0x03: i.op = Op::Jal; break;
    case 0x04: i.op = Op::Beq; break;
    case 0x05: i.op = Op::Bne; break;
    case 0x06: i.op = Op::Blez; break;
    case 0x07: i.op = Op::Bgtz; break;
    case 0x08: i.op = Op::Addi; break;
    case 0x09: i.op = Op::Addiu; break;
    case 0x0A: i.op = Op::Slti; break;
    case 0x0B: i.op = Op::Sltiu; break;
    case 0x0C: i.op = Op::Andi; break;
    case 0x0D: i.op = Op::Ori; break;
    case 0x0E: i.op = Op::Xori; break;
    case 0x0F: i.op = Op::Lui; break;
    case 0x10: i.op = Op::Cop0; break;
    case 0x11: i.op = cop1(raw, i); break;
    case 0x14: i.op = Op::Beql; break;
    case 0x15: i.op = Op::Bnel; break;
    case 0x16: i.op = Op::Blezl; break;
    case 0x17: i.op = Op::Bgtzl; break;
    case 0x1F: i.op = special3(raw, i); break;
    case 0x20: i.op = Op::Lb; break;
    case 0x21: i.op = Op::Lh; break;
    case 0x22: i.op = Op::Lwl; break;
    case 0x23: i.op = Op::Lw; break;
    case 0x24: i.op = Op::Lbu; break;
    case 0x25: i.op = Op::Lhu; break;
    case 0x26: i.op = Op::Lwr; break;
    case 0x28: i.op = Op::Sb; break;
    case 0x29: i.op = Op::Sh; break;
    case 0x2A: i.op = Op::Swl; break;
    case 0x2B: i.op = Op::Sw; break;
    case 0x2E: i.op = Op::Swr; break;
    case 0x2F: i.op = Op::Cache; break;
    case 0x30: i.op = Op::Ll; break;
    case 0x31: i.op = Op::Lwc1; break;
    case 0x38: i.op = Op::Sc; break;
    case 0x39: i.op = Op::Swc1; break;
    case 0x12:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x32: case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x3A: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
      i.op = Op::Vfpu;
      break;
    default: i.op = Op::Unknown; break;
  }

  if (isRelBranch(i.op)) i.target = addr + 4 + static_cast<uint32_t>(i.imm) * 4;
  return i;
}

bool isRelBranch(Op op) {
  switch (op) {
    case Op::Bltz: case Op::Bgez: case Op::Bltzl: case Op::Bgezl:
    case Op::Bltzal: case Op::Bgezal: case Op::Bltzall: case Op::Bgezall:
    case Op::Beq: case Op::Bne: case Op::Blez: case Op::Bgtz:
    case Op::Beql: case Op::Bnel: case Op::Blezl: case Op::Bgtzl:
    case Op::Bc1f: case Op::Bc1t: case Op::Bc1fl: case Op::Bc1tl:
      return true;
    default:
      return false;
  }
}

bool hasDelaySlot(Op op) {
  return isRelBranch(op) || op == Op::J || op == Op::Jal || op == Op::Jr || op == Op::Jalr;
}

bool isLikely(Op op) {
  switch (op) {
    case Op::Bltzl: case Op::Bgezl: case Op::Bltzall: case Op::Bgezall:
    case Op::Beql: case Op::Bnel: case Op::Blezl: case Op::Bgtzl:
    case Op::Bc1fl: case Op::Bc1tl:
      return true;
    default:
      return false;
  }
}

bool isLinking(Op op) {
  return op == Op::Bltzal || op == Op::Bgezal || op == Op::Bltzall || op == Op::Bgezall;
}

std::string disassemble(const Insn& i) {
  char buf[96];
  if (i.raw == 0) return "nop";
  if (isRelBranch(i.op) || i.op == Op::J || i.op == Op::Jal)
    std::snprintf(buf, sizeof buf, "%s r%u, r%u, 0x%08X", opName(i.op), i.rs, i.rt, i.target);
  else
    std::snprintf(buf, sizeof buf, "%s rs=%u rt=%u rd=%u sa=%u imm=%d", opName(i.op), i.rs, i.rt, i.rd, i.sa, i.imm);
  return buf;
}

}  // namespace mgspo
