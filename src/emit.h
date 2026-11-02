#pragma once
#include <cstddef>
#include <string>

#include "functions.h"

namespace mgspo {

struct EmitOptions {
  std::string outDir;
  size_t funcsPerFile = 150;
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

std::string funcName(uint32_t addr);

}  // namespace mgspo
