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
  `register_hle` takes an optional name that shows up in logs and the import report.
- `psp/kernel.h`: `kernel_init`, `register_all_hle` and `kernel_run_main` set up the firmware
  layer and run the module entry.

### Firmware layer

| File | Covers |
|---|---|
| `sysmem.cpp` | partitions, memory blocks, heap (`SysMemUserForUser`) |
| `threadman.cpp` | threads, semaphores, event flags, mutexes, message boxes, fixed/variable pools, callbacks, timers, interrupts |
| `io.cpp` | `sceIo*`; `disc0:/` maps to the extracted disc (read only), `ms0:/` to the save folder |
| `display.cpp` | framebuffer, vblank, `sceCtrl` |
| `ge.cpp` | `sceGe_user`: list queue, sync and callbacks (see `docs/ge.md`) |
| `gpu.cpp`, `gpu_draw.cpp` | GE command interpreter and software rasteriser that draws into emulated VRAM |
| `audio.cpp` | `sceAudio` channels mixed into a FIFO |
| `sas.cpp`, `sas_core.cpp` | `sceSasCore` software synth |
| `atrac.cpp`, `atrac3.cpp` | `sceAtrac3plus` API and the ATRAC3 decoder |
| `misc.cpp` | module manager, loadexec, rtc, utility, umd, power and the rest |
| `platform_win32.cpp` | window, keyboard/XInput, waveOut. `platform_null.cpp` is used for headless builds |

Guest threads are host threads, but only one runs at a time (a CPU token is handed over at
blocking calls and when a thread's time slice runs out), so recompiled code needs no locking.
The game keeps a per-thread table in `$k0`, so every thread gets a zeroed block for it.

### Startup

With the US game the first things called are the SDK version setters, `sceKernelCreateThread`/
`StartThread` for the real main thread, a few semaphores, the system parameter and rtc calls,
`sceUmdGetDriveStat` polling, and then `sceKernelLoadModuleByID` for `zlibdec.prx`, `sound2.prx`
and `kjfs.prx`. Run `mgspo-game ... --report imports.txt --timeout 15` to get the list of
unimplemented imports for your own build.

### Helper modules

`zlibdec`, `kjfs` and `sound2` are plain (unencrypted, uncompressed) PRX files, so they go through
the same lifter as the main module. Pass each one with `--helper`:

    mgspo-lift main.BIN gen/ --helper zlibdec.prx --helper kjfs.prx --helper sound2.prx

Lifted code uses absolute addresses, so every helper gets a fixed base placed after the main image
(page aligned, with a gap). Import indices are numbered across all modules, and `tables.cpp` gets a
`kModules` table with each module's file name, base, size, start/stop and export list.

At run time `sceKernelLoadModule`/`LoadModuleByID` match the file name against that table, read the
PRX from the disc folder, apply its relocations at the fixed base and copy it into guest memory.
`StartModule`/`StopModule` call the module's start/stop entry, and `UnloadModule` just marks it
unloaded. An import with no HLE handler is resolved through the export table of a loaded module
(that is how `kjfs` reaches `zlibdec` and the main module reaches `kjfs`/`sound2`). Modules that
were not lifted still load with a fake id and a log line.

Current blockers after module loading: `libfont.prx` (and the other `~SCE` encrypted modules) are
not lifted and `sceUmdActivate` is stubbed. `sceSasCore` and `sceAtrac3plus` are implemented
(see [audio.md](audio.md)).

## Numbers for the US release (ULUS10202 main module)

| | |
|---|---|
| Code words | 481,818 |
| Functions found | 6,398 |
| Switch table targets | 984 |
| VFPU instructions | 27,606 (+51 `bvf`/`bvt` branches, 27,657 total) |
| VFPU instructions left as `psp::unimpl` | 0 |
| COP0 / unknown | 0 / 0 |
| `jal` to non-code, non-import | 4 |
| Relocations not handled | 0 |

Lifting takes a few seconds and produces roughly 40 files that compile with MSVC. Running the
result with every import unimplemented gets through the module start stub and returns.

## VFPU

VFPU instructions are decoded in `src/vfpu.cpp` (`decodeVfpu` maps a raw word to a
`psp::vfpu::Op`, the list of which lives in `runtime/include/psp/vfpu_ops.h`) and emitted as
`psp::vfpu::exec(c, psp::vfpu::Op::Vadd, 0x...)`. The runtime (`runtime/src/vfpu.cpp`) pulls the
register fields back out of the raw word, so the generated code stays small and compiles quickly.
The `bvf`/`bvt`/`bvfl`/`bvtl` branches are ordinary `Op`s in the MIPS decoder and test
`psp::vfpu::cc_bit`.

State: `Cpu::v[128]` is eight 4x4 matrices, indexed `mtx * 16 + col * 4 + row`, and
`Cpu::vctrl[16]` holds the control registers (SPFX, TPFX, DPFX, CC, REV, RCX0-7). Prefixes are
applied and cleared the same way the hardware does it: every instruction except the prefix
setters, `lv`/`sv`, `mfv`/`mtv` and the nop/sync family consumes them.

Covered: arithmetic, dot products, cross products, compares and `vcmov`, the unary and
transcendental ops, constants, matrix multiply/scale/move/fill, `vtfm`/`vhtfm`, `vrot`,
float/int/half/byte/colour conversions, the sort and butterfly ops, `vrnd*`, loads and stores
(including `lvl.q`/`lvr.q`/`svl.q`/`svr.q`), and `mfv`/`mtv`/`vmfvc`/`vmtvc`.

Known simplifications:

- `sin`, `cos`, `asin`, `exp2`, `log2`, `rcp`, `rsq` use libm, not the hardware's lookup tables, so
  results can differ in the last bits (quarter-turn inputs are exact).
- On the ops that force their own swizzle (`vcrs`, `vdet`, `vhdp`, ...) a user swizzle in the S/T
  prefix is applied first and the fixed pattern is then computed from the result.
- Matrix ops ignore S/T/D prefixes, and the transcendental ops apply the D prefix to all lanes
  rather than only the last one.
- `vsbn`, `vsbz`, `vlgb` and `vwbn` decode but throw at run time; none of them occur in the
  main module (`mgspo-lift <module> --vfpu` prints a per-op histogram).
- NaN/denormal corner cases of `vmin`/`vmax`/`vsgn` are not modelled.

## Not done yet

- GE rendering is implemented against synthetic lists only (`docs/ge.md`); it has not been checked
  against the real game yet.
- The per-stage PRXs and encrypted helper modules (`libfont` etc.) are not lifted. Net libraries
  return an error. ATRAC3+ is decoded natively (see [audio.md](audio.md)).
- Naming NIDs (output uses `module:NID`).
- Function boundary detection is heuristic; 142 code words are not covered by any function
  (the gaps between import stubs and the code around them have not been inspected yet).
