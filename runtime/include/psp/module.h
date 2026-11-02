#pragma once
#include <cstddef>
#include <cstdint>

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
};

}  // namespace psp
