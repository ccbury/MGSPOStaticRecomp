#include "sas_core.h"

#include <algorithm>
#include <cstring>

#include <psp/memory.h>

namespace psp {
namespace sas {

namespace {

int clamp16(int v) { return std::clamp(v, -32768, 32767); }

int simple_rate(int n) {
  n &= 0x7F;
  if (n == 0x7F) return 0;
  int rate = ((7 - (n & 3)) << 26) >> (n >> 2);
  return rate ? rate : 1;
}

int exponent_rate(int n) {
  n &= 0x7F;
  if (n == 0x7F) return 0;
  int rate = ((7 - (n & 3)) << 24) >> (n >> 2);
  return rate ? rate : 1;
}

// ADPCM prediction filters (60/64 style coefficients from the original PlayStation SPU).
const int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

}  // namespace

SimpleAdsr decode_simple_adsr(uint32_t e1, uint32_t e2) {
  SimpleAdsr s;
  e1 &= 0xFFFF;
  e2 &= 0xFFFF;
  s.attackRate = simple_rate(static_cast<int>(e1 >> 8));
  s.attackType = (e1 & 0x8000) ? LinearBent : LinearInc;
  int dn = (e1 >> 4) & 0xF;
  s.decayRate = dn ? static_cast<int>(0x80000000u >> dn) : 0x7FFFFFFF;
  s.decayType = ExpDec;
  s.sustainType = (e2 >> 14) & 3;
  s.sustainRate = s.sustainType == ExpDec ? exponent_rate(static_cast<int>(e2 >> 6)) : simple_rate(static_cast<int>(e2 >> 6));
  s.releaseType = (e2 & 0x20) ? ExpDec : LinearDec;
  int rn = e2 & 0x1F;
  if (rn == 31) {
    s.releaseRate = 0;
  } else if (s.releaseType == LinearDec) {
    s.releaseRate = rn == 30 ? 0x40000000 : rn == 29 ? 1 : (0x10000000 >> rn);
  } else {
    s.releaseRate = rn == 0 ? 0x7FFFFFFF : static_cast<int>(0x80000000u >> rn);
  }
  s.sustainLevel = static_cast<int>(std::min<int64_t>(static_cast<int64_t>((e1 & 0xF) + 1) << 26, kEnvMax));
  return s;
}

// ---- envelope ----

void Envelope::key_on() {
  state_ = Attack;
  height_ = 0;
}

void Envelope::key_off() {
  if (state_ != Off) state_ = Release;
}

void Envelope::kill() {
  state_ = Off;
  height_ = 0;
}

void Envelope::set_rates(int flag, int a, int d, int s, int r) {
  if (flag & 1) attackRate = a;
  if (flag & 2) decayRate = d;
  if (flag & 4) sustainRate = s;
  if (flag & 8) releaseRate = r;
}

void Envelope::set_modes(int flag, int a, int d, int s, int r) {
  if (flag & 1) attackType = a;
  if (flag & 2) decayType = d;
  if (flag & 4) sustainType = s;
  if (flag & 8) releaseType = r;
}

void Envelope::set_simple(uint32_t e1, uint32_t e2) {
  SimpleAdsr s = decode_simple_adsr(e1, e2);
  attackRate = s.attackRate;
  attackType = s.attackType;
  decayRate = s.decayRate;
  decayType = s.decayType;
  sustainRate = s.sustainRate;
  sustainType = s.sustainType;
  releaseRate = s.releaseRate;
  releaseType = s.releaseType;
  sustainLevel = s.sustainLevel;
}

void Envelope::set_sustain_level(int level) {
  sustainLevel = std::clamp(level, 0, static_cast<int>(kEnvMax));
}

namespace {
int32_t exp_down(int32_t h, int rate) {
  int64_t d = (static_cast<int64_t>(h) * rate) >> 31;
  if (d < 1) d = 1;
  return static_cast<int32_t>(std::max<int64_t>(0, h - d));
}
}  // namespace

void Envelope::step() {
  switch (state_) {
    case Off:
      return;
    case Attack: {
      if (attackRate == 0) return;
      int64_t inc = attackRate;
      if (attackType == LinearBent && height_ >= (kEnvMax / 4) * 3) inc = std::max<int64_t>(1, inc / 4);
      int64_t h = height_ + inc;
      if (h >= kEnvMax) {
        height_ = kEnvMax;
        state_ = Decay;
      } else {
        height_ = static_cast<int32_t>(h);
      }
      return;
    }
    case Decay: {
      if (decayType == LinearDec)
        height_ = static_cast<int32_t>(std::max<int64_t>(0, static_cast<int64_t>(height_) - decayRate));
      else
        height_ = exp_down(height_, decayRate);
      if (height_ <= sustainLevel) {
        height_ = sustainLevel;
        state_ = Sustain;
      }
      return;
    }
    case Sustain: {
      if (sustainRate == 0) return;
      switch (sustainType) {
        case LinearInc:
        case ExpInc:
          height_ = static_cast<int32_t>(std::min<int64_t>(kEnvMax, static_cast<int64_t>(height_) + sustainRate));
          break;
        case LinearBent: {
          int64_t inc = height_ >= (kEnvMax / 4) * 3 ? std::max(1, sustainRate / 4) : sustainRate;
          height_ = static_cast<int32_t>(std::min<int64_t>(kEnvMax, height_ + inc));
          break;
        }
        case LinearDec:
          height_ = static_cast<int32_t>(std::max<int64_t>(0, static_cast<int64_t>(height_) - sustainRate));
          break;
        case ExpDec:
          height_ = exp_down(height_, sustainRate);
          break;
        default:
          break;
      }
      return;
    }
    case Release: {
      if (releaseRate == 0) return;
      if (releaseType == ExpDec)
        height_ = exp_down(height_, releaseRate);
      else
        height_ = static_cast<int32_t>(std::max<int64_t>(0, static_cast<int64_t>(height_) - releaseRate));
      if (height_ <= 0) {
        height_ = 0;
        state_ = Off;
      }
      return;
    }
  }
}

// ---- ADPCM ----

void VagDecoder::start(uint32_t addr, uint32_t size, bool loop) {
  addr_ = addr;
  numBlocks_ = static_cast<int>(size / 16);
  block_ = 0;
  loopStart_ = 0;
  loop_ = loop;
  loopAtEnd_ = false;
  ended_ = numBlocks_ == 0;
  pos_ = 28;
  s1_ = s2_ = 0;
}

bool VagDecoder::decode_block() {
  if (loopAtEnd_) {
    block_ = loopStart_;
    loopAtEnd_ = false;
  }
  if (block_ >= numBlocks_) return false;
  uint32_t a = addr_ + static_cast<uint32_t>(block_) * 16;
  int head = lbu(a);
  int flags = lbu(a + 1);
  if (flags == 7) return false;
  if (flags == 6) loopStart_ = block_;
  if (flags == 3 && loop_) loopAtEnd_ = true;
  int shift = head & 0xF;
  int idx = head >> 4;
  int c1 = idx < 5 ? kFilter[idx][0] : 0;
  int c2 = idx < 5 ? kFilter[idx][1] : 0;
  for (int i = 0; i < 28; i += 2) {
    int d = lbu(a + 2 + i / 2);
    int n0 = static_cast<int16_t>((d & 0xF) << 12) >> shift;
    int n1 = static_cast<int16_t>((d & 0xF0) << 8) >> shift;
    int y0 = clamp16(n0 + ((s1_ * c1 + s2_ * c2) >> 6));
    s2_ = s1_;
    s1_ = y0;
    int y1 = clamp16(n1 + ((s1_ * c1 + s2_ * c2) >> 6));
    s2_ = s1_;
    s1_ = y1;
    samples_[i] = static_cast<int16_t>(y0);
    samples_[i + 1] = static_cast<int16_t>(y1);
  }
  ++block_;
  pos_ = 0;
  return true;
}

bool VagDecoder::next(int16_t& out) {
  if (ended_) return false;
  if (pos_ >= 28 && !decode_block()) {
    ended_ = true;
    return false;
  }
  out = samples_[pos_++];
  return true;
}

// ---- voice ----

void Voice::reset_source() {
  frac = 0;
  cur = nxt = 0;
  primed = false;
  srcEnded = false;
  pcmPos = 0;
  switch (type) {
    case VoiceType::Vag:
      vag.start(vagAddr, vagSize, loop);
      break;
    default:
      break;
  }
}

void Voice::key_on() {
  reset_source();
  on = true;
  playing = true;
  env.key_on();
}

void Voice::key_off() {
  on = false;
  env.key_off();
}

bool Voice::read_source(int16_t& s) {
  switch (type) {
    case VoiceType::Vag:
      return vag.next(s);
    case VoiceType::Pcm:
      if (pcmPos >= pcmSize) {
        if (!loop) return false;
        pcmPos = pcmLoopPos;
      }
      s = lh(pcmAddr + pcmPos * 2);
      ++pcmPos;
      return true;
    case VoiceType::Noise:
      lfsr = lfsr * 1664525u + 1013904223u;
      s = static_cast<int16_t>(lfsr >> 16);
      return true;
    default:
      return false;
  }
}

// ---- reverb ----

void Reverb::set_type(int t) {
  type = t;
  configure();
}

void Reverb::set_params(int delay, int feedback) {
  delay_ = delay;
  feedback_ = feedback;
  if (type == 6 || type == 7) configure();
}

void Reverb::configure() {
  // Comb lengths in frames at 44.1kHz and feedback out of 4096.
  struct Preset {
    int a, b, fb;
  };
  static const Preset presets[] = {
      {1300, 1900, 1200},  // room
      {1800, 2300, 1400},  // studio small
      {2600, 3500, 1650},  // studio medium
      {3500, 4800, 1850},  // studio large
      {4800, 6600, 2250},  // hall
      {6600, 9200, 2650},  // space
      {0, 0, 0},           // echo
      {0, 0, 0},           // delay
      {900, 1300, 2450},   // pipe
  };
  int a = 0, b = 0;
  if (type >= 0 && type <= 8) {
    if (type == 6 || type == 7) {
      int len = std::clamp(delay_, 0, 127) * 350 + 700;
      a = len;
      b = len;
      fb_ = type == 6 ? std::clamp(feedback_, 0, 255) * 3300 / 255 : 0;
    } else {
      a = presets[type].a;
      b = presets[type].b;
      fb_ = presets[type].fb;
    }
  }
  int lens[2] = {a, b};
  for (int i = 0; i < 2; ++i) {
    l_[i].buf.assign(static_cast<size_t>(lens[i]), 0);
    r_[i].buf.assign(static_cast<size_t>(lens[i] + 37), 0);
    l_[i].pos = r_[i].pos = 0;
  }
}

void Reverb::process(const int32_t* send, int frames, int32_t* outL, int32_t* outR, int volL, int volR) {
  for (int i = 0; i < frames; ++i) {
    int32_t wetL = 0, wetR = 0;
    if (!l_[0].buf.empty()) {
      for (int k = 0; k < 2; ++k) {
        Line& l = l_[k];
        Line& r = r_[k];
        int32_t dl = l.buf[l.pos];
        int32_t dr = r.buf[r.pos];
        l.buf[l.pos] = static_cast<int32_t>(clamp16(send[i * 2] + ((dl * fb_) >> 12)));
        r.buf[r.pos] = static_cast<int32_t>(clamp16(send[i * 2 + 1] + ((dr * fb_) >> 12)));
        if (++l.pos >= l.buf.size()) l.pos = 0;
        if (++r.pos >= r.buf.size()) r.pos = 0;
        wetL += dl / 2;
        wetR += dr / 2;
      }
    }
    outL[i] = (wetL * volL) >> 12;
    outR[i] = (wetR * volR) >> 12;
  }
}

// ---- core ----

void Core::init(int grain, int maxVoices, int outputMode, int sampleRate) {
  grain_ = grain;
  maxVoices_ = std::clamp(maxVoices, 1, kMaxVoices);
  outputMode_ = outputMode;
  sampleRate_ = sampleRate;
  for (auto& v : voices) v = Voice{};
  reverb = Reverb{};
  dryOn = true;
  wetOn = false;
  revVolL = revVolR = 0;
  dry_.assign(static_cast<size_t>(grain) * 2, 0);
  send_.assign(static_cast<size_t>(grain) * 2, 0);
}

uint32_t Core::end_flags() const {
  uint32_t f = 0;
  for (int i = 0; i < maxVoices_; ++i)
    if (!voices[i].playing) f |= 1u << i;
  return f;
}

uint32_t Core::pause_flags() const {
  uint32_t f = 0;
  for (int i = 0; i < maxVoices_; ++i)
    if (voices[i].paused) f |= 1u << i;
  return f;
}

void Core::mix_voice(Voice& v) {
  auto finish = [&v] {
    v.playing = false;
    v.on = false;
    v.env.kill();
  };
  if (v.type == VoiceType::None || (v.type == VoiceType::Vag && !v.vagAddr) ||
      (v.type == VoiceType::Pcm && !v.pcmAddr)) {
    finish();
    return;
  }
  if (!v.primed) {
    int16_t s = 0;
    v.cur = v.read_source(s) ? s : 0;
    if (v.read_source(s)) v.nxt = s;
    else { v.nxt = 0; v.srcEnded = true; }
    v.primed = true;
  }
  for (int i = 0; i < grain_; ++i) {
    int sample = v.cur + (((v.nxt - v.cur) * static_cast<int>(v.frac)) >> 12);
    int32_t h = v.env.height();
    v.env.step();
    int s = static_cast<int>((static_cast<int64_t>(sample) * h) >> 30);
    dry_[i * 2] += (s * v.volL) >> 12;
    dry_[i * 2 + 1] += (s * v.volR) >> 12;
    send_[i * 2] += (s * v.effL) >> 12;
    send_[i * 2 + 1] += (s * v.effR) >> 12;
    if (!v.env.active()) {
      finish();
      return;
    }
    v.frac += static_cast<uint32_t>(v.pitch);
    while (v.frac >= static_cast<uint32_t>(kPitchBase)) {
      v.frac -= kPitchBase;
      if (v.srcEnded) {
        finish();
        return;
      }
      v.cur = v.nxt;
      int16_t n = 0;
      if (v.read_source(n)) v.nxt = n;
      else { v.nxt = 0; v.srcEnded = true; }
    }
  }
}

void Core::mix(int16_t* out, const int16_t* in, int inL, int inR) {
  std::fill(dry_.begin(), dry_.end(), 0);
  std::fill(send_.begin(), send_.end(), 0);
  for (int i = 0; i < maxVoices_; ++i) {
    Voice& v = voices[i];
    if (v.playing && !v.paused) mix_voice(v);
  }
  const int g = grain_;
  if (outputMode_ == Raw) {
    for (int i = 0; i < g; ++i) {
      out[i] = static_cast<int16_t>(clamp16(dry_[i * 2]));
      out[g + i] = static_cast<int16_t>(clamp16(dry_[i * 2 + 1]));
      out[2 * g + i] = static_cast<int16_t>(clamp16(send_[i * 2]));
      out[3 * g + i] = static_cast<int16_t>(clamp16(send_[i * 2 + 1]));
    }
    return;
  }
  std::vector<int32_t> wl, wr;
  bool wet = wetOn && reverb.type >= 0;
  if (wet) {
    wl.resize(static_cast<size_t>(g));
    wr.resize(static_cast<size_t>(g));
    reverb.process(send_.data(), g, wl.data(), wr.data(), revVolL, revVolR);
  }
  for (int i = 0; i < g; ++i) {
    int l = 0, r = 0;
    if (in) {
      l = (in[i * 2] * inL) >> 12;
      r = (in[i * 2 + 1] * inR) >> 12;
    }
    if (dryOn) {
      l += dry_[i * 2];
      r += dry_[i * 2 + 1];
    }
    if (wet) {
      l += wl[static_cast<size_t>(i)];
      r += wr[static_cast<size_t>(i)];
    }
    out[i * 2] = static_cast<int16_t>(clamp16(l));
    out[i * 2 + 1] = static_cast<int16_t>(clamp16(r));
  }
}

}  // namespace sas
}  // namespace psp
