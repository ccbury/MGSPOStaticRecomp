// Tests for the PSMF header parser and the MPEG-PS demuxer, using synthetic streams only.
#include <cstdio>
#include <cstring>
#include <vector>

#include "mpeg_demux.h"
#include "video_convert.h"

using namespace psp::mpeg;

static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

typedef std::vector<uint8_t> Bytes;

static void put_ts(Bytes& b, int marker, int64_t ts) {
  b.push_back(static_cast<uint8_t>((marker << 4) | (((ts >> 30) & 7) << 1) | 1));
  b.push_back(static_cast<uint8_t>(ts >> 22));
  b.push_back(static_cast<uint8_t>((((ts >> 15) & 0x7F) << 1) | 1));
  b.push_back(static_cast<uint8_t>(ts >> 7));
  b.push_back(static_cast<uint8_t>(((ts & 0x7F) << 1) | 1));
}

static void pack_header(Bytes& s) {
  const uint8_t h[] = {0, 0, 1, 0xBA, 0x44, 0, 4, 0, 4, 1, 0x89, 0xC3, 0xF8, 0};
  s.insert(s.end(), h, h + sizeof(h));
}

static void pes(Bytes& s, uint8_t id, bool pts, int64_t ts, const Bytes& payload, const Bytes& prefix = Bytes()) {
  Bytes body;
  body.push_back(0x80);
  body.push_back(pts ? 0x80 : 0);
  body.push_back(pts ? 5 : 0);
  if (pts) put_ts(body, 2, ts);
  body.insert(body.end(), prefix.begin(), prefix.end());
  body.insert(body.end(), payload.begin(), payload.end());
  s.push_back(0);
  s.push_back(0);
  s.push_back(1);
  s.push_back(id);
  s.push_back(static_cast<uint8_t>(body.size() >> 8));
  s.push_back(static_cast<uint8_t>(body.size()));
  s.insert(s.end(), body.begin(), body.end());
}

static Bytes atrac_frame(int units, uint8_t fill) {
  // Header 0F D0 plus a 10 bit length in 8 byte units, then the 8 byte header's remainder and payload.
  Bytes f = {0x0F, 0xD0, static_cast<uint8_t>((units >> 8) & 3), static_cast<uint8_t>(units), 0, 0, 0, 0};
  f.insert(f.end(), static_cast<size_t>(units) * 8 + 8, fill);
  return f;
}

static void test_header() {
  Bytes h(2048, 0);
  std::memcpy(h.data(), "PSMF0015", 8);
  h[8] = 0;
  h[9] = 0;
  h[10] = 0x08;
  h[11] = 0;
  h[15] = 0x40;
  h[142] = 30;
  h[143] = 17;
  PsmfInfo i = parse_psmf_header(h.data(), h.size());
  CHECK(i.valid);
  CHECK(i.streamOffset == 0x800);
  CHECK(i.streamSize == 0x40);
  CHECK(i.width == 480 && i.height == 272);
  h[0] = 'X';
  CHECK(!parse_psmf_header(h.data(), h.size()).valid);
}

static void test_video_split() {
  Bytes s;
  pack_header(s);
  pes(s, 0xE0, true, 90000, Bytes(10, 0x11));
  pes(s, 0xE0, false, 0, Bytes(7, 0x22));  // continuation of the same picture
  pack_header(s);
  pes(s, 0xE0, true, 93003, Bytes(5, 0x33));
  Demuxer d;
  d.feed(s.data(), s.size());
  Au a;
  CHECK(d.pop_video(a));
  CHECK(a.pts == 90000);
  CHECK(a.dts == 90000 - kVideoTimestampStep);
  CHECK(a.data.size() == 17);
  CHECK(!d.pop_video(a));  // the second picture is still open
  d.finish();
  CHECK(d.pop_video(a));
  CHECK(a.pts == 93003 && a.data.size() == 5);
  CHECK(!d.pop_video(a));
}

static void test_split_feed() {
  Bytes s;
  pack_header(s);
  pes(s, 0xE0, true, 1000, Bytes(100, 0x44));
  pes(s, 0xE0, true, 4003, Bytes(50, 0x55));
  Demuxer d;
  for (size_t i = 0; i < s.size(); i += 13) d.feed(s.data() + i, std::min<size_t>(13, s.size() - i));
  d.finish();
  Au a;
  CHECK(d.pop_video(a) && a.data.size() == 100);
  CHECK(d.pop_video(a) && a.data.size() == 50);
}

static void test_audio() {
  Bytes s;
  pack_header(s);
  Bytes f1 = atrac_frame(3, 0xA1), f2 = atrac_frame(3, 0xA2);
  Bytes both = f1;
  both.insert(both.end(), f2.begin(), f2.end());
  // Private stream 1: channel byte plus three bytes of header, and a frame split across two packets.
  Bytes prefix = {0x40, 0, 0, 0};
  size_t cut = 20;
  pes(s, 0xBD, true, 90000, Bytes(both.begin(), both.begin() + cut), prefix);
  pes(s, 0xBD, false, 0, Bytes(both.begin() + cut, both.end()), prefix);
  Demuxer d;
  d.feed(s.data(), s.size());
  Au a;
  CHECK(d.pop_audio(a));
  CHECK(a.pts == 90000);
  CHECK(a.data.size() == 3 * 8 + 8 + 0);
  CHECK(a.data[8] == 0xA1);
  CHECK(d.pop_audio(a));
  CHECK(a.pts == 90000 + kAudioTimestampStep);
  CHECK(!d.pop_audio(a));
}

static void test_leading_psmf_header() {
  Bytes s(2048, 0);
  std::memcpy(s.data(), "PSMF0014", 8);
  s[10] = 0x08;
  pack_header(s);
  pes(s, 0xE0, true, 500, Bytes(4, 0x66));
  Demuxer d;
  d.feed(s.data(), s.size());
  d.finish();
  Au a;
  CHECK(d.pop_video(a) && a.pts == 500);
}

static void test_nv12_convert() {
  // 4x2 picture, rows padded to a pitch of 8; the padding holds junk that must not leak in.
  const int w = 4, h = 2;
  Bytes y(8 * h, 0xEE), uv(8, 0xEE);
  for (int r = 0; r < h; ++r)
    for (int x = 0; x < w; ++x) y[r * 8 + x] = x < 2 ? 16 : 235;
  uv[0] = 128; uv[1] = 128; uv[2] = 128; uv[3] = 128;
  std::vector<uint8_t> out;
  nv12_to_rgba(y.data(), 8, uv.data(), 8, w, h, out);
  CHECK(out.size() == static_cast<size_t>(w * h * 4));
  CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 255);
  CHECK(out[3 * 4] == 255 && out[3 * 4 + 1] == 255 && out[3 * 4 + 2] == 255);
  CHECK(out[(w + 3) * 4] == 255);

  // Strong red chroma (V high, U low) on the second chroma pair only.
  uv[2] = 90; uv[3] = 240;
  nv12_to_rgba(y.data(), 8, uv.data(), 8, w, h, out);
  CHECK(out[0] == 0 && out[2] == 0);
  CHECK(out[2 * 4] > out[2 * 4 + 1] && out[2 * 4] > out[2 * 4 + 2]);
}

int main() {
  test_header();
  test_video_split();
  test_split_feed();
  test_audio();
  test_leading_psmf_header();
  test_nv12_convert();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("mpeg tests passed\n");
  return 0;
}
