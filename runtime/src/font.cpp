// sceLibFont replacement. The game loads libfont.prx from the disc, but that module is encrypted and
// is not lifted, so the eight functions the main module imports are implemented here instead. The
// console's font files (flash0:/font/*.pgf) are not available either, so glyphs come from the host
// (see plat::glyph_raster). Metrics and structure layouts follow the documented libfont ABI.
//
// If a decrypted libfont.prx is supplied and was lifted, its exports take precedence over these
// handlers (they are registered as soft handlers).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

#include "kernel_internal.h"
#include "platform.h"

namespace psp {
namespace k {

namespace {

constexpr const char* kLib = "sceLibFont";

constexpr uint32_t E_FONT_INVALID_PARAMETER = 0x80460002;
constexpr uint32_t E_FONT_BAD_HANDLE = 0x80460001;

constexpr uint32_t kFmt4 = 0, kFmt4Rev = 1, kFmt8 = 2, kFmt24 = 3, kFmt32 = 4;

struct Lib {
  float h = 12.0f, v = 12.0f, hRes = 128.0f, vRes = 128.0f;
};
struct Font {
  int32_t lib = 0;
  int px = 21;
};

std::mutex g_fontMutex;
std::map<int32_t, Lib> g_libs;
std::map<int32_t, Font> g_fonts;

int pixel_size(float pt, float dpi) {
  if (pt <= 0) pt = 12.0f;
  if (dpi <= 0) dpi = 128.0f;
  return std::max(6, std::min(128, static_cast<int>(std::lround(pt * dpi / 72.0f))));
}

uint32_t f2u(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

int32_t fx26(float px) { return static_cast<int32_t>(std::lround(px * 64.0f)); }

// sceFontNewLib(FontNewLibParams*, u32* errorCode)
void h_new_lib(Cpu& c) {
  std::lock_guard<std::mutex> lk(g_fontMutex);
  int32_t id = alloc_uid();
  g_libs[id] = Lib{};
  put32(arg(c, 1), 0);
  ret(c, static_cast<uint32_t>(id));
}

// sceFontDoneLib(lib)
void h_done_lib(Cpu& c) {
  std::lock_guard<std::mutex> lk(g_fontMutex);
  int32_t id = static_cast<int32_t>(arg(c, 0));
  for (auto it = g_fonts.begin(); it != g_fonts.end();) it = it->second.lib == id ? g_fonts.erase(it) : std::next(it);
  ret(c, g_libs.erase(id) ? 0 : E_FONT_BAD_HANDLE);
}

// sceFontFindOptimumFont(lib, FontStyle*, u32* errorCode): there is a single built-in font, index 0.
void h_find_optimum_font(Cpu& c) {
  std::lock_guard<std::mutex> lk(g_fontMutex);
  auto it = g_libs.find(static_cast<int32_t>(arg(c, 0)));
  if (it == g_libs.end()) {
    put32(arg(c, 2), E_FONT_BAD_HANDLE);
    return ret(c, E_FONT_BAD_HANDLE);
  }
  uint32_t style = arg(c, 1);
  if (style) {
    auto rd = [](uint32_t a) {
      uint32_t u = lw(a);
      float f;
      std::memcpy(&f, &u, 4);
      return f;
    };
    float h = rd(style), hRes = rd(style + 8);
    if (h > 0) it->second.h = h;
    if (hRes > 0) it->second.hRes = hRes;
  }
  put32(arg(c, 2), 0);
  ret(c, 0);
}

// sceFontOpen(lib, index, mode, u32* errorCode)
void h_open(Cpu& c) {
  std::lock_guard<std::mutex> lk(g_fontMutex);
  int32_t lib = static_cast<int32_t>(arg(c, 0));
  auto it = g_libs.find(lib);
  if (it == g_libs.end()) {
    put32(arg(c, 3), E_FONT_BAD_HANDLE);
    return ret(c, 0);
  }
  int32_t id = alloc_uid();
  g_fonts[id] = Font{lib, pixel_size(it->second.h, it->second.hRes)};
  put32(arg(c, 3), 0);
  ret(c, static_cast<uint32_t>(id));
}

// sceFontClose(font)
void h_close(Cpu& c) {
  std::lock_guard<std::mutex> lk(g_fontMutex);
  ret(c, g_fonts.erase(static_cast<int32_t>(arg(c, 0))) ? 0 : E_FONT_BAD_HANDLE);
}

// sceFontGetFontInfo(font, FontInfo*)
void h_get_font_info(Cpu& c) {
  Font f;
  {
    std::lock_guard<std::mutex> lk(g_fontMutex);
    auto it = g_fonts.find(static_cast<int32_t>(arg(c, 0)));
    if (it == g_fonts.end()) return ret(c, E_FONT_BAD_HANDLE);
    f = it->second;
  }
  uint32_t p = arg(c, 1);
  if (!p) return ret(c, E_FONT_INVALID_PARAMETER);
  plat::FontMetrics m = plat::font_metrics(f.px);
  float w = std::ceil(m.maxAdvance), asc = std::ceil(m.ascent), desc = std::ceil(m.descent);
  float h = asc + desc;
  std::memset(ptr(p), 0, 264);
  // integer pixel metrics, then the same values as floats
  uint32_t ints[10] = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(asc),
                       static_cast<uint32_t>(desc), 0, static_cast<uint32_t>(asc), static_cast<uint32_t>(w / 2),
                       static_cast<uint32_t>(asc), static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
  for (int i = 0; i < 10; ++i) {
    sw(p + i * 4, ints[i]);
    sw(p + 40 + i * 4, f2u(static_cast<float>(ints[i])));
  }
  sh(p + 80, static_cast<uint32_t>(w));
  sh(p + 82, static_cast<uint32_t>(h));
  sw(p + 84, 0xFFFF);  // charMapLength: whole BMP is addressable
  // font style
  uint32_t s = p + 92;
  float pt = f.px * 72.0f / 128.0f;
  sw(s + 0, f2u(pt));
  sw(s + 4, f2u(pt));
  sw(s + 8, f2u(128.0f));
  sw(s + 12, f2u(128.0f));
  sh(s + 20, 1);  // family: sans-serif
  sh(s + 22, 1);  // regular
  sh(s + 26, 2);  // language: latin
  sh(s + 28, 2);  // region
  sh(s + 30, 2);  // country
  write_cstr(s + 32, "Latin", 64);
  write_cstr(s + 96, "host-font", 64);
  sb(p + 260, 4);  // bits per pixel of the glyph bitmaps is configurable; report 4 like the originals
  ret(c, 0);
}

// sceFontGetCharInfo(font, charCode, CharInfo*)
void h_get_char_info(Cpu& c) {
  Font f;
  {
    std::lock_guard<std::mutex> lk(g_fontMutex);
    auto it = g_fonts.find(static_cast<int32_t>(arg(c, 0)));
    if (it == g_fonts.end()) return ret(c, E_FONT_BAD_HANDLE);
    f = it->second;
  }
  uint32_t p = arg(c, 2);
  if (!p) return ret(c, E_FONT_INVALID_PARAMETER);
  plat::Glyph g;
  plat::glyph_raster(arg(c, 1) & 0xFFFF, f.px, g);
  plat::FontMetrics m = plat::font_metrics(f.px);
  std::memset(ptr(p), 0, 60);
  sw(p + 0, static_cast<uint32_t>(g.width));
  sw(p + 4, static_cast<uint32_t>(g.height));
  sw(p + 8, static_cast<uint32_t>(g.left));
  sw(p + 12, static_cast<uint32_t>(g.top));
  sw(p + 16, static_cast<uint32_t>(fx26(static_cast<float>(g.width))));
  sw(p + 20, static_cast<uint32_t>(fx26(static_cast<float>(g.height))));
  sw(p + 24, static_cast<uint32_t>(fx26(static_cast<float>(g.top))));
  sw(p + 28, static_cast<uint32_t>(fx26(static_cast<float>(g.top - g.height))));
  sw(p + 32, static_cast<uint32_t>(fx26(static_cast<float>(g.left))));  // BearingHX
  sw(p + 36, static_cast<uint32_t>(fx26(static_cast<float>(g.top))));   // BearingHY
  sw(p + 40, static_cast<uint32_t>(fx26(-g.advance / 2)));              // BearingVX
  sw(p + 44, static_cast<uint32_t>(fx26(m.ascent)));                    // BearingVY
  sw(p + 48, static_cast<uint32_t>(fx26(g.advance)));                   // AdvanceH
  sw(p + 52, static_cast<uint32_t>(fx26(m.ascent + m.descent)));        // AdvanceV
  ret(c, 0);
}

void put_pixel(uint32_t buf, uint32_t fmt, uint32_t bytesPerLine, int x, int y, uint8_t a) {
  switch (fmt) {
    case kFmt4:
    case kFmt4Rev: {
      uint32_t addr = buf + y * bytesPerLine + (x >> 1);
      uint8_t v = static_cast<uint8_t>(a >> 4);
      bool low = (fmt == kFmt4) == ((x & 1) == 0);
      uint8_t b = lbu(addr);
      uint8_t old = low ? (b & 0xF) : (b >> 4);
      v = std::max(v, old);
      sb(addr, low ? ((b & 0xF0) | v) : ((b & 0x0F) | (v << 4)));
      break;
    }
    case kFmt8: {
      uint32_t addr = buf + y * bytesPerLine + x;
      sb(addr, std::max(lbu(addr), a));
      break;
    }
    case kFmt24: {
      uint32_t addr = buf + y * bytesPerLine + x * 3;
      sb(addr, a);
      sb(addr + 1, a);
      sb(addr + 2, a);
      break;
    }
    case kFmt32: {
      uint32_t addr = buf + y * bytesPerLine + x * 4;
      uint32_t old = lw(addr) >> 24;
      sw(addr, 0x00FFFFFFu | (static_cast<uint32_t>(std::max<uint32_t>(old, a)) << 24));
      break;
    }
    default:
      break;
  }
}

// sceFontGetCharGlyphImage_Clip(font, charCode, GlyphImage*, clipXPos, clipYPos, clipWidth, clipHeight)
void h_glyph_image_clip(Cpu& c) {
  Font f;
  {
    std::lock_guard<std::mutex> lk(g_fontMutex);
    auto it = g_fonts.find(static_cast<int32_t>(arg(c, 0)));
    if (it == g_fonts.end()) return ret(c, E_FONT_BAD_HANDLE);
    f = it->second;
  }
  uint32_t img = arg(c, 2);
  if (!img) return ret(c, E_FONT_INVALID_PARAMETER);
  int32_t clipX = static_cast<int32_t>(arg(c, 3)), clipY = static_cast<int32_t>(arg(c, 4));
  int32_t clipW = static_cast<int32_t>(arg(c, 5)), clipH = static_cast<int32_t>(arg(c, 6));
  uint32_t fmt = lw(img);
  int32_t penX = static_cast<int32_t>(lw(img + 4)) >> 6;
  int32_t penY = static_cast<int32_t>(lw(img + 8)) >> 6;
  uint32_t bufW = lhu(img + 12), bufH = lhu(img + 14), bpl = lhu(img + 16);
  uint32_t buf = lw(img + 20);
  plat::Glyph g;
  if (!buf || !plat::glyph_raster(arg(c, 1) & 0xFFFF, f.px, g)) return ret(c, 0);
  for (int y = 0; y < g.height; ++y)
    for (int x = 0; x < g.width; ++x) {
      int32_t dx = penX + x, dy = penY + y;
      if (dx < 0 || dy < 0 || dx >= static_cast<int32_t>(bufW) || dy >= static_cast<int32_t>(bufH)) continue;
      if (clipW > 0 && clipH > 0 && (dx < clipX || dy < clipY || dx >= clipX + clipW || dy >= clipY + clipH))
        continue;
      uint8_t a = g.alpha[static_cast<size_t>(y) * g.width + x];
      if (a) put_pixel(buf, fmt, bpl, dx, dy, a);
    }
  ret(c, 0);
}

}  // namespace

void register_font() {
  register_hle_soft(kLib, 0x67F17ED7, "sceFontNewLib", h_new_lib);
  register_hle_soft(kLib, 0x574B6FBC, "sceFontDoneLib", h_done_lib);
  register_hle_soft(kLib, 0x099EF33C, "sceFontFindOptimumFont", h_find_optimum_font);
  register_hle_soft(kLib, 0xA834319D, "sceFontOpen", h_open);
  register_hle_soft(kLib, 0x3AEA8CB6, "sceFontClose", h_close);
  register_hle_soft(kLib, 0x0DA7535E, "sceFontGetFontInfo", h_get_font_info);
  register_hle_soft(kLib, 0xDCC80C2F, "sceFontGetCharInfo", h_get_char_info);
  register_hle_soft(kLib, 0xCA1E6945, "sceFontGetCharGlyphImage_Clip", h_glyph_image_clip);
}

}  // namespace k
}  // namespace psp
