// Poor man's sampling profiler for local performance work (--sample). Not built into normal runs.
#pragma once
#include <cstdio>
#include <string_view>

namespace psp {
namespace prof {

inline bool is_guest_symbol(std::string_view s) {
  return s.rfind("f_", 0) == 0 || s.rfind("o_", 0) == 0 || s.rfind("ovl_", 0) == 0;
}

inline bool is_wait_symbol(std::string_view s) {
  return s.find("Wait") != std::string_view::npos || s.find("Sleep") != std::string_view::npos ||
         s.find("DelayExecution") != std::string_view::npos || s.find("SwitchTo") != std::string_view::npos ||
         s.find("Yield") != std::string_view::npos || s.find("condition_variable") != std::string_view::npos ||
         s.find("ZwRemoveIoCompletion") != std::string_view::npos;
}

// Starts a thread that samples the instruction pointer of every other thread in the process,
// beginning after delaySec seconds.
void sampler_start(int intervalUs, int delaySec);
// Prints where the time went: guest code, GE, waits and the biggest individual symbols.
void sampler_report(std::FILE* f);

}  // namespace prof
}  // namespace psp
