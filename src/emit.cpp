#include "emit.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

#include "mips.h"

namespace mgspo {

namespace {

std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08Xu", v);
  return b;
}

std::string label(uint32_t a) {
  char b[24];
  std::snprintf(b, sizeof b, "L_%08X", a);
  return b;
}

std::string R(unsigned n) { return n == 0 ? "0u" : "c.r[" + std::to_string(n) + "]"; }
std::string F(unsigned n) { return "c.f[" + std::to_string(n) + "]"; }
std::string S(unsigned n) { return "(int32_t)" + R(n); }

std::string W(unsigned n, const std::string& expr) {
  if (n == 0) return "";
  return "c.r[" + std::to_string(n) + "] = " + expr + ";";
}

std::string addrExpr(const Insn& i) {
  if (i.imm == 0) return R(i.rs);
  return "(" + R(i.rs) + " + " + hex32(static_cast<uint32_t>(i.imm)) + ")";
}

struct Ctx {
  const Program& prog;
  const Function& fn;
  size_t unimpl = 0;
};

std::string unimplStmt(Ctx& cx, const Insn& i) {
  ++cx.unimpl;
  return "psp::unimpl(c, " + hex32(i.addr) + ", " + hex32(i.raw) + ");";
}

// Straight-line (non delay slot) instructions.
std::string simple(Ctx& cx, const Insn& i) {
  const unsigned rs = i.rs, rt = i.rt, rd = i.rd, sa = i.sa;
  const std::string imm = hex32(static_cast<uint32_t>(i.imm));
  const std::string uimm = hex32(i.raw & 0xFFFF);
  switch (i.op) {
    case Op::Sll: return (rd == 0) ? "" : W(rd, R(rt) + " << " + std::to_string(sa));
    case Op::Srl: return W(rd, R(rt) + " >> " + std::to_string(sa));
    case Op::Sra: return W(rd, "(uint32_t)(" + S(rt) + " >> " + std::to_string(sa) + ")");
    case Op::Sllv: return W(rd, R(rt) + " << (" + R(rs) + " & 31)");
    case Op::Srlv: return W(rd, R(rt) + " >> (" + R(rs) + " & 31)");
    case Op::Srav: return W(rd, "(uint32_t)(" + S(rt) + " >> (" + R(rs) + " & 31))");
    case Op::Rotr: return W(rd, "psp::rotr(" + R(rt) + ", " + std::to_string(sa) + ")");
    case Op::Rotrv: return W(rd, "psp::rotr(" + R(rt) + ", " + R(rs) + ")");
    case Op::Movz: return rd ? "if (" + R(rt) + " == 0) " + W(rd, R(rs)) : "";
    case Op::Movn: return rd ? "if (" + R(rt) + " != 0) " + W(rd, R(rs)) : "";
    case Op::Syscall: return "psp::syscall_insn(c, " + hex32((i.raw >> 6) & 0xFFFFF) + ");";
    case Op::Break: return "psp::break_insn(c, " + hex32((i.raw >> 6) & 0xFFFFF) + ");";
    case Op::Sync: case Op::Cache: return "";
    case Op::Mfhi: return W(rd, "c.hi");
    case Op::Mflo: return W(rd, "c.lo");
    case Op::Mthi: return "c.hi = " + R(rs) + ";";
    case Op::Mtlo: return "c.lo = " + R(rs) + ";";
    case Op::Clz: return W(rd, "psp::clz(" + R(rs) + ")");
    case Op::Clo: return W(rd, "psp::clo(" + R(rs) + ")");
    case Op::Mult: return "psp::mult(c, " + R(rs) + ", " + R(rt) + ");";
    case Op::Multu: return "psp::multu(c, " + R(rs) + ", " + R(rt) + ");";
    case Op::Div: return "psp::div(c, " + R(rs) + ", " + R(rt) + ");";
    case Op::Divu: return "psp::divu(c, " + R(rs) + ", " + R(rt) + ");";
    case Op::Madd: return "psp::madd(c, " + R(rs) + ", " + R(rt) + ");";
    case Op::Maddu: return "psp::maddu(c, " + R(rs) + ", " + R(rt) + ");";
    case Op::Msub: return "psp::msub(c, " + R(rs) + ", " + R(rt) + ");";
    case Op::Msubu: return "psp::msubu(c, " + R(rs) + ", " + R(rt) + ");";
    case Op::Add: case Op::Addu: return W(rd, R(rs) + " + " + R(rt));
    case Op::Sub: case Op::Subu: return W(rd, R(rs) + " - " + R(rt));
    case Op::And: return W(rd, R(rs) + " & " + R(rt));
    case Op::Or: return W(rd, R(rs) + " | " + R(rt));
    case Op::Xor: return W(rd, R(rs) + " ^ " + R(rt));
    case Op::Nor: return W(rd, "~(" + R(rs) + " | " + R(rt) + ")");
    case Op::Slt: return W(rd, "(" + S(rs) + " < " + S(rt) + ") ? 1u : 0u");
    case Op::Sltu: return W(rd, "(" + R(rs) + " < " + R(rt) + ") ? 1u : 0u");
    case Op::Max: return W(rd, "psp::max_s(" + R(rs) + ", " + R(rt) + ")");
    case Op::Min: return W(rd, "psp::min_s(" + R(rs) + ", " + R(rt) + ")");
    case Op::Addi: case Op::Addiu: return W(rt, R(rs) + " + " + imm);
    case Op::Slti: return W(rt, "(" + S(rs) + " < (int32_t)" + imm + ") ? 1u : 0u");
    case Op::Sltiu: return W(rt, "(" + R(rs) + " < " + imm + ") ? 1u : 0u");
    case Op::Andi: return W(rt, R(rs) + " & " + uimm);
    case Op::Ori: return W(rt, R(rs) + " | " + uimm);
    case Op::Xori: return W(rt, R(rs) + " ^ " + uimm);
    case Op::Lui: return W(rt, hex32((i.raw & 0xFFFF) << 16));
    case Op::Ext: return W(rt, "psp::ext(" + R(rs) + ", " + std::to_string(sa) + ", " + std::to_string(rd + 1) + ")");
    case Op::Ins:
      return W(rt, "psp::ins(" + R(rt) + ", " + R(rs) + ", " + std::to_string(sa) + ", " +
                       std::to_string(static_cast<int>(rd) - static_cast<int>(sa) + 1) + ")");
    case Op::Wsbh: return W(rd, "psp::wsbh(" + R(rt) + ")");
    case Op::Wsbw: return W(rd, "psp::wsbw(" + R(rt) + ")");
    case Op::Seb: return W(rd, "psp::seb(" + R(rt) + ")");
    case Op::Seh: return W(rd, "psp::seh(" + R(rt) + ")");
    case Op::Bitrev: return W(rd, "psp::bitrev(" + R(rt) + ")");
    case Op::Lb: return W(rt, "(uint32_t)(int32_t)psp::lb(" + addrExpr(i) + ")");
    case Op::Lbu: return W(rt, "psp::lbu(" + addrExpr(i) + ")");
    case Op::Lh: return W(rt, "(uint32_t)(int32_t)psp::lh(" + addrExpr(i) + ")");
    case Op::Lhu: return W(rt, "psp::lhu(" + addrExpr(i) + ")");
    case Op::Lw: case Op::Ll: return W(rt, "psp::lw(" + addrExpr(i) + ")");
    case Op::Lwl: return W(rt, "psp::lwl(" + addrExpr(i) + ", " + R(rt) + ")");
    case Op::Lwr: return W(rt, "psp::lwr(" + addrExpr(i) + ", " + R(rt) + ")");
    case Op::Sb: return "psp::sb(" + addrExpr(i) + ", " + R(rt) + ");";
    case Op::Sh: return "psp::sh(" + addrExpr(i) + ", " + R(rt) + ");";
    case Op::Sw: return "psp::sw(" + addrExpr(i) + ", " + R(rt) + ");";
    case Op::Swl: return "psp::swl(" + addrExpr(i) + ", " + R(rt) + ");";
    case Op::Swr: return "psp::swr(" + addrExpr(i) + ", " + R(rt) + ");";
    case Op::Sc: return "psp::sw(" + addrExpr(i) + ", " + R(rt) + "); " + W(rt, "1u");
    case Op::Lwc1: return F(rt) + " = psp::u2f(psp::lw(" + addrExpr(i) + "));";
    case Op::Swc1: return "psp::sw(" + addrExpr(i) + ", psp::f2u(" + F(rt) + "));";
    case Op::Mfc1: return W(rt, "psp::f2u(" + F(rd) + ")");
    case Op::Mtc1: return F(rd) + " = psp::u2f(" + R(rt) + ");";
    case Op::Cfc1: return W(rt, "psp::cfc1(c, " + std::to_string(rd) + ")");
    case Op::Ctc1: return "psp::ctc1(c, " + std::to_string(rd) + ", " + R(rt) + ");";
    // FPU: ft = rt, fs = rd, fd = sa
    case Op::AddS: return F(sa) + " = " + F(rd) + " + " + F(rt) + ";";
    case Op::SubS: return F(sa) + " = " + F(rd) + " - " + F(rt) + ";";
    case Op::MulS: return F(sa) + " = " + F(rd) + " * " + F(rt) + ";";
    case Op::DivS: return F(sa) + " = " + F(rd) + " / " + F(rt) + ";";
    case Op::SqrtS: return F(sa) + " = std::sqrt(" + F(rd) + ");";
    case Op::AbsS: return F(sa) + " = std::fabs(" + F(rd) + ");";
    case Op::MovS: return F(sa) + " = " + F(rd) + ";";
    case Op::NegS: return F(sa) + " = -" + F(rd) + ";";
    case Op::RoundWS: return F(sa) + " = psp::u2f(psp::round_w(" + F(rd) + "));";
    case Op::TruncWS: return F(sa) + " = psp::u2f(psp::trunc_w(" + F(rd) + "));";
    case Op::CeilWS: return F(sa) + " = psp::u2f(psp::ceil_w(" + F(rd) + "));";
    case Op::FloorWS: return F(sa) + " = psp::u2f(psp::floor_w(" + F(rd) + "));";
    case Op::CvtWS: return F(sa) + " = psp::u2f(psp::cvt_w(" + F(rd) + "));";
    case Op::CvtSW: return F(sa) + " = (float)(int32_t)psp::f2u(" + F(rd) + ");";
    case Op::CmpS:
      return "c.fcc = psp::fcmp(" + F(rd) + ", " + F(rt) + ", " + std::to_string(i.cond & 7) + ");";
    default: return unimplStmt(cx, i);
  }
}

// Branch condition expression; empty string for an unconditional branch, "?" if unsupported.
std::string condition(const Insn& i) {
  switch (i.op) {
    case Op::Beq: case Op::Beql:
      return i.rs == i.rt ? "" : R(i.rs) + " == " + R(i.rt);
    case Op::Bne: case Op::Bnel: return R(i.rs) + " != " + R(i.rt);
    case Op::Blez: case Op::Blezl: return S(i.rs) + " <= 0";
    case Op::Bgtz: case Op::Bgtzl: return S(i.rs) + " > 0";
    case Op::Bltz: case Op::Bltzl: return S(i.rs) + " < 0";
    case Op::Bgez: case Op::Bgezl: return S(i.rs) + " >= 0";
    case Op::Bc1f: case Op::Bc1fl: return "!c.fcc";
    case Op::Bc1t: case Op::Bc1tl: return "c.fcc";
    default: return "?";
  }
}

std::string callTo(const Program& prog, uint32_t target) {
  auto st = prog.stubIndex.find(target);
  if (st != prog.stubIndex.end()) return "psp::hle(c, " + std::to_string(st->second) + ");";
  if (prog.find(target)) return funcName(target) + "(c);";
  return "psp::call(c, " + hex32(target) + ");";
}

}  // namespace

