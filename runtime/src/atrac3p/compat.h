// Minimal stand-ins for the handful of FFmpeg helpers the ATRAC3+ bitstream parser and DSP code lean on
// (bit reader, VLC lookup, float vector ops, sine windows and the two small MDCT variants). Everything the
// vendored files need is declared here so they can stay as close to upstream as possible.
#pragma once
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define av_cold
#define DECLARE_ALIGNED(a, t, n) t n
#define LOCAL_ALIGNED(a, t, v, s) t v s
#define LOCAL_ALIGNED_32(t, v, s) t v s
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
#define FFSWAP(type, a, b) \
  do {                     \
    type SWAP_tmp = b;     \
    b = a;                 \
    a = SWAP_tmp;          \
  } while (0)
#define av_assert0(x) ((void)0)

#define AVERROR(e) (-(e))
#define AVERROR_INVALIDDATA (-1094995529)
#define AVERROR_PATCHWELCOME (-1163346256)
#define AV_LOG_ERROR 16

struct AVCodecContext {
  int64_t frame_num = 0;
};

template <typename... A>
inline void av_log(AVCodecContext*, int, const char*, A...) {}
template <typename... A>
inline void avpriv_report_missing_feature(AVCodecContext*, const char*, A...) {}

inline int sign_extend(int val, unsigned bits) {
  unsigned shift = 32 - bits;
  return static_cast<int>(static_cast<unsigned>(val) << shift) >> shift;
}
inline int av_log2(unsigned v) {
  int n = 0;
  while (v >>= 1) ++n;
  return n;
}
inline unsigned av_zero_extend(unsigned v, unsigned bits) { return bits >= 32 ? v : v & ((1u << bits) - 1); }

struct GetBitContext {
  const uint8_t* buf = nullptr;
  int sizeBits = 0;
  int idx = 0;
};

inline int init_get_bits8(GetBitContext* gb, const uint8_t* data, int bytes) {
  gb->buf = data;
  gb->sizeBits = bytes * 8;
  gb->idx = 0;
  return 0;
}
inline int get_bits_left(const GetBitContext* gb) { return gb->sizeBits - gb->idx; }
inline unsigned peek_bits(const GetBitContext* gb, int n) {
  if (n <= 0) return 0;
  uint64_t v = 0;
  int byte = gb->idx >> 3, total = (gb->sizeBits + 7) >> 3;
  for (int k = 0; k < 5; ++k) v = (v << 8) | (byte + k < total ? gb->buf[byte + k] : 0);
  return static_cast<unsigned>((v >> (40 - (gb->idx & 7) - n)) & ((1ull << n) - 1));
}
inline unsigned get_bits(GetBitContext* gb, int n) {
  unsigned v = peek_bits(gb, n);
  gb->idx += n;
  return v;
}
inline unsigned get_bits1(GetBitContext* gb) { return get_bits(gb, 1); }
inline unsigned get_bitsz(GetBitContext* gb, int n) { return n ? get_bits(gb, n) : 0; }

struct VLCElem {
  int16_t sym;
  int16_t len;
};
struct VLC {
  VLCElem* table = nullptr;
  int bits = 0;
  int table_allocated = 0;
};
#define VLC_INIT_USE_STATIC 1

// Builds a single level lookup table for a canonical code. Code lengths must be sorted ascending, the first
// code of each length follows on from the last code of the previous one.
inline int ff_vlc_init_from_lengths(VLC* vlc, int nbBits, int nbCodes, const uint8_t* lens, int, const uint8_t* symbols, int,
                                    int, int, int, void*) {
  vlc->bits = nbBits;
  const unsigned size = 1u << nbBits;
  for (unsigned i = 0; i < size; ++i) vlc->table[i] = VLCElem{-1, 0};
  unsigned code = 0;
  for (int i = 0; i < nbCodes; ++i) {
    unsigned span = 1u << (nbBits - lens[i]);
    if (code + span > size) return -1;
    for (unsigned j = 0; j < span; ++j) vlc->table[code + j] = VLCElem{symbols[i], lens[i]};
    code += span;
  }
  return 0;
}

inline int get_vlc2(GetBitContext* gb, const VLCElem* table, int bits, int) {
  VLCElem e = table[peek_bits(gb, bits)];
  if (e.len <= 0) {
    gb->idx += bits;
    return -1;
  }
  gb->idx += e.len;
  return e.sym;
}

struct AVFloatDSPContext {
  void (*vector_fmul)(float* dst, const float* a, const float* b, int len);
  void (*vector_fmul_reverse)(float* dst, const float* a, const float* b, int len);
  void (*vector_fmul_scalar)(float* dst, const float* src, float mul, int len);
  void (*vector_fmac_scalar)(float* dst, const float* src, float mul, int len);
};
AVFloatDSPContext* compat_float_dsp();

extern float ff_sine_128[128];
extern float ff_sine_64[64];
void ff_init_ff_sine_windows(int n);

// Inverse MDCT with n coefficients. A full transform yields 2n samples, otherwise the middle n samples.
struct AVTXContext {
  int n = 0;
  bool full = false;
  float scale = 1.0f;
  std::vector<float> table;  // [out][k] cosine basis, row length n
};
typedef void (*av_tx_fn)(AVTXContext*, void* out, void* in, ptrdiff_t stride);
AVTXContext* compat_imdct_init(int n, bool full, float scale);
void compat_imdct_run(AVTXContext* ctx, void* out, void* in, ptrdiff_t stride);
