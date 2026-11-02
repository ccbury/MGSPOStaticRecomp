// sceMpeg: PSMF movie playback. The game owns a ring buffer in guest memory and refills it through a
// guest callback; we demux what arrives, hand out access units, and decode video into the buffer the
// game passes to sceMpegAvcDecode.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

#include "kernel_internal.h"
#include "mpeg_demux.h"
#include "video_decoder.h"

namespace psp {
namespace k {

namespace {

constexpr uint32_t kNoData = 0x80618001;
constexpr uint32_t kInvalidValue = 0x806101FE;
constexpr uint32_t kPacketSize = 2048;
constexpr uint32_t kAtracEsSize = 2112;
constexpr uint32_t kAtracOutSize = 8192;

// Guest ring buffer layout.
enum RbField : uint32_t {
  kRbPackets = 0,
  kRbRead = 4,
  kRbWritePos = 8,
  kRbAvail = 12,
  kRbPacketSize = 16,
  kRbData = 20,
  kRbCallback = 24,
  kRbCallbackArg = 28,
  kRbUpper = 32,
  kRbSema = 36,
  kRbMpeg = 40,
  kRbGp = 44,
};

struct Context {
  uint32_t ring = 0;
  uint32_t nextStream = 1;
  mpeg::Demuxer demux;
  bool eof = false;
  int width = 480, height = 272;
  int pixelFormat = 3;
  std::unique_ptr<mpeg::VideoDecoder> decoder;
  mpeg::Au video;
  bool haveVideo = false;
  bool gotPicture = false;
  mpeg::Picture last;
  uint32_t videoAus = 0, audioAus = 0, pictures = 0;
  int64_t firstPts = -1, lastPts = -1, audioPts = -1;
  uint64_t startUs = 0;
};

std::map<uint32_t, Context> g_ctx;

uint32_t be32(uint32_t a) {
  return (uint32_t(lbu(a)) << 24) | (uint32_t(lbu(a + 1)) << 16) | (uint32_t(lbu(a + 2)) << 8) | lbu(a + 3);
}

Context* ctx_of(uint32_t mpegPtr) {
  auto it = g_ctx.find(lw(mpegPtr));
  return it == g_ctx.end() ? nullptr : &it->second;
}

Context* ctx_of_ring(uint32_t rb) {
  uint32_t mp = lw(rb + kRbMpeg);
  return mp ? ctx_of(mp) : nullptr;
}

uint32_t queued_packets(Context& x, uint32_t cap) {
  uint32_t n = static_cast<uint32_t>((x.demux.pending_bytes() + kPacketSize - 1) / kPacketSize);
  return std::min(n, cap);
}

void sync_ring(Context& x) {
  if (x.ring) sw(x.ring + kRbAvail, queued_packets(x, lw(x.ring + kRbPackets)));
}

void put_ts(uint32_t addr, int64_t v) {
  sw(addr, static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32));
  sw(addr + 4, static_cast<uint32_t>(v));
}

// The game syncs picture timing to the audio it decodes. With no audio device to block on, the
// decode call itself has to wait until the stream's clock reaches the unit's timestamp.
void pace(Cpu& c, Context& x, int64_t pts) {
  if (pts < 0) return;
  if (x.firstPts < 0 || pts < x.lastPts) {
    x.firstPts = pts;
    x.startUs = now_us();
  }
  x.lastPts = pts;
  uint64_t due = x.startUs + static_cast<uint64_t>(pts - x.firstPts) * 1000000ull / 90000ull;
  if (due > now_us()) sleep_until(c, due, false);
}

void h_zero(Cpu& c) { ret(c, 0); }

void h_query_mem(Cpu& c) { ret(c, 0x10000); }

void h_rb_query_mem(Cpu& c) { ret(c, arg(c, 0) * (kPacketSize + 104)); }

void h_rb_construct(Cpu& c) {
  uint32_t rb = arg(c, 0), packets = arg(c, 1), data = arg(c, 2), size = arg(c, 3);
  if (size < packets * kPacketSize) return ret(c, 0x806102FE);
  sw(rb + kRbPackets, packets);
  sw(rb + kRbRead, 0);
  sw(rb + kRbWritePos, 0);
  sw(rb + kRbAvail, 0);
  sw(rb + kRbPacketSize, kPacketSize);
  sw(rb + kRbData, data);
  sw(rb + kRbCallback, arg(c, 4));
  sw(rb + kRbCallbackArg, arg(c, 5));
  sw(rb + kRbUpper, data + packets * kPacketSize);
  sw(rb + kRbSema, 0);
  sw(rb + kRbMpeg, 0);
  ret(c, 0);
}

void h_rb_available(Cpu& c) {
  uint32_t rb = arg(c, 0);
  Context* x = ctx_of_ring(rb);
  uint32_t packets = lw(rb + kRbPackets);
  uint32_t used = x ? queued_packets(*x, packets) : lw(rb + kRbAvail);
  ret(c, packets - std::min(used, packets));
}

void h_rb_put(Cpu& c) {
  uint32_t rb = arg(c, 0), want = arg(c, 1), avail = arg(c, 2);
  Context* x = ctx_of_ring(rb);
  if (!x) return ret(c, kInvalidValue);
  uint32_t packets = lw(rb + kRbPackets);
  uint32_t used = queued_packets(*x, packets);
  uint32_t n = std::min({want, avail, packets - std::min(used, packets)});
  uint32_t total = 0;
  bool ended = false;
  while (n > 0) {
    uint32_t pos = lw(rb + kRbWritePos) % packets;
    uint32_t chunk = std::min(n, packets - pos);
    uint32_t dest = lw(rb + kRbData) + pos * kPacketSize;
    int32_t got = static_cast<int32_t>(call_guest(c, lw(rb + kRbCallback), dest, chunk, lw(rb + kRbCallbackArg)));
    if (got <= 0) {
      ended = true;
      break;
    }
    got = std::min<int32_t>(got, static_cast<int32_t>(chunk));
    x->demux.feed(ptr(dest), static_cast<size_t>(got) * kPacketSize);
    sw(rb + kRbWritePos, lw(rb + kRbWritePos) + got);
    sw(rb + kRbRead, lw(rb + kRbRead) + got);
    total += got;
    n -= got;
    if (static_cast<uint32_t>(got) < chunk) break;
  }
  if (ended && total == 0 && !x->eof) {
    x->eof = true;
    x->demux.finish();
  }
  sync_ring(*x);
  ret(c, total);
}

void h_create(Cpu& c) {
  uint32_t mpegPtr = arg(c, 0), data = arg(c, 1), rb = arg(c, 3);
  uint32_t handle = data + 0x30;
  sw(mpegPtr, handle);
  Context& x = g_ctx[handle];
  x = Context();
  x.ring = rb;
  if (rb) sw(rb + kRbMpeg, mpegPtr);
  ret(c, 0);
}

void h_delete(Cpu& c) {
  g_ctx.erase(lw(arg(c, 0)));
  ret(c, 0);
}

void h_rb_destruct(Cpu& c) {
  for (auto& kv : g_ctx)
    if (kv.second.ring == arg(c, 0)) kv.second.ring = 0;
  ret(c, 0);
}

void h_regist_stream(Cpu& c) {
  Context* x = ctx_of(arg(c, 0));
  ret(c, x ? x->nextStream++ : 0);
}

void h_malloc_es(Cpu& c) { ret(c, 1); }

void h_init_au(Cpu& c) {
  uint32_t au = arg(c, 2);
  put_ts(au, -1);
  put_ts(au + 8, -1);
  sw(au + 16, arg(c, 1));
  sw(au + 20, kPacketSize);
  ret(c, 0);
}

void h_atrac_es_size(Cpu& c) {
  put32(arg(c, 1), kAtracEsSize);
  put32(arg(c, 2), kAtracOutSize);
  ret(c, 0);
}

void h_stream_offset(Cpu& c) {
  uint32_t hdr = arg(c, 1);
  if (memcmp(ptr(hdr), "PSMF", 4) != 0) return ret(c, kInvalidValue);
  uint32_t off = be32(hdr + 8);
  if (off & (kPacketSize - 1) || off == 0) return ret(c, kInvalidValue);
  if (Context* x = ctx_of(arg(c, 0))) {
    mpeg::PsmfInfo info = mpeg::parse_psmf_header(ptr(hdr), 0x90);
    if (info.valid && info.width && info.height) {
      x->width = info.width;
      x->height = info.height;
    }
  }
  put32(arg(c, 2), off);
  ret(c, 0);
}

void h_stream_size(Cpu& c) {
  uint32_t hdr = arg(c, 0);
  if (memcmp(ptr(hdr), "PSMF", 4) != 0) return ret(c, kInvalidValue);
  put32(arg(c, 1), be32(hdr + 12));
  ret(c, 0);
}

void h_get_avc_au(Cpu& c) {
  Context* x = ctx_of(arg(c, 0));
  if (!x) return ret(c, kInvalidValue);
  mpeg::Au au;
  if (!x->demux.pop_video(au)) return ret(c, kNoData);
  uint32_t out = arg(c, 2);
  put_ts(out, au.pts);
  put_ts(out + 8, au.dts);
  sw(out + 16, arg(c, 1));
  sw(out + 20, kPacketSize);
  put32(arg(c, 3), 1);
  x->video = std::move(au);
  x->haveVideo = true;
  ++x->videoAus;
  if (std::getenv("MGSPO_MPEG_DEBUG") && x->videoAus % 30 == 0)
    std::fprintf(stderr, "mpeg video au %u pts=%lld t=%llums\n", x->videoAus, (long long)x->video.pts, (unsigned long long)(now_us() / 1000));
  sync_ring(*x);
  ret(c, 0);
}

void h_get_atrac_au(Cpu& c) {
  Context* x = ctx_of(arg(c, 0));
  if (!x) return ret(c, kInvalidValue);
  mpeg::Au au;
  if (!x->demux.pop_audio(au)) return ret(c, kNoData);
  uint32_t out = arg(c, 2);
  put_ts(out, au.pts);
  put_ts(out + 8, -1);
  sw(out + 16, arg(c, 1));
  sw(out + 20, kAtracEsSize);
  put32(arg(c, 3), 0);
  x->audioPts = au.pts;
  ++x->audioAus;
  if (std::getenv("MGSPO_MPEG_DEBUG") && x->audioAus % 200 == 0)
    std::fprintf(stderr, "mpeg audio au %u pts=%lld t=%llums\n", x->audioAus, (long long)au.pts, (unsigned long long)(now_us() / 1000));
  sync_ring(*x);
  ret(c, 0);
}

void store_picture(const mpeg::Picture& p, uint32_t buf, uint32_t stride, int fmt) {
  for (int y = 0; y < p.height; ++y) {
    const uint8_t* s = &p.rgba[static_cast<size_t>(y) * p.width * 4];
    if (fmt == 3) {
      std::memcpy(ptr(buf + y * stride * 4), s, static_cast<size_t>(p.width) * 4);
    } else if (fmt == 0) {
      for (int xx = 0; xx < p.width; ++xx) {
        uint32_t v = (s[xx * 4] >> 3) | ((s[xx * 4 + 1] >> 2) << 5) | ((s[xx * 4 + 2] >> 3) << 11);
        sh(buf + (y * stride + xx) * 2, v);
      }
    }
  }
}

void h_avc_decode(Cpu& c) {
  Context* x = ctx_of(arg(c, 0));
  if (!x) return ret(c, kInvalidValue);
  uint32_t frameWidth = arg(c, 2) ? arg(c, 2) : 512;
  uint32_t bufPtr = arg(c, 3), initPtr = arg(c, 4);
  if (!x->haveVideo) {
    put32(initPtr, 0);
    return ret(c, kInvalidValue);
  }
  x->haveVideo = false;
  if (x->audioAus == 0) pace(c, *x, x->video.pts);  if (!x->decoder) {
    x->decoder = mpeg::create_video_decoder(x->width, x->height);
    std::fprintf(stderr, "movie: %dx%d, decoder: %s\n", x->width, x->height, x->decoder->name());
  }
  mpeg::Picture pic;
  bool got = x->decoder->decode(x->video.data.data(), x->video.data.size(), pic);
  if (got) {
    x->last = std::move(pic);
  } else if (x->last.rgba.empty()) {
    x->last.width = x->width;
    x->last.height = x->height;
    x->last.rgba.assign(static_cast<size_t>(x->width) * x->height * 4, 0);
    for (size_t i = 3; i < x->last.rgba.size(); i += 4) x->last.rgba[i] = 255;
  }
  uint32_t buf = bufPtr ? lw(bufPtr) : 0;
  if (buf) store_picture(x->last, buf, frameWidth, x->pixelFormat);
  ++x->pictures;
  put32(initPtr, 1);
  ret(c, 0);
}

void h_avc_decode_stop(Cpu& c) {
  put32(arg(c, 3), 0);
  ret(c, 0);
}

void h_avc_decode_mode(Cpu& c) {
  Context* x = ctx_of(arg(c, 0));
  uint32_t m = arg(c, 1);
  if (x && m) {
    int fmt = static_cast<int>(lw(m + 4));
    if (fmt >= 0 && fmt <= 3) x->pixelFormat = fmt;
  }
  ret(c, 0);
}

// There is no ATRAC3+ decoder yet, so movie audio plays back as silence with correct timing.
void h_atrac_decode(Cpu& c) {
  Context* x = ctx_of(arg(c, 0));
  if (x) pace(c, *x, x->audioPts);
  uint32_t buf = arg(c, 2);
  if (buf) std::memset(ptr(buf), 0, kAtracOutSize);
  ret(c, 0);
}

}  // namespace

void register_mpeg() {
  const char* m = "sceMpeg";
  register_hle(m, 0x682A619B, "sceMpegInit", h_zero);
  register_hle(m, 0x874624D6, "sceMpegFinish", h_zero);
  register_hle(m, 0xC132E22F, "sceMpegQueryMemSize", h_query_mem);
  register_hle(m, 0xD7A29F46, "sceMpegRingbufferQueryMemSize", h_rb_query_mem);
  register_hle(m, 0x37295ED8, "sceMpegRingbufferConstruct", h_rb_construct);
  register_hle(m, 0x13407F13, "sceMpegRingbufferDestruct", h_rb_destruct);
  register_hle(m, 0xB5F6DC87, "sceMpegRingbufferAvailableSize", h_rb_available);
  register_hle(m, 0xB240A59E, "sceMpegRingbufferPut", h_rb_put);
  register_hle(m, 0xD8C5F121, "sceMpegCreate", h_create);
  register_hle(m, 0x606A4649, "sceMpegDelete", h_delete);
  register_hle(m, 0x42560F23, "sceMpegRegistStream", h_regist_stream);
  register_hle(m, 0x591A4AA2, "sceMpegUnRegistStream", h_zero);
  register_hle(m, 0xA780CF7E, "sceMpegMallocAvcEsBuf", h_malloc_es);
  register_hle(m, 0xCEB870B1, "sceMpegFreeAvcEsBuf", h_zero);
  register_hle(m, 0x167AFD9E, "sceMpegInitAu", h_init_au);
  register_hle(m, 0xF8DCB679, "sceMpegQueryAtracEsSize", h_atrac_es_size);
  register_hle(m, 0x21FF80E4, "sceMpegQueryStreamOffset", h_stream_offset);
  register_hle(m, 0x611E9E11, "sceMpegQueryStreamSize", h_stream_size);
  register_hle(m, 0xFE246728, "sceMpegGetAvcAu", h_get_avc_au);
  register_hle(m, 0xE1CE83A7, "sceMpegGetAtracAu", h_get_atrac_au);
  register_hle(m, 0x0E3C2E9D, "sceMpegAvcDecode", h_avc_decode);
  register_hle(m, 0x740FCCD1, "sceMpegAvcDecodeStop", h_avc_decode_stop);
  register_hle(m, 0xA11C7026, "sceMpegAvcDecodeMode", h_avc_decode_mode);
  register_hle(m, 0x800C44DF, "sceMpegAtracDecode", h_atrac_decode);
}

}  // namespace k
}  // namespace psp
