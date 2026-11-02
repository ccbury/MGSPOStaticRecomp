#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <psp/runtime.h>

namespace psp {

namespace {

std::map<std::pair<std::string, uint32_t>, HleFn> g_registered;
const FuncEntry* g_funcs = nullptr;
size_t g_funcCount = 0;
const ImportEntry* g_imports = nullptr;
size_t g_importCount = 0;
std::vector<HleFn> g_handlers;
std::vector<bool> g_warned;
size_t g_unresolved = 0;

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

}  // namespace

void register_hle(const char* module, uint32_t nid, HleFn fn) { g_registered[{module, nid}] = fn; }

void bind_tables(const FuncEntry* funcs, size_t funcCount, const ImportEntry* imports, size_t importCount) {
  g_funcs = funcs;
  g_funcCount = funcCount;
  g_imports = imports;
  g_importCount = importCount;
  g_handlers.assign(importCount, nullptr);
  g_warned.assign(importCount, false);
  for (size_t i = 0; i < importCount; ++i) {
    auto it = g_registered.find({imports[i].module, imports[i].nid});
    if (it != g_registered.end()) g_handlers[i] = it->second;
  }
}

void hle(Cpu& c, uint32_t idx) {
  if (idx >= g_importCount) throw std::runtime_error("import index out of range");
  if (HleFn fn = g_handlers[idx]) {
    fn(c);
    return;
  }
  ++g_unresolved;
  if (!g_warned[idx]) {
    g_warned[idx] = true;
    std::fprintf(stderr, "unimplemented import %s:%08X\n", g_imports[idx].module, g_imports[idx].nid);
  }
  c.r[2] = 0;
}

void call(Cpu& c, uint32_t addr) {
  const FuncEntry* end = g_funcs + g_funcCount;
  const FuncEntry* it = std::lower_bound(g_funcs, end, addr, [](const FuncEntry& e, uint32_t a) { return e.addr < a; });
  if (it != end && it->addr == addr) {
    it->fn(c);
    return;
  }
  for (size_t i = 0; i < g_importCount; ++i) {
    if (addr >= g_imports[i].stub && addr < g_imports[i].stub + 8) {
      hle(c, static_cast<uint32_t>(i));
      return;
    }
  }
  throw std::runtime_error("call to unrecompiled address " + hex(addr));
}

void unimpl(Cpu&, uint32_t addr, uint32_t raw) {
  throw std::runtime_error("unimplemented instruction " + hex(raw) + " at " + hex(addr));
}

void syscall_insn(Cpu&, uint32_t code) { throw std::runtime_error("raw syscall instruction " + hex(code)); }
void break_insn(Cpu&, uint32_t code) { throw std::runtime_error("break instruction " + hex(code)); }

size_t unresolved_calls() { return g_unresolved; }

}  // namespace psp
