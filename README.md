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
brings CMake and Ninja). Reserve at least **20 GiB** for the work directory. Before each uncached
extract/lift/build step, the launcher requires **12 GiB free** on that drive.

Build the tools once, then let the launcher do the rest:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mgspo-launcher --parallel 1
build\mgspo-launcher run "D:\games\Portable Ops.iso"
```

(run these from an "x64 Native Tools Command Prompt"; 32-bit Windows builds are not supported.
The launcher itself finds Visual Studio through `vswhere` and selects the x64 compiler even when
started from an x86 developer prompt.) `play.bat` is a double-clickable wrapper around it; with no ISO known it opens a file
picker.

The first run checks the disc id, extracts the disc, lifts the main module, helper modules and the
**every stage module on the disc**, builds `mgspo-game` and starts it. The ISO path is remembered in
`%LOCALAPPDATA%\MGSPO\launcher.ini`, and every step is cached, so later runs start the game
without discovering/rebuilding stages during play. Setup, Build, Launch and update/rebuild all use
this same all-stage policy. An older partial build is invalidated automatically; saves and input/icon
settings are kept. `--no-relaunch` remains accepted for script compatibility, but automatic
stage-guessing/relaunch is no longer used.

The US disc contains **106 stage PRX files**. The first full lift produces roughly **493 MiB of C++**
(about 1,577 generated files) and compilation uses **one job**, including Ninja's generated-game
compilation pool. Expect tens of minutes or longer, depending on CPU/storage/toolchain; subsequent
unchanged builds are cached. The UI remains responsive and logs per-stage lift and per-file compiler
progress. Do not start competing heavy builds to accelerate it. Keep additional free disk space for
objects, the executable and future update versions.

Stages are enumerated recursively in deterministic order and duplicate paths are removed. A
completed `gen\stages.manifest` records every module and its `_zar` relocation archive by content,
and participates in cache validation together with ISO, source revision and tools. Additions,
removals, content changes and missing/partial manifests invalidate the lift/build. Unsupported or
encrypted stage files are explicit errors, not silently skipped stages. Compilation is **not**
playthrough coverage: emitted unsupported-instruction traps and unimplemented imports can still
block later gameplay, and compiling every overlay does not verify every mission.

### Desktop launcher (Windows)

Build `cmake --build build --target mgspo-desktop --parallel 1`, then double-click
`build\mgspo-desktop.exe`. This native Windows UI needs no console or additional UI framework.
Browse for your ISO, choose **Modern gamepad** or **Legacy PSP mapping**, and choose **Build / setup ISO**
or **Launch game**. Launch reuses the CLI's extract/lift/build pipeline and validates cached outputs
before starting. **Build / setup ISO builds all stages up front**, with the same disk checks and
long-first-build messaging as the CLI. Install Visual Studio 2022 / Build Tools with
**Desktop development with C++**, including CMake and Ninja; missing-tool errors explain what to install.
The UI does not install dependencies.

**Render scale** and **Window** are independent settings, applied on the next game launch:
1x renders at the native **480x272** (the default). Every integer scale from **2x through 8x** is
available: 960x544, 1440x816, 1920x1088, 2400x1360, 2880x1632, 3360x1904 and **3840x2176**.
Triangles, sprites and patches use the selected finer sample grid with separate host colour/depth buffers.
Window presets choose an initial
client size of 480x272, 960x544 (default), 1440x816 or 1920x1088. You can still resize the game window;
the presenter preserves aspect ratio with black letterboxing. A larger window alone does not increase
internal detail, and neither setting changes guest timing or input coordinates. Save settings to keep
them across launcher restarts and source updates; existing control/icon preferences are preserved.

The higher-resolution software mode is deliberately **native-authoritative**: it retains the exact native rendering
pass for guest CPU reads, transfers and texture feedback, then replays supported geometry into host
shadows. It does not downsample host pixels over guest VRAM. Work grows approximately with the square
of the scale: 2x has four times the coverage samples, 8x has 64 times, **plus the native pass**.
Extra synchronization and host storage also cost time. At most eight planes are cached; the byte
budget is the larger of 64 MiB and three 512x272 RGBA planes including native snapshots at the
selected scale (about 104 MiB at 8x). A typical 8x colour/depth pair uses about 52 MiB, before a second
colour target, presentation/conversion buffers and optional frame dumps. Unpinned targets are evicted;
if a required allocation cannot fit while targets are pinned, the game stops with an explicit
lower-scale error instead of silently claiming native pixels are high-resolution rendering.
Allocation failures also stop the run. Expect severe frame-time costs at high scales on this software
renderer; 1x remains available without the extra pass or surface allocations.

There are explicit correctness fallbacks, reported once per kind in the game log:
CPU/HLE VRAM pointer access (including readback) conservatively reseeds host buffers from native pixels;
retained-pointer pixel changes are detected too. Transfers and aliased format/stride views also reseed.
Runtime code retaining a raw writable VRAM pointer across GE execution must explicitly notify the
surface manager after writing, even for same-value stores; byte comparison cannot observe those stores.
Render-target textures sample their **native** source, so offscreen/feedback effects do not gain source
detail; same-target texture feedback and overlapping colour/depth targets use the native draw result.
Host targets are limited to valid VRAM ranges with stride/height at most 1024; RAM targets and surfaces
not matching the displayed address/stride/format present natively. Evicted surfaces are reseeded.
Lines and points preserve native-grid coverage expanded into host pixels. These paths are not claimed
as finer-detail rendering. Existing PSP textures, interface artwork and movies cannot acquire new source
detail; higher scales improve supported geometry edges/interpolation, not the source assets. Local frame dumps use
the actual displayed surface dimensions, including 480x272 when a whole-frame native fallback applies.

CLI equivalents (also accepted by the desktop launcher):

```powershell
mgspo-launcher run --render-scale 2 --window-size 1440x816
# Direct runtime: mgspo-game <boot.elf> ... --render-scale 2 --window-size 1440x816
```

Only the listed values are accepted; invalid/extreme sizes fail with an actionable error. These are
runtime settings, not stage-generation settings, so changing them does not require rebuilding stages.
For local comparison, `--dump-frame <png> --dump-native-frame <other.png>` captures the displayed
surface and authoritative native pixels at the same cadence without changing guest readback behaviour.

**Renderer** selects **Software** (default/reference), **Direct3D 11**, **Direct3D 12**, or **Vulkan**.
Use **Check GPU / driver** for the selected build/adapter status; this runs on the launcher's worker,
not the UI thread. Unavailable builds/drivers produce an error before game startup, never silently
select Software under a hardware label. The preference is independent of scale/window size,
survives updates and activated-UI restarts, and does not invalidate lifted stages.

The hardware path is an experimental **GPU compute rasterizer**, not a fixed-function graphics pipeline
or just GPU presentation. One shared shader performs triangle/sprite coverage, texture sampling,
packed colour writes, independent depth, alpha/colour/stencil tests, blending, logic operations and fog.
CPU transform/skinning/clipping and patch tessellation feed that shader; lines/points retain the
native-grid expansion described above. Existing CPU texture/CLUT/DXT conversion and identified menu
glyphs are reused. All scales 1x..8x are accepted, including genuine finer coverage at higher scales.
The exact native **CPU pass still runs**, even at hardware 1x, to preserve guest VRAM/readback semantics.
The same explicit feedback/CPU/movie/alias fallbacks apply. Neither hardware selection nor a native
fallback guarantees higher detail for every frame.

Consecutive draws with the same colour/depth planes and decoded texture contents are batched
in strict order, retaining per-draw depth/blend/scissor and other shader state. Host readback is
deferred until presentation, explicit host-plane access, incompatible target changes,
CPU/alias reseeding, reset or renderer replacement. Guest CPU reads still see the native pass
immediately, never deferred GPU pixels. Decoded texture contents are compared, not just guest addresses,
so changing a texture cannot reuse stale data. A texture change submits preceding commands with their
old snapshot, then changes the texture without uploading or reading back the Direct3D targets.
At most 4096 primitives accumulate before bounded execution; each submission still waits at the
64-tile limit, but Direct3D target pixels remain on the GPU across those fenced chunks.
GPU colour/depth/staging resources persist across completed submissions in D3D11 and D3D12,
rather than allocating per draw. Counters report target upload/readback pairs separately from dispatches.
The prerequisite-gated, unverified Vulkan path does not yet support retained-target transfers;
it keeps explicit bounded synchronous transfers rather than claiming this Direct3D optimization.

This reduces one measured 128-draw, 32x32 depth/blend fixture at 2x from 128 submissions to two
with one target upload/readback pair and an identical host-image hash: D3D11 was about 1999 ms before
batching versus 40..46 ms after; D3D12 about 457 ms versus 20 ms on an RTX 2070 SUPER. Reproduce with
`mgspo-ge-tests d3d11 --batch-workload` or `d3d12 --batch-workload`. These are small-workload
observations, not a game-speed comparison or a claim of being faster than Software.
Texture/target switches, conversion, native rendering and synchronous bounded-chunk waits still
cost time. Before batching, bounded prison observations were only about 0.5 flips/s with D3D11
and 0.8..1.8 with D3D12 at 1x, and some 2x stage/fade intervals about 0.2.
Latest bounded post-batching runs showed matching host/native PRISON pause menus at D3D11 1x and
D3D12 2x. Late profiler intervals were around 1.1 flips/s and 5.0..6.2 flips/s respectively;
these are different-scale/state smoke observations, not a controlled before/after game benchmark.
**Software remains recommended for playing** until broader hardware performance/correctness is established.
Hardware mode expands packed colour/depth
into 32-bit storage entries (including 16-bit guest formats). Upload arrays are bounded to 128 MiB
per job, ordered submissions to 64 tiles of at most 256x256 pixels, and completion waits to five
seconds with no retry after timeout/device failure. Both Direct3D backends reuse only completed allocations
within a 384 MiB logical buffer budget, in addition to host shadows/conversion and driver overhead.
Large targets can fail explicitly with a lower-scale recommendation. An unfinished terminal
submission is retained until process teardown, rather than freeing memory still in GPU use.
No driver/TDR settings are modified. Software 1x remains the lowest-resource option.

```powershell
mgspo-launcher run --renderer d3d11 --render-scale 2
mgspo-game --renderer d3d12 --renderer-info
# Explicit adapter-dependent tests, sequentially:
.\build\mgspo-hardware-tests.exe d3d11
.\build\mgspo-ge-tests.exe d3d11
.\build\mgspo-hardware-tests.exe d3d12
.\build\mgspo-ge-tests.exe d3d12
```

Windows D3D backends require a hardware feature-level-11.0 adapter and the Windows SDK libraries /
D3DCompiler; they never automatically use WARP. Startup logs identify the actual backend/adapter,
and completion/timeout diagnostics report GPU submissions, dispatches and primitives.
Vulkan's API source path is included, but availability additionally requires an installed Vulkan
driver/loader and **SPIR-V-enabled DXC**. No SDK/compiler/driver binaries are downloaded.
Configure `-DMGSPO_DXC="C:\path\to\dxc.exe"` with a DXC build supporting `-spirv -T cs_6_0`,
`-fspv-target-env=vulkan1.0`, `-fvk-use-dx-layout`, `-fvk-t-shift` and `-fvk-b-shift`.
CMake actually compiles a probe: advertising these switches in help is insufficient. Configure that
cache entry in both the launcher/tools build and the generated game's separate build directory.
The installed Windows SDK DXCs (10.0.19041/22000/22621/26100) on the verification host report
**SPIR-V CodeGen not available**, so Vulkan is explicitly unavailable there and has **not** been
GPU-tested. No particular DXC version alone guarantees that build option. After supplying a
compatible compiler, rebuild and run the same explicit fixtures with `vulkan` before relying on it.
`-DMGSPO_VULKAN=OFF` excludes that optional source path. Pinned Khronos API declarations and their
Apache-2.0 notices are under `runtime/third_party/vulkan`; no Vulkan loader is redistributed.

The **Icons** selector defaults to **Original PSP**. **Modern Xbox-style (menu X/Y)** currently
replaces positively identified title/pause menu Triangle and Square prompts with original-authored
Y and X artwork. Other symbols remain original; this is not yet a full-game Xbox prompt set.
Save settings and relaunch the game to change modes. See [prompt coverage](docs/controls.md#controller-icons).

ISO, control and icon settings are remembered in `%LOCALAPPDATA%\MGSPO\launcher.ini`; existing custom keys
are kept. Work, saves and full operation logs are under `%LOCALAPPDATA%\MGSPO\work` by default.
For a separate installation use `mgspo-desktop --config "D:\MGSPO\launcher.ini" --work "D:\MGSPO\work"
--source "D:\src\MGSPOStaticRecomp"` (on one command line). The CLI also accepts `--config` and
`--controls modern|legacy` and `--prompts original|modern`; trailing `-- game args` remain supported.
Keyboard bindings are unchanged.
Desktop launches conservatively use two renderer workers.

Build/game/update operations run in the background with a bounded live log. Controls are locked while
busy, and a work-directory lock prevents other launchers using that same directory concurrently.
**Cancel**, or closing while busy, explicitly asks before stopping only the operation's process tree;
unfinished steps are rebuilt on the next attempt. Saves are never removed by UI build/update actions.
Do not run manually started games/builds against the same work directory at the same time.

**Check updates** explicitly fetches `origin`'s `main`, shows both full revisions and compares ancestry.
Git for Windows and your existing Git/GitHub authentication are required; offline/authentication
failures are shown, and credentials are neither read nor stored by the UI. There is no startup network
check, automatic install, remote-script execution, or game-derived download.
Updates support only the official `ccbury/MGSPOStaticRecomp` GitHub origin and a clean, standalone
clone on `main`. Linked worktrees, archives, feature branches, local/untracked edits, unfinished Git
operations, ahead/divergent history and junction/symlink locations are refused. The UI never stashes,
resets, force-updates or edits the original source checkout; fetch changes only its Git metadata.

After review, **Update / rebuild** asks for confirmation, fetches/rechecks the exact revisions again,
then creates or fast-forwards a launcher-owned clone in `work\updates\source`. Tools/UI are built in
a new version directory, followed by a game rebuild using the new CLI, with `--parallel 1`.
Revision-based cache stamps invalidate old generated/game output; managed sources cannot be run with
tools compiled for a different revision. The running executables are not replaced. Only after both
builds succeed are settings switched to the new source/UI and **Restart UI** enabled. Later starts of
the original desktop executable forward to that explicitly activated local UI.

On failure the original launcher files, saves and settings remain, but the managed source and build
caches may already have advanced: this is **not transactional source rollback**. Use **Check updates**
then **Update / rebuild** to retry, even when source already matches main. Local edits in the managed
clone are also refused and preserved. Failed/older tool directories are kept for inspection, not
silently deleted. Keep build paths reasonably short for the Windows compiler. Updates compile locally
from your own ISO and never distribute game content.

Launcher regression checks build no generated game code:

```
cmake --build build --target mgspo-launcher-tests mgspo-launcher-update-tests --parallel 1
ctest --test-dir build -R mgspo-launcher --parallel 1 --output-on-failure
```

Updater tests use disposable local Git remotes, including successful staged tools/game-build
fixtures and deliberate build failures; the fixture game is a tiny test program, not the actual game.
For scripted native-window checks on an interactive Windows desktop, run
`powershell -File tests\test_desktop_ui.ps1 -Binary build\mgspo-desktop.exe -OutputDirectory build\ui-check`.
This checks settings, errors, unsupported-source refusal and close/reopen, retaining a UI-only
screenshot in the output directory; it does not play or rebuild the game.

Moving to a different source checkout, or finding a cached x86 compiler, automatically resets the
CMake configuration in the work directory before building. Extracted disc files and saves are kept.
The launcher builds with one compiler job to bound memory use. Ninja also serializes compilation
of `mgspo-game` sources, including when built manually; other targets can still build concurrently.

```
mgspo-launcher [run] [iso]     set up what is missing, then play (default)
mgspo-launcher setup           extract, lift and build only
mgspo-launcher rebuild         redo lift and build from scratch
mgspo-launcher clean [--saves] [--all]   remove generated files (keeps saves unless asked)
mgspo-launcher status          show what is cached
  --work <dir>   work directory, default %LOCALAPPDATA%\MGSPO\work
  --no-relaunch  compatibility option (all stages now build up front; no automatic relaunch)
  -- <args>      passed to mgspo-game, e.g.  -- --headless --timeout 60
