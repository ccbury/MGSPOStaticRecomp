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
};

struct EmitResult {
  size_t files = 0;
  size_t functions = 0;
  size_t bytes = 0;
  size_t unimplemented = 0;  // instructions replaced by psp::unimpl
};

// C++ source for one guest function (no includes, no namespace wrapper).
std::string emitFunction(const Program& prog, const Function& fn, size_t* unimplemented = nullptr);

// Writes funcs_NNN.cpp, funcs.h, tables.h and tables.cpp into opts.outDir.
EmitResult emitAll(const Program& prog, const EmitOptions& opts);
EmitResult emitAll(const std::vector<ModuleUnit>& units, const EmitOptions& opts);

std::string funcName(uint32_t addr);

}  // namespace mgspo
