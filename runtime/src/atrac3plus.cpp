#include "atrac3plus.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "atrac3p/atrac3plus.h"

namespace psp {
namespace atrac {

namespace {
std::once_flag g_once;
void init_static() {
  ff_atrac_generate_tables();
  ff_atrac3p_init_vlcs();
  ff_atrac3p_init_dsp_static();
}
}  // namespace

struct Atrac3PlusDecoder::Impl {
  int channels = 0;
  int numBlocks = 0;
  uint8_t blockTypes[2] = {};
  AVFloatDSPContext* fdsp = nullptr;
  AtracGCContext gainc{};
  AVTXContext* mdct = nullptr;
  AVTXContext* ipqf = nullptr;
  std::vector<Atrac3pChanUnitCtx> units;
  float samples[2][ATRAC3P_FRAME_SAMPLES];
  float mdctBuf[2][ATRAC3P_FRAME_SAMPLES];
  float timeBuf[2][ATRAC3P_FRAME_SAMPLES];
  float outBuf[2][ATRAC3P_FRAME_SAMPLES];

  ~Impl() {
    delete mdct;
    delete ipqf;
  }

  void setup(int ch) {
    channels = ch;
    numBlocks = 1;
    blockTypes[0] = ch == 1 ? CH_UNIT_MONO : CH_UNIT_STEREO;
    delete mdct;
    delete ipqf;
    ipqf = compat_imdct_init(16, false, 32.0f / 32768.0f);
    mdct = compat_imdct_init(128, true, -1.0f);
    fdsp = compat_float_dsp();
    ff_atrac_init_gain_compensation(&gainc, 6, 2);
    units.assign(numBlocks, Atrac3pChanUnitCtx{});
    for (auto& u : units) {
      for (int ch2 = 0; ch2 < 2; ++ch2) {
        Atrac3pChanParams& p = u.channels[ch2];
        p.ch_num = ch2;
        p.wnd_shape = &p.wnd_shape_hist[0][0];
        p.wnd_shape_prev = &p.wnd_shape_hist[1][0];
        p.gain_data = &p.gain_data_hist[0][0];
        p.gain_data_prev = &p.gain_data_hist[1][0];
        p.tones_info = &p.tones_info_hist[0][0];
        p.tones_info_prev = &p.tones_info_hist[1][0];
      }
      u.waves_info = &u.wave_synth_hist[0];
      u.waves_info_prev = &u.wave_synth_hist[1];
    }
  }

  void decode_residual_spectrum(Atrac3pChanUnitCtx* u, int nch) {
    float(*out)[ATRAC3P_FRAME_SAMPLES] = samples;
    if (u->mute_flag) {
      for (int ch = 0; ch < nch; ++ch) std::memset(out[ch], 0, sizeof(out[ch]));
      return;
    }
    int sbRng[ATRAC3P_SUBBANDS] = {};
    int rng = 0;
    for (int qu = 0; qu < u->used_quant_units; ++qu) rng += u->channels[0].qu_sf_idx[qu] + u->channels[1].qu_sf_idx[qu];
    for (int sb = 0; sb < u->num_coded_subbands; ++sb, rng += 128) sbRng[sb] = rng & 0x3FC;

    for (int ch = 0; ch < nch; ++ch) {
      std::memset(out[ch], 0, sizeof(out[ch]));
      for (int qu = 0; qu < u->used_quant_units; ++qu) {
        const int16_t* src = &u->channels[ch].spectrum[ff_atrac3p_qu_to_spec_pos[qu]];
        float* dst = &out[ch][ff_atrac3p_qu_to_spec_pos[qu]];
        int n = ff_atrac3p_qu_to_spec_pos[qu + 1] - ff_atrac3p_qu_to_spec_pos[qu];
        if (u->channels[ch].qu_wordlen[qu] > 0) {
          float q = ff_atrac3p_sf_tab[u->channels[ch].qu_sf_idx[qu]] * ff_atrac3p_mant_tab[u->channels[ch].qu_wordlen[qu]];
          for (int i = 0; i < n; ++i) dst[i] = src[i] * q;
        }
      }
      for (int sb = 0; sb < u->num_coded_subbands; ++sb)
        ff_atrac3p_power_compensation(u, fdsp, ch, &out[ch][0], sbRng[sb], sb);
    }

    if (u->unit_type == CH_UNIT_STEREO) {
      for (int sb = 0; sb < u->num_coded_subbands; ++sb) {
        float* l = &out[0][sb * ATRAC3P_SUBBAND_SAMPLES];
        float* r = &out[1][sb * ATRAC3P_SUBBAND_SAMPLES];
        if (u->swap_channels[sb])
          for (int i = 0; i < ATRAC3P_SUBBAND_SAMPLES; ++i) std::swap(l[i], r[i]);
        if (u->negate_coeffs[sb])
          for (int i = 0; i < ATRAC3P_SUBBAND_SAMPLES; ++i) r[i] = -r[i];
      }
    }
  }

