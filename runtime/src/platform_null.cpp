// Headless host layer used on non-Windows builds and with --headless.
#include "platform.h"

namespace psp {
namespace plat {

void init(bool) {}
void present(const uint32_t*, int, int) {}
bool quit_requested() { return false; }
Pad poll_pad() { return {}; }
bool audio_start(int, AudioFill) { return false; }
void audio_stop() {}

}  // namespace plat
}  // namespace psp
