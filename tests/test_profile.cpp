#include "kernel_internal.h"
#include "sampler.h"

#include <cstdio>

static int failures = 0;
#define CHECK(condition)                                                        \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);           \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

int main() {
  using psp::k::ProfileCounters;
  ProfileCounters pf;
  pf.record_flip(0);
  CHECK(pf.flips == 1 && !pf.hasLastFlip && pf.flipIntervals == 0);
  pf.enabled = true;
  pf.record_flip(0);
  CHECK(pf.flips == 2 && pf.hasLastFlip && pf.flipIntervals == 0);
  pf.record_flip(100);
  pf.record_flip(ProfileCounters::kVblankUs);
  pf.record_flip(ProfileCounters::kVblankUs * 3);
  pf.record_flip(ProfileCounters::kVblankUs * 6);
  pf.record_flip(ProfileCounters::kVblankUs * 10);
  pf.record_flip(ProfileCounters::kVblankUs * 15);
  pf.record_flip(ProfileCounters::kVblankUs * 25);
  CHECK(pf.flipGap[0] == 1);
  for (int i = 1; i < 5; ++i) CHECK(pf.flipGap[i] == 1);
  CHECK(pf.flipGap[5] == 2);
  CHECK(pf.flipIntervals == 7);
  CHECK(pf.flipIntervalUs == ProfileCounters::kVblankUs * 25);
  pf.enabled = false;
  pf.record_flip(ProfileCounters::kVblankUs * 100);
  pf.enabled = true;
  pf.record_flip(ProfileCounters::kVblankUs * 200);
  CHECK(pf.flipIntervals == 7);
  pf.hasLastFlip = false;  // display_init starts a new interval baseline
  pf.record_flip(ProfileCounters::kVblankUs * 300);
  CHECK(pf.flipIntervals == 7);

  for (const char* symbol : {"ZwDelayExecution", "NtDelayExecution", "SleepEx", "NtWaitForSingleObject",
                            "ZwWaitForMultipleObjects", "SwitchToThread", "YieldProcessor",
                            "std::condition_variable::wait", "ZwRemoveIoCompletion"})
    CHECK(psp::prof::is_wait_symbol(symbol));
  for (const char* symbol : {"f_08804128", "psp::ge::fast::sample<2>", "psp::vfpu::exec", "?"})
    CHECK(!psp::prof::is_wait_symbol(symbol));
  if (!failures) std::puts("Profile cadence and wait classification checks passed.");
  return failures ? 1 : 0;
}
