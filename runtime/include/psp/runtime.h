#pragma once
#include <cstddef>
#include <cstdint>

#include "cpu.h"
#include "memory.h"
#include "ops.h"

namespace psp {

// Registers a handler for an imported library function. Must be called before bind_tables().
void register_hle(const char* module, uint32_t nid, HleFn fn);

// Hands the generated function and import tables to the runtime and resolves imports.
void bind_tables(const FuncEntry* funcs, size_t funcCount, const ImportEntry* imports, size_t importCount);

// Calls an import by its index in the generated table. Unregistered imports log once and return 0.
void hle(Cpu& c, uint32_t importIndex);

// Indirect call/tail-call by guest address: recompiled function, import stub, or an error.
void call(Cpu& c, uint32_t addr);

// Instructions the recompiler could not translate (VFPU, COP0, ...).
[[noreturn]] void unimpl(Cpu& c, uint32_t addr, uint32_t raw);
[[noreturn]] void syscall_insn(Cpu& c, uint32_t code);
[[noreturn]] void break_insn(Cpu& c, uint32_t code);

// Number of imports that were called without a registered handler.
size_t unresolved_calls();

}  // namespace psp
