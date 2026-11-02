// SysMemUserForUser: a first-fit allocator over the user partition plus the block API.
#include <algorithm>
#include <map>
#include <string>

#include "kernel_internal.h"

namespace psp {
namespace k {

namespace {

struct Block {
  uint32_t size;
  std::string name;
};

std::map<uint32_t, Block> g_used;  // keyed by start address
std::map<int32_t, uint32_t> g_uids;  // block uid -> address
uint32_t g_lo = 0, g_hi = 0;

uint32_t align_up(uint32_t v, uint32_t a) { return a > 1 ? (v + a - 1) & ~(a - 1) : v; }

uint32_t largest_gap() {
  uint32_t best = 0, cur = g_lo;
  for (auto& kv : g_used) {
    if (kv.first > cur) best = std::max(best, kv.first - cur);
    cur = kv.first + kv.second.size;
  }
  if (g_hi > cur) best = std::max(best, g_hi - cur);
  return best;
}

uint32_t total_free() {
  uint32_t used = 0;
  for (auto& kv : g_used) used += kv.second.size;
  return (g_hi - g_lo) - used;
}

}  // namespace

void sysmem_init(uint32_t start, uint32_t end) {
  g_used.clear();
  g_uids.clear();
  g_lo = align_up(start, 0x100);
  g_hi = end & ~0xFFu;
}

uint32_t sysmem_alloc(uint32_t size, uint32_t align, bool high, const char* name) {
  size = align_up(std::max<uint32_t>(size, 1), 0x10);
  if (align < 0x10) align = 0x10;
  if (!high) {
    uint32_t cur = g_lo;
    auto it = g_used.begin();
    for (;;) {
      uint32_t gapEnd = it == g_used.end() ? g_hi : it->first;
      uint32_t a = align_up(cur, align);
      if (a >= cur && a + size <= gapEnd && a + size > a) {
        g_used[a] = {size, name ? name : ""};
        return a;
      }
      if (it == g_used.end()) return 0;
      cur = it->first + it->second.size;
      ++it;
    }
  }
  uint32_t end = g_hi;
  auto it = g_used.end();
  for (;;) {
    uint32_t gapStart = g_lo;
    if (it != g_used.begin()) {
      auto p = std::prev(it);
      gapStart = p->first + p->second.size;
    }
    if (end >= gapStart + size) {
      uint32_t a = (end - size) & ~(align - 1);
      if (a >= gapStart) {
        g_used[a] = {size, name ? name : ""};
        return a;
      }
    }
    if (it == g_used.begin()) return 0;
    --it;
    end = it->first;
  }
}

uint32_t sysmem_alloc_at(uint32_t addr, uint32_t size, const char* name) {
  size = align_up(std::max<uint32_t>(size, 1), 0x10);
  if (addr < g_lo || addr + size > g_hi) return 0;
  auto next = g_used.lower_bound(addr);
  if (next != g_used.end() && next->first < addr + size) return 0;
  if (next != g_used.begin()) {
    auto p = std::prev(next);
    if (p->first + p->second.size > addr) return 0;
  }
  g_used[addr] = {size, name ? name : ""};
  return addr;
}

bool sysmem_free(uint32_t addr) { return g_used.erase(addr) != 0; }

namespace {

// partition ids: 1 kernel, 2 user, 3 volatile (only the user partition is backed by the heap)
void h_alloc_partition(Cpu& c) {
  uint32_t part = arg(c, 0), name = arg(c, 1), type = arg(c, 2), size = arg(c, 3), addr = arg(c, 4);
  (void)part;
  uint32_t a = 0;
  std::string n = cstr(name, 32);
  switch (type) {
    case 0: a = sysmem_alloc(size, 0x10, false, n.c_str()); break;
    case 1: a = sysmem_alloc(size, 0x10, true, n.c_str()); break;
    case 2: a = sysmem_alloc_at(addr & 0x1FFFFFFF | 0x08000000, size, n.c_str()); break;
    case 3: a = sysmem_alloc(size, addr ? addr : 0x10, false, n.c_str()); break;
    case 4: a = sysmem_alloc(size, addr ? addr : 0x10, true, n.c_str()); break;
    default: ret(c, E_ILLEGAL_MODE); return;
  }
  if (!a) {
    ret(c, E_NO_MEMORY);
    return;
  }
  int32_t uid = alloc_uid();
  g_uids[uid] = a;
  ret(c, static_cast<uint32_t>(uid));
}

void h_free_partition(Cpu& c) {
  auto it = g_uids.find(static_cast<int32_t>(arg(c, 0)));
  if (it == g_uids.end()) {
    ret(c, E_UNKNOWN_UID);
    return;
  }
  sysmem_free(it->second);
  g_uids.erase(it);
  ret(c, 0);
}

void h_block_head(Cpu& c) {
  auto it = g_uids.find(static_cast<int32_t>(arg(c, 0)));
  ret(c, it == g_uids.end() ? 0 : it->second);
}

void h_max_free(Cpu& c) { ret(c, largest_gap()); }
void h_total_free(Cpu& c) { ret(c, total_free()); }
void h_zero(Cpu& c) { ret(c, 0); }
void h_devkit_version(Cpu& c) { ret(c, 0x02080010); }  // 2.80, the version the disc asks for

}  // namespace

void register_sysmem() {
  const char* m = "SysMemUserForUser";
  register_hle(m, 0x237DBD4F, "sceKernelAllocPartitionMemory", h_alloc_partition);
  register_hle(m, 0xB6D61D02, "sceKernelFreePartitionMemory", h_free_partition);
  register_hle(m, 0x9D9A5BA1, "sceKernelGetBlockHeadAddr", h_block_head);
  register_hle(m, 0xA291F107, "sceKernelMaxFreeMemSize", h_max_free);
  register_hle(m, 0xF919F628, "sceKernelTotalFreeMemSize", h_total_free);
  register_hle(m, 0x3FC9AE6A, "sceKernelDevkitVersion", h_devkit_version);
  register_hle(m, 0x7591C7DB, "sceKernelSetCompiledSdkVersion", h_zero);
  register_hle(m, 0xF77D77CB, "sceKernelSetCompilerVersion", h_zero);
  register_hle(m, 0x13A5ABEF, "sceKernelPrintf", h_zero);
}

}  // namespace k
}  // namespace psp
