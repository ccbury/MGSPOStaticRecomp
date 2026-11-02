# MGS Portable Ops static recompilation

An experiment in running *Metal Gear Solid: Portable Ops* natively on Windows, without an emulator,
by statically recompiling the game's PSP code.

The plan is the same one that has worked for other console projects:

1. Take the game's main module (an Allegrex/MIPS32 PRX) and lift its machine code to C++.
2. Link that output against a Windows runtime that reimplements the PSP firmware calls the game
   imports (threads, file IO, display, controller, audio, GE command lists, ...).
3. Load all game data from the user's own disc image at run time.

Nothing here ships any game code or data. See [docs/legal.md](docs/legal.md).

## Status

Early groundwork. There is a command line tool that opens a PSP disc image, lists and
extracts its files, reads `PARAM.SFO`, and parses the main module, plus a first-pass static
lifter that turns the module's MIPS code into C++ and a small runtime it links against. The
lifted code compiles and starts running, but no syscalls are implemented yet, and VFPU
instructions are not translated. Findings are in [docs/eboot-notes.md](docs/eboot-notes.md).

## Building

Requires CMake 3.16+ and a C++17 compiler.

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

On Windows with Visual Studio, run the above from a "x64 Native Tools" prompt (or use the
`-G "Visual Studio 17 2022"` generator).

## mgspo-extract

```
mgspo-extract info <iso>                       volume id and PARAM.SFO metadata
mgspo-extract ls <iso>                         list every file in the image
mgspo-extract extract <iso> <outdir> [prefix]  extract files, optionally only below a path prefix
mgspo-extract eboot <iso> <outdir>             pull PARAM.SFO and the boot binary, write boot.elf
mgspo-extract analyze <file>                   dump segments, sections, entry point, imports
```

Only plain, uncompressed `.iso` images are read. CSO/ZSO files need to be decompressed first.

Retail `EBOOT.BIN` files are `~PSP` containers and are encrypted; this tool does not decrypt them
(that needs console key material that does not belong in this repository). For this particular
disc that is not a problem, since `PSP_GAME/SYSDIR/BOOT.BIN` is the same module as a plain ELF/PRX,
and `eboot` falls back to it automatically. Otherwise point `analyze` at a decrypted dump.

## mgspo-lift

```
mgspo-lift <boot.elf> <outdir>            write C++ sources, one function per guest function
mgspo-lift <boot.elf> --stats             function discovery numbers only
mgspo-lift <boot.elf> --disasm <addr> <n> disassemble n words from a guest address
```

The module is loaded at a fixed base (`0x08804000`) with its relocations applied, so the output
uses real guest addresses. Output goes into a directory you pick (`/generated/` is git-ignored);
to build it into a runnable exe, configure with `-DMGSPO_GENERATED_DIR=<outdir>` and run
`mgspo-game <boot.elf>`. See [docs/lifter.md](docs/lifter.md) for how it works and what is missing.

## Layout

```
src/           ISO9660 reader, PARAM.SFO and PBP/~PSP parsers, ELF/PRX parser,
               MIPS decoder, relocation, function discovery, C++ emitter
runtime/       guest memory, register state, import dispatch that generated code links against
tools/extract/ the mgspo-extract command line tool
tools/lift/    the mgspo-lift command line tool
tests/         unit tests using small synthetic images (no game data needed)
docs/          legal notes and findings about the game binary
```
