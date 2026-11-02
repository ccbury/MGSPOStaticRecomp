# Notes on the main module

Findings from running `mgspo-extract` on the US release (disc ID `ULUS10202`). Everything below can
be regenerated from your own image with `mgspo-extract info|ls|analyze`.

## Disc

- Plain ISO9660, 637 entries, empty volume id.
- `UMD_DATA.BIN` at the root (48 bytes), everything else under `PSP_GAME/`.
- `PARAM.SFO`: `DISC_ID=ULUS10202`, `TITLE=METAL GEAR SOLID PORTABLE OPS`, `CATEGORY=UG`,
  `DISC_VERSION=1.02`, `PSP_SYSTEM_VER=2.81`, `REGION=0x8000`, `PARENTAL_LEVEL=9`, `BOOTABLE=1`.

```
PSP_GAME/
  ICON0.PNG ICON1.PMF PIC0.PNG PIC1.PNG SND0.AT3 PARAM.SFO
  SYSDIR/
    EBOOT.BIN         2693760 bytes, ~PSP container
    BOOT.BIN          2693412 bytes, plain PRX (same module)
    UPDATE/           firmware updater (DATA.BIN, EBOOT.BIN, PARAM.SFO)
  USRDIR/
    codec.dat, slot.dat (~200 MB archive)
    modules/          25 small helper PRXs (kjfs, sound2, zlibdec, libsha*, libmp3, ...)
    mpg/              PMF cutscene videos
    oss/              .sep audio
    stage/            ~105 stage directories (212 files, mostly .prx modules and _zar archives)
```

The per-stage `.prx` files are separate modules, so the game loads code overlays at run time in
addition to the main module. The recompiler will need to handle those too.

## EBOOT.BIN vs BOOT.BIN

`EBOOT.BIN` starts with `~PSP` (module `mgp_main`, 2 segments, attr 0x0000, comp 0x0000, elf size
2693412, psp size 2693760, boot entry 0x128, modinfo offset 0x1D7130). It is encrypted and we do not
decrypt it.

`SYSDIR/BOOT.BIN` has exactly the `elf size` from that header and parses as an ELF with the same
entry point and module info, so the disc carries an unencrypted copy of the module. A separately
supplied decrypted dump of the same module also parsed identically. That makes `BOOT.BIN` the input
for the recompiler.

## ELF layout

- Type `0xFFA0` (PRX, relocatable), machine MIPS, flags `0x10A23001`.
- Entry point `0x00000128`, module name `mgp_main`, version `0x0101`, `gp = 0x00015150`.
- Segments:

| # | file off | vaddr      | filesz   | memsz    | flags |
|---|----------|------------|----------|----------|-------|
| 0 | 0x0000C0 | 0x00000000 | 0x1EED14 | 0x1EED14 | RWX   |
| 1 | 0x1EEE00 | 0x001EED40 | 0x00D154 | 0x108A80 | RW    |
| 2 | relocs   |            | 0x095298 |          | PSP relocation segment (type 0x700000A0) |

- Main sections: `.text` at 0x40 (size 0x1D6828), `.rodata` at 0x1D7730 (0x175E4), `.data` at
  0x1EED40 (0xD154), `.bss` at 0x1FBEC0 (0xFB900), plus `.sceStub.text`, `.lib.stub`, `.lib.ent`,
  `.rodata.sceModuleInfo`, `.rodata.sceResident`, `.rodata.sceNid`, `.ctors`/`.dtors`, `.eh_frame`.
- Code is about 1.9 MB (roughly 480k instructions). Relocation sections (`.rel.*`, type
  0x700000A0) are present for every region, so the loader has real fixups to apply.
- The module exports one anonymous library: 2 functions and 3 variables.

## Imports

32 libraries, 240 functions, each imported by NID through 8-byte stubs in `.sceStub.text` (the
jump/delay-slot pair that the runtime has to resolve).

| Library | Funcs | Library | Funcs |
|---|---|---|---|
| ThreadManForUser | 22 | sceNetInet | 14 |
| sceUtility | 13 | sceGe_user | 11 |
| sceNetAdhocctl | 11 | sceLibFont | 8 |
| IoFileMgrForUser | 7 | ModuleMgrForUser | 7 |
| UtilsForUser | 7 | sceNetAdhoc | 7 |
| sceNetApctl | 7 | sceHttp | 7 |
| SysMemUserForUser | 6 | sceDisplay | 5 |
| sceNet | 5 | sceNetResolver | 5 |
| sceRtc | 3 | InterruptManager | 3 |
| sceSuspendForUser | 3 | StdioForUser | 3 |
| sceCtrl | 2 | sceWlanDrv | 2 |
| sceUmdUser | 2 | sceSsl | 2 |
| Kernel_Library | 2 | LoadExecForUser | 2 |
| scePower | 1 | sceImpose | 1 |
| sceOpenPSID | 1 | | |

Three more libraries are not firmware: `kjfs` (20), `sound2` (48) and `zlibdec` (3). They match
the helper modules in `USRDIR/modules/` that the game loads itself, so their NIDs resolve to those
PRXs rather than to the runtime we write.

### What this means for the runtime

- Graphics goes through `sceGe_user` (display list enqueue/sync/stall, callbacks) and
  `sceDisplay`; there is no direct VRAM-only path to avoid, so a GE command interpreter is needed.
- Input is only `sceCtrl` (2 calls) and power/RTC are tiny, so those are cheap.
- Audio is not in the firmware imports at all: it goes through the bundled `sound2` module, which
  itself will import the audio syscalls. Decoding the helper PRXs is a separate step.
- Threading is the biggest firmware surface (`ThreadManForUser`: 22 calls) plus file IO, memory
  and module management.
- Networking (`sceNet*`, `sceHttp`, `sceSsl`, `sceWlanDrv`) is only needed for the multiplayer
  and online features and can initially be stubbed to return errors.

The full NID list per library is printed by `mgspo-extract analyze`. NIDs are not resolved to
function names yet; a name table (from public PSP SDK documentation) is the next addition.

## Open items

- `PSP_GAME/USRDIR/modules/*.prx` and the per-stage `.prx` files need the same analysis.
- The `_zar` archives and `slot.dat` formats have not been looked at.
- The `~PSP` header's `bss` field (offset 0x38) reads as a negative number and is not trusted.
