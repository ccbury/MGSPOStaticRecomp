#include <atomic>
#include <chrono>

#include "kernel_internal.h"
#include "platform.h"

namespace psp {
namespace k {

namespace {
KernelConfig g_cfg;
std::chrono::steady_clock::time_point g_start;
std::atomic<int32_t> g_nextUid{0x1000};
}  // namespace

const KernelConfig& config() { return g_cfg; }

uint64_t now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count());
}

int32_t alloc_uid() { return g_nextUid.fetch_add(0x11); }

std::string cstr(uint32_t addr, size_t max) {
  std::string s;
  if (!addr) return s;
  for (size_t i = 0; i < max; ++i) {
    char ch = static_cast<char>(lbu(addr + static_cast<uint32_t>(i)));
    if (!ch) break;
    s.push_back(ch);
  }
  return s;
}

void write_cstr(uint32_t addr, const std::string& s, size_t max) {
  if (!addr || !max) return;
  size_t n = std::min(s.size(), max - 1);
  for (size_t i = 0; i < n; ++i) sb(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
  sb(addr + static_cast<uint32_t>(n), 0);
}

}  // namespace k

void kernel_init(const KernelConfig& cfg, uint32_t userHeapStart, uint32_t gp) {
  k::g_cfg = cfg;
  k::g_start = std::chrono::steady_clock::now();
  plat::init(cfg.headless);
  k::sysmem_init(userHeapStart, cfg.userMemEnd);
  k::sched_init(gp);
  k::io_init();
  k::display_init();
  k::ge_init();
  k::audio_init();
  set_hle_trace(cfg.trace);
}

void kernel_bind_modules(const ModuleDesc* mods, size_t count) { k::modules_init(mods, count); }

void register_all_hle() {
  k::register_sysmem();
  k::register_threadman();
  k::register_modulemgr();
  k::register_io();
  k::register_display();
  k::register_ctrl();
  k::register_ge();
  k::register_audio();
  k::register_misc();
  k::register_font();
}

}  // namespace psp