  void reconstruct_frame(Atrac3pChanUnitCtx* u, int nch) {
    for (int ch = 0; ch < nch; ++ch) {
      Atrac3pChanParams& cp = u->channels[ch];
      for (int sb = 0; sb < u->num_subbands; ++sb) {
        const int off = sb * ATRAC3P_SUBBAND_SAMPLES;
        ff_atrac3p_imdct(fdsp, mdct, compat_imdct_run, &samples[ch][off], &mdctBuf[ch][off],
                         (cp.wnd_shape_prev[sb] << 1) + cp.wnd_shape[sb], sb);
        ff_atrac_gain_compensation(&gainc, &mdctBuf[ch][off], &u->prev_buf[ch][off], &cp.gain_data_prev[sb],
                                   &cp.gain_data[sb], ATRAC3P_SUBBAND_SAMPLES, &timeBuf[ch][off]);
      }
      const size_t rest = size_t(ATRAC3P_SUBBANDS - u->num_subbands) * ATRAC3P_SUBBAND_SAMPLES;
      const size_t from = size_t(u->num_subbands) * ATRAC3P_SUBBAND_SAMPLES;
      std::memset(&u->prev_buf[ch][from], 0, rest * sizeof(float));
      std::memset(&timeBuf[ch][from], 0, rest * sizeof(float));

      if (u->waves_info->tones_present || u->waves_info_prev->tones_present) {
        for (int sb = 0; sb < u->num_subbands; ++sb)
          if (cp.tones_info[sb].num_wavs || cp.tones_info_prev[sb].num_wavs)
            ff_atrac3p_generate_tones(u, fdsp, ch, sb, &timeBuf[ch][sb * 128]);
      }

      ff_atrac3p_ipqf(ipqf, compat_imdct_run, &u->ipqf_ctx[ch], &timeBuf[ch][0], &outBuf[ch][0]);
    }

    for (int ch = 0; ch < nch; ++ch) {
      Atrac3pChanParams& cp = u->channels[ch];
      std::swap(cp.wnd_shape, cp.wnd_shape_prev);
      std::swap(cp.gain_data, cp.gain_data_prev);
      std::swap(cp.tones_info, cp.tones_info_prev);
    }
    std::swap(u->waves_info, u->waves_info_prev);
  }

  bool decode(const uint8_t* data, size_t size, int16_t* out) {
    AVCodecContext avctx;
    GetBitContext gb;
    init_get_bits8(&gb, data, static_cast<int>(size));
    if (get_bits1(&gb)) return false;

    int block = 0;
    int outCh = 0;
    int id;
    while (get_bits_left(&gb) >= 2 && (id = static_cast<int>(get_bits(&gb, 2))) != CH_UNIT_TERMINATOR) {
      if (id == CH_UNIT_EXTENSION) return false;
      if (block >= numBlocks || blockTypes[block] != id) return false;
      Atrac3pChanUnitCtx* u = &units[block];
      u->unit_type = id;
      int nch = id + 1;
      if (ff_atrac3p_decode_channel_unit(&gb, u, nch, &avctx) < 0) return false;
      decode_residual_spectrum(u, nch);
      reconstruct_frame(u, nch);
      for (int c = 0; c < nch; ++c) {
        for (int i = 0; i < ATRAC3P_FRAME_SAMPLES; ++i) {
          float v = outBuf[c][i] * 32768.0f;
          v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
          out[size_t(i) * channels + outCh + c] = static_cast<int16_t>(std::lrintf(v));
        }
      }
      ++block;
      outCh += nch;
    }
    return outCh == channels;
  }
};

Atrac3PlusDecoder::Atrac3PlusDecoder() : impl_(new Impl) {}
Atrac3PlusDecoder::~Atrac3PlusDecoder() = default;

bool Atrac3PlusDecoder::init(int channels) {
  if (channels < 1 || channels > 2) return false;
  std::call_once(g_once, init_static);
  impl_->setup(channels);
  reset();
  return true;
}

void Atrac3PlusDecoder::reset() {
  if (impl_->channels) impl_->setup(impl_->channels);
}

bool Atrac3PlusDecoder::decode_frame(const uint8_t* data, size_t size, int16_t* out) {
  if (!impl_->channels || !impl_->decode(data, size, out)) {
    std::fill(out, out + size_t(kPlusSamplesPerFrame) * std::max(impl_->channels, 1), int16_t(0));
    return false;
  }
  return true;
}

}  // namespace atrac
}  // namespace psp
