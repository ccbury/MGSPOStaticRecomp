#include "atrac3.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psp {
namespace atrac {

namespace {

// ---- RIFF helpers ----
uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

// ---- tables fixed by the ATRAC3 bitstream format ----
struct HuffEntry {
  uint8_t sym, len;
};

const HuffEntry kHuff1[] = {{31, 1}, {32, 3}, {33, 3}, {34, 4}, {35, 4}, {36, 5}, {37, 5}, {38, 5}, {39, 5}};
const HuffEntry kHuff2[] = {{31, 1}, {32, 3}, {30, 3}, {33, 3}, {29, 3}};
const HuffEntry kHuff3[] = {{31, 1}, {32, 3}, {30, 3}, {33, 4}, {29, 4}, {34, 4}, {28, 4}};
const HuffEntry kHuff4[] = {{31, 1}, {32, 3}, {30, 3}, {33, 4}, {29, 4}, {34, 5}, {28, 5}, {35, 5}, {27, 5}};
const HuffEntry kHuff5[] = {{31, 2}, {32, 3}, {30, 3}, {33, 4}, {29, 4}, {34, 4}, {28, 4}, {38, 4},
                            {24, 4}, {35, 5}, {27, 5}, {36, 6}, {26, 6}, {37, 6}, {25, 6}};
const HuffEntry kHuff6[] = {{31, 3}, {32, 4}, {30, 4}, {33, 4}, {29, 4}, {34, 4}, {28, 4}, {46, 4},
                            {16, 4}, {35, 5}, {27, 5}, {36, 5}, {26, 5}, {37, 5}, {25, 5}, {38, 6},
                            {24, 6}, {39, 6}, {23, 6}, {40, 6}, {22, 6}, {41, 6}, {21, 6}, {42, 7},
                            {20, 7}, {43, 7}, {19, 7}, {44, 7}, {18, 7}, {45, 7}, {17, 7}};
const HuffEntry kHuff7[] = {
    {31, 3}, {62, 4}, {0, 4},  {32, 5}, {30, 5}, {33, 5}, {29, 5}, {34, 5}, {28, 5}, {35, 5}, {27, 5},
    {36, 5}, {26, 5}, {37, 6}, {25, 6}, {38, 6}, {24, 6}, {39, 6}, {23, 6}, {40, 6}, {22, 6}, {41, 6},
    {21, 6}, {42, 6}, {20, 6}, {43, 6}, {19, 6}, {44, 6}, {18, 6}, {45, 7}, {17, 7}, {46, 7}, {16, 7},
    {47, 7}, {15, 7}, {48, 7}, {14, 7}, {49, 7}, {13, 7}, {50, 7}, {12, 7}, {51, 7}, {11, 7}, {52, 8},
    {10, 8}, {53, 8}, {9, 8},  {54, 8}, {8, 8},  {55, 8}, {7, 8},  {56, 8}, {6, 8},  {57, 8}, {5, 8},
    {58, 8}, {4, 8},  {59, 8}, {3, 8},  {60, 8}, {2, 8},  {61, 8}, {1, 8}};

const uint8_t kClcLength[8] = {0, 4, 3, 3, 4, 4, 5, 6};
const int8_t kMantissaClc[4] = {0, 1, -2, -1};
const int8_t kMantissaVlc[18] = {0, 0, 0, 1, 0, -1, 1, 0, -1, 0, 1, 1, 1, -1, -1, 1, -1, -1};
const float kInvMaxQuant[8] = {0.0f,        1.0f / 1.5f, 1.0f / 2.5f, 1.0f / 3.5f,
                               1.0f / 4.5f, 1.0f / 7.5f, 1.0f / 15.5f, 1.0f / 31.5f};
const uint16_t kSubband[33] = {0,   8,   16,  24,  32,  40,  48,  56,  64,  80,  96,
                               112, 128, 144, 160, 176, 192, 224, 256, 288, 320, 352,
                               384, 416, 448, 480, 512, 576, 640, 704, 768, 896, 1024};
const float kMatrix[8] = {0.0f, 2.0f, 2.0f, 2.0f, 0.0f, 0.0f, 1.0f, 1.0f};
const float kQmfHalf[24] = {-0.00001461907f,  -0.00009205479f, -0.000056157569f, 0.00030117269f,
                            0.0002422519f,    -0.00085293897f, -0.0005205574f,   0.0020340169f,
                            0.00078333891f,   -0.0042153862f,  -0.00075614988f,  0.0078402944f,
                            -0.000061169922f, -0.01344162f,    0.0024626821f,    0.021736089f,
                            -0.007801671f,    -0.034090221f,   0.01880949f,      0.054326009f,
                            -0.043596379f,    -0.099384367f,   0.13207909f,      0.46424159f};

// Canonical prefix code, codes assigned in table order.
struct Huff {
  struct Item {
    uint32_t code;
    uint8_t len, sym;
  };
  std::vector<Item> items;
  void build(const HuffEntry* e, int n) {
    uint32_t code = 0;
    int prevLen = e[0].len;
    for (int i = 0; i < n; i++) {
      code <<= (e[i].len - prevLen);
      prevLen = e[i].len;
      items.push_back({code, e[i].len, e[i].sym});
      code++;
    }
  }
};

struct Tables {
  Huff huff[7];
  float window[512];
  float sf[64];
  float qmf[48];
  float gainTab1[16];
  float gainTab2[31];
  std::vector<float> cosTab;  // [512][256]

