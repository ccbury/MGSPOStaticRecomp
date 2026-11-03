// Samples thread instruction pointers with SuspendThread/GetThreadContext and resolves them with
// dbghelp, which is enough to tell lifted guest code, the GE and kernel waits apart. Build with
// debug info (/Zi, linker /DEBUG) to get function names for the project's own code.
#include "sampler.h"

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace psp {
namespace prof {

namespace {

struct Sample {
  DWORD tid;
  uint64_t rip;
};

std::mutex g_mutex;
std::vector<Sample> g_samples;
std::atomic<bool> g_run{false};
std::thread g_thread;
int g_interval = 1000;
uint64_t g_rounds = 0;
std::chrono::steady_clock::time_point g_start;

void sample_loop() {
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  while (g_run) {
    if (std::chrono::steady_clock::now() < g_start) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
      THREADENTRY32 te;
      te.dwSize = sizeof te;
      for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
      CloseHandle(snap);
    }
    // no allocations while a thread is suspended, it might be holding the heap lock
    std::vector<Sample> got;
    got.reserve(tids.size());
    for (DWORD tid : tids) {
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
      if (!h) continue;
      if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(h, &ctx)) got.push_back({tid, ctx.Rip});
        ResumeThread(h);
      }
      CloseHandle(h);
    }
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      g_samples.insert(g_samples.end(), got.begin(), got.end());
      ++g_rounds;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(g_interval));
  }
}

enum Kind { kGuest, kGe, kWait, kOther };
const char* kKindName[] = {"lifted guest code", "GE (list execution, raster)", "waiting / idle", "other runtime"};

Kind classify(const std::string& s) {
  if (s.rfind("f_", 0) == 0 || s.rfind("ovl_", 0) == 0) return kGuest;
  if (s.find("psp::ge::") != std::string::npos || s.find("Raster") != std::string::npos) return kGe;
  if (is_wait_symbol(s)) return kWait;
  return kOther;
}

}  // namespace

void sampler_start(int intervalUs, int delaySec) {
  if (g_run.exchange(true)) return;
  g_start = std::chrono::steady_clock::now() + std::chrono::seconds(delaySec);
  g_interval = intervalUs > 0 ? intervalUs : 1000;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  g_thread = std::thread(sample_loop);
}

void sampler_report(std::FILE* f) {
  if (!g_run.exchange(false)) return;
  g_thread.join();
  std::vector<Sample> samples;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    samples.swap(g_samples);
  }
  HANDLE proc = GetCurrentProcess();
  std::unordered_map<uint64_t, std::string> names;
  auto resolve = [&](uint64_t rip) -> const std::string& {
    auto it = names.find(rip);
    if (it != names.end()) return it->second;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    std::string n;
    if (SymFromAddr(proc, rip, &disp, si)) n = si->Name;
    else n = "?";
    return names.emplace(rip, std::move(n)).first->second;
  };

  std::map<std::string, uint64_t> bySym;
  uint64_t byKind[4] = {};
  // per thread: the thread that spent the most non-wait time is the interesting one
  std::map<DWORD, std::map<std::string, uint64_t>> perThread;
  std::map<DWORD, uint64_t> threadBusy, threadAll;
  for (const Sample& s : samples) {
    const std::string& n = resolve(s.rip);
    Kind k = classify(n);
    byKind[k]++;
    threadAll[s.tid]++;
    if (k != kWait) {
      bySym[n]++;
      perThread[s.tid][n]++;
      threadBusy[s.tid]++;
    }
  }
  const uint64_t total = samples.size();
  std::fprintf(f, "\nSampling profile: %llu samples over %llu rounds (%d us apart)\n", static_cast<unsigned long long>(total),
               static_cast<unsigned long long>(g_rounds), g_interval);
  for (int k = 0; k < 4; ++k)
    std::fprintf(f, "  %-30s %6.1f%%\n", kKindName[k], total ? 100.0 * byKind[k] / total : 0.0);
  const uint64_t busy = byKind[kGuest] + byKind[kGe] + byKind[kOther];
  std::fprintf(f, "  of non-waiting samples: guest %.1f%%, GE %.1f%%, other %.1f%%\n", busy ? 100.0 * byKind[kGuest] / busy : 0.0,
               busy ? 100.0 * byKind[kGe] / busy : 0.0, busy ? 100.0 * byKind[kOther] / busy : 0.0);

  auto top = [&](const std::map<std::string, uint64_t>& m, size_t n, uint64_t denom) {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& kv : m) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < n; ++i)
      std::fprintf(f, "    %5.1f%%  %s\n", denom ? 100.0 * v[i].first / denom : 0.0, v[i].second.c_str());
  };
  std::fprintf(f, "Top symbols (non-waiting):\n");
  top(bySym, 30, busy ? busy : 1);
  for (auto& kv : threadBusy) {
    if (kv.second * 20 < busy) continue;
    std::fprintf(f, "Thread %lu: %.1f%% busy of its samples (%llu of %llu)\n", kv.first,
                 threadAll[kv.first] ? 100.0 * kv.second / threadAll[kv.first] : 0.0, static_cast<unsigned long long>(kv.second),
                 static_cast<unsigned long long>(threadAll[kv.first]));
    top(perThread[kv.first], 8, kv.second);
  }
}

}  // namespace prof
}  // namespace psp
