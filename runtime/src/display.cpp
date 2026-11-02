// sceDisplay and sceCtrl. Vblank is derived from the kernel clock at 59.94 Hz.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "kernel_internal.h"
#include "platform.h"

namespace psp {
namespace k {

namespace {

constexpr uint64_t kVblankUs = 16683;

struct FrameBuf {
  uint32_t addr = 0;
  uint32_t stride = 512;
  uint32_t format = 3;
};

FrameBuf g_fb;
uint64_t g_vcount = 0;
uint64_t g_nextVblank = 0;
uint64_t g_lastPresented = UINT64_MAX;
std::vector<uint32_t> g_bgra;
std::string g_dumpPath;
uint32_t g_autoButtons = 0;

uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return crc;
}

void png_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  auto be32 = [&](std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
  };
  be32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  be32(out, ~crc32_update(0xFFFFFFFFu, body.data(), body.size()));
}

// Uncompressed (stored) PNG so no zlib dependency is needed.
void write_png(const std::string& path, const uint32_t* argb, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      uint32_t p = argb[y * w + x];
      raw.push_back(static_cast<uint8_t>(p >> 16));
      raw.push_back(static_cast<uint8_t>(p >> 8));
      raw.push_back(static_cast<uint8_t>(p));
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t pos = 0; pos < raw.size();) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(n & 0xFF); z.push_back(n >> 8);
    z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  uint32_t ad = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<uint8_t>(ad >> s));
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  for (uint32_t v : {static_cast<uint32_t>(w), static_cast<uint32_t>(h)})
    for (int s = 24; s >= 0; s -= 8) ihdr.push_back(static_cast<uint8_t>(v >> s));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  png_chunk(out, "IHDR", ihdr);
  png_chunk(out, "IDAT", z);
  png_chunk(out, "IEND", {});
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
  }
}

void present_frame() {
  if (g_lastPresented == g_vcount) return;
  g_lastPresented = g_vcount;
  if (!g_fb.addr || g_fb.stride < 480) return;
  if ((g_fb.addr & 0x0F000000) == 0 && g_fb.addr < kVramSize) g_fb.addr += kVramBase;
  const int w = 480, h = 272;
  uint32_t bpp = g_fb.format == 3 ? 4 : 2;
  uint32_t last = g_fb.addr + (g_fb.stride * (h - 1) + w) * bpp - 1;
  ptr(g_fb.addr);
  ptr(last);
  g_bgra.resize(w * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = ptr(g_fb.addr + y * g_fb.stride * bpp);
    uint32_t* dst = &g_bgra[y * w];
    for (int x = 0; x < w; ++x) {
      uint32_t r, g, b;
      if (g_fb.format == 3) {
        uint32_t px;
        std::memcpy(&px, row + x * 4, 4);
        r = px & 0xFF;
        g = (px >> 8) & 0xFF;
        b = (px >> 16) & 0xFF;
      } else {
        uint16_t px;
        std::memcpy(&px, row + x * 2, 2);
        if (g_fb.format == 0) {
          r = (px & 31) * 255 / 31;
          g = ((px >> 5) & 63) * 255 / 63;
          b = ((px >> 11) & 31) * 255 / 31;
        } else if (g_fb.format == 1) {
          r = (px & 31) * 255 / 31;
          g = ((px >> 5) & 31) * 255 / 31;
          b = ((px >> 10) & 31) * 255 / 31;
        } else {
          r = (px & 15) * 17;
          g = ((px >> 4) & 15) * 17;
          b = ((px >> 8) & 15) * 17;
        }
      }
      dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
  }
  plat::present(g_bgra.data(), w, h);
  if (!g_dumpPath.empty() && g_vcount % 30 == 0) write_png(g_dumpPath, g_bgra.data(), w, h);
}

void wait_vblank(Cpu& c, bool cb) {
  sleep_until(c, display_next_vblank_us(now_us()), cb);
  display_service();
  present_frame();
  ret(c, 0);
}

void h_set_mode(Cpu& c) { ret(c, 0); }

}  // namespace

void display_set_dump_path(const std::string& p) { g_dumpPath = p; }
void display_set_auto_buttons(uint32_t mask) { g_autoButtons = mask; }

