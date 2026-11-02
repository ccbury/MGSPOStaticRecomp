#include "video_decoder.h"

namespace psp {
namespace mpeg {

namespace {
struct NullDecoder : VideoDecoder {
  bool decode(const uint8_t*, size_t, Picture&) override { return false; }
  const char* name() const override { return "none"; }
};
}  // namespace

std::unique_ptr<VideoDecoder> create_video_decoder(int, int) { return std::make_unique<NullDecoder>(); }
bool video_decoder_available() { return false; }

}  // namespace mpeg
}  // namespace psp
