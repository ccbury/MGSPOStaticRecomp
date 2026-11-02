// Host services the firmware layer needs: window, input, audio output.
#pragma once
#include <cstdint>

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

}  // namespace plat
}  // namespace psp