std::string funcName(uint32_t addr) {
  char b[24];
  std::snprintf(b, sizeof b, "f_%08X", addr);
  return b;
}

std::string emitFunction(const Program& prog, const Function& fn, size_t* unimplemented) {
  const PrxImage& img = *prog.img;
  Ctx cx{prog, fn};

  auto inFn = [&](uint32_t a) { return a >= fn.start && a < fn.end; };

  // Pass 1: which addresses need labels.
  std::set<uint32_t> labels;
  for (uint32_t a = fn.start; a < fn.end; a += 4) {
    Insn i = decode(img.word(a), a);
    if (isRelBranch(i.op) && inFn(i.target)) labels.insert(i.target);
    if (isRelBranch(i.op) && isLikely(i.op)) labels.insert(a + 8);
    if (img.jumpTargets.count(a) || img.addressTaken.count(a)) labels.insert(a);
    if (i.op == Op::J && inFn(i.target)) labels.insert(i.target);
  }
  std::vector<uint32_t> switchTargets;
  for (uint32_t a : labels)
    if (img.jumpTargets.count(a) || img.addressTaken.count(a)) switchTargets.push_back(a);

  std::string out;
  std::set<uint32_t> skip, terminal;
  out += "void " + funcName(fn.start) + "(psp::Cpu& c) {\n";

  auto delayOf = [&](uint32_t pc) {
    Insn d = decode(img.word(pc + 4), pc + 4);
    if (hasDelaySlot(d.op)) return unimplStmt(cx, d);
    return simple(cx, d);
  };
  auto tail = [&](uint32_t target) {
    return inFn(target) ? "goto " + label(target) + ";" : callTo(prog, target) + " return;";
  };

  for (uint32_t pc = fn.start; pc < fn.end; pc += 4) {
    bool labelled = labels.count(pc) != 0;
    if (skip.count(pc) && !labelled) continue;
    if (labelled) out += label(pc) + ":;\n";
    Insn i = decode(img.word(pc), pc);

    if (!hasDelaySlot(i.op)) {
      std::string s = simple(cx, i);
      if (!s.empty()) out += "  " + s + "\n";
      continue;
    }

    std::string delay = delayOf(pc);
    bool linkedBranch = isLinking(i.op);
    switch (i.op) {
      case Op::Jr:
        if (i.rs == 31) {
          out += "  { " + delay + " return; }\n";
        } else {
          out += "  { uint32_t t = " + R(i.rs) + "; " + delay + " switch (t) {";
          for (uint32_t t : switchTargets) out += " case " + hex32(t) + ": goto " + label(t) + ";";
          out += " default: psp::call(c, t); return; } }\n";
        }
        terminal.insert(pc);
        skip.insert(pc + 4);
        break;
      case Op::Jalr:
        out += "  { uint32_t t = " + R(i.rs) + "; " + delay + " " + W(i.rd, hex32(pc + 8)) +
               " psp::call(c, t); }\n";
        skip.insert(pc + 4);
        break;
      case Op::Jal:
        out += "  c.r[31] = " + hex32(pc + 8) + "; " + delay + " " + callTo(prog, i.target) + "\n";
        skip.insert(pc + 4);
        break;
      case Op::J:
        out += "  { " + delay + " " + tail(i.target) + " }\n";
        terminal.insert(pc);
        skip.insert(pc + 4);
        break;
      default: {
        std::string cond = condition(i);
        if (linkedBranch) {
          // Only the unconditional "bal" form shows up in practice.
          if (i.op == Op::Bgezal && i.rs == 0) {
            out += "  c.r[31] = " + hex32(pc + 8) + "; " + delay + " " + callTo(prog, i.target) + "\n";
          } else {
            out += "  " + unimplStmt(cx, i) + "\n";
          }
          skip.insert(pc + 4);
        } else if (cond == "?") {
          out += "  " + unimplStmt(cx, i) + "\n";
        } else if (cond.empty()) {
          out += "  { " + delay + " " + tail(i.target) + " }\n";
          terminal.insert(pc);
          skip.insert(pc + 4);
        } else if (isLikely(i.op)) {
          out += "  if (" + cond + ") { " + delay + " " + tail(i.target) + " }\n";
          out += "  goto " + label(pc + 8) + ";\n";
          skip.insert(pc + 4);
        } else {
          // The delay slot is emitted again below for the not-taken path.
          out += "  { bool t = " + cond + "; if (t) { " + delay + " " + tail(i.target) + " } }\n";
        }
      }
    }
  }

  bool ends = terminal.count(fn.end - 8) || terminal.count(fn.end - 4);
  if (!ends) out += "  " + callTo(prog, fn.end) + "\n";
  out += "}\n";

  // Drop labels nothing jumps to (keeps the compiler quiet).
  for (uint32_t a : labels) {
    if (out.find("goto " + label(a) + ";") == std::string::npos) {
      std::string def = label(a) + ":;\n";
      size_t p = out.find(def);
      if (p != std::string::npos) out.erase(p, def.size());
    }
  }

  if (unimplemented) *unimplemented += cx.unimpl;
  return out;
}

