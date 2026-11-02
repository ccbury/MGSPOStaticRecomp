# Lifter and runtime

## Pipeline

1. `loadPrx` (src/prx.cpp) maps the PRX's load segments at a fixed base and applies the PSP
   relocation sections (types 0x700000A0): `R_32`, `R_26`, and `HI16`/`LO16` pairs. After this step
   every `jal` target and every `lui`/`addiu` constant is a real guest address. Code pointers
   created by relocations are recorded; runs of adjacent pointers are treated as switch tables.
2. `discover` (src/functions.cpp) finds function starts: the entry point, exports, every `jal`
   target in code, plus `j` targets and address-taken pointers that follow a `jr $ra` (so tail
   jumps and switch labels are not mistaken for functions). Import stubs are 8 bytes each and are
   never lifted; three of the game's helper libraries (`kjfs`, `sound2`, `zlibdec`) have their
   stubs inside `.text`, so stub addresses also act as function boundaries.
3. `emitFunction` (src/emit.cpp) writes one C++ function per guest function:
   - registers live in `psp::Cpu` (`c.r[n]`, `c.f[n]`, `c.hi`, `c.lo`, `c.fcc`)
   - branches become `goto`s, with the delay slot placed inline; branch-likely only runs the slot
     when taken
   - `jal` calls the target directly, import stubs call `psp::hle(c, index)`
   - `jr $ra` is `return`; other `jr` becomes a `switch` over the function's known labels with a
     fall back to `psp::call` for unknown targets
   - anything that cannot be translated becomes `psp::unimpl(...)`, which throws at run time
4. Output is split into `funcs_NNN.cpp`, plus `funcs.h`, `tables.h` and `tables.cpp` (function
   table sorted by address, import table with library name and NID).

## Runtime (runtime/)

- `psp/memory.h`: guest RAM at `0x08000000` (64 MB), VRAM at `0x04000000`, scratchpad. Addresses
  are masked with `0x3FFFFFFF`, so the uncached and kernel mirrors work. Unaligned `lwl`/`lwr`
  and friends are implemented. Bad addresses throw.
- `psp/ops.h`: helpers with the semantics the compiler cannot express directly (mult/div/madd,
  bit field ops, saturating float to int, FPU compares).
- `psp/runtime.h`: `register_hle(module, nid, fn)` to implement an import, `bind_tables`, and
  `call` for indirect calls. Imports without a handler log once and return 0.

## Numbers for the US release (ULUS10202 main module)

| | |
|---|---|
| Code words | 481,818 |
| Functions found | 5,297 (4,892 call targets, 37 jump roots, 363 pointer roots) |
| Switch table targets | 984 |
| VFPU instructions | 27,657 |
| COP0 / unknown | 0 / 0 |
| `jal` to non-code, non-import | 4 |
| Relocations not handled | 0 |

Lifting takes a few seconds and produces roughly 40 files that compile with MSVC. Running the
result with every import unimplemented gets through the module start stub and returns.

## Not done yet

- VFPU translation. The register file exists in `Cpu` but nothing uses it.
- Real implementations of the imported libraries (threads, I/O, GE, display, audio, controls).
- The game's other modules (`kjfs`, `sound2`, `zlibdec` and the per-stage PRXs) are separate
  binaries and have not been analysed.
- Naming NIDs (output uses `module:NID`).
- Function boundary detection is heuristic; 142 code words are not covered by any function
  (the gaps between import stubs and the code around them have not been inspected yet).