  Tables() {
    huff[0].build(kHuff1, 9);
    huff[1].build(kHuff2, 5);
    huff[2].build(kHuff3, 7);
    huff[3].build(kHuff4, 9);
    huff[4].build(kHuff5, 15);
    huff[5].build(kHuff6, 31);
    huff[6].build(kHuff7, 63);
    const double pi = 3.14159265358979323846;
    for (int i = 0, j = 255; i < 128; i++, j--) {
      double wi = std::sin(((i + 0.5) / 256.0 - 0.5) * pi) + 1.0;
      double wj = std::sin(((j + 0.5) / 256.0 - 0.5) * pi) + 1.0;
      double w = 0.5 * (wi * wi + wj * wj);
      window[i] = window[511 - i] = float(wi / w);
      window[j] = window[511 - j] = float(wj / w);
    }
    for (int i = 0; i < 64; i++) sf[i] = float(std::pow(2.0, (i - 15) / 3.0));
    for (int i = 0; i < 24; i++) qmf[i] = qmf[47 - i] = kQmfHalf[i] * 2.0f;
    for (int i = 0; i < 16; i++) gainTab1[i] = float(std::pow(2.0, 4 - i));
    for (int i = -15; i < 16; i++) gainTab2[i + 15] = float(std::pow(2.0, -1.0 / 8.0 * i));
    cosTab.resize(512 * 256);
    for (int n = 0; n < 512; n++)
      for (int k = 0; k < 256; k++)
        cosTab[n * 256 + k] = float(std::cos(pi / 256.0 * (n + 0.5 + 128.0) * (k + 0.5)));
  }
};

const Tables& tables() {
  static const Tables t;
  return t;
}

struct Bits {
  const uint8_t* d = nullptr;
  size_t nbits = 0, pos = 0;
  void init(const uint8_t* p, size_t bytes) {
    d = p;
    nbits = bytes * 8;
    pos = 0;
  }
  uint32_t get(int n) {
    uint32_t v = 0;
    while (n--) {
      uint32_t b = 0;
      if (pos < nbits) b = (d[pos >> 3] >> (7 - (pos & 7))) & 1;
      pos++;
      v = (v << 1) | b;
    }
    return v;
  }
  int sget(int n) {
    int v = int(get(n));
    return (v << (32 - n)) >> (32 - n);
  }
  bool overrun() const { return pos > nbits; }
};

int read_vlc(Bits& b, const Huff& h) {
  uint32_t code = 0;
  for (int len = 1; len <= 8; len++) {
    code = (code << 1) | b.get(1);
    for (const auto& it : h.items)
      if (it.len == len && it.code == code) return int(it.sym) - 31;
  }
  return 0;
}

struct GainInfo {
  int num = 0;
  int lev[7] = {0}, loc[7] = {0};
};
struct GainBlock {
  GainInfo g[4];
};
struct Tonal {
  int pos = 0, num = 0;
  float coef[8] = {0};
};

}  // namespace

struct Atrac3Decoder::Impl {
  struct Unit {
    int bandsCoded = 0;
    int numComponents = 0;
    float prev[1024] = {0};
    int gcSwitch = 0;
    Tonal comps[64];
    GainBlock gain[2];
    float spectrum[1024] = {0};
    float imdct[1024] = {0};
    float delay1[46] = {0}, delay2[46] = {0}, delay3[46] = {0};
  };
  int channels = 0, blockAlign = 0;
  bool js = false;
  std::vector<Unit> units;
  int wdelay[2][6];
  int mPrev[2][4], mNow[2][4], mNext[2][4];
  float temp[1070];
  std::vector<float> chOut;  // channels * 1024
  std::vector<uint8_t> rev;

