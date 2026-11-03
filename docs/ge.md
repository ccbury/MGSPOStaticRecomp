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

## Fast rasteriser (`raster_fast.cpp`)

Triangles and sprites that use common state go through an integer span rasteriser instead of the float
reference path in `gpu_draw.cpp`. The reference path stays as the fallback and as the correctness
reference (`MGSPO_GE_REF=1` forces it).

- Vertices snap to 1/16 pixel like the hardware, coverage is solved per row, and z, colour, fog and
  texture coordinates come from plane equations (perspective divide only when the draw needs it).
- 12 templated span functions (textured or not, blended or not, depth test or not) are picked per draw,
  so the per-fragment loop has no state switches.
- Draws are queued, not drawn: `fast::sync()` renders the queue split into bands of rows on a small
  thread pool (`MGSPO_GE_THREADS`, default half the logical CPUs, total rendering threads clamped
  to 1-6 including the calling thread). Invalid environment values report a diagnostic and use one
  thread; out-of-range counts are clamped with a diagnostic. Inside a band draws keep
  their order and bands never overlap, so the output does not depend on the thread count. The queue
  is synced at the end of every list, before block transfers, before any reference-path pixel work,
  and before textures that live in VRAM or when a cached texture is replaced. It also drains
  synchronously before exceeding 1,024 draws or 4,096 primitives, preserving draw order and palettes
  rather than dropping work. Queue and cache mutation stay on the GE caller; sync waits for workers
  before clearing or reusing their data.
- Textures are decoded once into a cache keyed by address, format and size (swizzle and 16-bit formats
  to RGBA8888; CLUT8/CLUT4 to indices with the palette applied per fragment). A content hash of the
  source bytes detects rewrites. The LRU cache holds at most 96 entries and 96 MiB of decoded
  texels, accounting for an incoming entry before allocating it. Sources larger than that budget
  use the reference path.
- Not handled (falls back to the reference path): colour test, stencil, logic op, secondary colour with
  lighting, DXT and CLUT16/32 textures, and coordinates beyond +-8192. Clear-mode rectangles use a
  dedicated fill.
- The spline patch code precomputes the basis weights per grid row/column.
- Fragment counts and per-class raster time in `--ge-stats` are attributed at sync time.

`tests/test_ge.cpp` renders 128 random through-mode scenes with both paths and compares them. Edge
coverage differs by about 0.5% of pixels because the reference path uses float vertices.
`mgspo-raster-tests` is a standalone resource-boundary check with one-pixel queue/eviction cases
and a small single-thread/two-thread output comparison; it needs no generated game code.

### Profiling

`--sample [delay]` starts a sampling profiler with a requested 1 ms interval (Windows, needs debug symbols for names) and
prints the share of lifted guest code, GE, waiting and other runtime plus the top symbols when the
run ends. `--profile` and `--ge-stats` give flips/s and GE time per second.

For an optimized MSVC build with useful guest/runtime names, configure with
`-DMGSPO_PROFILE_SYMBOLS=ON` and build with `--parallel 1`. This adds `/Zi` and
linker `/DEBUG`, retaining `/OPT:REF` and `/OPT:ICF`; it does not switch Release
compilation to Debug. The option is off by default. Keep the executable and PDB
together when profiling.

`--profile` also reports observed framebuffer-address-change intervals in vblank
buckets (0, 1, 2, 3, 4, 5+), mean milliseconds between those changes, and completed
guest display-vblank wait calls with their elapsed wall time. The first flip
starts the baseline, not an interval. Multiple flips within one vblank remain in
the zero bucket; display initialization resets the interval baseline.

These are **observations, not a requested refresh rate**. Re-presenting a buffer
does not imply a new image, and a guest vblank wait's elapsed time includes any
other guest work scheduled while the caller is blocked. All-thread sampling
percentages include parked guest/raster threads, so the aggregate waiting share
is not the main thread's idle percentage. Both `NtDelayExecution` and
`ZwDelayExecution` are classified as waiting, rather than charging the latter
to active runtime work. The requested interval is not a guaranteed sampling
rate: thread enumeration/suspension and host sleep resolution add overhead.
Sampling itself can perturb scheduling.

### Cadence investigation (two rendering threads)

