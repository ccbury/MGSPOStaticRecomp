// Decodes an .at3 file (ATRAC3 or ATRAC3+) to a 16 bit WAV. Used to eyeball and compare decoder output
// against other decoders: at3dec in.at3 out.wav
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "atrac3.h"
#include "atrac3plus.h"

using namespace psp::atrac;

static void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
static void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x));
  v.push_back(uint8_t(x >> 8));
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::puts("usage: at3dec in.at3 out.wav");
    return 1;
  }
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  At3Info info;
  if (!parse_at3(in.data(), in.size(), info)) {
    std::puts("not a usable at3 file");
    return 1;
  }
  const int ch = info.channels;
  std::vector<int16_t> pcm;
  int bad = 0;
  if (info.codec == Codec::Atrac3Plus) {
    Atrac3PlusDecoder dec;
    if (!dec.init(ch)) return 1;
    std::vector<int16_t> out(size_t(kPlusSamplesPerFrame) * ch);
    for (uint32_t i = 0; i < info.frameCount(); ++i) {
      if (!dec.decode_frame(in.data() + info.dataOffset + size_t(i) * info.blockAlign, info.blockAlign, out.data())) ++bad;
      pcm.insert(pcm.end(), out.begin(), out.end());
    }
  } else {
    Atrac3Decoder dec;
    if (!dec.init(ch, info.blockAlign, info.jointStereo)) return 1;
    std::vector<int16_t> out(size_t(kSamplesPerFrame) * ch);
    for (uint32_t i = 0; i < info.frameCount(); ++i) {
      dec.decode_frame(in.data() + info.dataOffset + size_t(i) * info.blockAlign, out.data());
      pcm.insert(pcm.end(), out.begin(), out.end());
    }
  }
  std::vector<uint8_t> w = {'R', 'I', 'F', 'F'};
  put32(w, uint32_t(36 + pcm.size() * 2));
  w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(w, 16);
  put16(w, 1);
  put16(w, uint16_t(ch));
  put32(w, uint32_t(info.sampleRate));
  put32(w, uint32_t(info.sampleRate * ch * 2));
  put16(w, uint16_t(ch * 2));
  put16(w, 16);
  w.insert(w.end(), {'d', 'a', 't', 'a'});
  put32(w, uint32_t(pcm.size() * 2));
  std::ofstream o(argv[2], std::ios::binary);
  o.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
  o.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(pcm.size() * 2));
  std::printf("%u frames, %d bad\n", info.frameCount(), bad);
  return 0;
}
