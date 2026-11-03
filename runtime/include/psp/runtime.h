#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "cpu.h"
#include "memory.h"
#include "ops.h"
#include "game_controls.h"
#include "game_prompts.h"

namespace psp {

// Registers a handler for an imported library function. Must be called before bind_tables().
void register_hle(const char* module, uint32_t nid, HleFn fn);

// Same, with a function name that shows up in logs and the import report.
void register_hle(const char* module, uint32_t nid, const char* name, HleFn fn);

// Like register_hle, but if a lifted module that was loaded exports the same library/NID, the
// module's code is called instead. Used for replacements of firmware modules that a user may supply.
void register_hle_soft(const char* module, uint32_t nid, const char* name, HleFn fn);

// Handler for every import of a library that has no handler of its own.
void register_hle_fallback(const char* module, HleFn fn);

// Name given at registration time, or nullptr.
const char* hle_name(const char* module, uint32_t nid);

// Called before every import is dispatched (scheduler hook).
void set_hle_poll(void (*fn)(Cpu&));

// For imports without a handler: returns the guest address of a matching export in an already loaded
// module, or 0. The import is then dispatched as a normal call to that address.
void set_import_linker(uint32_t (*fn)(const char* module, uint32_t nid));

// Log every import call (name, arguments, result) to stderr.
void set_hle_trace(bool on);

// Writes imports in order of first call: module, nid, name, call count, whether it has a handler.
void write_import_report(std::FILE* out);

// Hands the generated function and import tables to the runtime and resolves imports.
void bind_tables(const FuncEntry* funcs, size_t funcCount, const ImportEntry* imports, size_t importCount);

// Calls an import by its index in the generated table. Unregistered imports log once and return 0.
void hle(Cpu& c, uint32_t importIndex);

// Indirect call/tail-call by guest address: recompiled function, import stub, or an error.
void call(Cpu& c, uint32_t addr);

// Overlay modules (stages) share an address range; their functions are found through a table of
// function pointers indexed by (address - base) / 4 that is filled when a module is loaded.
extern void (**g_ovTable)(Cpu&);
extern uint32_t g_ovBase;
[[noreturn]] void ov_missing(Cpu& c, uint32_t addr);
inline void ovcall(Cpu& c, uint32_t addr) {
  void (*fn)(Cpu&) = g_ovTable[(addr - g_ovBase) >> 2];
  if (!fn) ov_missing(c, addr);
  fn(c);
}
void ov_init(uint32_t base, uint32_t end);
void ov_map(const FuncEntry* funcs, size_t count, const StubEntry* stubs, size_t stubCount);
void ov_unmap(uint32_t base, uint32_t size);

// Instructions the recompiler could not translate (VFPU, COP0, ...).
[[noreturn]] void unimpl(Cpu& c, uint32_t addr, uint32_t raw);
[[noreturn]] void syscall_insn(Cpu& c, uint32_t code);
[[noreturn]] void break_insn(Cpu& c, uint32_t code);

// Number of imports that were called without a registered handler.
size_t unresolved_calls();

}  // namespace psp
