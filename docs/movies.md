# Movie playback (sceMpeg)

The game only imports `sceMpeg` (no sceMpegbase, sceVideocodec or sceAudiocodec). The HLE lives in
`runtime/src/mpeg.cpp`.

## How it works

- The game builds a ring buffer in guest memory (`sceMpegRingbufferConstruct`) and refills it from
  disc with a guest callback. `sceMpegRingbufferPut` calls that callback and feeds the freshly
  written packets to a demuxer; `sceMpegRingbufferAvailableSize` reports free packets, which is
  what makes the game keep streaming. A callback result of 0 marks the end of the stream and the
  last access unit is flushed.
- `runtime/src/mpeg_demux.cpp` splits the PSMF's MPEG-2 program stream into H.264 access units
  (one per PES packet that carries a PTS) and ATRAC3+ frames. It is plain data in/data out and is
  covered by `tests/test_mpeg.cpp` with synthetic streams.
- `sceMpegGetAvcAu` / `sceMpegGetAtracAu` hand out units with PSP-style timestamps.
  `sceMpegAvcDecode` decodes the pending unit and writes RGBA8888 (or RGB565, see
  `sceMpegAvcDecodeMode`) into the buffer the game passes in, using the frame width it asks for.
- NIDs: `0xB5F6DC87` is `sceMpegRingbufferAvailableSize` and `0xB240A59E` is
  `sceMpegRingbufferPut`.

## Video decoder dependency

On Windows the H.264 decode uses the stock Media Foundation decoder MFT (`CLSID_CMSH264DecoderMFT`,
`runtime/src/video_decoder_mf.cpp`), linked against `mfplat`, `mfuuid` and `ole32`. Nothing is
bundled or statically linked, so there is no extra licence to carry; the codec is the one shipped
with Windows (it is missing on the "N" editions without the Media Feature Pack). If it cannot be
created the game still runs and the movie is black. Other platforms build
`video_decoder_null.cpp`, which never produces a picture. Swapping in FFmpeg/libav later only means
implementing `mpeg::VideoDecoder`.

## Checking the picture

On current main the movies decode cleanly: the intro movie shows correctly in-game (`--dump-frame`), and
the other PSMFs on the disc (`s01_01`, `s14_04`, `oce_tel`) decode one picture per access unit when
run through the demuxer and decoder outside the game. The `MF_MT_DEFAULT_STRIDE` of 480 is the plain
width and the output buffer is exactly `w*h*3/2`. The 16-pixel colour bands seen earlier came from the
stage's own texture handling, not the AVC decode. The NV12 to RGBA step lives in `video_convert.h` (unit
tested), and the chroma plane offset is derived from the buffer size rather than assuming a 16 row
alignment, so decoders that pad the luma plane still convert correctly.

## Not done

- Movie audio is decoded with the ATRAC3+ decoder (see [audio.md](audio.md)); only stereo/mono streams.
- No frame pacing in the HLE; playback speed depends on the game's own timing (not measured yet).
- Decoder latency is not drained at end of stream, so the last few pictures may be dropped.
