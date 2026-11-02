// mgspo-lift: turn a PSP PRX/ELF into C++ sources for the runtime to link against.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "bytes.h"
#include "emit.h"
#include "functions.h"
#include "mips.h"
#include "prx.h"
#include "vfpu.h"

using namespace mgspo;

static void usage() {
  std::cerr << "usage: mgspo-lift <module.elf> <outdir> [--base 0xADDR] [--per-file N]\n"
               "       mgspo-lift <module.elf> --stats [--base 0xADDR]\n"
               "       mgspo-lift <module.elf> --vfpu\n"
               "       mgspo-lift <module.elf> --disasm <start> <count>\n";
}

static void printStats(const Program& p, const PrxImage& img) {
  const DiscoveryStats& s = p.stats;
  uint64_t covered = 0, total = 0;
  for (const auto& f : p.funcs) covered += (f.end - f.start) / 4;
  for (const auto& r : img.code) total += (r.end - r.start) / 4;
  std::printf("module base       0x%08X\n", img.base);
  std::printf("entry             0x%08X\n", img.entry);
  std::printf("gp                0x%08X\n", img.gp);
  std::printf("imports           %zu\n", p.imports.size());
  std::printf("functions         %zu (call targets %u, jump roots %u, pointer roots %u)\n", p.funcs.size(),
              s.callTargets, s.jumpRoots, s.pointerRoots);
  std::printf("code words        %llu (covered by functions %llu)\n", static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(covered));
  std::printf("vfpu insns        %u (undecoded %u, cc branches %u)\n", s.vfpuInsns, s.vfpuUndecoded, s.vfpuBranches);
  std::printf("cop0 insns        %u\n", s.cop0Insns);
  std::printf("unknown insns     %u\n", s.unknownInsns);
  std::printf("calls outside code %u\n", s.callsOutsideCode);
  std::printf("switch targets    %zu\n", img.jumpTargets.size());
  std::printf("unhandled relocs  %u\n", img.unknownRelocs);
}

static void printVfpuHistogram(const PrxImage& img) {
  std::map<std::string, unsigned> hist;
  std::map<uint32_t, unsigned> bad;
  for (const auto& r : img.code)
    for (uint32_t a = r.start; a + 4 <= r.end; a += 4) {
      Insn i = decode(img.word(a), a);
      if (i.op != Op::Vfpu) continue;
      psp::vfpu::Op v = decodeVfpu(i.raw);
      if (v == psp::vfpu::Op::Invalid) ++bad[i.raw];
      else ++hist[psp::vfpu::op_name(v)];
    }
  for (const auto& h : hist) std::printf("%-8s %u\n", h.first.c_str(), h.second);
  for (const auto& b : bad) std::printf("undecoded %08X x%u\n", b.first, b.second);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  try {
    std::string in = argv[1];
    std::string out;
    uint32_t base = kDefaultBase;
    size_t perFile = 150;
    bool stats = false, disasm = false, vfpuHist = false;
    uint32_t dStart = 0, dCount = 0;
    for (int i = 2; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--base" && i + 1 < argc) base = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 0));
      else if (a == "--per-file" && i + 1 < argc) perFile = std::stoul(argv[++i]);
      else if (a == "--stats") stats = true;
      else if (a == "--vfpu") vfpuHist = true;
      else if (a == "--disasm" && i + 2 < argc) {
        disasm = true;
        dStart = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 0));
        dCount = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 0));
      } else if (a[0] != '-' && out.empty()) out = a;
      else {
        usage();
        return 2;
      }
    }

    PrxImage img = loadPrx(readFile(in), base);
    if (vfpuHist) {
      printVfpuHistogram(img);
      return 0;
    }
    if (disasm) {
      for (uint32_t k = 0; k < dCount; ++k) {
        uint32_t a = dStart + k * 4;
        if (!img.contains(a)) break;
        Insn i = decode(img.word(a), a);
        std::printf("%08X  %08X  %s\n", a, i.raw, disassemble(i).c_str());
      }
      return 0;
    }

    Program prog = discover(img);
    if (stats || out.empty()) {
      printStats(prog, img);
      if (out.empty()) return 0;
    }

    EmitOptions opts;
    opts.outDir = out;
    opts.funcsPerFile = perFile;
    EmitResult r = emitAll(prog, opts);
    std::printf("wrote %zu files, %zu functions, %.1f MB, %zu untranslated instructions\n", r.files,
                r.functions, r.bytes / 1048576.0, r.unimplemented);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
