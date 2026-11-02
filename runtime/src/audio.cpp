// sceAudio. Channels queue stereo samples that a host audio thread mixes; blocking calls are paced by the clock.
#include <algorithm>
#include <deque>
#include <mutex>

#include "kernel_internal.h"
#include "platform.h"

namespace psp {
namespace k {

namespace {

constexpr int kRate = 44100;
constexpr int kChannels = 9;  // 0-7 normal, 8 for the SRC channel
constexpr int kSrcChannel = 8;
constexpr size_t kMaxQueuedFrames = kRate / 2;

struct Channel {
  bool reserved = false;
  uint32_t samples = 0;
  uint32_t format = 0;
  int32_t volL = 0x8000, volR = 0x8000;
  uint32_t srcRate = kRate;
  uint64_t busyUntil = 0;
  std::deque<int16_t> fifo;  // interleaved stereo
};

Channel g_ch[kChannels];
std::mutex g_audioMutex;
bool g_started = false;

void mix(int16_t* out, uint32_t frames) {
  std::lock_guard<std::mutex> lk(g_audioMutex);
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t l = 0, r = 0;
    for (auto& c : g_ch) {
      if (c.fifo.size() < 2) continue;
      l += c.fifo[0];
      r += c.fifo[1];
      c.fifo.pop_front();
      c.fifo.pop_front();
    }
    out[i * 2] = static_cast<int16_t>(std::clamp(l, -32768, 32767));
    out[i * 2 + 1] = static_cast<int16_t>(std::clamp(r, -32768, 32767));
  }
}

void ensure_started() {
  if (g_started) return;
  g_started = true;
  plat::audio_start(kRate, mix);
}

bool valid(int ch) { return ch >= 0 && ch < kChannels && g_ch[ch].reserved; }

// Queues a buffer and returns the clock time at which the previous one has finished playing.
uint64_t queue(int ch, int32_t volL, int32_t volR, uint32_t buf) {
  Channel& c = g_ch[ch];
  uint32_t n = c.samples;
  bool mono = (c.format & 0x10) != 0;
  uint32_t bytes = n * (mono ? 2 : 4);
  if (buf && n) {
    ptr(buf);
    ptr(buf + bytes - 1);
  }
  uint32_t outFrames = static_cast<uint32_t>(static_cast<uint64_t>(n) * kRate / c.srcRate);
  {
    std::lock_guard<std::mutex> lk(g_audioMutex);
    if (buf && n && c.fifo.size() / 2 < kMaxQueuedFrames) {
      for (uint32_t i = 0; i < outFrames; ++i) {
        uint32_t s = static_cast<uint32_t>(static_cast<uint64_t>(i) * c.srcRate / kRate);
        int32_t l, r;
        if (mono) {
          l = r = static_cast<int16_t>(lhu(buf + s * 2));
        } else {
          l = static_cast<int16_t>(lhu(buf + s * 4));
          r = static_cast<int16_t>(lhu(buf + s * 4 + 2));
        }
        c.fifo.push_back(static_cast<int16_t>(std::clamp(l * volL / 0x8000, -32768, 32767)));
        c.fifo.push_back(static_cast<int16_t>(std::clamp(r * volR / 0x8000, -32768, 32767)));
      }
    }
  }
  uint64_t now = now_us();
  uint64_t start = std::max(c.busyUntil, now);
  c.busyUntil = start + static_cast<uint64_t>(outFrames) * 1000000 / kRate;
  return start;
}

void out_common(Cpu& c, int ch, int32_t l, int32_t r, uint32_t buf, bool block) {
  if (!valid(ch)) return ret(c, E_ERROR);
  ensure_started();
  uint64_t start = queue(ch, l, r, buf);
  uint32_t n = g_ch[ch].samples;
  if (block && start > now_us()) sleep_until(c, start, false);
  ret(c, n);
}

void h_reserve(Cpu& c) {
  int ch = static_cast<int>(arg(c, 0));
  if (ch < 0) {
    for (int i = 0; i < kSrcChannel; ++i)
      if (!g_ch[i].reserved) {
        ch = i;
        break;
      }
  }
  if (ch < 0 || ch >= kSrcChannel || g_ch[ch].reserved) return ret(c, E_ERROR);
  g_ch[ch] = Channel{};
  g_ch[ch].reserved = true;
  g_ch[ch].samples = arg(c, 1);
  g_ch[ch].format = arg(c, 2);
  ret(c, static_cast<uint32_t>(ch));
}

void h_release(Cpu& c) {
  int ch = static_cast<int>(arg(c, 0));
  if (!valid(ch)) return ret(c, E_ERROR);
  std::lock_guard<std::mutex> lk(g_audioMutex);
  g_ch[ch] = Channel{};
  ret(c, 0);
}

void h_output_blocking(Cpu& c) {
  int32_t v = static_cast<int32_t>(arg(c, 1));
  out_common(c, static_cast<int>(arg(c, 0)), v, v, arg(c, 2), true);
}
void h_output(Cpu& c) {
  int32_t v = static_cast<int32_t>(arg(c, 1));
  out_common(c, static_cast<int>(arg(c, 0)), v, v, arg(c, 2), false);
}
void h_output_panned_blocking(Cpu& c) {
  out_common(c, static_cast<int>(arg(c, 0)), static_cast<int32_t>(arg(c, 1)), static_cast<int32_t>(arg(c, 2)),
             arg(c, 3), true);
}
void h_output_panned(Cpu& c) {
  out_common(c, static_cast<int>(arg(c, 0)), static_cast<int32_t>(arg(c, 1)), static_cast<int32_t>(arg(c, 2)),
             arg(c, 3), false);
}

void h_rest_len(Cpu& c) {
  int ch = static_cast<int>(arg(c, 0));
  if (!valid(ch)) return ret(c, E_ERROR);
  uint64_t now = now_us();
  uint64_t left = g_ch[ch].busyUntil > now ? g_ch[ch].busyUntil - now : 0;
  ret(c, static_cast<uint32_t>(left * kRate / 1000000));
}

void h_set_len(Cpu& c) {
  int ch = static_cast<int>(arg(c, 0));
  if (!valid(ch)) return ret(c, E_ERROR);
  g_ch[ch].samples = arg(c, 1);
  ret(c, 0);
}

void h_change_config(Cpu& c) {
  int ch = static_cast<int>(arg(c, 0));
  if (!valid(ch)) return ret(c, E_ERROR);
  g_ch[ch].format = arg(c, 1);
  ret(c, 0);
}

void h_change_volume(Cpu& c) {
  int ch = static_cast<int>(arg(c, 0));
  if (!valid(ch)) return ret(c, E_ERROR);
  g_ch[ch].volL = static_cast<int32_t>(arg(c, 1));
  g_ch[ch].volR = static_cast<int32_t>(arg(c, 2));
  ret(c, 0);
}

// sceAudioSRCChReserve(samples, freq, channels)
void h_src_reserve(Cpu& c) {
  Channel& ch = g_ch[kSrcChannel];
  ch = Channel{};
  ch.reserved = true;
  ch.samples = arg(c, 0);
  ch.srcRate = arg(c, 1) ? arg(c, 1) : kRate;
  ch.format = arg(c, 2) == 1 ? 0x10 : 0;
  ret(c, 0);
}

void h_src_release(Cpu& c) {
  std::lock_guard<std::mutex> lk(g_audioMutex);
  g_ch[kSrcChannel] = Channel{};
  ret(c, 0);
}

void h_src_output(Cpu& c) {
  int32_t v = static_cast<int32_t>(arg(c, 0));
  out_common(c, kSrcChannel, v, v, arg(c, 1), true);
}

}  // namespace

void audio_init() {
  for (auto& c : g_ch) c = Channel{};
}

void register_audio() {
  const char* m = "sceAudio";
  register_hle(m, 0x5EC81C55, "sceAudioChReserve", h_reserve);
  register_hle(m, 0x6FC46853, "sceAudioChRelease", h_release);
  register_hle(m, 0x136CAF51, "sceAudioOutputBlocking", h_output_blocking);
  register_hle(m, 0x8C1009B2, "sceAudioOutput", h_output);
  register_hle(m, 0x13F592BC, "sceAudioOutputPannedBlocking", h_output_panned_blocking);
  register_hle(m, 0xE2D56B2D, "sceAudioOutputPanned", h_output_panned);
  register_hle(m, 0xE9D97901, "sceAudioGetChannelRestLen", h_rest_len);
  register_hle(m, 0xCB2E439E, "sceAudioSetChannelDataLen", h_set_len);
  register_hle(m, 0x95FD0C2D, "sceAudioChangeChannelConfig", h_change_config);
  register_hle(m, 0xB7E1D8E7, "sceAudioChangeChannelVolume", h_change_volume);
  register_hle(m, 0x38553111, "sceAudioSRCChReserve", h_src_reserve);
  register_hle(m, 0x5C37C0AE, "sceAudioSRCChRelease", h_src_release);
  register_hle(m, 0xE0727056, "sceAudioSRCOutputBlocking", h_src_output);
}

}  // namespace k
}  // namespace psp
