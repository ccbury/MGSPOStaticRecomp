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

Early groundwork. What exists today is a command line tool that opens a PSP disc image, lists and
extracts its files, reads `PARAM.SFO`, and parses the main module so we know what the recompiler
has to deal with. The findings are written up in [docs/eboot-notes.md](docs/eboot-notes.md).

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

## Layout

```
src/           ISO9660 reader, PARAM.SFO and PBP/~PSP parsers, ELF/PRX parser
tools/extract/ the mgspo-extract command line tool
tests/         unit tests using small synthetic images (no game data needed)
docs/          legal notes and findings about the game binary
```
