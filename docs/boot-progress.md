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

## Debugging aids

- Every lifted function keeps a small shadow call stack. A guest fault (bad address, call into an
  unlifted overlay, ...) prints the faulting pc, the last 32 callers, all registers and, with
  `--fault-dump <addr> <len>`, a hex dump of a memory range.
- `--watch <addr> <len>` logs every guest store into a range along with the call stack. It needs a
  build configured with `-DMGSPO_WATCH=ON`, since it adds a check to every store.
- `--dump-frame <png>` rewrites a PNG of the displayed frame about every 30 vblanks. Keep it
  outside the repo.
- `--auto-press <mask>` holds PSP buttons periodically for unattended runs (Start is `0x8`).
- `--ge-stats` prints draw/triangle counts; `--ge-dump` writes the executed GE commands.

## Where it stops

With the init, r_main01/02, r_net01, gsmenu and title stages lifted the game loads three stages,
inflates the big stage archive, plays the intro movie and reaches the title screen ("PRESS START
BUTTON" over the red camera background, drawn from the decoded movie frames). It runs without
faulting (checked up to 100 s headless). Movie playback is described in [movies.md](movies.md).

Faults found and fixed on the way:

- `bad guest address 6`/`0` came from three separate problems:
  1. `sceIoDevctl` 0x02425818 (memory stick free space) passes its info pointer in the input
     buffer, not the output buffer.
  2. The GE signal/finish callbacks were called with `(listId, ..., signal)`. The game's callback
     takes the 16-bit signal/finish data as its first argument and stores it in a ring buffer, so
     the wrong argument order overwrote the resource table at 0x08A31C6C.
  3. Lifter: function discovery added tail-jump roots before switch-case roots, so a branch
     into a function that was split later pointed at a non-root and ended up as
     `call to 0x08B9B7FC in an overlay that is not loaded or not lifted`. Both root passes now
     run until neither adds anything.
- `sceUtility` `0x9A1C91D7` is `sceUtilityMsgDialogGetStatus`. The game polls it every frame;
  returning 0 (no dialog) is correct for now, it was not related to the fault.
- 0 triangles: the game programs the GE framebuffer, depth and texture/CLUT addresses as plain
  VRAM offsets (e.g. 0x44000) with no 0x04 region byte. The rasteriser rejected them
  (`begin()` failed for every draw). Addresses below 2 MB with no region bits are now treated as
  VRAM. Triangles and sprites now rasterise (tens of thousands per run, none culled).

What the screen shows: the intro movie plays through (about 280 pictures) and the game then sits on
the title screen. `--auto-press 0x4000` (Cross only) stays there. Pressing Start (`0x8`) skips the
movie if it is still playing; on the title screen it leads to a black screen, and with
`--auto-press 0x4008` to a full-screen black/white dither pattern. The game keeps running frames
in both cases (hundreds of GE lists, sas voices, no faults, no new stage loaded), so this looks
like a rendering problem in the post-title screen, not a hang. Not investigated further yet.

Next blockers, roughly in order:

1. **Screen after Start.** See above. Dump the GE commands of one frame (`--ge-dump`) and check
   texture format 4 (swizzled CLUT8) and format 5 uploads, the CLUT loads and the sprite draws
   in 5551 framebuffers first.
2. **More stages.** Each new stage the game requests that is not lifted shows up as `not lifted`
   with its size; add it with `--stage`. Jump-table style stubs reached by computed jumps can still
   be missing from function discovery.
3. **Movie audio.** The movie's ATRAC3+ stream is demuxed and paced (`sceMpegGetAtracAu` /
   `sceMpegAtracDecode` run in step with the video) but there is no ATRAC3+ decoder, so it plays as
   silence.
4. **Network imports** (sceNet*, sceHttp) used by stages need stubs that report no connection.
5. **Audio**: `sceSasCore`/`sceAtrac3plus` coverage.
## Title menu and name entry

- The GE blend register was decoded wrongly (source factor is bits 0-3, destination bits 4-7, function bits 8-10). Fixing it makes the title menu text and background render.
- Start now reaches the title menu; New Game reaches the name entry prompt.
- sceUtility msgdialog and OSK have basic HLE, but the OSK result struct layout is not right yet, so the game reports 'entered text is too short' and loops. Next blocker.
