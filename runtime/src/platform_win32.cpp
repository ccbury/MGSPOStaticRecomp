// Win32 host layer: GDI window, keyboard/XInput pad, waveOut audio.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstdlib>
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "platform.h"
#include <psp/game_controls.h>

namespace psp {
namespace plat {

namespace {

constexpr int kScale = 2;
constexpr int kW = 480, kH = 272;

std::atomic<bool> g_quit{false};
std::atomic<bool> g_foreground{false};
bool g_headless = false;
HWND g_hwnd = nullptr;
std::thread g_windowThread;
std::mutex g_frameMutex;
std::vector<uint32_t> g_frame;
std::condition_variable g_ready;
bool g_windowUp = false;

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_CLOSE:
      g_quit = true;
      return 0;
    case WM_SETFOCUS: g_foreground = true; return 0;
    case WM_KILLFOCUS: g_foreground = false; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      std::lock_guard<std::mutex> lk(g_frameMutex);
      if (!g_frame.empty()) {
        RECT rc;
        GetClientRect(h, &rc);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = kW;
        bi.bmiHeader.biHeight = -kH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, (rc.right >= kW * 2) ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        // keep the 480x272 aspect ratio and fill the rest with black
        int dw = rc.right, dh = rc.bottom;
        if (dw * kH > dh * kW) dw = dh * kW / kH;
        else dh = dw * kH / kW;
        int dx = (rc.right - dw) / 2, dy = (rc.bottom - dh) / 2;
        PatBlt(dc, 0, 0, rc.right, dy, BLACKNESS);
        PatBlt(dc, 0, dy + dh, rc.right, rc.bottom - dy - dh, BLACKNESS);
        PatBlt(dc, 0, dy, dx, dh, BLACKNESS);
        PatBlt(dc, dx + dw, dy, rc.right - dx - dw, dh, BLACKNESS);
        if (dw > 0 && dh > 0)
          StretchDIBits(dc, dx, dy, dw, dh, 0, 0, kW, kH, g_frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
      }
      EndPaint(h, &ps);
      return 0;
    }
    default: return DefWindowProcA(h, msg, w, l);
  }
}

void window_main() {
  WNDCLASSA wc{};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = "mgspo_window";
  RegisterClassA(&wc);
  RECT rc{0, 0, kW * kScale, kH * kScale};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
  AdjustWindowRect(&rc, style, FALSE);
  g_hwnd = CreateWindowA("mgspo_window", "Metal Gear Solid: Portable Ops", style | WS_VISIBLE, CW_USEDEFAULT,
                         CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance,
                         nullptr);
  {
    std::lock_guard<std::mutex> lk(g_frameMutex);
    g_windowUp = true;
  }
  g_ready.notify_all();
  MSG msg;
  while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
}

// ---- XInput, loaded dynamically so there is no hard dependency ----

struct XGamepad {
  WORD buttons;
  BYTE lt, rt;
  SHORT lx, ly, rx, ry;
};
struct XState {
  DWORD packet;
  XGamepad pad;
};
using XGetState = DWORD(WINAPI*)(DWORD, XState*);
XGetState g_xinput = nullptr;

void load_xinput() {
  for (const char* name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
    HMODULE m = LoadLibraryA(name);
    if (m) {
      g_xinput = reinterpret_cast<XGetState>(GetProcAddress(m, "XInputGetState"));
      if (g_xinput) return;
    }
  }
}

bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ---- audio ----

constexpr int kBuffers = 4;
constexpr uint32_t kBufFrames = 1024;
HWAVEOUT g_wave = nullptr;
WAVEHDR g_hdr[kBuffers];
std::vector<int16_t> g_abuf[kBuffers];
std::thread g_audioThread;
std::atomic<bool> g_audioRun{false};
AudioFill g_fill = nullptr;

void audio_main() {
  while (g_audioRun) {
    bool fed = false;
    for (int i = 0; i < kBuffers; ++i) {
      if (g_hdr[i].dwFlags & WHDR_INQUEUE) continue;
      g_fill(g_abuf[i].data(), kBufFrames);
      waveOutWrite(g_wave, &g_hdr[i], sizeof(WAVEHDR));
      fed = true;
    }
    if (!fed) Sleep(5);
  }
}

}  // namespace

void init(bool headless) {
  g_headless = headless;
  timeBeginPeriod(1);
  if (headless) return;
  load_xinput();
  g_windowThread = std::thread(window_main);
  g_windowThread.detach();
  std::unique_lock<std::mutex> lk(g_frameMutex);
  g_ready.wait_for(lk, std::chrono::seconds(5), [] { return g_windowUp; });
}

