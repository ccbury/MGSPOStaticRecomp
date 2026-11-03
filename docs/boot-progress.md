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
3. **Movie audio.** The movie's ATRAC3+ stream is demuxed, paced and decoded (see audio.md).
4. **Network imports** (sceNet*, sceHttp) used by stages need stubs that report no connection.
5. **Audio**: `sceSasCore`/`sceAtrac3plus` coverage.
## Title menu and name entry

- The GE blend register was decoded wrongly (source factor is bits 0-3, destination bits 4-7, function bits 8-10). Fixing it makes the title menu text and background render.
- Start now reaches the title menu; New Game reaches the name entry prompt.
- The OSK result is fixed (see below). Next blockers are listed at the end of this file.

## Name entry, savedata and the opening movie

- OSK data struct: `outtextlength` +0x24 is the buffer size (0x200), `outtext` +0x28, `result` +0x2C (2 = finished), `outtextlimit` +0x30. The HLE writes the name as UTF-16 into `outtext` and reports the length in characters. The common header is 0x30 bytes, so the data pointer sits at +0x34.
- The game filters names, so the stub name has to be an ordinary word (it uses JACK). A name that looks like a banned word gets a msgdialog and the prompt repeats.
- `sceUtilitySavedata*` is implemented against `<save>/PSP/SAVEDATA/<gameName><saveName>/<fileName>`. Modes load/save/list-load/list-save/delete are handled, everything else reports success. `saveNameList` (+0x60) is an array of `char[20]` names, not an array of pointers.
- `umd0:`/`umd1:` are served as the raw disc image (offset 0), which the game polls with `sceIoOpenAsync("umd1:")` before streaming from sector offsets. Without it the game spins forever on error 0x80010013. It needs `--iso`.
- The first cutscene is `stage/flashdemo/flashdemo.prx`: it has to be lifted like the other stages. It is a flash-style comic with an `sceMpeg` movie on top.
- Menu confirm is Circle here, not Cross.
- Movie end: a short read from the ring buffer callback now marks EOF and flushes the demuxer, so the last pictures are delivered and the game tears the movie down.
- `sceKernelReleaseWaitThread` on a thread that is inside `sceKernelDelayThread` used to wake the delay, after which the thread blocked in its next semaphore wait with the "stop" flag unread. The release is now remembered and cancels that next semaphore wait. This unblocked the sound-buffer thread join that froze the screen at the end of the movie.
- `--timeout` now also prints the state of every guest thread (name, wait type and id), which is the quickest way to find a hang.

### Where it stops now

The null-object fault after the movie (flash VM at 0x0884158C calling a handler with a null object, fault at `0x000000A8`) is fixed. It was not a threading problem: the game reads its flash data from the raw `umd1:` device with `sceIoLseekAsync`/`sceIoReadAsync`, and on that device both the offsets and the read sizes are in 2048-byte sectors, not bytes. We were treating them as bytes, so the object table was filled with garbage and the handle lookup in 0x0899AC94 returned 0. Raw `umd` files now convert seeks and reads to sectors.

With that in, a 150 s run goes through the opening cutscene with subtitles and then loads a string of stage overlays (all already lifted) without faulting. Remaining issues seen in that run:

- Movie frames are wrong: the picture is a handful of flat vertical colour bands (roughly 16-pixel-wide strips) with the subtitles drawn correctly on top. The AVC decode in `mpeg.cpp` is only producing partial output, so that is the next thing to look at.
- The run ends on `Main` still running with the sound threads waiting; I have not yet got to a playable stage.

Keyboard and XInput mapping in `platform_win32.cpp` was re-read and is complete (arrows/IJKL, Z/X/A/S, Enter/RShift, Q/E; XInput buttons, triggers/shoulders and left stick with a deadzone).

### Performance

Headless, the software GE plus the movie decoder run at roughly 20-25 fps in the movie scenes (game time falls behind wall time). Ideas, roughly in order of payoff: split the rasteriser into screen tiles and fill them on worker threads; use SIMD (SSE2/AVX2) for flat and Gouraud triangle spans; cache decoded textures (swizzle and CLUT lookups) per texture address and mark them dirty on writes; skip the full-screen clears that are overwritten by a full-screen sprite; decode the movie on its own thread instead of inside `sceMpegAvcDecode`.

## First stage

Starting from the cutscene, `st04vo` (the prison stage, 1,099,556 bytes) is the next module to be loaded, so it has to be lifted too (`--stage ...\stage\st04vo\st04vo.prx`). With `--auto-press 0x2008` (Start+Circle) a run goes logo, title, name entry, cutscene, and reaches the cell around the 60 s mark with no fault over a 240 s run. Start in game opens the pause menu, so scripted tests should use `--press time:mask[:hold[:lx:ly]]` instead (times and holds in game seconds, stick values -128..127). Scripted stick input turns and walks Snake.

### Lifter fix: jump tables

The first fault in the stage was `call to unrecompiled address 0x088E6D14`, a case of a switch reached through the table at 0x089EC4C4. Two things were wrong in the lifter:

- Table detection discarded a whole run of code pointers when the run spanned 0x2000 bytes or more, which happens when a table sits right next to other pointer arrays. Runs are now cut where the target window exceeds 0x2000 and each table is recorded (`PrxImage::jumpTables`).
- A case body was treated as reachable if any computed `jr` followed it, even one that belongs to another table. Each non-`ra` `jr` is now tied to the table whose address it builds (lui + addiu/ori within 96 bytes before it); cases without a `jr` of their own table become function roots. A `jr` with no recognisable table keeps the old behaviour.

### Profiling (`--profile`, `--ge-stats`)

`--profile` prints a line every 5 s with flips/s, presents/s, skipped vblanks, time spent in GE list execution and in the present/convert step. `--ge-stats` now also reports raster time per draw class and the fragment count.

In-game and pause menu scenes (this machine, one guest thread, Release build):

- 7.5-15 flips/s, GE 290-770 ms per second of wall time (up to about 75%), present 0.45-0.8 ms per frame (negligible).
- 100 s run: about 860 M fragments, 10.8 M triangles in 1260 lists, about 41 s in the rasteriser (clear 2.2 s, flat 8 s, textured 14.6 s, textured+blend 14.4 s), so roughly 45-50 ns per fragment and about 6-7 screens of overdraw per frame.
- Tried: resolved palette per draw, cached keep mask, per-row span narrowing and skipping unused attribute interpolation in triangles. None of them moved the numbers measurably, so the cost is in the per-fragment pipeline (generic state checks, float to int conversions, blend factor switch) rather than coverage tests.

Next steps for speed: a specialised fragment path for the common state combinations (through-mode sprites with CLUT8 textures, modulate/replace, simple alpha blend, no depth/stencil), then tile-based multithreaded fill. The lifted guest code was not the limit in these runs (GE plus present account for most of the wall time), but it has not been profiled separately.

### Remaining blockers

- Frame rate in game (see above).
- In-game frames after the first scene look dark and blotchy; not yet diagnosed (fade, lighting or dither).
- Movie frame decoding is handled elsewhere.
