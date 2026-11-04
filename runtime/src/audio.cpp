// sceAudio. Blocking output follows device consumption, or the clock when no device is available.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>

#include "audio_buffer.h"
#include "kernel_internal.h"
#include "platform.h"

namespace psp {
namespace k {

namespace {

constexpr int kRate = 44100;
constexpr int kChannels = 9;  // 0-7 normal, 8 for the SRC channel
constexpr int kSrcChannel = 8;

struct Channel {
  bool reserved = false;
  uint32_t samples = 0;
  uint32_t format = 0;
  int32_t volL = 0x8000, volR = 0x8000;
  uint32_t srcRate = kRate;
  uint64_t busyUntil = 0;
  uint64_t generation = 0;
  audio::OutputBuffer fifo;
  uint64_t queued = 0, missing = 0, dropped = 0, calls = 0;
  int32_t lastL = 0, lastR = 0, peak = 0;
};

Channel g_ch[kChannels];
std::mutex g_audioMutex;
bool g_started = false;
bool g_deviceActive = false;
uint64_t g_nextGeneration = 1;
const bool g_diagnostics = std::getenv("MGSPO_AUDIO_STATS") != nullptr;

void mix(int16_t* out, uint32_t frames) {
  std::lock_guard<std::mutex> lk(g_audioMutex);
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t l = 0, r = 0;
    for (auto& c : g_ch) {
      if (!c.fifo.pop(l, r)) {
        if (g_diagnostics && c.reserved && c.calls) ++c.missing;
        continue;
      }
    }
    out[i * 2] = static_cast<int16_t>(std::clamp(l, -32768, 32767));
    out[i * 2 + 1] = static_cast<int16_t>(std::clamp(r, -32768, 32767));
  }
  if (g_diagnostics) {
    static uint64_t mixed = 0, report = 0;
    mixed += frames;
    if (mixed - report >= kRate) {
      report = mixed;
      for (int i = 0; i < kChannels; ++i) {
        const auto& c = g_ch[i];
        if (c.calls)
          std::fprintf(stderr, "[audio] ch=%d rate=%u block=%u calls=%llu queued=%llu pending=%zu missing=%llu dropped=%llu mixed=%llu vol=%d/%d peak=%d\n",
                       i, c.srcRate, c.samples, (unsigned long long)c.calls, (unsigned long long)c.queued,
                       c.fifo.frames(), (unsigned long long)c.missing, (unsigned long long)c.dropped,
                       (unsigned long long)mixed, c.lastL, c.lastR, c.peak);
      }
    }
  }
}

void ensure_started() {
  if (g_started) return;
  g_started = true;
  g_deviceActive = plat::audio_start(kRate, mix);
}

bool valid(int ch) { return ch >= 0 && ch < kChannels && g_ch[ch].reserved; }

// Returns the previous buffer's clock deadline for device-free pacing.
std::optional<uint64_t> queue(int ch, int32_t volL, int32_t volR, uint32_t buf) {
  Channel& c = g_ch[ch];
  uint32_t n = c.samples;
  const uint64_t converted = static_cast<uint64_t>(n) * kRate / c.srcRate;
  if (n > 65536 || converted > audio::OutputBuffer::kMaxFrames) {
    std::fprintf(stderr, "[audio] Output block exceeds bounded buffer: channel=%d samples=%u rate=%u.\n",
                 ch, n, c.srcRate);
    return std::nullopt;
  }
  bool mono = (c.format & 0x10) != 0;
  uint32_t bytes = n * (mono ? 2 : 4);
  if (buf && n) {
    ptr(buf);
    ptr(buf + bytes - 1);
  }
  uint32_t outFrames = static_cast<uint32_t>(converted);
  {
    std::lock_guard<std::mutex> lk(g_audioMutex);
    ++c.calls;
    c.lastL = volL; c.lastR = volR;
    if (g_deviceActive && buf && n) {
      if (!c.fifo.can_append(outFrames)) {
        c.dropped += outFrames;
        std::fprintf(stderr, "[audio] Output queue full: channel=%d pending=%zu incoming=%u.\n",
                     ch, c.fifo.frames(), outFrames);
        return std::nullopt;
      }
      c.queued += outFrames;
      for (uint32_t i = 0; i < outFrames; ++i) {
        uint32_t s = static_cast<uint32_t>(static_cast<uint64_t>(i) * c.srcRate / kRate);
        int32_t l, r;
        if (mono) {
          l = r = static_cast<int16_t>(lhu(buf + s * 2));
        } else {
          l = static_cast<int16_t>(lhu(buf + s * 4));
          r = static_cast<int16_t>(lhu(buf + s * 4 + 2));
        }
        if (g_diagnostics) c.peak = std::max(c.peak, std::max(std::abs(l), std::abs(r)));
        c.fifo.push(static_cast<int16_t>(std::clamp(l * volL / 0x8000, -32768, 32767)),
                    static_cast<int16_t>(std::clamp(r * volR / 0x8000, -32768, 32767)));
      }
    }
  }
  uint64_t now = now_us();
  uint64_t start = std::max(c.busyUntil, now);
  c.busyUntil = start + static_cast<uint64_t>(outFrames) * 1000000 / kRate;
  return start;
}

void out_common(Cpu& c, int ch, int32_t l, int32_t r, uint32_t buf, bool block) {
  if (!valid(ch)) {
    if (g_diagnostics) std::fprintf(stderr, "[audio] Invalid output channel %d\n", ch);
    return ret(c, E_ERROR);
  }
  ensure_started();
  auto start = queue(ch, l, r, buf);
  if (!start) return ret(c, E_ERROR);
  uint32_t n = g_ch[ch].samples;
  const uint64_t generation = g_ch[ch].generation;
  if (block && g_deviceActive) {
    // The device pulls 1024-frame chunks; retain a bounded lead across synchronous GE work.
    const uint64_t deadline = now_us() + 1000000;
    for (;;) {
      {
        std::lock_guard<std::mutex> lk(g_audioMutex);
        if (!g_ch[ch].reserved || g_ch[ch].generation != generation) {
          std::fprintf(stderr, "[audio] Channel %d changed during output wait.\n", ch);
          return ret(c, E_ERROR);
        }
        if (buf ? !g_ch[ch].fifo.producer_waiting() : g_ch[ch].fifo.frames() == 0) break;
      }
      if (now_us() >= deadline) {
        std::fprintf(stderr, "[audio] Output device did not drain channel %d within one second.\n", ch);
        return ret(c, E_ERROR);
      }
      sleep_until(c, now_us() + 1000, false);
    }
  } else if (block && *start > now_us()) sleep_until(c, *start, false);
  ret(c, n);
}

void h_reserve(Cpu& c) {
  std::lock_guard<std::mutex> lk(g_audioMutex);
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
  g_ch[ch].generation = g_nextGeneration++;
  g_ch[ch].reserved = true;
  g_ch[ch].samples = arg(c, 1);
  g_ch[ch].format = arg(c, 2);
  if (g_diagnostics) std::fprintf(stderr, "[audio] reserve ch=%d samples=%u format=%x\n", ch, g_ch[ch].samples, g_ch[ch].format);
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
  if (g_deviceActive) {
    std::lock_guard<std::mutex> lk(g_audioMutex);
    return ret(c, static_cast<uint32_t>(g_ch[ch].fifo.frames()));
  }
  uint64_t now = now_us();
  uint64_t left = g_ch[ch].busyUntil > now ? g_ch[ch].busyUntil - now : 0;
  ret(c, static_cast<uint32_t>(left * kRate / 1000000));
}

void h_set_len(Cpu& c) {
  int ch = static_cast<int>(arg(c, 0));
  if (!valid(ch)) return ret(c, E_ERROR);
  std::lock_guard<std::mutex> lk(g_audioMutex);
  g_ch[ch].samples = arg(c, 1);
  ret(c, 0);
}

void h_change_config(Cpu& c) {
  int ch = static_cast<int>(arg(c, 0));
  if (!valid(ch)) return ret(c, E_ERROR);
  std::lock_guard<std::mutex> lk(g_audioMutex);
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
  std::lock_guard<std::mutex> lk(g_audioMutex);
  Channel& ch = g_ch[kSrcChannel];
  ch = Channel{};
  ch.generation = g_nextGeneration++;
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
  register_hle(m, 0xB011922F, "sceAudioGetChannelRestLength", h_rest_len);
  register_hle(m, 0xCB2E439E, "sceAudioSetChannelDataLen", h_set_len);
  register_hle(m, 0x95FD0C2D, "sceAudioChangeChannelConfig", h_change_config);
  register_hle(m, 0xB7E1D8E7, "sceAudioChangeChannelVolume", h_change_volume);
  register_hle(m, 0x38553111, "sceAudioSRCChReserve", h_src_reserve);
  register_hle(m, 0x5C37C0AE, "sceAudioSRCChRelease", h_src_release);
  register_hle(m, 0xE0727056, "sceAudioSRCOutputBlocking", h_src_output);
}

}  // namespace k
}  // namespace psp