  void reset_state() {
    units.assign(channels, Unit());
    for (int p = 0; p < 2; p++) {
      int init[6] = {0, 7, 0, 7, 0, 7};
      std::memcpy(wdelay[p], init, sizeof(init));
      for (int i = 0; i < 4; i++) mPrev[p][i] = mNow[p][i] = mNext[p][i] = 3;
    }
  }

  void read_mantissas(Bits& b, int selector, bool clc, int* out, int count) {
    const Tables& T = tables();
    if (selector == 1) count /= 2;
    if (clc) {
      int nb = kClcLength[selector];
      if (selector > 1) {
        for (int i = 0; i < count; i++) out[i] = nb ? b.sget(nb) : 0;
      } else {
        for (int i = 0; i < count; i++) {
          int c = nb ? int(b.get(nb)) : 0;
          out[i * 2] = kMantissaClc[c >> 2];
          out[i * 2 + 1] = kMantissaClc[c & 3];
        }
      }
    } else if (selector != 1) {
      for (int i = 0; i < count; i++) out[i] = read_vlc(b, T.huff[selector - 1]);
    } else {
      for (int i = 0; i < count; i++) {
        int s = read_vlc(b, T.huff[0]);
        s = std::clamp(s, 0, 8);
        out[i * 2] = kMantissaVlc[s * 2];
        out[i * 2 + 1] = kMantissaVlc[s * 2 + 1];
      }
    }
  }

  int decode_spectrum(Bits& b, float* out) {
    const Tables& T = tables();
    int numSub = int(b.get(5));
    bool clc = b.get(1) != 0;
    int sel[33], sfi[33] = {0};
    for (int i = 0; i <= numSub; i++) sel[i] = int(b.get(3));
    for (int i = 0; i <= numSub; i++)
      if (sel[i]) sfi[i] = int(b.get(6));
    int mant[128];
    int i;
    for (i = 0; i <= numSub; i++) {
      int first = kSubband[i], last = kSubband[i + 1];
      if (sel[i]) {
        read_mantissas(b, sel[i], clc, mant, last - first);
        float scale = T.sf[sfi[i]] * kInvMaxQuant[sel[i]];
        for (int j = 0; first < last; first++, j++) out[first] = mant[j] * scale;
      } else {
        for (; first < last; first++) out[first] = 0;
      }
    }
    for (int f = kSubband[i]; f < 1024; f++) out[f] = 0;
    return numSub;
  }

  bool decode_tonal(Bits& b, Unit& u, int numBands) {
    const Tables& T = tables();
    int nb = int(b.get(5));
    u.numComponents = 0;
    if (nb == 0) return true;
    int modeSel = int(b.get(2));
    if (modeSel == 2) return false;
    bool clc = modeSel & 1;
    int count = 0;
    for (int i = 0; i < nb; i++) {
      int flags[4] = {0};
      for (int k = 0; k <= numBands; k++) flags[k] = int(b.get(1));
      int perComp = int(b.get(3));
      int qstep = int(b.get(3));
      if (qstep <= 1) return false;
      if (modeSel == 3) clc = b.get(1) != 0;
      for (int k = 0; k < (numBands + 1) * 4; k++) {
        if (!flags[k >> 2]) continue;
        int coded = int(b.get(3));
        for (int c = 0; c < coded; c++) {
          int sfi = int(b.get(6));
          if (count >= 64) return false;
          Tonal& t = u.comps[count];
          t.pos = k * 64 + int(b.get(6));
          int nvals = std::min(1024 - t.pos, perComp + 1);
          float scale = T.sf[sfi] * kInvMaxQuant[qstep];
          int m[8];
          read_mantissas(b, qstep, clc, m, nvals);
          t.num = nvals;
          for (int j = 0; j < nvals; j++) t.coef[j] = m[j] * scale;
          count++;
        }
      }
    }
    u.numComponents = count;
    return true;
  }

