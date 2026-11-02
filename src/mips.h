#pragma once
#include <cstdint>
#include <string>

namespace mgspo {

// Allegrex (PSP CPU) instruction set: MIPS32r2 subset plus a handful of custom ops.
// VFPU instructions other than the CC branches decode to Op::Vfpu; vfpu.h sorts out which one.
#define MGSPO_OPS(X)                                                                                              \
  X(Invalid) X(Sll) X(Srl) X(Sra) X(Sllv) X(Srlv) X(Srav) X(Rotr) X(Rotrv) X(Jr) X(Jalr) X(Movz) X(Movn)         \
  X(Syscall) X(Break) X(Sync) X(Mfhi) X(Mthi) X(Mflo) X(Mtlo) X(Clz) X(Clo) X(Mult) X(Multu) X(Div) X(Divu)       \
  X(Madd) X(Maddu) X(Msub) X(Msubu) X(Add) X(Addu) X(Sub) X(Subu) X(And) X(Or) X(Xor) X(Nor) X(Slt) X(Sltu)       \
  X(Max) X(Min) X(Bltz) X(Bgez) X(Bltzl) X(Bgezl) X(Bltzal) X(Bgezal) X(Bltzall) X(Bgezall) X(J) X(Jal) X(Beq)   \
  X(Bne) X(Blez) X(Bgtz) X(Beql) X(Bnel) X(Blezl) X(Bgtzl) X(Addi) X(Addiu) X(Slti) X(Sltiu) X(Andi) X(Ori)      \
  X(Xori) X(Lui) X(Ext) X(Ins) X(Wsbh) X(Wsbw) X(Seb) X(Seh) X(Bitrev) X(Lb) X(Lh) X(Lwl) X(Lw) X(Lbu) X(Lhu)    \
  X(Lwr) X(Sb) X(Sh) X(Swl) X(Sw) X(Swr) X(Cache) X(Ll) X(Sc) X(Lwc1) X(Swc1) X(Mfc1) X(Cfc1) X(Mtc1) X(Ctc1)    \
  X(Bc1f) X(Bc1t) X(Bc1fl) X(Bc1tl) X(AddS) X(SubS) X(MulS) X(DivS) X(SqrtS) X(AbsS) X(MovS) X(NegS)             \
  X(RoundWS) X(TruncWS) X(CeilWS) X(FloorWS) X(CvtWS) X(CvtSW) X(CmpS) X(Bvf) X(Bvt) X(Bvfl) X(Bvtl) X(Cop0) X(Vfpu) X(Unknown)

enum class Op : uint8_t {
#define X(n) n,
  MGSPO_OPS(X)
#undef X
};

const char* opName(Op op);

struct Insn {
  Op op = Op::Invalid;
  uint32_t raw = 0;
  uint32_t addr = 0;
  uint8_t rs = 0, rt = 0, rd = 0, sa = 0;
  int32_t imm = 0;        // sign-extended 16-bit immediate / branch offset in words
  uint32_t target = 0;    // absolute target for J/Jal, branch target for conditional branches
  uint8_t cond = 0;       // FPU compare condition
};

Insn decode(uint32_t raw, uint32_t addr);

// True for instructions followed by a delay slot.
bool hasDelaySlot(Op op);
// Conditional or unconditional PC-relative branch (not J/Jal/Jr/Jalr).
bool isRelBranch(Op op);
// Branch-likely: the delay slot only runs when the branch is taken.
bool isLikely(Op op);
// Branches that write the link register.
bool isLinking(Op op);

std::string disassemble(const Insn& i);

}  // namespace mgspo
