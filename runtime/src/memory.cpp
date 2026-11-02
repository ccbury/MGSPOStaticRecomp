#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <psp/memory.h>

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
  std::free(g_ram);
  std::free(g_vram);
  std::free(g_scratch);
  g_ram = g_vram = g_scratch = nullptr;
}

uint8_t* ptr_slow(uint32_t addr) {
  uint32_t p = addr & 0x3FFFFFFF;
  if (p - kVramBase < kVramSize) return g_vram + (p - kVramBase);
  if (p - kScratchBase < kScratchSize) return g_scratch + (p - kScratchBase);
  char msg[64];
  std::snprintf(msg, sizeof msg, "bad guest address 0x%08X", addr);
  throw std::runtime_error(msg);
}

void mem_write(uint32_t addr, const void* data, size_t size) {
  const uint8_t* src = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) *ptr(addr + static_cast<uint32_t>(i)) = src[i];
}

}  // namespace psp
