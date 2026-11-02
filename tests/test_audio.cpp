// Unit tests for the SAS synth, the ATRAC3 container parser/decoder and the sceSasCore/sceAtrac HLE glue.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include <psp/kernel.h>
#include <psp/runtime.h>

#include "atrac3.h"
#include "atrac3plus.h"
#include "sas_core.h"

using namespace psp;

static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

static const uint32_t kBuf = 0x08A00000;

static void put_vag_block(uint32_t addr, int shift, int flags, int nibble) {
  uint8_t b[16] = {};
  b[0] = static_cast<uint8_t>(shift);
  b[1] = static_cast<uint8_t>(flags);
  for (int i = 2; i < 16; ++i) b[i] = static_cast<uint8_t>(nibble | (nibble << 4));
  mem_write(addr, b, 16);
}

static void test_vag() {
  put_vag_block(kBuf, 12, 0, 4);
  put_vag_block(kBuf + 16, 12, 1, 4);
  sas::VagDecoder d;
  d.start(kBuf, 32, false);
  int16_t s = 0;
  int n = 0;
  while (d.next(s)) {
    CHECK(s == 4);
    if (++n > 100) break;
  }
  CHECK(n == 56);
}

static void test_envelope() {
  sas::Envelope e;
  e.key_on();
  CHECK(e.active());
  for (int i = 0; i < 10; ++i) e.step();
  CHECK(e.height() > 0);
  e.key_off();
  for (int i = 0; i < 100000 && e.active(); ++i) e.step();
  CHECK(!e.active());
  CHECK(e.height() == 0);
}

static void test_mix_and_end() {
  const int count = 64;
  put_vag_block(kBuf, 12, 0, 4);
  put_vag_block(kBuf + 16, 12, 1, 4);
  sas::Core core;
  core.init(64, 8, sas::Mixed, 44100);
  CHECK(core.end_flags() == 0xFFu);
  sas::Voice& v = core.voices[0];
  v.type = sas::VoiceType::Vag;
  v.vagAddr = kBuf;
  v.vagSize = 32;
  v.loop = false;
  v.volL = v.volR = 0x1000;
  v.pitch = 0x800;
  v.env.set_rates(0x1F, 0x7FFFFFFF, 0x7FFFFFFF, 0, 0x40000000);
  v.key_on();
  std::vector<int16_t> out(count * 2);
  core.mix(out.data(), nullptr, 0, 0);
  CHECK((core.end_flags() & 1) == 0);
  core.mix(out.data(), nullptr, 0, 0);
  CHECK((core.end_flags() & 1) != 0);

  // silence once the voice is done, and CoreWithMix style input passes through
  std::vector<int16_t> in(count * 2, 1000);
  core.mix(out.data(), in.data(), 0x1000, 0x1000);
  CHECK(out[0] == 1000 && out[1] == 1000);
}

static void test_simple_adsr() {
  sas::SimpleAdsr a = sas::decode_simple_adsr(0, 0);
  CHECK(a.sustainLevel >= 0);
}

static void test_atrac_parse() {
  std::vector<uint8_t> f;
  auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f.push_back(uint8_t(v >> (8 * i))); };
  auto p16 = [&](uint16_t v) { f.push_back(uint8_t(v)); f.push_back(uint8_t(v >> 8)); };
  f.insert(f.end(), {'R', 'I', 'F', 'F'});
  p32(0);
  f.insert(f.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  p32(32);
  p16(0x270); p16(2); p32(44100); p32(0); p16(192); p16(0); p16(14);
  p16(1); p32(0x1000); p16(1); p16(1); p16(1); p16(0);
  f.insert(f.end(), {'f', 'a', 'c', 't'});
  p32(8); p32(2048); p32(0);
  f.insert(f.end(), {'d', 'a', 't', 'a'});
  p32(192 * 3);
  f.resize(f.size() + 192 * 3, 0);

  atrac::At3Info i;
  CHECK(atrac::parse_at3(f.data(), f.size(), i));
  CHECK(i.codec == atrac::Codec::Atrac3);
  CHECK(i.channels == 2 && i.blockAlign == 192 && i.jointStereo);
  CHECK(i.totalSamples == 2048);
  CHECK(i.frameCount() == 3);

  // a zeroed frame has no valid sound unit header and must be rejected rather than crash
  atrac::Atrac3Decoder d;
  CHECK(d.init(2, 192, true));
  int16_t pcm[2048];
  CHECK(!d.decode_frame(f.data() + i.dataOffset, pcm));

  CHECK(!atrac::parse_at3(f.data(), 8, i));
}

static const ImportEntry kImports[] = {
    {0x08800000, "sceSasCore", 0x42778A9F},
    {0x08800008, "sceSasCore", 0xA3589D81},
    {0x08800010, "sceSasCore", 0x68A46B95},
    {0x08800018, "sceAtrac3plus", 0x132F1ECA},
};