namespace {

void writeFile(const std::filesystem::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + p.string());
  f << s;
}

}  // namespace

EmitResult emitAll(const Program& prog, const EmitOptions& opts) {
  namespace fs = std::filesystem;
  fs::create_directories(opts.outDir);
  EmitResult res;
  const PrxImage& img = *prog.img;

  std::string header =
      "// Generated by mgspo-lift. Do not edit.\n#pragma once\n#include <psp/runtime.h>\n\n";
  for (const auto& f : prog.funcs) header += "void " + funcName(f.start) + "(psp::Cpu& c);\n";
  writeFile(fs::path(opts.outDir) / "funcs.h", header);
  res.bytes += header.size();

  size_t n = 0, fileNo = 0;
  std::string buf;
  auto flush = [&]() {
    if (buf.empty()) return;
    char name[32];
    std::snprintf(name, sizeof name, "funcs_%03zu.cpp", fileNo++);
    std::string text = "// Generated by mgspo-lift. Do not edit.\n#include <cmath>\n#include \"funcs.h\"\n\n" + buf;
    writeFile(fs::path(opts.outDir) / name, text);
    res.bytes += text.size();
    ++res.files;
    buf.clear();
  };
  for (const auto& f : prog.funcs) {
    buf += emitFunction(prog, f, &res.unimplemented) + "\n";
    ++res.functions;
    if (++n % opts.funcsPerFile == 0) flush();
  }
  flush();

  std::string th =
      "// Generated by mgspo-lift. Do not edit.\n#pragma once\n#include <cstddef>\n#include <psp/cpu.h>\n\n"
      "extern const psp::FuncEntry kFuncs[];\nextern const size_t kFuncCount;\n"
      "extern const psp::ImportEntry kImports[];\nextern const size_t kImportCount;\n"
      "constexpr uint32_t kModuleBase = " + hex32(img.base) + ";\n"
      "constexpr uint32_t kModuleEntry = " + hex32(img.entry) + ";\n"
      "constexpr uint32_t kModuleGp = " + hex32(img.gp) + ";\n";
  writeFile(fs::path(opts.outDir) / "tables.h", th);

  std::string tc = "// Generated by mgspo-lift. Do not edit.\n#include \"tables.h\"\n#include \"funcs.h\"\n\n"
                   "const psp::FuncEntry kFuncs[] = {\n";
  for (const auto& f : prog.funcs) tc += "  {" + hex32(f.start) + ", " + funcName(f.start) + "},\n";
  tc += "};\nconst size_t kFuncCount = " + std::to_string(prog.funcs.size()) + ";\n\n";
  tc += "const psp::ImportEntry kImports[] = {\n";
  for (const auto& im : prog.imports) {
    char nid[16];
    std::snprintf(nid, sizeof nid, "0x%08Xu", im.nid);
    tc += "  {" + hex32(im.stub) + ", \"" + im.module + "\", " + nid + "},\n";
  }
  tc += "};\nconst size_t kImportCount = " + std::to_string(prog.imports.size()) + ";\n";
  writeFile(fs::path(opts.outDir) / "tables.cpp", tc);
  res.files += 3;
  res.bytes += th.size() + tc.size();
  return res;
}

}  // namespace mgspo
