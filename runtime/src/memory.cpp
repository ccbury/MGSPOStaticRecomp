#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>

#include <psp/cpu.h>
#include <psp/memory.h>
#include "render_surface.h"

namespace psp {

uint8_t* g_ram = nullptr;
static uint8_t* g_vram = nullptr;
static uint8_t* g_scratch = nullptr;

void init_memory() {
  free_memory();
  g_ram = static_cast<uint8_t*>(std::calloc(kRamSize, 1));
  g_vram = static_cast<uint8_t*>(std::calloc(kVramSize, 1));
  g_scratch = static_cast<uint8_t*>(std::calloc(kScratchSize, 1));
  if (!g_ram || !g_vram || !g_scratch) throw std::runtime_error("out of memory for guest RAM");
}

void free_memory() {
  ge::surface::reset();
  std::free(g_ram);
  std::free(g_vram);
  std::free(g_scratch);
  g_ram = g_vram = g_scratch = nullptr;
}

namespace {
thread_local Cpu* t_cpu = nullptr;
uint32_t g_dumpAddr = 0, g_dumpLen = 0;
std::mutex g_faultMutex;
std::string g_faultReport;

std::string hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%08X", v);
  return b;
}
}  // namespace

void set_fault_dump(uint32_t addr, uint32_t len) {
  g_dumpAddr = addr;
  g_dumpLen = len;
}

#ifdef MGSPO_WATCH
uint32_t g_watchLo = 1, g_watchHi = 0;
void watch_hit(uint32_t addr, uint32_t value, int size) {
  std::string s = "[watch] store" + std::to_string(size * 8) + " [" + hex8(addr) + "] = " + hex8(value);
  if (Cpu* c = t_cpu) {
    uint32_t n = c->depth < 6 ? c->depth : 6;
    s += " in";
    for (uint32_t i = 0; i < n; ++i) s += " " + hex8(c->calls[(c->depth - 1 - i) % kCallRing]);
  }
  std::fprintf(stderr, "%s\n", s.c_str());
}
void set_watch(uint32_t addr, uint32_t len) {
  g_watchLo = addr;
  g_watchHi = addr + len;
}
#else
void set_watch(uint32_t, uint32_t) {
  std::fprintf(stderr, "--watch needs a build with -DMGSPO_WATCH=ON\n");
}
#endif

void set_fault_cpu(Cpu* c) { t_cpu = c; }

std::string last_fault_report() {
  std::lock_guard<std::mutex> lk(g_faultMutex);
  return g_faultReport;
}

void guest_fault(const std::string& what) {
  std::string rep;
  char b[96];
  if (Cpu* c = t_cpu) {
    rep += "guest call stack (innermost first, " + std::to_string(c->depth) + " deep):\n";
    uint32_t n = c->depth < kCallRing ? c->depth : kCallRing;
    for (uint32_t i = 0; i < n; ++i) {
      std::snprintf(b, sizeof b, "  #%u  f_%08X\n", i, c->calls[(c->depth - 1 - i) % kCallRing]);
      rep += b;
    }
    static const char* names[32] = {"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                    "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                    "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    rep += "registers:";
    for (int i = 0; i < 32; ++i) {
      std::snprintf(b, sizeof b, "%s%s=%08X", i % 8 == 0 ? "\n  " : " ", names[i], c->r[i]);
      rep += b;
    }
    rep += "\n";
  }
  if (g_dumpLen) {
    rep += "memory:";
    for (uint32_t i = 0; i < g_dumpLen; ++i) {
      uint32_t a = g_dumpAddr + i;
      uint32_t p = a & 0x3FFFFFFF;
      bool ok = p - kRamBase < kRamSize || p - kVramBase < kVramSize;
      if (i % 16 == 0) {
        std::snprintf(b, sizeof b, "\n  %08X:", a);
        rep += b;
      }
      std::snprintf(b, sizeof b, " %02X", ok ? *ptr(a) : 0);
      rep += b;
    }
    rep += "\n";
  }
  {
    std::lock_guard<std::mutex> lk(g_faultMutex);
    g_faultReport = rep;
  }
  throw std::runtime_error(what);
}

uint8_t* ptr_slow(uint32_t addr) {
  uint32_t p = addr & 0x3FFFFFFF;
  if (p - kVramBase < kVramSize) {
    ge::surface::native_access();
    return g_vram + (p - kVramBase);
  }
  if (p - kScratchBase < kScratchSize) return g_scratch + (p - kScratchBase);
  char msg[64];
  std::snprintf(msg, sizeof msg, "bad guest address 0x%08X", addr);
  guest_fault(msg);
}

void mem_write(uint32_t addr, const void* data, size_t size) {
  const uint8_t* src = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) *ptr(addr + static_cast<uint32_t>(i)) = src[i];
}

}  // namespace psp
