// Headless host layer used on non-Windows builds and with --headless.
#include <algorithm>

#include "platform.h"

namespace psp {
namespace plat {

void init(bool) {}
void present(const uint32_t*, int, int) {}
bool quit_requested() { return false; }
Pad poll_pad() { return {}; }
bool audio_start(int, AudioFill) { return false; }
void audio_stop() {}

// No font rendering without a host: report plain boxes so layout code still gets sane metrics.
bool glyph_raster(uint32_t cp, int px, Glyph& g) {
  g.advance = px * 0.6f;
  if (cp == ' ' || cp == 0xA0) return true;
  g.width = std::max(1, px / 2);
  g.height = std::max(1, px * 2 / 3);
  g.left = 1;
  g.top = g.height;
  g.alpha.assign(static_cast<size_t>(g.width) * g.height, 0xFF);
  return true;
}

FontMetrics font_metrics(int px) { return {px * 0.8f, px * 0.2f, px * 0.6f}; }

}  // namespace plat
}  // namespace psp
