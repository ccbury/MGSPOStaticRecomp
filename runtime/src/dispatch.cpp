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

struct Registered {
  HleFn fn = nullptr;
  std::string name;
  bool soft = false;
};

std::map<std::pair<std::string, uint32_t>, Registered> g_registered;
std::map<std::string, HleFn> g_fallbacks;
const FuncEntry* g_funcs = nullptr;
size_t g_funcCount = 0;
const ImportEntry* g_imports = nullptr;
size_t g_importCount = 0;
std::vector<HleFn> g_handlers;
std::vector<uint64_t> g_calls;
std::vector<uint32_t> g_order;  // import indices in order of first call
size_t g_unresolved = 0;
void (*g_poll)(Cpu&) = nullptr;
bool g_trace = false;
uint32_t (*g_linker)(const char*, uint32_t) = nullptr;
std::vector<uint8_t> g_soft;    // handlers that a loaded module's export overrides
std::vector<uint8_t> g_linked;  // imports that were forwarded to a loaded module

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

}  // namespace

void register_hle(const char* module, uint32_t nid, HleFn fn) { g_registered[{module, nid}].fn = fn; }

void register_hle(const char* module, uint32_t nid, const char* name, HleFn fn) {
  Registered& r = g_registered[{module, nid}];
  r.fn = fn;
  r.name = name;
}

void register_hle_soft(const char* module, uint32_t nid, const char* name, HleFn fn) {
  register_hle(module, nid, name, fn);
  g_registered[{module, nid}].soft = true;
}

void register_hle_fallback(const char* module, HleFn fn) { g_fallbacks[module] = fn; }

const char* hle_name(const char* module, uint32_t nid) {
  auto it = g_registered.find({module, nid});
  return it != g_registered.end() && !it->second.name.empty() ? it->second.name.c_str() : nullptr;
}

void set_hle_poll(void (*fn)(Cpu&)) { g_poll = fn; }
void set_hle_trace(bool on) { g_trace = on; }
void set_import_linker(uint32_t (*fn)(const char* module, uint32_t nid)) { g_linker = fn; }

void bind_tables(const FuncEntry* funcs, size_t funcCount, const ImportEntry* imports, size_t importCount) {
  g_funcs = funcs;
  g_funcCount = funcCount;
  g_imports = imports;
  g_importCount = importCount;
  g_handlers.assign(importCount, nullptr);
  g_calls.assign(importCount, 0);
  g_linked.assign(importCount, 0);
  g_soft.assign(importCount, 0);
  g_order.clear();
  g_unresolved = 0;
  for (size_t i = 0; i < importCount; ++i) {
    auto it = g_registered.find({imports[i].module, imports[i].nid});
    if (it != g_registered.end()) {
      g_handlers[i] = it->second.fn;
      g_soft[i] = it->second.soft;
    } else {
      auto fb = g_fallbacks.find(imports[i].module);
      if (fb != g_fallbacks.end()) g_handlers[i] = fb->second;
    }
  }
}

void hle(Cpu& c, uint32_t idx) {
  if (idx >= g_importCount) throw std::runtime_error("import index out of range");
  const ImportEntry& imp = g_imports[idx];
  if (g_calls[idx]++ == 0) g_order.push_back(idx);
  if (g_poll) g_poll(c);
  HleFn fn = g_handlers[idx];
  if (g_trace) {
    const char* n = hle_name(imp.module, imp.nid);
    std::fprintf(stderr, "[hle] %s:%08X %s(%X, %X, %X, %X)\n", imp.module, imp.nid, n ? n : "?", c.r[4], c.r[5],
                 c.r[6], c.r[7]);
  }
  if (fn && g_soft[idx] && g_linker) {
    if (uint32_t target = g_linker(imp.module, imp.nid)) {
      g_linked[idx] = 1;
      call(c, target);
      return;
    }
  }
  if (fn) {
    fn(c);
    if (g_trace) std::fprintf(stderr, "[hle]   -> %08X\n", c.r[2]);
    return;
  }
  if (g_linker) {
    // Imports of another lifted module resolve to its export once that module has been loaded.
    if (uint32_t target = g_linker(imp.module, imp.nid)) {
      g_linked[idx] = 1;
      if (g_trace) std::fprintf(stderr, "[hle]   -> module export at %08X\n", target);
      call(c, target);
      return;
    }
  }
  ++g_unresolved;
  if (g_calls[idx] == 1) {
    std::fprintf(stderr, "unimplemented import %s:%08X (a0=%X a1=%X a2=%X a3=%X)\n", imp.module, imp.nid, c.r[4],
                 c.r[5], c.r[6], c.r[7]);
  }
  c.r[2] = 0;
}

void write_import_report(std::FILE* out) {
  std::fprintf(out, "%-18s %-10s %-34s %8s  %s\n", "module", "nid", "name", "calls", "state");
  for (uint32_t idx : g_order) {
    const ImportEntry& imp = g_imports[idx];
    const char* n = hle_name(imp.module, imp.nid);
    std::fprintf(out, "%-18s 0x%08X %-34s %8llu  %s\n", imp.module, imp.nid, n ? n : "-",
                 static_cast<unsigned long long>(g_calls[idx]),
                 g_handlers[idx] ? "implemented" : g_linked[idx] ? "linked" : "UNIMPLEMENTED");
  }
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
