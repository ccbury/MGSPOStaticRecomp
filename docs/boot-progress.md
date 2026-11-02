# Boot progress and encrypted modules

Notes from tracing the game after the helper modules load. Written against ULUS10202 with the
decrypted `BOOT.BIN`; nothing here needs console keys.

## Encrypted modules

`USRDIR/modules` holds 25 modules. Three (`zlibdec`, `kjfs`, `sound2`) are plain PRX and are
lifted as helpers. The other 22 start with `~SCE` and are encrypted:

libadler, libbase64, libccc, libdeflt, libfont, libfpu, libheap, libmd5, libmp3, libmt19937,
libpsmfplayer, libqpcode, libsha0, libsha1, libsha224, libsha256, libsha384, libsha512, psmf,
pspnet_ap_dialog_dummy, scan, show_macaddr.

Which of them does the game use? Looking at the main module's import table and at every
`sceKernelLoadModule*` call seen while running:

* Only **libfont** is ever loaded (`sceKernelLoadModuleByID`, then `sceKernelStartModule`).
* The main module imports nothing from libmp3/psmf/the hash libs/etc., and none of the other 21
  is requested at run time in what we have reached so far.
* From libfont the main module imports 8 functions (`sceLibFont`):

| NID | Function |
|---|---|
| 0x67F17ED7 | sceFontNewLib |
| 0x574B6FBC | sceFontDoneLib |
| 0xA834319D | sceFontOpen |
| 0x3AEA8CB6 | sceFontClose |
| 0x099EF33C | sceFontFindOptimumFont |
| 0x0DA7535E | sceFontGetFontInfo |
| 0xDCC80C2F | sceFontGetCharInfo |
| 0xCA1E6945 | sceFontGetCharGlyphImage_Clip |

## Runtime replacement (HLE)

`runtime/src/font.cpp` implements those eight calls natively. Glyph bitmaps come from the host
through `plat::glyph_raster` (GDI on Windows, a box-glyph fallback in the null platform).
The handlers are registered as *soft* handlers (`register_hle_soft`): if a lifted module exports
the same library and NID, the lifted code wins, otherwise the native one runs.

The `FontInfo`/`CharInfo` struct layouts are reconstructed from public homebrew headers and have
not been checked against what the game actually reads yet. Treat font output as unverified until
the game gets far enough to draw text.

## Supplying decrypted modules

If you have decrypted copies of any of those modules (from your own console or a tool of your
choice), the project can lift them like the other helpers:

```
mgspo-lift BOOT.BIN gen --helper zlibdec.prx --helper-dir my-decrypted/
mgspo-game BOOT.BIN --disc disc --iso game.iso --modules my-decrypted/
```

* `--helper-dir` adds every plain ELF/PRX in the folder (sorted by name) and skips anything that
  still begins with `~SCE`, saying so.
* At run time `--modules <dir>` is searched first for a file with the same base name as the one
  the game loads; the disc copy is the fallback. A module that is still encrypted gets a stand-in
  id and the HLE is used.
* The lift and the run need the same set and order of helpers, since helpers get fixed bases.

## Boot trace

What the game does once the three helpers are started:

1. `sound2` spawns its audio thread, which immediately calls `sceSasCore` / `sceAtrac3plus`
   (unimplemented here; another branch covers those). The thread spins, which is harmless apart
   from CPU use.
2. The game polls `sceUmdGetDriveStat`, then opens files as
   `disc0:/sce_lbn0x<lba>_size0x<bytes>`. These are raw sector windows onto the disc. The LBA
   comes from the file's `SceIoStat.st_private[0]` (offset 64, also embedded in the `sceIoDread`
   entries), so `sceIoGetstat`/`sceIoDread` now report the real LBA. `--iso` is needed to read
   them; `--disc` is still used for ordinary paths.
3. Data is read in 0x80000 byte chunks by a loader thread and inflated through `zlibdec`.
4. The game then loads stage code with `sceKernelLoadModuleByID` on one of these windows and
   starts it.

Lifter fixes found on the way (all were "call to unrecompiled address" crashes):

* A branch or jump that targets another function is turned into a root, repeated until stable.
* Pointer roots are accepted after an unconditional jump, not just after `jr ra`.
* A branch-likely whose fall-through lands outside the function emits a call instead of a `goto`.

## Stage modules

The 106 stage modules are overlays: the game loads one at a time into the `mgp_stage` slot via
`disc0:/sce_lbn0x<lba>_size0x<bytes>` (the size is in bytes, not sectors) with
`sceKernelLoadModuleByID`, starts it, and later stops and unloads it. The lifter handles them with
`--stage <prx>` (opt-in, see README). Each stage is lifted at its slot address, functions are
deduplicated by content hash across stages, and at run time the loaded image is matched to a lifted
module by hash. The runtime writes the unlinked image; the game's own relocator applies the `.rlc`
link table in the `_zar` archive, and the lifter applies the same table to its copy so calls from
stages into the main module resolve to real addresses. Main entry points that only stages reference
are seeded as roots.

Lifting all 106 stages gives about 321K functions (181K distinct, roughly 440 MB of C++), so it is
not done by default.

## Where it stops

With the init, r_main01/02, r_net01, gsmenu and title stages lifted the game loads three stages,
inflates the big stage archive through the lifted zlib module and issues 77 GE lists
(218 draws, 2552 vertices), but still rasterises 0 triangles. It then stops in `Main` with
`bad guest address 0x00000000`.

Next blockers, roughly in order:

1. **Null dereference after the title stage starts.** Need the faulting pc (the error only names the
   address). It follows `sceUtility` import `0x9A1C91D7`, which is stubbed to return 0 for now and
   whose meaning is unknown; the real result may be a pointer or handle the game expects.
2. **More stages.** Each new stage the game requests that is not lifted shows up as `not lifted`
   with its size; add it with `--stage`. Jump-table style stubs reached by computed jumps can still
   be missing from function discovery.
3. **Triangles.** Draws so far are state/clear only; verify with `--ge-dump` once the game gets
   further.
4. **Network imports** (sceNet*, sceHttp) used by stages need stubs that report no connection.
5. **Audio**: `sceSasCore`/`sceAtrac3plus` coverage.