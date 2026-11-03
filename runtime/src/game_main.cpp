// Entry point for the recompiled game. Loads the module image from a plain PRX/ELF the
// user extracted from their own disc, then runs the generated entry function on the
// emulated kernel.
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#include <psp/ge_debug.h>
#include <psp/kernel.h>
#include <psp/runtime.h>

#include "sampler.h"
#include "bytes.h"
#include "prx.h"
#include "tables.h"
#include "funcs.h"

#ifndef MGSPO_PROMPT_ICONS_SUPPORTED
#define MGSPO_PROMPT_ICONS_SUPPORTED 0
#endif

static void usage(const char* prog);

namespace psp { namespace k { void display_set_dump_path(const std::string& p); void display_set_auto_buttons(uint32_t mask); void display_set_press_script(const std::string& s); void display_set_profile(bool on); } }
namespace psp { namespace k { void display_set_gamepad_script(const std::string& s); } }

static void usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s <module.elf> [--disc <dir>] [--save <dir>] [--iso <file>] [--modules <dir>] [--headless] [--trace] [--report <file>]\n"
               "  --disc    directory containing PSP_GAME (extracted disc), served as disc0:/\n"
               "  --iso     the disc image, for raw sector reads (disc0:/sce_lbn...)\n"
               "  --modules directory with decrypted helper modules (e.g. libfont.prx) lifted via --helper\n"
               "  --save    directory backing ms0:/ (default: ./savedata)\n"
               "  --headless  no window or audio\n"
               "  --controls <modern|legacy>  modern gamepad camera/weapons (default) or original PSP mapping\n"
               "  --controls-trace  log native direct equipment selections\n"
               "  --prompts <original|modern>  controller icons (default original; identified menus only)\n"
               "  --gamepad-press <script>  time:PSP-mask[:hold[:rx:ry[:lx:ly]]] (up = negative Y)\n"
               "  --trace   log every import call\n"
               "  --report  write the list of imports that were called to a file\n"
               "  --ge-dump write every GE command that is executed to a file\n"
               "  --ge-stats print GE command/draw statistics when the run ends\n"
               "  --fault-dump <addr> <len>  hex dump this guest memory range in fault reports\n"
               "  --watch <addr> <len>  log guest stores to a range (build with -DMGSPO_WATCH=ON)\n"
               "  --sample [delay]  sample thread instruction pointers (after delay seconds) and print where time went at exit\n"
               "  --profile  print frame rate, GE and present times every 5 seconds\n"
"  --press <sec:mask,...>  scripted presses: time:mask[:hold[:lx:ly]] with time/hold in game seconds, stick -128..127\n"
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
  psp::prof::sampler_report(stderr);
  psp::ge_write_stats(stderr);
  psp::kernel_dump_threads(stderr);
  std::fflush(nullptr);
}

int main(int argc, char** argv) {
  std::string module, reportPath;
  int timeoutSec = 0;
  bool modernControls = true, controlsTrace = false;
  bool modernPrompts = false;
  psp::KernelConfig cfg;
  psp::GeDebugOptions geOpt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    bool more = i + 1 < argc;
    if (a == "--disc" && more) cfg.discRoot = argv[++i];
    else if (a == "--controls" && more) {
      std::string mode = argv[++i];
      if (mode != "modern" && mode != "legacy") {
        std::fprintf(stderr, "invalid controls mode: %s (expected modern or legacy)\n", mode.c_str());
        return 2;
      }
      modernControls = mode == "modern";
    }
    else if (a == "--controls-trace") controlsTrace = true;
    else if (a == "--prompts" && more) {
      std::string mode = argv[++i];
      if (mode != "original" && mode != "modern") {
        std::fprintf(stderr, "invalid prompts mode: %s (expected original or modern)\n", mode.c_str());
        return 2;
      }
      modernPrompts = mode == "modern";
    }
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
    else if (a == "--sample") psp::prof::sampler_start(1000, more && argv[i + 1][0] != '-' ? std::atoi(argv[++i]) : 0);
    else if (a == "--headless") cfg.headless = true;
    else if (a == "--trace") cfg.trace = true;
    else if (a == "--ge-dump" && more) geOpt.dumpPath = argv[++i];
    else if (a == "--ge-stats") geOpt.stats = true;
    else if (a == "--auto-press" && more) psp::k::display_set_auto_buttons(static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)));
  else if (a == "--profile") psp::k::display_set_profile(true);
  else if (a == "--press" && more) psp::k::display_set_press_script(argv[++i]);
    else if (a == "--gamepad-press" && more) {
      try {
        psp::k::display_set_gamepad_script(argv[++i]);
      } catch (const std::invalid_argument& e) {
        std::fprintf(stderr, "invalid gamepad script: %s\n", e.what());
        return 2;
      } catch (const std::out_of_range& e) {
        std::fprintf(stderr, "gamepad script value out of range: %s\n", e.what());
        return 2;
      }
    }
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
    if (modernControls && !kModernControlsSupported)
      throw std::runtime_error("modern controls require the supported ULUS10202 lift; use --controls legacy");
    psp::controls_init(modernControls, controlsTrace);
    uint32_t heap = (img.base + static_cast<uint32_t>(img.mem.size()) + 0xFFF) & ~0xFFFu;
    // helper modules are mapped at fixed addresses behind the main image, keep the heap clear of them
    for (size_t i = 0; i < kModuleCount; ++i)
      heap = std::max(heap, (kModules[i].base + kModules[i].size + 0xFFF) & ~0xFFFu);

    psp::ov_init(kOverlayBase, kOverlayEnd);
    psp::kernel_init(cfg, heap, kModuleGp + kModuleBase);
    if (modernPrompts && !MGSPO_PROMPT_ICONS_SUPPORTED)
      throw std::runtime_error("modern prompts require a supported ULUS10202 lift; rebuild or use --prompts original");
    psp::prompts_init(modernPrompts);
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
