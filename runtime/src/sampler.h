// Poor man's sampling profiler for local performance work (--sample). Not built into normal runs.
#pragma once
#include <cstdio>

namespace psp {
namespace prof {

// Starts a thread that samples the instruction pointer of every other thread in the process,
// beginning after delaySec seconds.
void sampler_start(int intervalUs, int delaySec);
// Prints where the time went: guest code, GE, waits and the biggest individual symbols.
void sampler_report(std::FILE* f);

}  // namespace prof
}  // namespace psp
