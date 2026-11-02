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

## Not done

- ATRAC3+ audio: the frames are demuxed and paced but not decoded (`atrac3.cpp` handles ATRAC3
  only), so `sceMpegAtracDecode` outputs silence.
- No frame pacing in the HLE; playback speed depends on the game's own timing (not measured yet).
- Decoder latency is not drained at end of stream, so the last few pictures may be dropped.
