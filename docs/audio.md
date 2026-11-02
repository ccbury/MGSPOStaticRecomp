# Audio

The main module has no audio imports of its own. Everything goes through `sound2.prx`, which imports
`sceAudio`, `sceSasCore` and `sceAtrac3plus`. The game mixes with SAS and ATRAC itself and then pushes the
result out with `sceAudioOutput*`, so the HLE only has to produce correct sample data.

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

ATRAC3+ (wave format tag 0xFFFE) is recognised, so setting the data returns an id, but it decodes to silence.
A real ATRAC3+ decoder is the main piece of audio work left. A small ATRAC3+ blob does appear next to
the ATRAC3 data in some BGM containers.

## Tests

`mgspo-audio-tests` covers the ADPCM decoder, envelopes, voice mixing and end flags, the ATRAC RIFF parser
and the HLE entry points. No game data is used.
