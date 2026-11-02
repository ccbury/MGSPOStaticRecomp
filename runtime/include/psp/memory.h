#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace psp {

constexpr uint32_t kRamBase = 0x08000000;
constexpr uint32_t kRamSize = 0x04000000;  // 64 MB; the 32 MB models just use less of it
constexpr uint32_t kVramBase = 0x04000000;
constexpr uint32_t kVramSize = 0x00200000;
constexpr uint32_t kScratchBase = 0x00010000;
constexpr uint32_t kScratchSize = 0x00004000;

extern uint8_t* g_ram;

void init_memory();
void free_memory();
void mem_write(uint32_t addr, const void* data, size_t size);

// Handles VRAM, scratchpad and error reporting for addresses outside main RAM.
uint8_t* ptr_slow(uint32_t addr);

struct Cpu;
// Registers the Cpu of the calling host thread so faults can report its call stack and registers.
void set_fault_cpu(Cpu* c);
// Guest memory range printed as part of every fault report.
void set_fault_dump(uint32_t addr, uint32_t len);
// Records the call stack and registers of the calling thread, then throws std::runtime_error(what).
[[noreturn]] void guest_fault(const std::string& what);
// Report captured by the most recent guest_fault (empty if none).
std::string last_fault_report();

// Addresses are masked the way the hardware does it, so the cached (0x0...),
// uncached (0x4...) and kernel mirrors (0x8.../0xA...) of the same memory all resolve.
inline uint8_t* ptr(uint32_t addr) {
  uint32_t p = addr & 0x3FFFFFFF;
  if (p - kRamBase < kRamSize) return g_ram + (p - kRamBase);
  return ptr_slow(addr);
}

#ifdef MGSPO_WATCH
// Debug builds only: report guest stores that land in the watched range (see --watch).
extern uint32_t g_watchLo, g_watchHi;
void watch_hit(uint32_t addr, uint32_t value, int size);
inline void watch(uint32_t a, uint32_t v, int size) {
  if (a - g_watchLo < g_watchHi - g_watchLo) watch_hit(a, v, size);
}
#else
inline void watch(uint32_t, uint32_t, int) {}
#endif
void set_watch(uint32_t addr, uint32_t len);

inline uint8_t lbu(uint32_t a) { return *ptr(a); }
inline int8_t lb(uint32_t a) { return static_cast<int8_t>(*ptr(a)); }
inline uint16_t lhu(uint32_t a) { uint16_t v; std::memcpy(&v, ptr(a), 2); return v; }
inline int16_t lh(uint32_t a) { return static_cast<int16_t>(lhu(a)); }
inline uint32_t lw(uint32_t a) { uint32_t v; std::memcpy(&v, ptr(a), 4); return v; }
inline void sb(uint32_t a, uint32_t v) { watch(a, v, 1); *ptr(a) = static_cast<uint8_t>(v); }
inline void sh(uint32_t a, uint32_t v) { watch(a, v, 2); uint16_t x = static_cast<uint16_t>(v); std::memcpy(ptr(a), &x, 2); }
inline void sw(uint32_t a, uint32_t v) { watch(a, v, 4); std::memcpy(ptr(a), &v, 4); }

// Unaligned load/store halves (little endian).
inline uint32_t lwl(uint32_t a, uint32_t reg) {
  uint32_t s = (a & 3) * 8, w = lw(a & ~3u);
  return (reg & (0x00FFFFFFu >> s)) | (w << (24 - s));
}
inline uint32_t lwr(uint32_t a, uint32_t reg) {
  uint32_t s = (a & 3) * 8, w = lw(a & ~3u);
  return (reg & (0xFFFFFF00u << (24 - s))) | (w >> s);
}
inline void swl(uint32_t a, uint32_t reg) {
  uint32_t s = (a & 3) * 8, m = lw(a & ~3u);
  sw(a & ~3u, (m & (0xFFFFFF00u << s)) | (reg >> (24 - s)));
}
inline void swr(uint32_t a, uint32_t reg) {
  uint32_t s = (a & 3) * 8, m = lw(a & ~3u);
  sw(a & ~3u, (m & (0x00FFFFFFu >> (24 - s))) | (reg << s));
}

}  // namespace psp
