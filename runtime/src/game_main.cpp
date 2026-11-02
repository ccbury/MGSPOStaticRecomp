// Entry point for the recompiled game. Loads the module image from a plain PRX/ELF the
// user extracted from their own disc, then runs the generated entry function.
#include <cstdio>
#include <exception>
#include <string>

#include <psp/runtime.h>

#include "bytes.h"
#include "prx.h"
#include "tables.h"
#include "funcs.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <module.elf>\n", argv[0]);
    return 2;
  }
  try {
    mgspo::PrxImage img = mgspo::loadPrx(mgspo::readFile(argv[1]), kModuleBase);
    psp::init_memory();
    psp::mem_write(img.base, img.mem.data(), img.mem.size());
    psp::bind_tables(kFuncs, kFuncCount, kImports, kImportCount);

    psp::Cpu cpu;
    cpu.r[28] = kModuleGp + kModuleBase;
    cpu.r[29] = psp::kRamBase + psp::kRamSize - 0x1000;
    cpu.r[31] = 0;
    psp::call(cpu, kModuleEntry);
    std::printf("module returned %u\n", cpu.r[2]);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "stopped: %s\n", e.what());
    return 1;
  }
  return 0;
}
