// Entry point for the recompiled game. Loads the module image from a plain PRX/ELF the
// user extracted from their own disc, then runs the generated entry function on the
// emulated kernel.
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>

#include <psp/ge_debug.h>
#include <psp/kernel.h>
#include <psp/runtime.h>

#include "bytes.h"
#include "prx.h"
#include "tables.h"
#include "funcs.h"

static void usage(const char* prog);

namespace psp { namespace k { void display_set_dump_path(const std::string& p); void display_set_auto_buttons(uint32_t mask); void display_set_press_script(const std::string& s); } }

static void usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s <module.elf> [--disc <dir>] [--save <dir>] [--iso <file>] [--modules <dir>] [--headless] [--trace] [--report <file>]\n"
               "  --disc    directory containing PSP_GAME (extracted disc), served as disc0:/\n"
               "  --iso     the disc image, for raw sector reads (disc0:/sce_lbn...)\n"
               "  --modules directory with decrypted helper modules (e.g. libfont.prx) lifted via --helper\n"
               "  --save    directory backing ms0:/ (default: ./savedata)\n"
               "  --headless  no window or audio\n"
               "  --trace   log every import call\n"
               "  --report  write the list of imports that were called to a file\n"
               "  --ge-dump write every GE command that is executed to a file\n"
               "  --ge-stats print GE command/draw statistics when the run ends\n"
               "  --fault-dump <addr> <len>  hex dump this guest memory range in fault reports\n"
               "  --watch <addr> <len>  log guest stores to a range (build with -DMGSPO_WATCH=ON)\n"
               "  --press <sec:mask,...>  scripted one-shot button presses at the given run times (seconds)\n"
               "  --dump-frame <png>  keep writing the displayed frame to this PNG (local debugging aid)\n"
               "  --auto-press <mask>  periodically press these PSP buttons (e.g. 0x8 = Start) for unattended runs\n"
               "  --timeout stop after this many seconds (and write the report)\n",
               prog);
}

static void write_report(const std::string& path) {
  if (!path.empty()) {
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      psp::write_import_report(f);
      std::fclose(f);
    }
  } else {
    psp::write_import_report(stderr);
  }
  psp::ge_write_stats(stderr);
  std::fflush(nullptr);
}

int main(int argc, char** argv) {
  std::string module, reportPath;
  int timeoutSec = 0;
  psp::KernelConfig cfg;
  psp::GeDebugOptions geOpt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    bool more = i + 1 < argc;
    if (a == "--disc" && more) cfg.discRoot = argv[++i];
    else if (a == "--save" && more) cfg.saveRoot = argv[++i];
    else if (a == "--iso" && more) cfg.discImage = argv[++i];
    else if (a == "--modules" && more) cfg.moduleDir = argv[++i];
    else if (a == "--report" && more) reportPath = argv[++i];
    else if (a == "--timeout" && more) timeoutSec = std::atoi(argv[++i]);
    else if (a == "--fault-dump" && i + 2 < argc) {
      uint32_t addr = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
      psp::set_fault_dump(addr, static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
    }
    else if (a == "--watch" && i + 2 < argc) {
      uint32_t addr = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
      psp::set_watch(addr, static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
    }
    else if (a == "--headless") cfg.headless = true;
    else if (a == "--trace") cfg.trace = true;
    else if (a == "--ge-dump" && more) geOpt.dumpPath = argv[++i];
    else if (a == "--ge-stats") geOpt.stats = true;
    else if (a == "--auto-press" && more) psp::k::display_set_auto_buttons(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
    else if (a == "--press" && more) psp::k::display_set_press_script(argv[++i]);
    else if (a == "--dump-frame" && more) psp::k::display_set_dump_path(argv[++i]);
    else if (module.empty() && a[0] != '-') module = a;
    else {
      usage(argv[0]);
      return 2;
    }
  }
  if (module.empty()) {
    usage(argv[0]);
    return 2;
  }

  if (timeoutSec > 0) {
    std::thread([=] {
      std::this_thread::sleep_for(std::chrono::seconds(timeoutSec));
      std::fprintf(stderr, "timeout reached, stopping\n");
      write_report(reportPath);
      std::_Exit(0);
    }).detach();
  }

  int code = 0;
  try {
    mgspo::PrxImage img = mgspo::loadPrx(mgspo::readFile(module), kModuleBase);
    psp::init_memory();
    psp::ge_set_debug(geOpt);
    psp::mem_write(img.base, img.mem.data(), img.mem.size());
    uint32_t heap = (img.base + static_cast<uint32_t>(img.mem.size()) + 0xFFF) & ~0xFFFu;
    // helper modules are mapped at fixed addresses behind the main image, keep the heap clear of them
    for (size_t i = 0; i < kModuleCount; ++i)
      heap = std::max(heap, (kModules[i].base + kModules[i].size + 0xFFF) & ~0xFFFu);

    psp::ov_init(kOverlayBase, kOverlayEnd);
    psp::kernel_init(cfg, heap, kModuleGp + kModuleBase);
    psp::kernel_bind_modules(kModules, kModuleCount);
    psp::register_all_hle();
    psp::bind_tables(kFuncs, kFuncCount, kImports, kImportCount);
    code = psp::kernel_run_main(kModuleEntry);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "stopped: %s\n", e.what());
    code = 1;
  }

  write_report(reportPath);
  // other guest threads may still be parked on host threads, so skip normal teardown
  std::_Exit(code);
}
