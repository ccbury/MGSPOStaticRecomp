#pragma once
#include <cstdint>
#include <string>

#include "cpu.h"
#include "module.h"

namespace psp {

struct KernelConfig {
  std::string discRoot;             // directory that contains PSP_GAME (the extracted disc)
  std::string discImage;            // the .iso itself; needed for raw sce_lbn reads (disc0:/sce_lbn0x..._size0x...)
  std::string moduleDir;            // optional folder of already-decrypted helper modules, used instead of the disc copies
  std::string saveRoot = "savedata";  // backs ms0:/
  bool headless = false;            // no window, no audio device
  bool trace = false;               // log every import call
  uint32_t userMemEnd = 0x0A000000;  // end of the user partition (32 MB model)
};

// Sets up the kernel state. userHeapStart is the first free guest address after the module image.
void kernel_init(const KernelConfig& cfg, uint32_t userHeapStart, uint32_t gp);

// Tells the module manager which modules were lifted. The first entry is the main module (already
// resident); the others are mapped from the disc when the game loads them. Call after kernel_init().
void kernel_bind_modules(const ModuleDesc* mods, size_t count);

// Registers every firmware import handler. Call before bind_tables().
void register_all_hle();

// Runs the module entry on the calling host thread as the PSP "user main" thread and returns once
// the game exits, every thread has finished, or the program is stuck. Returns a process exit code.
void kernel_dump_threads(FILE* f);
int kernel_run_main(uint32_t entry, uint32_t argSize = 0, const void* args = nullptr);

}  // namespace psp
