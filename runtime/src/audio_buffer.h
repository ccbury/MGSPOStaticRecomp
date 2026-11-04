#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace psp {
namespace audio {

class OutputBuffer {
 public:
  static constexpr size_t kLeadFrames = 2048;
  static constexpr size_t kMaxFrames = 44100 / 2;

  size_t frames() const { return samples_.size() / 2; }
  bool can_append(size_t count) const { return count <= kMaxFrames && frames() <= kMaxFrames - count; }
  bool producer_waiting() const { return frames() > kLeadFrames; }
  void push(int16_t l, int16_t r) {
    samples_.push_back(l);
    samples_.push_back(r);
  }
  bool pop(int32_t& l, int32_t& r) {
    if (samples_.empty()) return false;
    l += samples_[0];
    r += samples_[1];
    samples_.pop_front();
    samples_.pop_front();
    return true;
  }

 private:
  std::deque<int16_t> samples_;
};

}  // namespace audio
}  // namespace psp
