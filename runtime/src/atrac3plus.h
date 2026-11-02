// ATRAC3+ decoder front end. The bitstream parser and DSP live in atrac3p/ (see atrac3p/README.md for where
// they come from); this class owns the per-stream state and converts the float output to 16 bit PCM.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace psp {
namespace atrac {

constexpr int kPlusSamplesPerFrame = 2048;

class Atrac3PlusDecoder {
 public:
  Atrac3PlusDecoder();
  ~Atrac3PlusDecoder();
  // Mono and stereo streams are supported.
  bool init(int channels);
  void reset();
  // Decodes one frame into kPlusSamplesPerFrame * channels interleaved samples. Returns false if the frame is
  // corrupt, in which case the output is silence.
  bool decode_frame(const uint8_t* data, size_t size, int16_t* out);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atrac
}  // namespace psp
