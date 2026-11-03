#pragma once
#include <cstdint>

namespace psp {
struct Cpu;
namespace plat { struct Pad; }

enum class ControlConsumer { Gameplay, Camera, Equipment };

uint32_t camera_directions(int16_t x, int16_t y);
uint32_t controls_gamepad_buttons(uint16_t buttons, uint8_t leftTrigger, uint8_t rightTrigger);
void controls_route_sticks(plat::Pad& pad);
void controls_init(bool modern, bool trace = false);
void controls_host_sample(uint32_t dpad, uint32_t camera, uint32_t otherDirections, bool active);
void controls_guest_sample(uint32_t destination, uint32_t source);
uint32_t controls_load(uint32_t address, ControlConsumer consumer);
void controls_equipment(Cpu& c);

}  // namespace psp