  bool decode_gain(Bits& b, GainBlock& blk, int numBands) {
    int k;
    for (k = 0; k <= numBands; k++) {
      GainInfo& g = blk.g[k];
      g.num = int(b.get(3));
      for (int j = 0; j < g.num; j++) {
        g.lev[j] = int(b.get(4));
        g.loc[j] = int(b.get(5));
        if (j && g.loc[j] <= g.loc[j - 1]) return false;
      }
    }
    for (; k < 4; k++) blk.g[k].num = 0;
    return true;
  }

  void imlt(float* in, float* out, bool odd) {
    const Tables& T = tables();
    float tmp[256];
    std::memcpy(tmp, in, sizeof(tmp));
    if (odd)
      for (int i = 0; i < 128; i++) std::swap(tmp[i], tmp[255 - i]);
    for (int n = 0; n < 512; n++) {
      const float* c = &T.cosTab[n * 256];
      float s = 0;
      for (int k = 0; k < 256; k++) s += tmp[k] * c[k];
      out[n] = -s * T.window[n];
    }
  }

  void gain_comp(const float* in, float* prev, const GainInfo& now, const GainInfo& next, float* out) {
    const Tables& T = tables();
    float scale = next.num ? T.gainTab1[next.lev[0]] : 1.0f;
    int pos = 0;
    if (!now.num) {
      for (; pos < 256; pos++) out[pos] = in[pos] * scale + prev[pos];
    } else {
      for (int i = 0; i < now.num; i++) {
        int last = now.loc[i] << 3;
        float lev = T.gainTab1[now.lev[i]];
        float inc = T.gainTab2[(i + 1 < now.num ? now.lev[i + 1] : 4) - now.lev[i] + 15];
        for (; pos < last; pos++) out[pos] = (in[pos] * scale + prev[pos]) * lev;
        for (; pos < last + 8; pos++) {
          out[pos] = (in[pos] * scale + prev[pos]) * lev;
          lev *= inc;
        }
      }
      for (; pos < 256; pos++) out[pos] = in[pos] * scale + prev[pos];
    }
    std::memcpy(prev, in + 256, 256 * sizeof(float));
  }

  bool decode_unit(Bits& b, Unit& u, float* out, int ch) {
    GainBlock& g1 = u.gain[u.gcSwitch];
    GainBlock& g2 = u.gain[1 - u.gcSwitch];
    if (js && (ch & 1)) {
      if (b.get(2) != 3) return false;
    } else {
      if (b.get(6) != 0x28) return false;
    }
    u.bandsCoded = int(b.get(2));
    if (!decode_gain(b, g2, u.bandsCoded)) return false;
    if (!decode_tonal(b, u, u.bandsCoded)) return false;
    int numSub = decode_spectrum(b, u.spectrum);
    int lastTonal = -1;
    for (int i = 0; i < u.numComponents; i++) {
      Tonal& t = u.comps[i];
      lastTonal = std::max(t.pos + t.num, lastTonal);
      for (int j = 0; j < t.num; j++) u.spectrum[t.pos + j] += t.coef[j];
    }
    int numBands = (kSubband[numSub + 1] - 1) >> 8;
    if (lastTonal >= 0) numBands = std::max((lastTonal + 256) >> 8, numBands);
    for (int band = 0; band < 4; band++) {
      if (band <= numBands)
        imlt(&u.spectrum[band * 256], u.imdct, band & 1);
      else
        std::memset(u.imdct, 0, 512 * sizeof(float));
      gain_comp(u.imdct, &u.prev[band * 256], g1.g[band], g2.g[band], &out[band * 256]);
    }
    u.gcSwitch ^= 1;
    return !b.overrun();
  }

  static float interp(float o, float n, int i) { return o + i * 0.125f * (n - o); }

