#include "compat.h"

float ff_sine_128[128];
float ff_sine_64[64];

void ff_init_ff_sine_windows(int bits) {
  int n = 1 << bits;
  float* w = n == 128 ? ff_sine_128 : ff_sine_64;
  for (int i = 0; i < n; ++i) w[i] = static_cast<float>(std::sin((i + 0.5) / (2.0 * n) * M_PI));
}

static void fmul(float* dst, const float* a, const float* b, int len) {
  for (int i = 0; i < len; ++i) dst[i] = a[i] * b[i];
}
static void fmul_reverse(float* dst, const float* a, const float* b, int len) {
  for (int i = 0; i < len; ++i) dst[i] = a[i] * b[len - 1 - i];
}
static void fmul_scalar(float* dst, const float* src, float mul, int len) {
  for (int i = 0; i < len; ++i) dst[i] = src[i] * mul;
}
static void fmac_scalar(float* dst, const float* src, float mul, int len) {
  for (int i = 0; i < len; ++i) dst[i] += src[i] * mul;
}

AVFloatDSPContext* compat_float_dsp() {
  static AVFloatDSPContext ctx = {fmul, fmul_reverse, fmul_scalar, fmac_scalar};
  return &ctx;
}

AVTXContext* compat_imdct_init(int n, bool full, float scale) {
  auto* c = new AVTXContext;
  c->n = n;
  c->full = full;
  c->scale = scale;
  int outs = full ? 2 * n : n;
  int first = full ? 0 : n / 2;
  c->table.resize(size_t(outs) * n);
  for (int i = 0; i < outs; ++i)
    for (int k = 0; k < n; ++k)
      c->table[size_t(i) * n + k] = static_cast<float>(std::cos(M_PI / n * (i + first + 0.5 + n / 2.0) * (k + 0.5)));
  return c;
}

void compat_imdct_run(AVTXContext* c, void* out, void* in, ptrdiff_t stride) {
  const float* src = static_cast<const float*>(in);
  float* dst = static_cast<float*>(out);
  int outs = c->full ? 2 * c->n : c->n;
  ptrdiff_t step = stride / static_cast<ptrdiff_t>(sizeof(float));
  for (int i = 0; i < outs; ++i) {
    const float* row = &c->table[size_t(i) * c->n];
    float acc = 0;
    for (int k = 0; k < c->n; ++k) acc += src[k * step] * row[k];
    dst[i * step] = acc * c->scale;
  }
}
