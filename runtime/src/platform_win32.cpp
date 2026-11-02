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
      if (b & 0x0001) p.buttons |= 0x10;
      if (b & 0x0008) p.buttons |= 0x20;
      if (b & 0x0002) p.buttons |= 0x40;
      if (b & 0x0004) p.buttons |= 0x80;
      if (b & 0x0010) p.buttons |= 0x8;
      if (b & 0x0020) p.buttons |= 0x1;
      if (b & 0x0100) p.buttons |= 0x100;
      if (b & 0x0200) p.buttons |= 0x200;
      if (b & 0x1000) p.buttons |= 0x4000;
      if (b & 0x2000) p.buttons |= 0x2000;
      if (b & 0x4000) p.buttons |= 0x8000;
      if (b & 0x8000) p.buttons |= 0x1000;
      auto axis = [](SHORT v) { return std::abs(v) < 8000 ? 0 : v / 258; };
      dx += axis(st.pad.lx);
      dy -= axis(st.pad.ly);
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

}  // namespace plat
}  // namespace psp
