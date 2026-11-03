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
Currently **In-game**. still work to be done.

Work in progress. The main module, the helper libraries and a chosen set of stage modules are
lifted to C++ and run against the runtime. The game boots, loads its stage modules from the disc,
streams and inflates their data and issues GE command lists. The intro movie plays (see
[docs/movies.md](docs/movies.md)), the game goes through the title, name entry and opening
cutscene, and then reaches the first stage (the prison cell, with HUD and radar) where Snake
can be moved. Performance there is well below real time; see the profiling notes in
[docs/boot-progress.md](docs/boot-progress.md); background on the
binary is in [docs/eboot-notes.md](docs/eboot-notes.md).

## Quick start (Windows)

You need your own legally obtained disc image of the US release (`ULUS10202`, plain `.iso`) and
Visual Studio 2022 or Build Tools with the "Desktop development with C++" workload (this also
brings CMake and Ninja). About 6 GB of free disk space is needed for the work directory.

Build the tools once, then let the launcher do the rest:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mgspo-launcher
build\mgspo-launcher run "D:\games\Portable Ops.iso"
```

(run these from an "x64 Native Tools Command Prompt", or any prompt that already has CMake and
Ninja; the launcher itself finds Visual Studio through `vswhere` and sets up the compiler
environment.) `play.bat` is a double-clickable wrapper around it; with no ISO known it opens a file
picker.

The first run checks the disc id, extracts the disc, lifts the main module, helper modules and the
default stage list, builds `mgspo-game` and starts it. The ISO path is remembered in
`%LOCALAPPDATA%\MGSPO\launcher.ini`, and every step is cached, so later runs start the game
straight away. If the game loads a stage that was not lifted, the launcher spots the `not lifted`
line in the game log, adds the stage, re-lifts, rebuilds and restarts.

Moving to a different source checkout automatically resets the CMake configuration in the work
directory before building. Extracted disc files and saves are kept.

```
mgspo-launcher [run] [iso]     set up what is missing, then play (default)
mgspo-launcher setup           extract, lift and build only
mgspo-launcher rebuild         redo lift and build from scratch
mgspo-launcher clean [--saves] [--all]   remove generated files (keeps saves unless asked)
mgspo-launcher status          show what is cached
  --work <dir>   work directory, default %LOCALAPPDATA%\MGSPO\work
  --no-relaunch  do not rebuild and restart when another stage is needed
  -- <args>      passed to mgspo-game, e.g.  -- --headless --timeout 60
```

All output (extracted files, generated C++, build, saves, logs) lives in the work directory,
outside the repository. It contains material derived from the game, so never commit or share it.

### Advanced: doing it by hand

Run everything from an "x64 Native Tools Command Prompt". This is what the launcher automates.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build

:: pull the disc files out (read only use; nothing here goes into git)
build\mgspo-extract extract "Portable Ops.iso" disc PSP_GAME
build\mgspo-extract eboot "Portable Ops.iso" boot

:: lift the main module, the helper libraries and the stage modules you want
set U=disc\PSP_GAME\USRDIR
build\mgspo-lift boot\boot.elf gen ^
    --helper %U%\modules\zlibdec.prx --helper %U%\modules\kjfs.prx --helper %U%\modules\sound2.prx ^
    --stage %U%\stage\init\init.prx --stage %U%\stage\r_main01\r_main01.prx ^
    --stage %U%\stage\r_main02\r_main02.prx --stage %U%\stage\r_net01\r_net01.prx ^
    --stage %U%\stage\gsmenu\gsmenu.prx --stage %U%\stage\title\title.prx ^
    --stage %U%\stage\flashdemo\flashdemo.prx --stage %U%\stage\st04vo\st04vo.prx

:: build the game around the lifted code (re-run configure after every re-lift)
cmake -S . -B bgame -G Ninja -DCMAKE_BUILD_TYPE=Release -DMGSPO_GENERATED_DIR=%CD%/gen -DMGSPO_BUILD_TESTS=OFF
cmake --build bgame

:: run
bgame\mgspo-game boot\boot.elf --disc disc --iso "Portable Ops.iso" --save sav
```

Notes:

- `boot.elf` is `PSP_GAME/SYSDIR/BOOT.BIN`, which is a plain ELF on this disc. A separately
  decrypted main module also works in its place.
- `--stage <prx>` lifts a stage module as an overlay; `--stage-dir <dir>` does all of them. Lifting
  every stage is huge (hundreds of MB of C++ and a long build), so lift only the stages the game
  asks for. When the game loads a stage that was not lifted, the log shows `not lifted` with its
  size; find the file with that size under `USRDIR\stage` and add it with `--stage`.
- Add `--headless --timeout 60 --ge-stats` to run without a window and print GE statistics, and
  `--ge-dump <file>` to dump the command lists.
- Delete the `--save` folder between runs while debugging; the game behaves differently when it
  finds old save data.
- `--modules <dir>` is optional and takes already decrypted firmware modules.

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
mgspo-lift <boot.elf> <outdir> --helper <mod.prx>   also lift an extra plain-ELF module (repeatable)
mgspo-lift <boot.elf> <outdir> --helper-dir <dir>   same, for every plain-ELF module in a folder
mgspo-lift <boot.elf> --stats             function discovery numbers only
mgspo-lift <boot.elf> --disasm <addr> <n> disassemble n words from a guest address
```

The module is loaded at a fixed base (`0x08804000`) with its relocations applied, so the output
uses real guest addresses. Output goes into a directory you pick (`/generated/` is git-ignored);
to build it into a runnable exe, configure with `-DMGSPO_GENERATED_DIR=<outdir>` and run
`mgspo-game`. See [docs/lifter.md](docs/lifter.md) for how it works and what is missing.

## mgspo-game

```
mgspo-game <boot.elf> [--disc <dir>] [--iso <file>] [--modules <dir>] [--save <dir>]
                      [--headless] [--trace] [--report <file>] [--timeout <sec>]
```

`--disc` is the extracted disc (the folder that contains `PSP_GAME`), served to the game as
`disc0:/` (read only). `--save` is a plain folder backing `ms0:/`. Without `--headless` you get a
window (2x) with keyboard or XInput controls: arrows = d-pad, Z/X/A/S = cross/circle/square/triangle,
Q/E = L/R, Enter = start, Right Shift = select, IJKL = analog stick.

The game also reads the disc by raw sector (`sce_lbn0x<lba>_size0x<bytes>`), so pass `--iso` as well to serve those. `--modules` points at a folder of already decrypted modules; see [docs/boot-progress.md](docs/boot-progress.md).

Every import the game calls without an implementation is logged once with its module and NID.
`--report` writes all imports in order of first call with call counts, which is the list to work
from. Details are in [docs/lifter.md](docs/lifter.md).

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
