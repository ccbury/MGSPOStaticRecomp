#pragma once
#include <cstddef>
#include <cstdint>

#include "cpu.h"

namespace psp {

struct ModuleExport {
  const char* lib;  // empty for the module's own table (module_start/module_stop)
  uint32_t nid;
  uint32_t addr;  // absolute guest address
};

// A module that was lifted ahead of time. Its image is read from the disc at load time and mapped at
// `base`, which is fixed so the generated code can use absolute addresses.
struct ModuleDesc {
  const char* name;
  const char* file;  // file name on the disc; empty for the main module
  uint32_t base;
  uint32_t size;
  uint32_t start;  // module_start, 0 if the module has none
  uint32_t stop;   // module_stop, 0 if the module has none
  uint32_t gp;
  const ModuleExport* exports;
  size_t exportCount;
  uint64_t hash;  // content hash of the module file, 0 if it is only matched by name
  // Overlay modules only: functions by address and the import stubs inside the image.
  const FuncEntry* funcs;
  size_t funcCount;
  const StubEntry* stubs;
  size_t stubCount;
};

}  // namespace psp
