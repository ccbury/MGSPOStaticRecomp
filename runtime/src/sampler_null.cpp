#include "sampler.h"

namespace psp {
namespace prof {

void sampler_start(int, int) {}
void sampler_report(std::FILE* f) { std::fprintf(f, "sampling profiler is only available on Windows\n"); }

}  // namespace prof
}  // namespace psp
