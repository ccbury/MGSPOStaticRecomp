#pragma once
#include <cstdint>

namespace psp {
// Mode is selected at launch, before guest threads or queued draws exist.
void prompts_init(bool modern);
uint32_t prompts_texture(uint32_t descriptor, uint32_t original);
// Zero means the resource/context is not positively identified.
char prompts_label(uint32_t pack, uint32_t sprite);
bool prompts_pixel(char label, unsigned x, unsigned y);
}  // namespace psp
