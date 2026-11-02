// H.264 decoder backed by the stock Media Foundation decoder MFT (Windows 8 or later; it is missing
// from the "N" editions without the media feature pack). Nothing is bundled or linked statically.
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

#include "video_decoder.h"

// CLSID_CMSH264DecoderMFT
DEFINE_GUID(kClsidH264Decoder, 0x62CE7E72, 0x4C71, 0x4D20, 0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D);

namespace psp {
namespace mpeg {

using Microsoft::WRL::ComPtr;

namespace {

bool ensure_mf() {
  static const bool ok = [] {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  }();
  thread_local bool threadInit = false;
  if (ok && !threadInit) {
    threadInit = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  }
  return ok;
}

uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

class MfH264Decoder : public VideoDecoder {
 public:
  MfH264Decoder(int w, int h) : width_(w), height_(h) { ok_ = init(); }
  bool ok() const { return ok_; }
  const char* name() const override { return "Media Foundation H.264"; }

  bool decode(const uint8_t* data, size_t size, Picture& out) override {
    if (!ok_ || !size) return false;
    ensure_mf();
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buf))) return false;
    BYTE* dst = nullptr;
    if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
    std::copy(data, data + size, dst);
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(size));
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(time_);
    sample->SetSampleDuration(333333);
    time_ += 333333;

    bool got = false;
    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      got |= drain(out);
      hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return got;
    got |= drain(out);
    return got;
  }

 private:
  bool init() {
    if (!ensure_mf()) return fail("MFStartup", E_FAIL);
    HRESULT hr = CoCreateInstance(kClsidH264Decoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
    if (FAILED(hr)) return fail("CoCreateInstance", hr);
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    ComPtr<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(&in))) return false;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width_, height_);
    hr = mft_->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
      in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264_ES);
      in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      hr = mft_->SetInputType(0, in.Get(), 0);
    }
    if (FAILED(hr)) return fail("SetInputType", hr);
    if (!select_output()) return fail("select_output", E_FAIL);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  static bool fail(const char* stage, HRESULT hr) {
    std::fprintf(stderr, "movie: H.264 decoder setup failed at %s (0x%08lX)\n", stage, static_cast<unsigned long>(hr));
    return false;
  }

  bool select_output() {
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> t;
      if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) return false;
      GUID sub{};
      if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
        if (FAILED(mft_->SetOutputType(0, t.Get(), 0))) return false;
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w && h) {
          width_ = static_cast<int>(w);
          height_ = static_cast<int>(h);
        }
        stride_ = static_cast<int>(MFGetAttributeUINT32(t.Get(), MF_MT_DEFAULT_STRIDE, w));
        if (stride_ <= 0) stride_ = width_;
        MFT_OUTPUT_STREAM_INFO info{};
        mft_->GetOutputStreamInfo(0, &info);
        providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outSize_ = info.cbSize;
        return true;
      }
    }
  }

  bool drain(Picture& out) {
    bool got = false;
    for (int guard = 0; guard < 8; ++guard) {
      MFT_OUTPUT_DATA_BUFFER ob{};
      ComPtr<IMFSample> own;
      if (!providesSamples_) {
        ComPtr<IMFMediaBuffer> mb;
        if (FAILED(MFCreateMemoryBuffer(outSize_ ? outSize_ : width_ * height_ * 3 / 2 + 4096, &mb))) break;
        MFCreateSample(&own);
        own->AddBuffer(mb.Get());
        ob.pSample = own.Get();
      }
      DWORD status = 0;
      HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
      if (ob.pEvents) ob.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (!select_output()) break;
        continue;
      }
      if (FAILED(hr)) break;
      ComPtr<IMFSample> s;
      if (providesSamples_) s.Attach(ob.pSample);
      else s = own;
      if (s && convert(s.Get(), out)) got = true;
    }
    return got;
  }

  bool convert(IMFSample* s, Picture& out) {
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(s->ConvertToContiguousBuffer(&mb))) return false;
    BYTE* d = nullptr;
    DWORD len = 0;
    if (FAILED(mb->Lock(&d, nullptr, &len))) return false;
    const int w = width_, h = height_, st = stride_;
    const int alignedH = (h + 15) & ~15;
    bool ok = static_cast<size_t>(st) * alignedH + static_cast<size_t>(st) * h / 2 <= len;
    if (ok) {
      out.width = w;
      out.height = h;
      out.rgba.resize(static_cast<size_t>(w) * h * 4);
      const uint8_t* yp = d;
      const uint8_t* uv = d + static_cast<size_t>(st) * alignedH;
      for (int y = 0; y < h; ++y) {
        uint8_t* o = &out.rgba[static_cast<size_t>(y) * w * 4];
        const uint8_t* yr = yp + static_cast<size_t>(y) * st;
        const uint8_t* ur = uv + static_cast<size_t>(y / 2) * st;
        for (int x = 0; x < w; ++x) {
          int Y = yr[x] - 16, U = ur[x & ~1] - 128, V = ur[(x & ~1) + 1] - 128;
          int c = 298 * Y;
          o[0] = clamp8((c + 409 * V + 128) >> 8);
          o[1] = clamp8((c - 100 * U - 208 * V + 128) >> 8);
          o[2] = clamp8((c + 516 * U + 128) >> 8);
          o[3] = 255;
          o += 4;
        }
      }
    }
    mb->Unlock();
    return ok;
  }

  ComPtr<IMFTransform> mft_;
  int width_, height_;
  int stride_ = 0;
  DWORD outSize_ = 0;
  bool providesSamples_ = false;
  bool ok_ = false;
  LONGLONG time_ = 0;
};

struct NullDecoder : VideoDecoder {
  bool decode(const uint8_t*, size_t, Picture&) override { return false; }
  const char* name() const override { return "none"; }
};

}  // namespace

std::unique_ptr<VideoDecoder> create_video_decoder(int width, int height) {
  auto d = std::make_unique<MfH264Decoder>(width, height);
  if (d->ok()) return d;
  std::fprintf(stderr, "movie: Media Foundation H.264 decoder unavailable, video will be black\n");
  return std::make_unique<NullDecoder>();
}

bool video_decoder_available() {
  static const bool ok = create_video_decoder(480, 272)->name()[0] != 'n';
  return ok;
}

}  // namespace mpeg
}  // namespace psp