namespace {

void h_set_framebuf(Cpu& c) {
  g_fb.addr = arg(c, 0);
  g_fb.stride = arg(c, 1);
  g_fb.format = arg(c, 2);
  ret(c, 0);
}

void h_get_framebuf(Cpu& c) {
  put32(arg(c, 0), g_fb.addr);
  put32(arg(c, 1), g_fb.stride);
  put32(arg(c, 2), g_fb.format);
  ret(c, 0);
}

void h_wait_vblank_start(Cpu& c) { wait_vblank(c, false); }
void h_wait_vblank_start_cb(Cpu& c) { wait_vblank(c, true); }
void h_get_vcount(Cpu& c) { ret(c, static_cast<uint32_t>(display_vcount())); }
void h_hcount(Cpu& c) { ret(c, static_cast<uint32_t>(now_us() * 15734 / 1000000)); }
void h_zero(Cpu& c) { ret(c, 0); }

// ---- sceCtrl ----

struct CtrlState {
  uint32_t mode = 0;
};
CtrlState g_ctrl;

void write_pad(uint32_t buf, uint32_t count, bool negative) {
  plat::Pad p = plat::poll_pad();
  // Scripted input for unattended runs: held for 5 frames out of every 40.
  if (g_autoButtons && (g_vcount % 40) < 5) p.buttons |= g_autoButtons;
  for (uint32_t i = 0; i < std::max<uint32_t>(count, 1); ++i) {
    uint32_t a = buf + i * 16;
    uint32_t buttons = negative ? ~p.buttons : p.buttons;
    sw(a, static_cast<uint32_t>(now_us()));
    sw(a + 4, buttons);
    sb(a + 8, p.lx);
    sb(a + 9, p.ly);
    for (int j = 10; j < 16; ++j) sb(a + j, 0);
  }
}

void h_ctrl_mode(Cpu& c) {
  g_ctrl.mode = arg(c, 0);
  ret(c, 0);
}

void h_ctrl_read(Cpu& c) {
  // Reads block until the next vblank so polling loops pace themselves like real hardware.
  sleep_until(c, display_next_vblank_us(now_us()), false);
  write_pad(arg(c, 0), arg(c, 1), false);
  ret(c, arg(c, 1));
}

void h_ctrl_peek(Cpu& c) {
  write_pad(arg(c, 0), arg(c, 1), false);
  ret(c, arg(c, 1));
}

void h_ctrl_read_neg(Cpu& c) {
  sleep_until(c, display_next_vblank_us(now_us()), false);
  write_pad(arg(c, 0), arg(c, 1), true);
  ret(c, arg(c, 1));
}

void h_ctrl_peek_neg(Cpu& c) {
  write_pad(arg(c, 0), arg(c, 1), true);
  ret(c, arg(c, 1));
}

}  // namespace

void display_init() {
  g_fb = FrameBuf{};
  g_vcount = 0;
  g_nextVblank = now_us() + kVblankUs;
  g_lastPresented = UINT64_MAX;
}

uint64_t display_next_vblank_us(uint64_t after) {
  if (g_nextVblank > after) return g_nextVblank;
  uint64_t behind = (after - g_nextVblank) / kVblankUs + 1;
  return g_nextVblank + behind * kVblankUs;
}

uint32_t display_vcount() { return static_cast<uint32_t>(g_vcount); }

uint64_t display_next_event_us() { return intr_handlers_registered(kIntrVblank) ? g_nextVblank : UINT64_MAX; }

void display_service() {
  uint64_t now = now_us();
  if (now < g_nextVblank) return;
  uint64_t behind = (now - g_nextVblank) / kVblankUs + 1;
  g_vcount += behind;
  g_nextVblank += behind * kVblankUs;
  if (intr_handlers_registered(kIntrVblank)) dispatch_intr(kIntrVblank);
}

void register_display() {
  const char* m = "sceDisplay";
  register_hle(m, 0x0E20F177, "sceDisplaySetMode", h_set_mode);
  register_hle(m, 0x289D82FE, "sceDisplaySetFrameBuf", h_set_framebuf);
  register_hle(m, 0xEEDA2E54, "sceDisplayGetFrameBuf", h_get_framebuf);
  register_hle(m, 0x984C27E7, "sceDisplayWaitVblankStart", h_wait_vblank_start);
  register_hle(m, 0x46F186C3, "sceDisplayWaitVblankStartCB", h_wait_vblank_start_cb);
  register_hle(m, 0x36CDFADE, "sceDisplayWaitVblank", h_wait_vblank_start);
  register_hle(m, 0x8EB9EC49, "sceDisplayWaitVblankCB", h_wait_vblank_start_cb);
  register_hle(m, 0x9C6EAAD7, "sceDisplayGetVcount", h_get_vcount);
  register_hle(m, 0x210EAB3A, "sceDisplayGetAccumulatedHcount", h_hcount);
  register_hle(m, 0x773DD3A3, "sceDisplayGetCurrentHcount", h_hcount);
  register_hle(m, 0xDEA197D4, "sceDisplayGetMode", h_zero);
}

void register_ctrl() {
  const char* m = "sceCtrl";
  register_hle(m, 0x1F4011E6, "sceCtrlSetSamplingMode", h_ctrl_mode);
  register_hle(m, 0x6A2774F3, "sceCtrlSetSamplingCycle", h_zero);
  register_hle(m, 0x1F803938, "sceCtrlReadBufferPositive", h_ctrl_read);
  register_hle(m, 0x3A622550, "sceCtrlPeekBufferPositive", h_ctrl_peek);
  register_hle(m, 0x60B81F86, "sceCtrlReadBufferNegative", h_ctrl_read_neg);
  register_hle(m, 0xC152080A, "sceCtrlPeekBufferNegative", h_ctrl_peek_neg);
}

}  // namespace k
}  // namespace psp
