// Incremental demuxer for the MPEG-2 program stream inside a PSMF movie. Splits it into H.264 access
// units (one per picture) and ATRAC3+ frames. Pure data in, data out: no guest memory involved.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace psp {
namespace mpeg {

struct Au {
  int64_t pts = 0;  // 90 kHz
  int64_t dts = 0;
  std::vector<uint8_t> data;
};

constexpr int kVideoTimestampStep = 3003;  // one frame at 29.97 fps
constexpr int kAudioTimestampStep = 4180;  // one ATRAC3+ frame (2048 samples at 44.1 kHz)

struct PsmfInfo {
  bool valid = false;
  uint32_t version = 0;
  uint32_t streamOffset = 0;
  uint32_t streamSize = 0;
  int width = 0;
  int height = 0;
};

// Reads the 2048 byte PSMF header the game hands to sceMpegQueryStreamOffset/Size.
PsmfInfo parse_psmf_header(const uint8_t* data, size_t size);

class Demuxer {
 public:
  void feed(const uint8_t* data, size_t size);
  // The source ran dry: whatever is still being assembled becomes a final access unit.
  void finish();
  void reset();

  bool pop_video(Au& out);
  bool pop_audio(Au& out);
  bool has_video() const { return !video_.empty(); }
  bool has_audio() const { return !audio_.empty(); }
  // Bytes held back (queued units plus partially received data).
  size_t pending_bytes() const;
  bool saw_video() const { return sawVideo_; }

 private:
  void parse();
  void on_video(const uint8_t* p, size_t n, bool hasPts, int64_t pts, int64_t dts);
  void on_audio(const uint8_t* p, size_t n, bool hasPts, int64_t pts);
  void flush_video();
  void split_pictures(Au& au);
  void extract_audio_frames(bool final);

  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  bool first_ = true;
  bool sawVideo_ = false;

  Au curVideo_;
  bool haveVideo_ = false;
  int videoStream_ = -1;
  std::deque<Au> video_;
  size_t videoBytes_ = 0;

  int audioChannel_ = -1;
  std::vector<uint8_t> audioBuf_;
  int64_t audioPts_ = 0;
  bool audioPtsKnown_ = false;
  std::deque<Au> audio_;
  size_t audioBytes_ = 0;
};

}  // namespace mpeg
}  // namespace psp
