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
open/read/poll/wait/EOF regression.

The measured Software 1x title/movie sequence previously submitted only zero PCM. With corrected
dispatch, the matching 44.791-second capture has RMS 3309; title playback between 20 and 40 seconds
is over 99.9% nonzero. These are signal/routing checks, not a listening test or proof of exact
audio/video synchronization. Codec output was not changed.

## Comic prefetch and asynchronous reads

The original comic script deliberately repeats BGM IDs across adjacent segments. Segments 2 and 3
share ID 211 and a 1,116,160-byte buffer, but only segment 2's BGM filename is present on disc.
Segments 4 and 5 likewise share ID 212. Segment 3 is supposed to reuse the active segment 2 buffer;
it is not supposed to open an invented or substituted asset.

A bounded cache trace exposed the failure: the higher-priority prefetch loader completed every
`sceIoReadAsync` synchronously, including its first poll. It reached segment 3 before the main thread
could activate segment 2's double-buffered sound bank. ID 211 was still in the inactive slot, so the
loader allocated again and attempted the absent segment 3 filename, then retried descriptor -1.
Restoring pending read completion gives the guest's existing poll/backoff or blocking-wait path a
chance to run the bank activation. The original shared-ID lookup then reuses the buffer.

Read completion now has a fixed 1 ms guest-clock command latency. Host file reads still execute
synchronously; this is a bounded completion/scheduling approximation, **not** a measured PSP UMD
seek/throughput model or a background host I/O worker. Polling while pending returns 1 without
touching the result pointer; blocking waits release the guest CPU token until completion is due.
Only one unconsumed operation is allowed per descriptor, including completed-but-uncollected results.
Invalid descriptors and overlapping operations return PSP errors without creating or replacing
completion records. `sceIoGetAsyncStat(fd, poll, result)` uses its three-argument ABI, and consumed
results return the correct `0x8002032A` no-async error.

The final Software 1x/two-worker run progressed through changing comic panels and dialogue from
40 through 85 seconds, beyond the previous roughly 35-second stall, with no file-open failures.
Local mixed PCM from 35-45 and 55-65 seconds had RMS 4315 and 2369 respectively; this proves output
is present, not that every voice/music mix, clipping event or A/V synchronization detail is exact.
Comic windows generally reached about 30 flips/s, falling to 28.2 in the final heavy window.
The 90.228-second monitored run peaked at 93,986,816 private bytes with at least 17,502,732,288
physical and 25,120,137,216 commit bytes free. Temporary game-specific tracing was removed;
no generated stages or game assets are part of the fix.

Remaining reproduced limitations: a prison scene at about 17 flips/s underfeeds the active mixer by about 18% in late
intervals with no queue-cap drops. Slowdown-related starvation is not fixed by import routing.
Late heavy comic panels also underfeed the mixer; the cache fix does not eliminate general
software-renderer slowdown or certify audio against a retail listening/synchronization reference.

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

`mgspo-runtime-tests` exercises pending poll output preservation, scheduler-aware waits with a
higher-priority reader and lower-priority cache owner, callback waits, the three-argument async-stat
ABI, descriptor-valued open completion, EOF, duplicate submission, sync access while busy,
completion consumption, async close, and invalid-descriptor rejection.

`mgspo-audio-tests` covers the ADPCM decoder, envelopes, voice mixing and end flags, the ATRAC RIFF parser,
ATRAC3+ error handling and the HLE entry points. No game data is used unless the environment variables above
are set.

The loop HLE regression builds synthetic RIFF streams from terminator-only ATRAC3+ frames. It checks
rejected negative/zero/positive loop requests on a non-looped stream, unchanged loop status, a
three-sample final partial frame and EOF, reset, finite loop-count exhaustion, and infinite looping
followed by disabling the loop. The regression fails against the old unconditional loop setter.