A controlled 140-second, fresh-save, headless run of the existing prison-stage
workload used `MGSPO_GE_THREADS=2`, legacy controls and periodic Start/Cross input.
After excluding the scene-transition window, 20 approximately five-second
gameplay windows averaged 17.4 buffer flips/s and 345.6 ms of GE work per wall
second (19.9 ms GE work per flip). Observed flip intervals were approximately
57-58 ms, mostly three or four vblanks apart. These figures exclude title/movie
windows; they are not a 60 fps target or a claim that the renderer alone limits
the frame rate.

Read-only inspection of the local lifted pacing routine found an initial
two-vblank minimum and an adaptive elapsed-vblank threshold capped at six.
Its dynamic threshold was not directly sampled, so the initial setting is not
proof that every prison frame requests exactly two vblanks. Simulation, audio,
vblank deadlines and guest pacing remain unchanged.

Two candidates were evaluated sequentially on the same scripted workload:
four-channel SIMD bilinear filtering and a constant-colour span specialization.
Both passed targeted correctness checks but neither demonstrated an end-to-end
gain (17.2/17.1 flips/s and 385.5/384.0 ms GE per wall second respectively).
Both were discarded; neither is included in the renderer. These single-run
comparisons are subject to host/scheduling variation, not statistical proof of
a regression.

A separate, symbolized 90-second run sampled the final 30 seconds in the prison
scene. It identified indexed texture sampling as the largest active symbol;
aggregate samples were 92.2% waiting, with 81.7% of non-wait samples in GE work.
That sampling run still contained the subsequently discarded constant-span
candidate and is diagnostic evidence, not an after-performance result for the
unchanged renderer.

The final diagnostic-only build, with symbols enabled and neither shader
candidate, completed the same 140-second workload. Nineteen steady gameplay
windows averaged 17.1 flips/s, 405.7 ms GE per wall second and 23.8 ms GE per flip.
This is not a speedup: symbol/link-layout differences and host/scheduling
variation were not isolated. The default build leaves symbol generation off.
The retained changes improve measurement, not raster throughput; indexed texture
sampling remains a profiling lead, not a validated optimization.

All runs used sequential single-job builds, fresh isolated saves and CPU/memory
monitoring with stop limits at 8 GiB minimum free physical/commit memory or
8 GiB owned-tree private bytes. The four 140-second runs sampled under 87 MiB
of private memory each, with over 20 GiB of physical headroom. Sampled resource
peaks can miss transients. The ISO and existing user saves were not modified.
No game assets, lifted code, or captures are part of these measurements' source
changes.

### Numbers (prison cell, same machine)

| | before | after |
|---|---|---|
| flips/s | 7.5-15 | 16-19 |
| GE time per wall second | 580-790 ms | 290-390 ms |
| ns per fragment | ~47 | ~10 |

These historical numbers do not establish that guest pacing is the dominant
limit. See the controlled cadence investigation above for the distinction
between aggregate thread waits, active GE cost and observed buffer changes.

## Debugging

- `--ge-dump <file>` (or `MGSPO_GE_DUMP`) writes every executed command with its address.
  `MGSPO_GE_DUMP_FROM=<n>` skips the dump until the n-th enqueued list, which keeps the file small
  when the interesting frame is far into a run. `MGSPO_GE_DUMP_WHEN=<file>` only dumps while that
  file exists, so you can `touch` it when the frame of interest is on screen. `MGSPO_VDUMP=1` additionally prints the first eight
  transformed vertices (position, uv, colour) after each `PRIM`.
- `--ge-stats` (or `MGSPO_GE_STATS=1`) prints list/draw/primitive counts, vertex types, texture and
  framebuffer formats and per-command counts when the run ends.

## Through-mode uv

In through mode the uv are texel coordinates whatever the vertex format, float included. The first
version only normalised the integer formats, which made every float-uv through draw (the game's comic
panels are drawn as 150 px strips with uv 0.5..149.5) sample a single clamped texel. Symptom: full-height
flat colour columns with the subtitles still correct on top. Normal (non-through) float uv are 0..1.

## Tests

`tests/test_ge.cpp` builds command lists in guest memory and checks the pixels in VRAM, plus the
sceGe imports through the HLE dispatcher.

## Known gaps

- Only validated with synthetic lists; the game has not been traced through its GE use yet.
- The cull-mode orientation is derived from the documented behaviour and is unverified on real data.
- No mipmaps (level 0 only), environment map texgen is approximate, patch prim type/facing is
  ignored, dithering is skipped and only the near plane is clipped.
- Execution is synchronous, so `sceGeBreak` cannot interrupt anything and timing is not modelled.
