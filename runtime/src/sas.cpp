// sceSasCore: guest-facing side of the software synth in sas_core.cpp. Mixed grains are written back to guest
// memory; the game then feeds them to sceAudio like any other buffer.
#include <map>
#include <memory>

#include "kernel_internal.h"
#include "sas_core.h"

namespace psp {
namespace k {

namespace {

constexpr uint32_t E_SAS_GRAIN = 0x80420001;
constexpr uint32_t E_SAS_VOICES = 0x80420002;
constexpr uint32_t E_SAS_OUTMODE = 0x80420003;
constexpr uint32_t E_SAS_RATE = 0x80420004;
constexpr uint32_t E_SAS_ADDR = 0x80420005;
constexpr uint32_t E_SAS_VOICE = 0x80420010;
constexpr uint32_t E_SAS_NOISE = 0x80420011;
constexpr uint32_t E_SAS_PITCH = 0x80420012;
constexpr uint32_t E_SAS_CURVE = 0x80420013;
constexpr uint32_t E_SAS_PARAM = 0x80420014;
constexpr uint32_t E_SAS_LOOPPOS = 0x80420015;
constexpr uint32_t E_SAS_PAUSED = 0x80420016;
constexpr uint32_t E_SAS_VOLUME = 0x80420018;
constexpr uint32_t E_SAS_ADSR = 0x80420019;
constexpr uint32_t E_SAS_SIZE = 0x8042001A;
constexpr uint32_t E_SAS_EFFECT = 0x80420020;
constexpr uint32_t E_SAS_NOT_INIT = 0x80420030;

std::map<uint32_t, std::unique_ptr<sas::Core>> g_cores;

sas::Core* find_core(uint32_t addr) {
  auto it = g_cores.find(addr);
  return it == g_cores.end() ? nullptr : it->second.get();
}

// Looks up the core in a0 and the voice in a1; on failure writes the error and returns null.
sas::Voice* voice_of(Cpu& c, sas::Core*& core) {
  core = find_core(arg(c, 0));
  if (!core) {
    ret(c, E_SAS_NOT_INIT);
    return nullptr;
  }
  int32_t n = static_cast<int32_t>(arg(c, 1));
  if (n < 0 || n >= sas::kMaxVoices) {
    ret(c, E_SAS_VOICE);
    return nullptr;
  }
  return &core->voices[n];
}

#define VOICE_OR_RETURN()      \
  sas::Core* core = nullptr;   \
  sas::Voice* v = voice_of(c, core); \
  if (!v) return;              \
  (void)core

void h_init(Cpu& c) {
  uint32_t addr = arg(c, 0);
  uint32_t grain = arg(c, 1), voices = arg(c, 2), mode = arg(c, 3), rate = arg(c, 4);
  if (grain < 0x40 || grain > 0x800 || (grain & 0x1F)) return ret(c, E_SAS_GRAIN);
  if (voices == 0 || voices > sas::kMaxVoices) return ret(c, E_SAS_VOICES);
  if (mode > 1) return ret(c, E_SAS_OUTMODE);
  if (rate != 44100) return ret(c, E_SAS_RATE);
  auto& slot = g_cores[addr];
  if (!slot) slot = std::make_unique<sas::Core>();
  slot->init(static_cast<int>(grain), static_cast<int>(voices), static_cast<int>(mode), static_cast<int>(rate));
  ret(c, 0);
}

void run_mix(Cpu& c, uint32_t outAddr, bool withInput, int inL, int inR) {
  sas::Core* core = find_core(arg(c, 0));
  if (!core || !core->initialised()) return ret(c, E_SAS_NOT_INIT);
  uint32_t g = static_cast<uint32_t>(core->grain());
  uint32_t bytes = g * (core->output_mode() == sas::Raw ? 8 : 4);
  if (!outAddr) return ret(c, E_SAS_ADDR);
  ptr(outAddr);
  ptr(outAddr + bytes - 1);
  std::vector<int16_t> in;
  if (withInput) {
    in.resize(g * 2);
    for (uint32_t i = 0; i < g * 2; ++i) in[i] = lh(outAddr + i * 2);
  }
  std::vector<int16_t> out(bytes / 2);
  core->mix(out.data(), withInput ? in.data() : nullptr, inL, inR);
  mem_write(outAddr, out.data(), bytes);
  ret(c, 0);
}

void h_core(Cpu& c) { run_mix(c, arg(c, 1), false, 0, 0); }
void h_core_with_mix(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  if (core && core->output_mode() == sas::Raw) return ret(c, E_SAS_OUTMODE);
  run_mix(c, arg(c, 1), true, static_cast<int32_t>(arg(c, 2)), static_cast<int32_t>(arg(c, 3)));
}

void h_end_flag(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  ret(c, core ? core->end_flags() : E_SAS_NOT_INIT);
}

void h_pause_flag(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  ret(c, core ? core->pause_flags() : E_SAS_NOT_INIT);
}

void h_set_pause(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  if (!core) return ret(c, E_SAS_NOT_INIT);
  uint32_t bits = arg(c, 1);
  bool pause = static_cast<int32_t>(arg(c, 2)) != 0;
  for (int i = 0; i < sas::kMaxVoices; ++i)
    if (bits & (1u << i)) core->voices[i].paused = pause;
  ret(c, 0);
}

void h_set_voice(Cpu& c) {
  VOICE_OR_RETURN();
  uint32_t addr = arg(c, 2), size = arg(c, 3);
  int32_t loop = static_cast<int32_t>(arg(c, 4));
  if (size == 0 || (size & 0xF) || static_cast<int32_t>(size) < 0) return ret(c, E_SAS_SIZE);
  if (loop != 0 && loop != 1) return ret(c, E_SAS_PARAM);
  if (!addr) return ret(c, E_SAS_ADDR);
  bool changed = v->type != sas::VoiceType::Vag || v->vagAddr != addr || v->vagSize != size || v->loop != (loop != 0);
  v->type = sas::VoiceType::Vag;
  v->vagAddr = addr;
  v->vagSize = size;
  v->loop = loop != 0;
  if (changed) v->reset_source();
  ret(c, 0);
}

void h_set_voice_pcm(Cpu& c) {
  VOICE_OR_RETURN();
  uint32_t addr = arg(c, 2);
  int32_t size = static_cast<int32_t>(arg(c, 3)), loopPos = static_cast<int32_t>(arg(c, 4));
  if (size <= 0 || size > 0x10000) return ret(c, E_SAS_SIZE);
  if (loopPos >= size) return ret(c, E_SAS_LOOPPOS);
  if (!addr) return ret(c, E_SAS_ADDR);
  v->type = sas::VoiceType::Pcm;
  v->pcmAddr = addr;
  v->pcmSize = static_cast<uint32_t>(size);
  v->pcmLoopPos = loopPos >= 0 ? static_cast<uint32_t>(loopPos) : 0;
  v->loop = loopPos >= 0;
  v->reset_source();
  v->playing = true;
  ret(c, 0);
}

void h_set_noise(Cpu& c) {
  VOICE_OR_RETURN();
  int32_t freq = static_cast<int32_t>(arg(c, 2));
  if (freq < 0 || freq >= 64) return ret(c, E_SAS_NOISE);
  v->type = sas::VoiceType::Noise;
  v->noiseFreq = freq;
  ret(c, 0);
}

void h_set_volume(Cpu& c) {
  VOICE_OR_RETURN();
  int32_t vol[4];
  for (int i = 0; i < 4; ++i) {
    vol[i] = static_cast<int32_t>(arg(c, 2 + i));
    if (vol[i] > sas::kVolMax || vol[i] < -sas::kVolMax) return ret(c, E_SAS_VOLUME);
  }
  v->volL = vol[0];
  v->volR = vol[1];
  v->effL = vol[2];
  v->effR = vol[3];
  ret(c, 0);
}

void h_set_pitch(Cpu& c) {
  VOICE_OR_RETURN();
  int32_t p = static_cast<int32_t>(arg(c, 2));
  if (p < 0 || p > sas::kPitchMax) return ret(c, E_SAS_PITCH);
  v->pitch = p;
  ret(c, 0);
}

void h_key_on(Cpu& c) {
  VOICE_OR_RETURN();
  if (v->paused || v->on) return ret(c, E_SAS_PAUSED);
  v->key_on();
  ret(c, 0);
}

void h_key_off(Cpu& c) {
  VOICE_OR_RETURN();
  if (v->paused || !v->on) return ret(c, E_SAS_PAUSED);
  v->key_off();
  ret(c, 0);
}

void h_set_adsr(Cpu& c) {
  VOICE_OR_RETURN();
  int flag = static_cast<int>(arg(c, 2));
  int a = static_cast<int>(arg(c, 3)), d = static_cast<int>(arg(c, 4)), s = static_cast<int>(arg(c, 5)),
      r = static_cast<int>(arg(c, 6));
  int bad = (a < 0 ? 1 : 0) | (d < 0 ? 2 : 0) | (s < 0 ? 4 : 0) | (r < 0 ? 8 : 0);
  if (bad & flag) return ret(c, E_SAS_ADSR);
  v->env.set_rates(flag, a, d, s, r);
  ret(c, 0);
}

void h_set_adsr_mode(Cpu& c) {
  VOICE_OR_RETURN();
  int flag = static_cast<int>(arg(c, 2));
  int m[4];
  for (int i = 0; i < 4; ++i) m[i] = static_cast<int>(arg(c, 3 + i) & 0x7FFFFFFF);
  int bad = (m[0] > 5 ? 1 : 0) | (m[1] > 5 ? 2 : 0) | (m[2] > 5 ? 4 : 0) | (m[3] > 5 ? 8 : 0);
  if (bad & flag) return ret(c, E_SAS_CURVE);
  v->env.set_modes(flag, m[0], m[1], m[2], m[3]);
  ret(c, 0);
}

void h_set_simple_adsr(Cpu& c) {
  VOICE_OR_RETURN();
  v->env.set_simple(arg(c, 2), arg(c, 3));
  ret(c, 0);
}

void h_set_sl(Cpu& c) {
  VOICE_OR_RETURN();
  v->env.set_sustain_level(static_cast<int32_t>(arg(c, 2)));
  ret(c, 0);
}

void h_env_height(Cpu& c) {
  VOICE_OR_RETURN();
  ret(c, static_cast<uint32_t>(v->env.height()));
}

void h_all_env_heights(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  if (!core) return ret(c, E_SAS_NOT_INIT);
  uint32_t out = arg(c, 1);
  if (!out) return ret(c, E_SAS_ADDR);
  for (int i = 0; i < sas::kMaxVoices; ++i) sw(out + i * 4, static_cast<uint32_t>(core->voices[i].env.height()));
  ret(c, 0);
}

void h_rev_type(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  if (!core) return ret(c, E_SAS_NOT_INIT);
  int32_t t = static_cast<int32_t>(arg(c, 1));
  if (t < -1 || t > 8) return ret(c, E_SAS_EFFECT);
  core->reverb.set_type(t);
  ret(c, 0);
}

void h_rev_param(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  if (!core) return ret(c, E_SAS_NOT_INIT);
  core->reverb.set_params(static_cast<int32_t>(arg(c, 1)), static_cast<int32_t>(arg(c, 2)));
  ret(c, 0);
}

void h_rev_evol(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  if (!core) return ret(c, E_SAS_NOT_INIT);
  core->revVolL = static_cast<int32_t>(arg(c, 1));
  core->revVolR = static_cast<int32_t>(arg(c, 2));
  ret(c, 0);
}

void h_rev_von(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  if (!core) return ret(c, E_SAS_NOT_INIT);
  core->dryOn = static_cast<int32_t>(arg(c, 1)) != 0;
  core->wetOn = static_cast<int32_t>(arg(c, 2)) != 0;
  ret(c, 0);
}

void h_get_grain(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  ret(c, core ? static_cast<uint32_t>(core->grain()) : E_SAS_NOT_INIT);
}

void h_set_grain(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  if (!core) return ret(c, E_SAS_NOT_INIT);
  uint32_t g = arg(c, 1);
  if (g < 0x40 || g > 0x800 || (g & 0x1F)) return ret(c, E_SAS_GRAIN);
  core->set_grain(static_cast<int>(g));
  ret(c, 0);
}

void h_get_out_mode(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  ret(c, core ? static_cast<uint32_t>(core->output_mode()) : E_SAS_NOT_INIT);
}

void h_set_out_mode(Cpu& c) {
  sas::Core* core = find_core(arg(c, 0));
  if (!core) return ret(c, E_SAS_NOT_INIT);
  if (arg(c, 1) > 1) return ret(c, E_SAS_OUTMODE);
  core->set_output_mode(static_cast<int>(arg(c, 1)));
  ret(c, 0);
}

}  // namespace

void sas_init() { g_cores.clear(); }

void register_sas() {
  const char* m = "sceSasCore";
  register_hle(m, 0x42778A9F, "__sceSasInit", h_init);
  register_hle(m, 0xA3589D81, "__sceSasCore", h_core);
  register_hle(m, 0x50A14DFC, "__sceSasCoreWithMix", h_core_with_mix);
  register_hle(m, 0x68A46B95, "__sceSasGetEndFlag", h_end_flag);
  register_hle(m, 0x440CA7D8, "__sceSasSetVolume", h_set_volume);
  register_hle(m, 0xAD84D37F, "__sceSasSetPitch", h_set_pitch);
  register_hle(m, 0x99944089, "__sceSasSetVoice", h_set_voice);
  register_hle(m, 0xE1CD9561, "__sceSasSetVoicePCM", h_set_voice_pcm);
  register_hle(m, 0xB7660A23, "__sceSasSetNoise", h_set_noise);
  register_hle(m, 0x019B25EB, "__sceSasSetADSR", h_set_adsr);
  register_hle(m, 0x9EC3676A, "__sceSasSetADSRmode", h_set_adsr_mode);
  register_hle(m, 0x5F9529F6, "__sceSasSetSL", h_set_sl);
  register_hle(m, 0xCBCD4F79, "__sceSasSetSimpleADSR", h_set_simple_adsr);
  register_hle(m, 0x74AE582A, "__sceSasGetEnvelopeHeight", h_env_height);
  register_hle(m, 0x07F58C24, "__sceSasGetAllEnvelopeHeights", h_all_env_heights);
  register_hle(m, 0x76F01ACA, "__sceSasSetKeyOn", h_key_on);
  register_hle(m, 0xA0CF2FA4, "__sceSasSetKeyOff", h_key_off);
  register_hle(m, 0x2C8E6AB3, "__sceSasGetPauseFlag", h_pause_flag);
  register_hle(m, 0x787D04D5, "__sceSasSetPause", h_set_pause);
  register_hle(m, 0xF983B186, "__sceSasRevVON", h_rev_von);
  register_hle(m, 0xD5A229C9, "__sceSasRevEVOL", h_rev_evol);
  register_hle(m, 0x33D4AB37, "__sceSasRevType", h_rev_type);
  register_hle(m, 0x267A6DD2, "__sceSasRevParam", h_rev_param);
  register_hle(m, 0xBD11B7C2, "__sceSasGetGrain", h_get_grain);
  register_hle(m, 0xD1E0A01E, "__sceSasSetGrain", h_set_grain);
  register_hle(m, 0xE175EF66, "__sceSasGetOutputmode", h_get_out_mode);
  register_hle(m, 0xE855BF76, "__sceSasSetOutputmode", h_set_out_mode);
}

}  // namespace k
}  // namespace psp
