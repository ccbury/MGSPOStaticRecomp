#pragma once
#include <cstdio>
#include <string>

namespace psp {

struct GeDebugOptions {
  std::string dumpPath;  // write every executed GE command to this file
  bool stats = false;    // count commands, primitives, vertex types, texture formats, ...
};

// Takes effect the next time the kernel is initialised. MGSPO_GE_DUMP=<file> and MGSPO_GE_STATS=1
// in the environment do the same thing when no options were set explicitly.
void ge_set_debug(const GeDebugOptions& opt);

// Prints the collected statistics (nothing if stats are off).
void ge_write_stats(std::FILE* out);

}  // namespace psp
