#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

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

// Addresses are masked the way the hardware does it, so the cached (0x0...),
// uncached (0x4...) and kernel mirrors (0x8.../0xA...) of the same memory all resolve.
inline uint8_t* ptr(uint32_t addr) {
  uint32_t p = addr & 0x3FFFFFFF;
  if (p - kRamBase < kRamSize) return g_ram + (p - kRamBase);
  return ptr_slow(addr);
}

inline uint8_t lbu(uint32_t a) { return *ptr(a); }
inline int8_t lb(uint32_t a) { return static_cast<int8_t>(*ptr(a)); }
inline uint16_t lhu(uint32_t a) { uint16_t v; std::memcpy(&v, ptr(a), 2); return v; }
inline int16_t lh(uint32_t a) { return static_cast<int16_t>(lhu(a)); }
inline uint32_t lw(uint32_t a) { uint32_t v; std::memcpy(&v, ptr(a), 4); return v; }
inline void sb(uint32_t a, uint32_t v) { *ptr(a) = static_cast<uint8_t>(v); }
inline void sh(uint32_t a, uint32_t v) { uint16_t x = static_cast<uint16_t>(v); std::memcpy(ptr(a), &x, 2); }
inline void sw(uint32_t a, uint32_t v) { std::memcpy(ptr(a), &v, 4); }

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