  void reverse_matrixing(float* s1, float* s2, const int* prev, const int* cur) {
    for (int i = 0, band = 0; band < 1024; band += 256, i++) {
      int a = prev[i], c = cur[i];
      int n = band;
      if (a != c) {
        float m1l = kMatrix[a * 2], m1r = kMatrix[a * 2 + 1];
        float m2l = kMatrix[c * 2], m2r = kMatrix[c * 2 + 1];
        for (; n < band + 8; n++) {
          float x = s1[n], y = s2[n];
          y = x * interp(m1l, m2l, n - band) + y * interp(m1r, m2r, n - band);
          s1[n] = y;
          s2[n] = x * 2.0f - y;
        }
      }
      switch (c) {
        case 0:
          for (; n < band + 256; n++) {
            float x = s1[n], y = s2[n];
            s1[n] = y * 2.0f;
            s2[n] = (x - y) * 2.0f;
          }
          break;
        case 1:
          for (; n < band + 256; n++) {
            float x = s1[n], y = s2[n];
            s1[n] = (x + y) * 2.0f;
            s2[n] = y * -2.0f;
          }
          break;
        default:
          for (; n < band + 256; n++) {
            float x = s1[n], y = s2[n];
            s1[n] = x + y;
            s2[n] = x - y;
          }
      }
    }
  }

  static void weights(int index, int flag, float w[2]) {
    if (index == 7) {
      w[0] = w[1] = 1.0f;
    } else {
      w[0] = (index & 7) / 7.0f;
      w[1] = std::sqrt(2.0f - w[0] * w[0]);
      if (flag) std::swap(w[0], w[1]);
    }
  }

  void channel_weighting(float* s1, float* s2, const int* p) {
    if (p[1] == 7 && p[3] == 7) return;
    float w[2][2];
    weights(p[1], p[0], w[0]);
    weights(p[3], p[2], w[1]);
    for (int band = 256; band < 1024; band += 256) {
      int n = band;
      for (; n < band + 8; n++) {
        s1[n] *= interp(w[0][0], w[0][1], n - band);
        s2[n] *= interp(w[1][0], w[1][1], n - band);
      }
      for (; n < band + 256; n++) {
        s1[n] *= w[1][0];
        s2[n] *= w[1][1];
      }
    }
  }

  void iqmf(float* lo, float* hi, int nIn, float* out, float* delay) {
    const Tables& T = tables();
    std::memcpy(temp, delay, 46 * sizeof(float));
    float* p3 = temp + 46;
    for (int i = 0; i < nIn; i += 2) {
      p3[2 * i + 0] = lo[i] + hi[i];
      p3[2 * i + 1] = lo[i] - hi[i];
      p3[2 * i + 2] = lo[i + 1] + hi[i + 1];
      p3[2 * i + 3] = lo[i + 1] - hi[i + 1];
    }
    float* p1 = temp;
    for (int j = nIn; j != 0; j--) {
      float s1 = 0, s2 = 0;
      for (int i = 0; i < 48; i += 2) {
        s1 += p1[i] * T.qmf[i];
        s2 += p1[i + 1] * T.qmf[i + 1];
      }
      out[0] = s2;
      out[1] = s1;
      p1 += 2;
      out += 2;
    }
    std::memcpy(delay, temp + nIn * 2, 46 * sizeof(float));
  }

