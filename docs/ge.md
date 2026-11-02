# GE (graphics) runtime

The PSP's graphics engine is emulated by a command interpreter plus a software rasteriser
(`runtime/src/gpu.cpp`, `gpu_draw.cpp`). Drawing goes straight into the emulated 2 MB VRAM in the
pixel format the game selected, so 16-bit and 32-bit framebuffers, render-to-texture, CPU readback
of the framebuffer and `sceGuCopyImage` all behave like they do on hardware without any extra
translation step. `sceDisplay` then converts the display buffer to BGRA and hands it to the
presenter, which scales it to the window keeping the 480x272 aspect ratio (letterboxed).

A Direct3D 11 presenter would only replace that last blit; nothing in the GE code depends on it.

## sceGe_user (`ge.cpp`)

Lists run synchronously when they are enqueued, up to their stall address, and again whenever
`sceGeListUpdateStallAddr` moves the stall forward. Sync calls report the state a real list would be
in (completed 0, queued 1, drawing 2, stalling 3, paused 4) and wait (in short sleeps) while a list
is stalled or paused.

- `SIGNAL`/`FINISH` take effect at the following `END`. Callbacks are raised as guest calls with
  `(listid, arg, data, 0)`. Signal behaviours: continue, pause (resumed by `sceGeContinue`),
  jump/call/ret and the relative/origin variants.
- `sceGeListSync`, `sceGeDrawSync`, `sceGeListDeQueue`, `sceGeSetCallback`/`UnsetCallback`,
  `sceGeGetCmd`, `sceGeGetMtx`, `sceGeSaveContext`/`RestoreContext`, edram queries.
  `sceGeBreak` is a no-op, since nothing is ever mid-flight.

## Supported commands

Vertex types (all position/normal/uv/colour formats, weights, indices, morph), `PRIM` (points,
lines, strips, triangles, fans, sprites), `BEZIER`/`SPLINE` (with knot types), `BBOX`, jump/call/ret,
`BASE`/`OFFSET`/`ORIGIN`, world/view/projection/texgen/bone matrices, viewport, scissor, culling,
near-plane clipping, lighting (4 lights: directional/point/spot, ambient/diffuse/specular), fog,
texture mapping (uv, matrix and environment texgen), swizzled textures, CLUT4/8/16/32, DXT1/3/5,
nearest/bilinear, wrap/clamp, texture functions, alpha test, colour test, stencil, depth test and
mask, blending (all equations and fix colours), logic op, pixel masks, clear mode, and `TRXKICK`
block transfers.

## Debugging

- `--ge-dump <file>` (or `MGSPO_GE_DUMP`) writes every executed command with its address.
- `--ge-stats` (or `MGSPO_GE_STATS=1`) prints list/draw/primitive counts, vertex types, texture and
  framebuffer formats and per-command counts when the run ends.

## Tests

`tests/test_ge.cpp` builds command lists in guest memory and checks the pixels in VRAM, plus the
sceGe imports through the HLE dispatcher.

## Known gaps

- Only validated with synthetic lists; the game has not been traced through its GE use yet.
- The cull-mode orientation is derived from the documented behaviour and is unverified on real data.
- No mipmaps (level 0 only), environment map texgen is approximate, patch prim type/facing is
  ignored, dithering is skipped and only the near plane is clipped.
- Execution is synchronous, so `sceGeBreak` cannot interrupt anything and timing is not modelled.
