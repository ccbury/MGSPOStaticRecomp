# Audio

The main module has no audio imports of its own. Everything goes through `sound2.prx`, which imports
`sceAudio`, `sceSasCore` and `sceAtrac3plus`. The game mixes with SAS and ATRAC itself and then pushes the
result out with `sceAudioOutput*`, so the HLE only has to produce correct sample data.

Cross-module calls to main-module import stubs must resolve the import before a lifted function at
the same address. Some import placeholders are also present as empty generated functions. Selecting
those first bypassed `sound2`'s streamed-PCM configuration/output exports: movie audio decoded into
nonzero PCM but never reached `sceAudio`. Runtime dispatch now indexes the eight-byte import ranges
and resolves them first, including delay-slot calls, without changing generated stages or codec output.

Async file-open completion also returns the opened descriptor rather than a generic zero success
value, so callers can use the completed result for subsequent reads. This has a synthetic
open/read/poll/wait/EOF regression; it does not by itself resolve the comic loader issue below.

The measured Software 1x title/movie sequence previously submitted only zero PCM. With corrected
dispatch, the matching 44.791-second capture has RMS 3309; title playback between 20 and 40 seconds
is over 99.9% nonzero. These are signal/routing checks, not a listening test or proof of exact
audio/video synchronization. Codec output was not changed.

Remaining reproduced limitations: an unskipped opening comic stalls around 35 seconds while
`fdemo_sound_Load` retries reads using descriptor -1. Its preceding file lookup requests
`oss_bgm_bgm01_01_03.sep`, absent from this image's directory (the voice segment exists).
The async-open completion fix does not prevent this; the incorrect request/loader state still
needs investigation rather than substituting another music file or suppressing the error.
Separately, a prison scene at about 17 flips/s underfeeds the active mixer by about 18% in late
intervals with no queue-cap drops. Slowdown-related starvation is not fixed by import routing.

## Device-paced host buffering

The output HLE formerly rebased each channel's next-buffer deadline to the
current wall clock after a late submission. The game supplies 256-frame
blocks (5.8 ms), while Windows pulls 1,024-frame chunks. Synchronous GE work
can prevent the guest sound thread from running for tens of milliseconds.
Rebasing its deadline on each return prevents the producer from replenishing
lost queue depth, even when the game meets its requested frame cadence.
The host then inserts silence between otherwise valid PCM blocks; this is
separate from codec decoding and the comic bank-loading failure.

With an active audio device, blocking output now follows actual FIFO
consumption, yielding the guest CPU while more than 2,048 output frames
remain. This permits approximately 46 ms of host-side lead, plus the
currently submitted block, to absorb short rendering/scheduling gaps.
Windows still has its existing four 1,024-frame device buffers. This is
deliberate additional host buffering/latency, not cycle-exact PSP audio
hardware emulation or proof of retail-perfect audio/video synchronization.
Sample order, sample rate, volume conversion, codec output and simulation
timing are unchanged. Null-buffer blocking calls drain the channel; remaining
length queries report the unconsumed FIFO when a device is active.

Without a device (including ordinary headless runs), the original clock
pacing remains, without retaining samples that cannot be consumed. A channel
reset/re-reservation cancels an outstanding device wait. A device that fails
to drain for one second returns an explicit logged error rather than hanging
forever. Each FIFO is bounded to 22,050 stereo output frames (88,200 payload
bytes, excluding container overhead). Oversized blocks and full queues now
return a logged error instead of silently reporting a dropped block as
successful; blocks exceeding 65,536 input samples are also rejected before
byte-size arithmetic.

### Measured starvation and cadence

