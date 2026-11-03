#pragma once
#include <cstdint>

namespace psp {
struct Cpu;

enum class ControlConsumer { Gameplay, Camera, Equipment };

uint32_t camera_directions(int16_t x, int16_t y);
void controls_init(bool modern, bool trace = false);
void controls_host_sample(uint32_t dpad, uint32_t camera, uint32_t otherDirections, bool active);
void controls_guest_sample(uint32_t destination, uint32_t source);
uint32_t controls_load(uint32_t address, ControlConsumer consumer);
void controls_equipment(Cpu& c);

}  // namespace psp
