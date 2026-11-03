// sceAtrac3plus: guest-facing API. ATRAC3 (atrac3.cpp) and ATRAC3+ (atrac3plus.cpp) streams are decoded natively.
#include <map>
#include <vector>

#include "atrac3.h"
#include "atrac3plus.h"
#include "kernel_internal.h"

namespace psp {
namespace k {

namespace {

constexpr uint32_t E_ATRAC_API_FAIL = 0x80630002;
constexpr uint32_t E_ATRAC_NO_ID = 0x80630003;
constexpr uint32_t E_ATRAC_INVALID_SIZE = 0x80630004;
constexpr uint32_t E_ATRAC_BAD_ID = 0x80630005;
constexpr uint32_t E_ATRAC_UNKNOWN_FORMAT = 0x80630006;
constexpr uint32_t E_ATRAC_NO_LOOP_INFORMATION = 0x80630021;
constexpr uint32_t E_ATRAC_ALL_DATA_DECODED = 0x80630024;
constexpr int kMaxIds = 6;
constexpr int kPlusFrameSamples = 2048;

struct Context {
  std::vector<uint8_t> file;
  atrac::At3Info info;
  atrac::Atrac3Decoder dec;
  atrac::Atrac3PlusDecoder decPlus;
  int32_t loopNum = 0;  // -1 loops forever
  uint32_t pos = 0;     // next sample to hand out
  uint32_t skipInFrame = 0;
  uint32_t frame = 0;  // next frame to decode

