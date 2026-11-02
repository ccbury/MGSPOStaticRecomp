// H.264 decoding for the movie player. The Windows build uses the H.264 decoder that ships with
// Media Foundation; other platforms get a stub that never produces a picture.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace psp {
namespace mpeg {

struct Picture {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4, R G B A
};

class VideoDecoder {
 public:
  virtual ~VideoDecoder() = default;
  // Feeds one Annex B access unit. Returns true when a picture came out.
  virtual bool decode(const uint8_t* data, size_t size, Picture& out) = 0;
  virtual const char* name() const = 0;
};

// Never returns null; check name() or available() to see whether real decoding is possible.
std::unique_ptr<VideoDecoder> create_video_decoder(int width, int height);
bool video_decoder_available();

}  // namespace mpeg
}  // namespace psp
