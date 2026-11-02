// Host services the firmware layer needs: window, input, audio output.
#pragma once
#include <cstdint>
#include <vector>

namespace psp {
namespace plat {

void init(bool headless);  // timer resolution etc.; opens the window unless headless

// Display. pixels are BGRA8888, width*height, tightly packed.
void present(const uint32_t* bgra, int width, int height);
bool quit_requested();

// Input in PSP button bits; analog values are 0..255 with 128 centred.
struct Pad {
  uint32_t buttons = 0;
  uint8_t lx = 128, ly = 128;
};
Pad poll_pad();

// Audio. The callback must fill `frames` stereo s16 frames. Returns false if no device is available.
using AudioFill = void (*)(int16_t* out, uint32_t frames);
bool audio_start(int rate, AudioFill fill);
void audio_stop();

// Glyph rasteriser used by the sceLibFont replacement. The console's own font files are not on the
// disc, so text is drawn with whatever the host provides.
struct Glyph {
  int width = 0, height = 0;  // bitmap size in pixels
  int left = 0, top = 0;      // bitmap origin relative to the pen on the baseline (top is up)
  float advance = 0;          // horizontal advance in pixels
  std::vector<uint8_t> alpha;  // width*height coverage, 0..255
};
struct FontMetrics {
  float ascent = 0, descent = 0, maxAdvance = 0;
};
bool glyph_raster(uint32_t codepoint, int pixelSize, Glyph& out);
FontMetrics font_metrics(int pixelSize);

}  // namespace plat
}  // namespace psp