Sequential Software 1x runs used two raster threads, isolated saves, mixed
PCM capture, five-second screenshots and no sampling profiler. The same
finite Start/Circle script through 33.333 seconds, followed by Cross-only at
40 seconds, produced the image-confirmed prison cell with no later input.
An independent 90-second unskipped-comic comparison used inputs only through
21.333 seconds. Comic verification combined this change locally with the
separate [async completion fix](https://github.com/ccbury/MGSPOStaticRecomp/pull/39);
that fix is necessary for progression and is not duplicated here.

| Active channel 3, matched mixed-frame interval | Clock-paced baseline | Device-paced |
| --- | --- | --- |
| Cell, approximately 46-55 seconds: missing / 405,504 frames | 125,952 (31.06%) | 768 (0.19%) |
| Comic, approximately 76-86 seconds: missing / 450,560 frames | 211,456 (46.93%) | 0 |
| Comic cumulative missing through about 89 seconds, including startup | 494,848 | 2,560 |

No queue-cap drops or output errors occurred. The final standalone cell
run's last profile window was 20.0 flips/s, its previously measured
three-vblank target, with 680.3 GE ms per wall second. The matching baseline
was also 20.0 flips/s with 672.2 GE ms/s: this is an audio-continuity fix, not
a prison-rendering speedup. The final comic window was 29.7 flips/s versus
28.2 in the earlier run, but this single pair does not isolate a general
performance improvement.

Local PCM checks corroborate the missing-frame counts: cell 45-55 second
nonzero samples increased from 68.88% to 99.71%; comic 75-85 seconds from
56.42% to 99.98%. Silence may also be intentional, so these percentages alone
are not a correctness metric. The title 20-40 second capture was 99.99%
nonzero with RMS 3941 and no clipped samples, and its later windows remained
30.0 flips/s. Comic 35-45 seconds still contained about 0.037% clipped mixed
samples in both runs. These are captured-output/statistical checks, not a
listening comparison with a retail PSP. Long stalls can still underrun this
bounded queue; the final comic capture retained a small number of late
missing frames.

Thirteen selected audio/runtime/controller/GE/raster/profile tests passed.
Synthetic checks cover ordered stereo accumulation, empty/partial consumption,
1,024-frame pulls, the exact producer lead boundary, overflow-safe capacity
checks, headless output beyond the FIFO capacity and explicit oversized-block
failure. Builds were serialized with `--parallel 1` and reused generated-stage
objects. The final cell, comic and title runs exited normally, each below
95 MB private bytes with at least 16.8 GB physical and 24.7 GB commit headroom.
The ISO was read-only; shared outputs/saves were untouched, and captures
remain local.

## Local output diagnostics

On Windows, set `MGSPO_AUDIO_DUMP_RAW` to a local output file to capture the samples submitted to
waveOut: stereo signed 16-bit little-endian PCM at 44100 Hz. Capture is optional, flushed per host
block, bounded to 64 MiB, and stops with an explicit diagnostic on a write failure or the size limit.
It also enables host audio in `--headless` mode; a working Windows audio device is still required.
It records the mixed submission, not the physical speaker/driver output, and adds file I/O overhead.
Keep captures local: they can contain game audio and must not be committed.

`MGSPO_AUDIO_STATS=1` reports each active channel's queued, missing and dropped frame counts,
pending frames, source rate/block size, supplied volume and peak source magnitude once per mixed
second. Missing frames include startup and intentionally idle periods after a channel first submits;
interpret interval deltas in context rather than treating the cumulative count as proof of a glitch.
Neither switch changes guest timing or resamples the simulation to conceal slow rendering.

## Imports used by sound2

| Library | Functions |
|---|---|
| sceAudio | OutputBlocking, OutputPannedBlocking, OutputPanned, ChReserve, ChRelease, ChangeChannelConfig, ChangeChannelVolume, SetChannelDataLen, GetChannelRestLength |
| sceSasCore | Init, Core, CoreWithMix, SetVoice, SetVolume, SetPitch, SetADSR, SetADSRmode, SetSimpleADSR, SetSL, SetNoise, SetKeyOn, SetKeyOff, SetPause, GetPauseFlag, GetEndFlag, GetEnvelopeHeight, RevType, RevParam, RevEVOL, RevVON |
| sceAtrac3plus | Reinit, SetDataAndGetID, ReleaseAtracID, DecodeData, ResetPlayPosition, SetLoopNum, GetSoundSample, GetMaxSample, GetLoopStatus |

## SAS (`sas_core.cpp`, `sas.cpp`)

`sas_core` is a plain C++ model with no guest dependency beyond reading sample data from guest memory:

- up to 32 voices, each ADPCM (VAG), PCM or noise
- ADSR envelope with the linear/bent/exponential curve modes and the `SetSimpleADSR` bitfields
- pitch resampling (0x1000 is 1.0, up to 0x4000) with linear interpolation
- per voice dry and effect volumes, mixed or raw (four plane) output
- a comb based reverb with the preset types and `RevParam` delay/feedback

`sas.cpp` maps the `sceSasCore` NIDs onto it. Each core is keyed by the guest core address.

Not implemented: ATRAC voices on SAS, triangle/steep waves, and exact hardware reverb. The reverb is an
approximation. Nothing models the real time the hardware takes per grain.

## ATRAC (`atrac.cpp`, `atrac3.cpp`)

The game's music is stored as `.at3` style RIFF blobs. `atrac3.cpp` parses the RIFF header and decodes
ATRAC3 (the 66/105/132 kbps modes, mono, stereo and joint stereo) natively: spectrum and tonal component
decode, gain control, IMDCT, and the three stage QMF synthesis. The decoder was checked against an
independent decoder on a real stream and the output matched sample for sample (apart from rounding).

`atrac.cpp` implements the id based API on top: loop counts, end sample, reset position and the loop status
queries. Streams are expected to be fully resident; the streaming functions (`AddStreamData` and friends)
are not provided because the game does not call them.

`sceAtracSetLoopNum` rejects streams without loop metadata with `0x80630021`
(no loop information), including requests for zero repeats, and leaves their loop count at zero.
This matters beyond audio playback: `sound2` requests infinite looping even for non-looped voice
clips, then uses the reported loop count to choose its end-of-stream mixing path. Accepting that
request could leave its high-priority mixer spinning without consuming samples after a short final
grain, starving the main thread on a black screen when skipping the prison-door conversation.
Finite and infinite looping remain supported for streams with loop metadata.

ATRAC3+ (wave format tag 0xFFFE) goes through `atrac3plus.cpp`, a mono/stereo decoder that produces 2048 samples
per frame. The bitstream parser (`atrac3p/bitstream.cpp`), the synthesis DSP (`atrac3p/dsp.cpp`) and the
Huffman/spectral tables are taken from FFmpeg's libavcodec ATRAC3+ decoder (Maxim Poliakovski, LGPL-2.1-or-later).
The format has far too many code tables to reasonably retype, so that code is vendored with only include and
helper changes; `atrac3p/compat.{h,cpp}` supplies the few FFmpeg helpers it needs (bit reader, VLC tables,
sine windows, a plain IMDCT). See `runtime/src/atrac3p/README.md` for provenance and `COPYING.LGPL-2.1` for
the licence. `atrac3plus.cpp` is our own front end: frame decode, spectrum reconstruction, and float to
int16 conversion. A corrupt frame returns false and produces silence. Limitations: no extension channel
units (more than 2 channels) and no GHA amplitude mode 0, same as the FFmpeg decoder.

The decoder is used by `sceAtracDecodeData` (including seeks, which re-run the previous frames to rebuild
the overlap state) and by `sceMpegAtracDecode`, which decodes the ATRAC3+ access units demuxed from PSMF
movies. Frames are 744 bytes in the movies I looked at.

### Verification

Decoding `SND0.AT3` (181 frames, stereo) and the audio of a movie (300 access units, wrapped in an at3
container) gave output within 1 LSB of ffmpeg's `atrac3plus` decoder over the whole stream. All access units
in the movies tried decode without errors. `mgspo-at3dec in.at3 out.wav` decodes a file for comparison.
The audio test also does this check when `MGSPO_AT3_FILE` (an .at3) and `MGSPO_AT3_REF` (a WAV from another
decoder) are set; without them only the synthetic cases run, so no game data is needed or committed.

## Tests

`mgspo-audio-tests` covers the ADPCM decoder, envelopes, voice mixing and end flags, the ATRAC RIFF parser,
ATRAC3+ error handling and the HLE entry points. No game data is used unless the environment variables above
are set.

The loop HLE regression builds synthetic RIFF streams from terminator-only ATRAC3+ frames. It checks
rejected negative/zero/positive loop requests on a non-looped stream, unchanged loop status, a
three-sample final partial frame and EOF, reset, finite loop-count exhaustion, and infinite looping
followed by disabling the loop. The regression fails against the old unconditional loop setter.