void present(const uint32_t* bgra, int width, int height) {
  if (g_headless || !g_hwnd || width != kW || height != kH) return;
  {
    std::lock_guard<std::mutex> lk(g_frameMutex);
    g_frame.assign(bgra, bgra + width * height);
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

bool quit_requested() { return g_quit.load(); }

Pad poll_pad() {
  Pad p;
  if (g_headless || !g_foreground) return p;
  struct Map {
    int vk;
    uint32_t bit;
  };
  static const Map map[] = {
      {VK_UP, 0x10},     {VK_RIGHT, 0x20}, {VK_DOWN, 0x40},  {VK_LEFT, 0x80}, {'Q', 0x100},    {'E', 0x200},
      {'Z', 0x4000},     {'X', 0x2000},    {'A', 0x8000},    {'S', 0x1000},   {VK_RETURN, 0x8}, {VK_RSHIFT, 0x1},
      {VK_SPACE, 0x4000},
  };
  for (const Map& m : map)
    if (key(m.vk)) p.buttons |= m.bit;
  p.otherDirections = p.buttons & 0xF0;
  // IJKL drives the analog stick
  int dx = 0, dy = 0;
  if (key('J')) dx -= 127;
  if (key('L')) dx += 127;
  if (key('I')) dy -= 127;
  if (key('K')) dy += 127;
  if (g_xinput) {
    XState st{};
    if (g_xinput(0, &st) == 0) {
      WORD b = st.pad.buttons;
      p.gamepadDirections = ((b & 1) ? 0x10 : 0) | ((b & 8) ? 0x20 : 0) |
                            ((b & 2) ? 0x40 : 0) | ((b & 4) ? 0x80 : 0);
      p.buttons |= p.gamepadDirections;
      p.gamepadActive = true;
      p.buttons |= controls_gamepad_buttons(b, st.pad.lt, st.pad.rt);
      p.gamepadLx = st.pad.lx;
      p.gamepadLy = st.pad.ly;
      p.gamepadRx = st.pad.rx;
      p.gamepadRy = st.pad.ry;
    }
  }
  p.lx = static_cast<uint8_t>(128 + std::max(-128, std::min(127, dx)));
  p.ly = static_cast<uint8_t>(128 + std::max(-128, std::min(127, dy)));
  return p;
}

bool audio_start(int rate, AudioFill fill) {
  if (g_headless || g_wave) return false;
  WAVEFORMATEX fmt{};
  fmt.wFormatTag = WAVE_FORMAT_PCM;
  fmt.nChannels = 2;
  fmt.nSamplesPerSec = static_cast<DWORD>(rate);
  fmt.wBitsPerSample = 16;
  fmt.nBlockAlign = 4;
  fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
  if (waveOutOpen(&g_wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
    g_wave = nullptr;
    return false;
  }
  g_fill = fill;
  for (int i = 0; i < kBuffers; ++i) {
    g_abuf[i].assign(kBufFrames * 2, 0);
    std::memset(&g_hdr[i], 0, sizeof(WAVEHDR));
    g_hdr[i].lpData = reinterpret_cast<LPSTR>(g_abuf[i].data());
    g_hdr[i].dwBufferLength = kBufFrames * 4;
    waveOutPrepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
  }
  g_audioRun = true;
  g_audioThread = std::thread(audio_main);
  g_audioThread.detach();
  return true;
}

void audio_stop() { g_audioRun = false; }

namespace {
HDC g_fontDc = nullptr;
HFONT g_font = nullptr;
int g_fontPx = 0;

bool select_font(int px) {
  if (!g_fontDc) g_fontDc = CreateCompatibleDC(nullptr);
  if (!g_fontDc) return false;
  if (g_fontPx != px) {
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    if (!f) return false;
    SelectObject(g_fontDc, f);
    if (g_font) DeleteObject(g_font);
    g_font = f;
    g_fontPx = px;
  }
  return true;
}
}  // namespace

bool glyph_raster(uint32_t cp, int px, Glyph& g) {
  static std::mutex m;
  std::lock_guard<std::mutex> lk(m);
  if (!select_font(px)) return false;
  const MAT2 ident = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
  GLYPHMETRICS gm{};
  DWORD size = GetGlyphOutlineW(g_fontDc, cp, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &ident);
  if (size == GDI_ERROR) return false;
  g.advance = static_cast<float>(gm.gmCellIncX);
  g.width = static_cast<int>(gm.gmBlackBoxX);
  g.height = static_cast<int>(gm.gmBlackBoxY);
  g.left = gm.gmptGlyphOrigin.x;
  g.top = gm.gmptGlyphOrigin.y;
  g.alpha.assign(static_cast<size_t>(g.width) * g.height, 0);
  if (size == 0 || g.width == 0 || g.height == 0) {
    g.width = g.height = 0;
    g.alpha.clear();
    return true;
  }
  std::vector<uint8_t> raw(size);
  if (GetGlyphOutlineW(g_fontDc, cp, GGO_GRAY8_BITMAP, &gm, size, raw.data(), &ident) == GDI_ERROR) return false;
  size_t pitch = (static_cast<size_t>(g.width) + 3) & ~3u;
  for (int y = 0; y < g.height; ++y)
    for (int x = 0; x < g.width; ++x)
      g.alpha[static_cast<size_t>(y) * g.width + x] =
          static_cast<uint8_t>(std::min(255, raw[y * pitch + x] * 255 / 64));
  return true;
}

FontMetrics font_metrics(int px) {
  static std::mutex m;
  std::lock_guard<std::mutex> lk(m);
  FontMetrics fm;
  TEXTMETRICW tm{};
  if (select_font(px) && GetTextMetricsW(g_fontDc, &tm)) {
    fm.ascent = static_cast<float>(tm.tmAscent);
    fm.descent = static_cast<float>(tm.tmDescent);
    fm.maxAdvance = static_cast<float>(tm.tmMaxCharWidth);
  } else {
    fm = {px * 0.8f, px * 0.2f, px * 0.6f};
  }
  return fm;
}

}  // namespace plat
}  // namespace psp
