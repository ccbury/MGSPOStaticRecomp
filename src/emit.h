#pragma once
#include <cstddef>
#include <string>

#include "functions.h"

namespace mgspo {

struct EmitOptions {
  std::string outDir;
  size_t funcsPerFile = 150;
};

// One lifted module. The first unit passed to emitAll is the main module; its import indices
// (Program::imports[i].index) must already be offset so they are unique across all units.
struct ModuleUnit {
  const Program* prog = nullptr;
  std::string name;  // module name used for logs and kModules
  std::string file;  // disc file name the runtime loads the image from (empty for the main module)
  // Overlay (stage) modules share one address range with other overlays. Their functions are
  // de-duplicated across modules and reached through an address table, and their imports must be
  // numbered with the shared import table (same library and NID gives the same index).
  bool overlay = false;
  uint64_t hash = 0;  // content hash of the file the runtime must see to use this unit
};

// FNV-1a over the raw file bytes; the runtime uses it to recognise a module by content.
uint64_t hashBytes(const uint8_t* data, size_t size);

constexpr const char* kOverlayNameToken = "@FN@";

struct EmitResult {
  size_t files = 0;
  size_t functions = 0;
  size_t bytes = 0;
  size_t unimplemented = 0;  // instructions replaced by psp::unimpl
  size_t overlayFunctions = 0;  // function bodies in all overlay modules before de-duplication
  size_t overlayUnique = 0;     // distinct bodies actually written
};

// C++ source for one guest function (no includes, no namespace wrapper).
std::string emitFunction(const Program& prog, const Function& fn, size_t* unimplemented = nullptr,
                         bool overlay = false);

// Writes funcs_NNN.cpp, funcs.h, tables.h and tables.cpp into opts.outDir.
EmitResult emitAll(const Program& prog, const EmitOptions& opts);
EmitResult emitAll(const std::vector<ModuleUnit>& units, const EmitOptions& opts);

std::string funcName(uint32_t addr);

}  // namespace mgspo
