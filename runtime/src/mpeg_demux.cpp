#include "mpeg_demux.h"

#include <algorithm>
#include <cstring>

namespace psp {
namespace mpeg {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

int64_t read_ts(const uint8_t* p) {
  return (int64_t(p[0] >> 1) & 7) << 30 | int64_t(p[1]) << 22 | (int64_t(p[2] >> 1)) << 15 | int64_t(p[3]) << 7 |
         int64_t(p[4] >> 1);
}

}  // namespace

PsmfInfo parse_psmf_header(const uint8_t* d, size_t n) {
  PsmfInfo i;
  if (n < 0x90 || std::memcmp(d, "PSMF", 4) != 0) return i;
  i.version = (d[4] - '0') * 1000 + (d[5] - '0') * 100 + (d[6] - '0') * 10 + (d[7] - '0');
  i.streamOffset = be32(d + 8);
  i.streamSize = be32(d + 12);
  i.width = d[142] * 16;
  i.height = d[143] * 16;
  i.valid = i.streamOffset != 0 && (i.streamOffset & 2047) == 0;
  return i;
}

void Demuxer::reset() { *this = Demuxer(); }

size_t Demuxer::pending_bytes() const {
  return videoBytes_ + audioBytes_ + curVideo_.data.size() + audioBuf_.size() + (buf_.size() - pos_);
}

void Demuxer::feed(const uint8_t* data, size_t size) {
  buf_.insert(buf_.end(), data, data + size);
  if (first_ && buf_.size() >= 16) {
    first_ = false;
    // The game may have queued the PSMF header in front of the stream; jump over it.
    if (std::memcmp(buf_.data(), "PSMF", 4) == 0) pos_ = std::min<size_t>(be32(buf_.data() + 8), buf_.size());
  }
  parse();
  if (pos_ > 0) {
    buf_.erase(buf_.begin(), buf_.begin() + pos_);
    pos_ = 0;
  }
}

void Demuxer::parse() {
  while (buf_.size() - pos_ >= 4) {
    const uint8_t* p = buf_.data() + pos_;
    if (p[0] != 0 || p[1] != 0 || p[2] != 1) {
      ++pos_;
      continue;
    }
    uint8_t id = p[3];
    size_t avail = buf_.size() - pos_;
    if (id == 0xBA) {
      if (avail < 14) return;
      size_t len = (p[4] & 0xC0) == 0x40 ? 14 + (p[13] & 7) : 12;
      if (avail < len) return;
      pos_ += len;
    } else if (id == 0xB9) {
      pos_ += 4;
    } else if (id == 0xBB || id == 0xBC || id == 0xBE || id == 0xBF || id == 0xBD || (id >= 0xC0 && id <= 0xEF)) {
      if (avail < 6) return;
      size_t len = be16(p + 4);
      if (avail < 6 + len) return;
      if (id == 0xBD || (id >= 0xE0 && id <= 0xEF)) {
        const uint8_t* q = p + 6;
        const uint8_t* end = q + len;
        bool hasPts = false;
        int64_t pts = 0, dts = 0;
        if (len >= 3 && (q[0] & 0xC0) == 0x80) {
          uint8_t flags = q[1];
          size_t hl = q[2];
          if (3 + hl <= len) {
            if ((flags & 0x80) && hl >= 5) {
              hasPts = true;
              pts = dts = read_ts(q + 3);
              if ((flags & 0x40) && hl >= 10) dts = read_ts(q + 8);
            }
            q += 3 + hl;
          } else {
            q = end;
          }
        }
        if (id == 0xBD) {
          if (end - q > 4) {
            int ch = q[0];
            if (audioChannel_ < 0) audioChannel_ = ch;
            if (ch == audioChannel_) on_audio(q + 4, end - q - 4, hasPts, pts);
          }
        } else {
          if (videoStream_ < 0) videoStream_ = id;
          if (id == videoStream_) on_video(q, end - q, hasPts, pts, dts);
        }
      }
      pos_ += 6 + len;
    } else {
      ++pos_;
    }
  }
}

void Demuxer::flush_video() {
  if (haveVideo_ && !curVideo_.data.empty()) {
    split_pictures(curVideo_);
  }
  curVideo_ = Au();
  haveVideo_ = false;
}

// A PES packet with a timestamp can carry several pictures. Cut it at each new picture (first slice
// of a frame, plus any parameter sets / AUD / SEI queued in front of it) so the game gets one frame
// per access unit, with timestamps stepped by one frame.
void Demuxer::split_pictures(Au& au) {
  const std::vector<uint8_t>& d = au.data;
  std::vector<size_t> cuts;
  size_t prefix = SIZE_MAX;
  bool haveSlice = false;
  for (size_t i = 0; i + 4 < d.size(); ++i) {
    if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
    size_t start = (i > 0 && d[i - 1] == 0) ? i - 1 : i;
    int type = d[i + 3] & 0x1F;
    if (type == 1 || type == 5) {
      bool firstSlice = (d[i + 4] & 0x80) != 0;
      if (firstSlice) {
        if (haveSlice) cuts.push_back(prefix != SIZE_MAX ? prefix : start);
        haveSlice = true;
      }
      prefix = SIZE_MAX;
    } else if (type >= 6 && type <= 9 && haveSlice) {
      if (prefix == SIZE_MAX) prefix = start;
    }
    i += 2;
  }
  size_t from = 0;
  int64_t step = 0;
  for (size_t k = 0; k <= cuts.size(); ++k) {
    size_t to = k < cuts.size() ? cuts[k] : d.size();
    Au part;
    part.pts = au.pts + step;
    part.dts = au.dts + step;
    part.data.assign(d.begin() + from, d.begin() + to);
    videoBytes_ += part.data.size();
    video_.push_back(std::move(part));
    from = to;
    step += kVideoTimestampStep;
  }
}

void Demuxer::on_video(const uint8_t* p, size_t n, bool hasPts, int64_t pts, int64_t dts) {
  sawVideo_ = true;
  if (hasPts) {
    flush_video();
    curVideo_.pts = pts;
    curVideo_.dts = dts != pts ? dts : pts - kVideoTimestampStep;
    haveVideo_ = true;
  } else if (!haveVideo_) {
    haveVideo_ = true;
  }
  curVideo_.data.insert(curVideo_.data.end(), p, p + n);
}

void Demuxer::on_audio(const uint8_t* p, size_t n, bool hasPts, int64_t pts) {
  if (hasPts && (audioBuf_.empty() || !audioPtsKnown_)) {
    audioPts_ = pts;
    audioPtsKnown_ = true;
  }
  audioBuf_.insert(audioBuf_.end(), p, p + n);
  extract_audio_frames(false);
}

void Demuxer::extract_audio_frames(bool) {
  size_t start = 0;
  while (audioBuf_.size() - start >= 4) {
    const uint8_t* a = audioBuf_.data() + start;
    if (a[0] != 0x0F || a[1] != 0xD0) {
      ++start;
      continue;
    }
    size_t payload = ((size_t(a[2] & 3) << 8) | a[3]) * 8 + 8;
    size_t total = payload + 8;
    if (audioBuf_.size() - start < total) break;
    Au au;
    au.pts = au.dts = audioPts_;
    au.data.assign(a + 8, a + total);
    audioBytes_ += au.data.size();
    audio_.push_back(std::move(au));
    audioPts_ += kAudioTimestampStep;
    start += total;
  }
  if (start) audioBuf_.erase(audioBuf_.begin(), audioBuf_.begin() + start);
}

void Demuxer::finish() { flush_video(); }

bool Demuxer::pop_video(Au& out) {
  if (video_.empty()) return false;
  out = std::move(video_.front());
  video_.pop_front();
  videoBytes_ -= out.data.size();
  return true;
}

bool Demuxer::pop_audio(Au& out) {
  if (audio_.empty()) return false;
  out = std::move(audio_.front());
  audio_.pop_front();
  audioBytes_ -= out.data.size();
  return true;
}

}  // namespace mpeg
}  // namespace psp