```

All output (extracted files, generated C++, build, saves, logs) lives in the work directory,
outside the repository. It contains material derived from the game, so never commit or share it.

### Advanced: doing it by hand

Run everything from an "x64 Native Tools Command Prompt". This is what the launcher automates.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 1
ctest --test-dir build

:: pull the disc files out (read only use; nothing here goes into git)
build\mgspo-extract extract "Portable Ops.iso" disc PSP_GAME
build\mgspo-extract eboot "Portable Ops.iso" boot

:: lift the main module, helpers and ALL stages (the normal launcher default)
set U=disc\PSP_GAME\USRDIR
build\mgspo-lift boot\boot.elf gen ^
    --helper %U%\modules\zlibdec.prx --helper %U%\modules\kjfs.prx --helper %U%\modules\sound2.prx ^
    --stage-dir "%U%\stage"

:: build the game around the lifted code (re-run configure after every re-lift)
cmake -S . -B bgame -G Ninja -DCMAKE_BUILD_TYPE=Release -DMGSPO_GENERATED_DIR=%CD%/gen -DMGSPO_BUILD_TESTS=OFF
cmake --build bgame --parallel 1

:: run
bgame\mgspo-game boot\boot.elf --disc disc --iso "Portable Ops.iso" --save sav
```

Notes:

