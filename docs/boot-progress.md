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
   `disc0:/sce_lbn0x<lba>_size0x<sectors>`. These are raw sector windows onto the disc. The LBA
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

## Where it stops

With `--ge-stats` the run now reaches the GPU:

```
lists 12, draw commands 20, vertices 320, triangles rasterised 0
```

then stops in `Main` with `bad guest address 0x00000040` right after
`sceKernelStartModule` on the first stage module. Nothing is presented yet.

Remaining blockers, roughly in order:

1. **Stage modules.** The 106 `.prx` files under `USRDIR/stage` are plain ELF but are not lifted
   or mapped. `sceKernelLoadModuleByID` currently returns an id with no exports, so the first call
   into one dereferences null. They import only IoFileMgr, sceHttp, sceMpeg, sceNet*, sceNetAdhoc*,
   sceNetApctl, sceNetInet, sceParseUri, sceRtc, sceSuspendForUser, sceUsb*, sceUtility and
   ThreadMan. The fix is to lift them (as `--helper` modules or a separate overlay set), identify
   which one a given disc window holds, map it at a base and run its start entry.
2. **Audio.** `sceSasCore` and `sceAtrac3plus`, in progress elsewhere.
3. **Triangles.** The 20 draw commands so far rasterise nothing; likely clear or state setup, to
   be checked with `--ge-dump` once more of the game runs.
4. **Network imports** (sceNet*, sceHttp) used by the stage modules will need stubs that report
   "no connection".