  uint32_t frame_samples() const { return info.codec == atrac::Codec::Atrac3Plus ? kPlusFrameSamples : 1024; }
  uint32_t total() const {
    if (info.totalSamples) return info.totalSamples;
    return info.frameCount() * frame_samples();
  }
  bool decode_into(uint32_t f, int16_t* out) {
    uint32_t off = info.dataOffset + f * info.blockAlign;
    if (off + info.blockAlign > file.size()) return false;
    if (info.codec == atrac::Codec::Atrac3Plus) {
      decPlus.decode_frame(file.data() + off, info.blockAlign, out);
      return true;
    }
    return dec.decode_frame(file.data() + off, out);
  }
  void seek(uint32_t sample) {
    uint32_t fs = frame_samples();
    pos = std::min(sample, total());
    frame = pos / fs;
    skipInFrame = pos % fs;
    // the overlap and filter state need the preceding frames
    std::vector<int16_t> tmp(size_t(fs) * info.channels);
    if (info.codec == atrac::Codec::Atrac3) {
      dec.reset();
      if (frame > 0) decode_into(frame - 1, tmp.data());
    } else if (info.codec == atrac::Codec::Atrac3Plus) {
      decPlus.reset();
      for (uint32_t f = frame >= 2 ? frame - 2 : 0; f < frame; ++f) decode_into(f, tmp.data());
    }
  }
};

std::map<int, Context> g_ctx;

Context* find(Cpu& c, int id) {
  auto it = g_ctx.find(id);
  if (it == g_ctx.end()) {
    ret(c, id < 0 || id >= kMaxIds ? E_ATRAC_BAD_ID : E_ATRAC_NO_ID);
    return nullptr;
  }
  return &it->second;
}

void put32(uint32_t addr, uint32_t v) {
  if (addr) sw(addr, v);
}

void h_set_data_get_id(Cpu& c) {
  uint32_t buf = arg(c, 0), size = arg(c, 1);
  if (!buf || size < 12) return ret(c, E_ATRAC_INVALID_SIZE);
  std::vector<uint8_t> data(ptr(buf), ptr(buf) + size);
  atrac::At3Info info;
  if (!atrac::parse_at3(data.data(), data.size(), info)) return ret(c, E_ATRAC_UNKNOWN_FORMAT);
  int id = -1;
  for (int i = 0; i < kMaxIds; ++i)
    if (!g_ctx.count(i)) {
      id = i;
      break;
    }
  if (id < 0) return ret(c, E_ATRAC_NO_ID);
  Context& x = g_ctx[id];
  x.file = std::move(data);
  x.info = info;
  if (info.codec == atrac::Codec::Atrac3 && !x.dec.init(info.channels, info.blockAlign, info.jointStereo)) {
    g_ctx.erase(id);
    return ret(c, E_ATRAC_UNKNOWN_FORMAT);
  }
  if (info.codec == atrac::Codec::Atrac3Plus && !x.decPlus.init(info.channels)) {
    g_ctx.erase(id);
    return ret(c, E_ATRAC_UNKNOWN_FORMAT);
  }
  x.loopNum = 0;
  x.seek(0);
  ret(c, static_cast<uint32_t>(id));
}

void h_release(Cpu& c) {
  int id = static_cast<int>(arg(c, 0));
  if (!find(c, id)) return;
  g_ctx.erase(id);
  ret(c, 0);
}

void h_decode(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  uint32_t out = arg(c, 1), pSamples = arg(c, 2), pFinish = arg(c, 3), pRemain = arg(c, 4);
  uint32_t fs = x->frame_samples();
  uint32_t total = x->total();
  if (x->pos >= total) {
    put32(pSamples, 0);
    put32(pFinish, 1);
    put32(pRemain, static_cast<uint32_t>(-1));
    return ret(c, E_ATRAC_ALL_DATA_DECODED);
  }
  int ch = x->info.channels;
  std::vector<int16_t> pcm(size_t(fs) * ch);
  if (!x->decode_into(x->frame, pcm.data())) {
    put32(pSamples, 0);
    put32(pFinish, 1);
    return ret(c, E_ATRAC_API_FAIL);
  }
  uint32_t first = x->skipInFrame;
  uint32_t n = fs - first;
  uint32_t limit = total;
  bool looping = x->info.hasLoop && x->loopNum != 0 && x->info.loopEnd < total;
  if (looping) limit = x->info.loopEnd + 1;
  bool wrap = false;
  if (x->pos + n >= limit) {
    n = limit - x->pos;
    wrap = looping;
  }
  if (out && n) mem_write(out, pcm.data() + size_t(first) * ch, size_t(n) * ch * 2);
  x->skipInFrame = 0;
  x->frame++;
  x->pos += n;
  if (wrap) {
    if (x->loopNum > 0) x->loopNum--;
    x->seek(x->info.loopStart);
  }
  put32(pSamples, n);
  put32(pFinish, x->pos >= total ? 1u : 0u);
  put32(pRemain, static_cast<uint32_t>(-1));
  ret(c, 0);
}

void h_reset_position(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  uint32_t sample = arg(c, 1);
  if (sample > x->total()) return ret(c, E_ATRAC_API_FAIL);
  x->seek(sample);
  ret(c, 0);
}

void h_set_loop_num(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  if (!x->info.hasLoop) return ret(c, E_ATRAC_NO_LOOP_INFORMATION);
  x->loopNum = static_cast<int32_t>(arg(c, 1));
  ret(c, 0);
}

void h_sound_sample(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  uint32_t total = x->total();
  put32(arg(c, 1), total ? total - 1 : 0);
  put32(arg(c, 2), x->info.hasLoop ? x->info.loopStart : static_cast<uint32_t>(-1));
  put32(arg(c, 3), x->info.hasLoop ? x->info.loopEnd : static_cast<uint32_t>(-1));
  ret(c, 0);
}

void h_max_sample(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  put32(arg(c, 1), x->frame_samples());
  ret(c, 0);
}

void h_loop_status(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  put32(arg(c, 1), static_cast<uint32_t>(x->loopNum));
  put32(arg(c, 2), x->info.hasLoop ? 1u : 0u);
  ret(c, 0);
}

void h_reinit(Cpu& c) { ret(c, 0); }

void h_bitrate(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  uint32_t kbps = x->info.blockAlign * x->info.sampleRate / x->frame_samples() * 8 / 1000;
  put32(arg(c, 1), kbps);
  ret(c, 0);
}

void h_channel(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  put32(arg(c, 1), static_cast<uint32_t>(x->info.channels));
  ret(c, 0);
}

void h_next_sample(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  uint32_t left = x->total() - std::min(x->pos, x->total());
  put32(arg(c, 1), std::min(left, x->frame_samples() - x->skipInFrame));
  ret(c, 0);
}

void h_remain_frame(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  put32(arg(c, 1), static_cast<uint32_t>(-1));
  ret(c, 0);
}

void h_next_decode_pos(Cpu& c) {
  Context* x = find(c, static_cast<int>(arg(c, 0)));
  if (!x) return;
  if (x->pos >= x->total()) return ret(c, E_ATRAC_ALL_DATA_DECODED);
  put32(arg(c, 1), x->pos);
  ret(c, 0);
}

}  // namespace

void atrac_init() { g_ctx.clear(); }

void register_atrac() {
  const char* m = "sceAtrac3plus";
  register_hle(m, 0x7A20E7AF, "sceAtracSetDataAndGetID", h_set_data_get_id);
  register_hle(m, 0x61EB33F5, "sceAtracReleaseAtracID", h_release);
  register_hle(m, 0x6A8C3CD5, "sceAtracDecodeData", h_decode);
  register_hle(m, 0x644E5607, "sceAtracResetPlayPosition", h_reset_position);
  register_hle(m, 0x868120B5, "sceAtracSetLoopNum", h_set_loop_num);
  register_hle(m, 0xA2BBA8BE, "sceAtracGetSoundSample", h_sound_sample);
  register_hle(m, 0xD6A5F2F7, "sceAtracGetMaxSample", h_max_sample);
  register_hle(m, 0xFAA4F89B, "sceAtracGetLoopStatus", h_loop_status);
  register_hle(m, 0x132F1ECA, "sceAtracReinit", h_reinit);
  register_hle(m, 0xA554A158, "sceAtracGetBitrate", h_bitrate);
  register_hle(m, 0x31668BAA, "sceAtracGetChannel", h_channel);
  register_hle(m, 0x36FAABFB, "sceAtracGetNextSample", h_next_sample);
  register_hle(m, 0x9AE849A7, "sceAtracGetRemainFrame", h_remain_frame);
  register_hle(m, 0xE23E3A35, "sceAtracGetNextDecodePosition", h_next_decode_pos);
}

}  // namespace k
}  // namespace psp