- `boot.elf` is `PSP_GAME/SYSDIR/BOOT.BIN`, which is a plain ELF on this disc. A separately
  decrypted main module also works in its place.
- `--stage-dir <dir>` includes every `.prx` recursively (case-insensitive extension), sorted and
  deduplicated with any explicit `--stage` inputs. Invalid/encrypted files, missing/empty stage
  directories and filesystem errors fail visibly. The lifter writes the completed inventory only
  after emission succeeds. `--stage <prx>` remains available for advanced, deliberately partial
  manual builds; those can still stop when the game requests an omitted overlay. The launcher
  always uses the complete disc stage directory.
- Add `--headless --timeout 60 --ge-stats` to run without a window and print GE statistics, and
  `--ge-dump <file>` to dump the command lists.
- Delete the `--save` folder between runs while debugging; the game behaves differently when it
  finds old save data.
- `--modules <dir>` is optional and takes already decrypted firmware modules.

## Building

Requires CMake 3.16+ and a C++17 compiler.

```
cmake -S . -B build
cmake --build build --parallel 1
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

XInput gamepads use **modern controls** by default: left stick moves, right stick controls the
gameplay camera, and the D-pad directly selects equipment using the game's existing directional
weapon/item slots, without holding B. LT aims / enters first person; left stick still moves and
right stick aims there. RT or physical Xbox X fires/punches, and B or RB opens the weapon menu.
D-pad menu/dialog navigation is unchanged, and the right stick is inactive in menus. Keyboard
mappings are unchanged, including the original Circle + arrows selection.

Use `--controls legacy` to restore the original XInput mapping (D-pad camera, B + D-pad equipment,
right stick unused), or `--controls modern` to select the default explicitly. Through the launcher:
`mgspo-launcher run -- --controls legacy`. Manual builds must **re-lift** the main module with the
updated lifter before rebuilding the game. See [docs/controls.md](docs/controls.md) for implementation
details and input diagnostics.

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
