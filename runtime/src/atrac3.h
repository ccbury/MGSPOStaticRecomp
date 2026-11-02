// ATRAC3 (LP2/LP4 and the 66kbps joint-stereo mode) decoder plus a parser for the RIFF .at3 wrapper.
// ATRAC3+ streams are recognised by the parser but not decoded.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace psp {
namespace atrac {

constexpr int kSamplesPerFrame = 1024;

enum class Codec { Unknown, Atrac3, Atrac3Plus };

struct At3Info {
  Codec codec = Codec::Unknown;
  int channels = 0;
  int sampleRate = 0;
  int blockAlign = 0;
  bool jointStereo = false;
  uint32_t dataOffset = 0;  // first frame, relative to the start of the buffer
  uint32_t dataSize = 0;
  uint32_t totalSamples = 0;  // from the fact chunk
  uint32_t skipSamples = 0;   // encoder delay to drop from the start
  bool hasLoop = false;
  uint32_t loopStart = 0, loopEnd = 0;
  uint32_t frameCount() const { return blockAlign ? dataSize / blockAlign : 0; }
};

// Parses the RIFF header at the start of an .at3 buffer. Returns false if it is not a usable header.
bool parse_at3(const uint8_t* data, size_t size, At3Info& out);

class Atrac3Decoder {
 public:
  Atrac3Decoder();
  ~Atrac3Decoder();
  bool init(int channels, int blockAlign, bool jointStereo);
  void reset();
  // Decodes one block of blockAlign bytes into kSamplesPerFrame * channels interleaved samples.
  bool decode_frame(const uint8_t* data, int16_t* out);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atrac
}  // namespace psp