static uint32_t call(int idx, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t t0 = 0) {
  Cpu c;
  c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[7] = a3; c.r[8] = t0;
  hle(c, idx);
  return c.r[2];
}

static void test_hle() {
  const uint32_t core = 0x08A10000, out = 0x08A20000;
  CHECK(call(0, core, 0x40, 8, 0, 44100) == 0);
  CHECK(call(0, core, 0x41, 8, 0, 44100) != 0);  // grain must be a multiple of 0x20
  CHECK(call(1, core, out) == 0);
  CHECK(call(2, core) == 0xFFu);
  CHECK(call(1, 0x08A30000, out) != 0);  // unknown core
  CHECK(call(3, 1, 1) == 0);
}

static void test_atrac3plus() {
  using namespace psp::atrac;
  Atrac3PlusDecoder dec;
  CHECK(!dec.init(0));
  CHECK(!dec.init(3));
  CHECK(dec.init(2));
  std::vector<int16_t> out(kPlusSamplesPerFrame * 2, 123);
  // Terminator-only frame: no channel data, output must be silent.
  uint8_t term[376] = {0x60};
  dec.decode_frame(term, sizeof(term), out.data());
  bool silent = true;
  for (int16_t s : out) silent = silent && s == 0;
  CHECK(silent);
  // Start bit set: corrupt, must not crash and must output silence.
  uint8_t bad[376];
  std::memset(bad, 0xFF, sizeof(bad));
  std::fill(out.begin(), out.end(), int16_t(55));
  CHECK(!dec.decode_frame(bad, sizeof(bad), out.data()));
  silent = true;
  for (int16_t s : out) silent = silent && s == 0;
  CHECK(silent);
  // Truncated input
  CHECK(!dec.decode_frame(term, 0, out.data()));
  dec.reset();
  CHECK(dec.init(1));
  dec.decode_frame(term, sizeof(term), out.data());

  // Optional comparison against a reference decode of real data. MGSPO_AT3_FILE points at an .at3 and
  // MGSPO_AT3_REF at a 16 bit stereo WAV produced by another decoder.
  const char* f = std::getenv("MGSPO_AT3_FILE");
  const char* r = std::getenv("MGSPO_AT3_REF");
  if (f && r) {
    auto slurp = [](const char* p) {
      std::vector<uint8_t> v;
      if (FILE* fp = std::fopen(p, "rb")) {
        uint8_t tmp[65536];
        size_t n;
        while ((n = std::fread(tmp, 1, sizeof(tmp), fp)) > 0) v.insert(v.end(), tmp, tmp + n);
        std::fclose(fp);
      }
      return v;
    };
    std::vector<uint8_t> in = slurp(f), ref = slurp(r);
    At3Info info;
    CHECK(parse_at3(in.data(), in.size(), info) && info.codec == Codec::Atrac3Plus && ref.size() > 44);
    Atrac3PlusDecoder d;
    CHECK(d.init(info.channels));
    std::vector<int16_t> o(size_t(kPlusSamplesPerFrame) * info.channels);
    size_t dataOff = 0;
    for (size_t i = 12; i + 8 <= ref.size();) {
      uint32_t sz = ref[i + 4] | ref[i + 5] << 8 | ref[i + 6] << 16 | uint32_t(ref[i + 7]) << 24;
      if (!std::memcmp(&ref[i], "data", 4)) {
        dataOff = i + 8;
        break;
      }
      i += 8 + sz + (sz & 1);
    }
    CHECK(dataOff != 0);
    if (!dataOff) return;
    const int16_t* rs = reinterpret_cast<const int16_t*>(ref.data() + dataOff);
    size_t refSamples = (ref.size() - dataOff) / 2, pos = 0;
    int maxDiff = 0;
    for (uint32_t i = 0; i < info.frameCount(); ++i) {
      CHECK(d.decode_frame(in.data() + info.dataOffset + size_t(i) * info.blockAlign, info.blockAlign, o.data()));
      for (size_t k = 0; k < o.size() && pos < refSamples; ++k, ++pos)
        maxDiff = std::max(maxDiff, std::abs(int(o[k]) - int(rs[pos])));
    }
    std::printf("atrac3+ vs reference: max diff %d over %zu samples\n", maxDiff, pos);
    CHECK(pos > 0 && maxDiff <= 2);
  }
}

int main() {
  init_memory();
  KernelConfig cfg;
  cfg.discRoot = (std::filesystem::temp_directory_path() / "mgspo_audio_test").string();
  cfg.saveRoot = cfg.discRoot;
  cfg.headless = true;
  kernel_init(cfg, 0x08900000, 0);
  register_all_hle();
  bind_tables(nullptr, 0, kImports, 4);

  test_vag();
  test_envelope();
  test_mix_and_end();
  test_simple_adsr();
  test_atrac_parse();
  test_atrac3plus();
  test_hle();

  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("audio tests passed\n");
  return 0;
}