  bool decode(const uint8_t* data, int16_t* pcm) {
    Bits b;
    if (js) {
      int pairBytes = (blockAlign / channels) * 2;
      for (int ch = 0; ch < channels; ch += 2) {
        int pair = ch / 2;
        const uint8_t* pd = data + pair * pairBytes;
        b.init(pd, pairBytes);
        if (!decode_unit(b, units[ch], &chOut[ch * 1024], ch)) return false;
        rev.resize(pairBytes);
        for (int i = 0; i < pairBytes; i++) rev[i] = pd[pairBytes - 1 - i];
        int skip = 0;
        while (rev[skip] == 0xF8) {
          skip++;
          if (skip + 4 >= pairBytes) return false;
        }
        b.init(rev.data() + skip, pairBytes - skip);
        std::memmove(wdelay[pair], &wdelay[pair][2], 4 * sizeof(int));
        wdelay[pair][4] = int(b.get(1));
        wdelay[pair][5] = int(b.get(3));
        for (int i = 0; i < 4; i++) {
          mPrev[pair][i] = mNow[pair][i];
          mNow[pair][i] = mNext[pair][i];
          mNext[pair][i] = int(b.get(2));
        }
        if (!decode_unit(b, units[ch + 1], &chOut[(ch + 1) * 1024], ch + 1)) return false;
        reverse_matrixing(&chOut[ch * 1024], &chOut[(ch + 1) * 1024], mPrev[pair], mNow[pair]);
        channel_weighting(&chOut[ch * 1024], &chOut[(ch + 1) * 1024], wdelay[pair]);
      }
    } else {
      int per = blockAlign / channels;
      for (int ch = 0; ch < channels; ch++) {
        b.init(data + ch * per, per);
        if (!decode_unit(b, units[ch], &chOut[ch * 1024], ch)) return false;
      }
    }
    for (int ch = 0; ch < channels; ch++) {
      float* p1 = &chOut[ch * 1024];
      float* p2 = p1 + 256;
      float* p3 = p2 + 256;
      float* p4 = p3 + 256;
      iqmf(p1, p2, 256, p1, units[ch].delay1);
      iqmf(p4, p3, 256, p3, units[ch].delay2);
      iqmf(p1, p3, 512, p1, units[ch].delay3);
    }
    for (int i = 0; i < 1024; i++)
      for (int ch = 0; ch < channels; ch++) {
        float v = std::nearbyint(chOut[ch * 1024 + i]);
        pcm[i * channels + ch] = int16_t(std::clamp(v, -32768.0f, 32767.0f));
      }
    return true;
  }
};

Atrac3Decoder::Atrac3Decoder() = default;
Atrac3Decoder::~Atrac3Decoder() = default;

bool Atrac3Decoder::init(int channels, int blockAlign, bool jointStereo) {
  if (channels < 1 || channels > 2 || blockAlign <= 0 || blockAlign > 4096) return false;
  if (jointStereo && (channels & 1)) return false;
  impl_ = std::make_unique<Impl>();
  impl_->channels = channels;
  impl_->blockAlign = blockAlign;
  impl_->js = jointStereo;
  impl_->chOut.assign(size_t(channels) * 1024, 0.0f);
  impl_->reset_state();
  return true;
}

void Atrac3Decoder::reset() {
  if (impl_) impl_->reset_state();
}

bool Atrac3Decoder::decode_frame(const uint8_t* data, int16_t* out) {
  return impl_ && impl_->decode(data, out);
}

bool parse_at3(const uint8_t* d, size_t size, At3Info& o) {
  o = At3Info();
  if (size < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return false;
  size_t p = 12;
  bool haveFmt = false, haveData = false;
  while (p + 8 <= size) {
    uint32_t len = rd32(d + p + 4);
    const uint8_t* c = d + p + 8;
    size_t avail = size - (p + 8);
    if (!std::memcmp(d + p, "fmt ", 4) && len >= 16 && avail >= 16) {
      uint16_t tag = rd16(c);
      o.channels = rd16(c + 2);
      o.sampleRate = int(rd32(c + 4));
      o.blockAlign = rd16(c + 12);
      if (tag == 0x270) {
        o.codec = Codec::Atrac3;
        // coding mode lives in the 14 byte extension after cbSize
        if (len >= 32 && avail >= 32) o.jointStereo = rd16(c + 18 + 6) != 0;
      } else if (tag == 0xFFFE) {
        o.codec = Codec::Atrac3Plus;
      }
      haveFmt = true;
    } else if (!std::memcmp(d + p, "fact", 4) && len >= 4 && avail >= 4) {
      o.totalSamples = rd32(c);
      if (len >= 12 && avail >= 12) o.skipSamples = rd32(c + 8);
    } else if (!std::memcmp(d + p, "smpl", 4) && len >= 36 && avail >= 36) {
      uint32_t n = rd32(c + 28);
      if (n >= 1 && len >= 36 + 24 && avail >= 36 + 24) {
        o.hasLoop = true;
        o.loopStart = rd32(c + 36 + 8);
        o.loopEnd = rd32(c + 36 + 12);
      }
    } else if (!std::memcmp(d + p, "data", 4)) {
      o.dataOffset = uint32_t(p + 8);
      o.dataSize = uint32_t(std::min<size_t>(len, avail));
      haveData = true;
      break;
    }
    p += 8 + size_t(len) + (len & 1);
  }
  return haveFmt && haveData && o.codec != Codec::Unknown && o.channels > 0 && o.blockAlign > 0;
}

}  // namespace atrac
}  // namespace psp
