#include <psp/runtime.h>
#include <psp/kernel.h>
#include "../runtime/src/platform.h"
#include "../runtime/src/ctrl_sampling.h"
#include <cstdio>
#include <stdexcept>

namespace psp { namespace k {
void display_set_gamepad_script(const std::string&);
void display_set_press_script(const std::string&);
} }

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
  std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)
constexpr uint32_t pad = 0x08A793DC, source = 0x08A19984;
constexpr uint32_t actor = 0x08A90000, settings = 0x08AA0000;
int guardCalls = 0, selections = 0;
uint32_t blocked = 0, selectedDirections = 0;

void guard(psp::Cpu& c) {
  ++guardCalls;
  CHECK(c.r[4] == actor);
  c.r[2] = blocked;
}

void select(psp::Cpu& c) {
  ++selections;
  CHECK(c.r[4] == actor);
  selectedDirections = psp::controls_load(pad, psp::ControlConsumer::Equipment);
  CHECK((selectedDirections & 0x2000) != 0);
  CHECK((psp::controls_load(pad + 0x14, psp::ControlConsumer::Equipment) & 0xF0) ==
        (selectedDirections & 0xF0));
  CHECK(psp::lw(pad) == psp::lw(source + 4));
  // Deliberately clobber caller-saved registers, like the real selector.
  c.r[2] = 123;
  c.r[5] = 456;
}

void sample(uint32_t physical, uint32_t camera, uint32_t other = 0, uint32_t extra = 0,
            bool active = true) {
  uint32_t raw = physical | other | extra;
  psp::sw(source + 4, raw);
  psp::sw(pad, raw);
  psp::sw(pad + 0x14, raw);
  psp::sw(pad + 0x18, 0);
  psp::controls_host_sample(physical, camera, other, active);
  psp::controls_guest_sample(pad, source);
}

uint32_t gameplay(uint32_t offset = 0) {
  return psp::controls_load(pad + offset, psp::ControlConsumer::Gameplay);
}
uint32_t equipment(uint32_t offset = 0) {
  return psp::controls_load(pad + offset, psp::ControlConsumer::Equipment);
}

void testRouting() {
  psp::controls_init(true);
  sample(0x10, 0x80, 0, 0x4000);
  CHECK(gameplay() == 0x4080);
  CHECK(psp::controls_load(pad, psp::ControlConsumer::Camera) == 0x4080);
  CHECK(equipment() == 0x4000);
  CHECK(psp::lw(pad) == 0x4010);  // Menus and firmware still see physical D-pad.
  CHECK((gameplay(0x14) & 0xF0) == 0x80);
  for (int i = 0; i != 4; ++i) CHECK(gameplay() == 0x4080);
  sample(0x10, 0x80, 0, 0x2000);
  CHECK(gameplay() == 0x2080);  // Physical B cannot send camera directions to weapons.
  CHECK(equipment() == 0x2000);
  CHECK((gameplay(0x14) & 0xF0) == 0);
  sample(0, 0);
  CHECK((gameplay(0x18) & 0xF0) == 0x80);
  CHECK((gameplay(0x14) & 0xF0) == 0);
  sample(0x30, 0xC0, 0x10);
  CHECK(gameplay() == 0xD0);  // Preserve a keyboard direction overlapping physical D-pad.
  CHECK(equipment() == 0x10);
  sample(0, 0, 0x20, 0x2000);
  CHECK(equipment() == 0x2020);  // Keyboard B + direction unchanged.
  CHECK(gameplay() == 0x2020);
  psp::sw(pad + 8, 0x12345678);
  CHECK(psp::controls_load(pad + 8, psp::ControlConsumer::Gameplay) == 0x12345678);
  psp::sh(pad + 0x1E, 4);
  sample(0x10, 0x20);
  CHECK((gameplay() & 0xF0) == 0);
  psp::sh(pad + 0x1E, 0);
  sample(0, 0);
  CHECK((gameplay() & 0xF0) == 0);
  psp::controls_init(false);
  sample(0x10, 0x80);
  CHECK(gameplay() == 0x10 && equipment() == 0x10);
}

void testGamepadMapping() {
  psp::controls_init(true);
  CHECK(psp::controls_gamepad_buttons(0, 30, 30) == 0);
  CHECK(psp::controls_gamepad_buttons(0, 31, 0) == 0x200);
  CHECK(psp::controls_gamepad_buttons(0, 0, 31) == 0x8000);
  CHECK(psp::controls_gamepad_buttons(0, 255, 255) == 0x8200);
  CHECK(psp::controls_gamepad_buttons(0x0200, 0, 0) == 0x2000);
  CHECK(psp::controls_gamepad_buttons(0x2000, 0, 0) == 0x2000);
  CHECK(psp::controls_gamepad_buttons(0x4000, 0, 0) == 0x8000);
  auto frame = [](uint16_t b, uint8_t lt, uint8_t rt) {
    sample(0, 0, 0, psp::controls_gamepad_buttons(b, lt, rt));
  };
  // Aliases are combined before guest edge detection, not released independently.
  uint32_t previous = 0;
  for (auto input : {std::pair<uint16_t, uint8_t>{0x4000, 0}, {0x4000, 255}, {0, 255}, {0, 0}}) {
    uint32_t next = psp::controls_gamepad_buttons(input.first, 255, input.second);
    CHECK((next & 0x200) != 0);  // LT remains independent of RT.
    uint32_t released = previous & ~next;
    CHECK((released & 0x8000) == (input.first == 0 && input.second == 0 ? 0x8000u : 0u));
    previous = next;
    frame(input.first, 255, input.second);
    CHECK(gameplay() == next && equipment() == next);
  }
  previous = 0;
  for (uint16_t input : {0x2000, 0x2200, 0x0200, 0x0000}) {
    uint32_t next = psp::controls_gamepad_buttons(input, 0, 0);
    CHECK((next & 0x200) == 0);  // RB never enters first person.
    CHECK((previous & ~next) == (input == 0 ? 0x2000u : 0u));
    previous = next;
  }
  // Release the trigger/shoulder alias first as well as the face-button alias first.
  CHECK(psp::controls_gamepad_buttons(0x4000, 0, 255) ==
        psp::controls_gamepad_buttons(0x4000, 0, 0));
  CHECK(psp::controls_gamepad_buttons(0x2200, 0, 0) ==
        psp::controls_gamepad_buttons(0x2000, 0, 0));
  psp::controls_init(false);
  CHECK(psp::controls_gamepad_buttons(0x0200, 255, 255) == 0x200);
  CHECK(psp::controls_gamepad_buttons(0, 255, 255) == 0);
  CHECK(psp::controls_gamepad_buttons(0xF330, 0, 0) == 0xF309);
}

void testStickRoles() {
  psp::controls_init(true);
  psp::sh(pad + 0x1E, 0);
  auto route = [](uint32_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    psp::plat::Pad p;
    p.gamepadActive = true;
    p.buttons = buttons;
    p.gamepadLx = lx; p.gamepadLy = ly;
    p.gamepadRx = rx; p.gamepadRy = ry;
    psp::controls_route_sticks(p);
    return p;
  };
  auto p = route(0, 32767, 0, 0, 32767);
  CHECK(p.lx == 255 && p.ly == 128 && p.cameraDirections == 0x10);
  p = route(0x200, 32767, 0, 0, 32767);
  CHECK(p.lx == 128 && p.ly == 1 && p.cameraDirections == 0x20);
  // Right stick only cannot move; left stick only cannot aim in native first person.
  p = route(0x200, 0, 0, -32768, 32767);
  CHECK(p.lx == 1 && p.ly == 1 && p.cameraDirections == 0);
  p = route(0x200, -32768, 32767, 0, 0);
  CHECK(p.lx == 128 && p.ly == 128 && p.cameraDirections == 0x90);
  sample(0x20, p.cameraDirections, 0, 0x200);
  CHECK(gameplay() == 0x290 && equipment() == 0x200);
  CHECK(psp::lw(pad) == 0x220);  // Physical D-pad remains available to menus.
  p = route(0x200, 7849, -7849, 8689, -8689);
  CHECK(p.lx == 128 && p.ly == 128 && p.cameraDirections == 0);
  p = route(0x200, 7850, -7850, 8690, -8690);
  CHECK(p.lx > 128 && p.ly > 128 && p.cameraDirections == 0x60);
  p = route(0x2000, 32767, 0, 0, 32767);
  CHECK(p.lx == 255 && p.ly == 128 && p.cameraDirections == 0x10);
  for (uint16_t inhibit : {1, 2, 4}) {
    psp::sh(pad + 0x1E, inhibit);
    p = route(0x200, 32767, 0, 0, 32767);
    CHECK(p.lx == 255 && p.ly == 128 && p.cameraDirections == 0x10);
  }
  psp::sh(pad + 0x1E, 0);
  for (auto block : {std::pair<uint32_t, uint32_t>{0x08A5CB7C, 0x2000},
                    {0x08A5CB80, 0x8000}, {0x08A5CB74, 0x80000000u},
                    {0x08A5CB78, 0x80000000u}, {0x08A5CBA0, 1}}) {
    psp::sw(block.first, block.second);
    p = route(0x200, 32767, 0, 0, 32767);
    CHECK(p.lx == 255 && p.ly == 128 && p.cameraDirections == 0x10);
    psp::sw(block.first, 0);
  }
  psp::plat::Pad keyboard;
  keyboard.buttons = 0x200;
  keyboard.lx = 192; keyboard.ly = 64;
  psp::controls_route_sticks(keyboard);
  CHECK(keyboard.lx == 192 && keyboard.ly == 64 && keyboard.cameraDirections == 0);
  keyboard.gamepadActive = true;
  keyboard.gamepadRx = 32767;
  keyboard.gamepadRy = -32768;
  psp::controls_route_sticks(keyboard);
  CHECK(keyboard.lx == 255 && keyboard.ly == 191 && keyboard.cameraDirections == 0);
  keyboard = {};
  keyboard.gamepadLx = keyboard.gamepadRx = 32767;
  psp::controls_route_sticks(keyboard);
  CHECK(keyboard.lx == 128 && keyboard.ly == 128 && keyboard.cameraDirections == 0);
  p = route(0, 0, 0, 0, 0);
  CHECK(p.lx == 128 && p.ly == 128 && p.cameraDirections == 0);
  psp::controls_init(false);
  p = route(0x200, 32767, 0, 0, 32767);
  CHECK(p.lx == 255 && p.ly == 128 && p.cameraDirections == 0x10);
}

void testNativeSelection() {
  const psp::FuncEntry functions[] = {{0x08894C04, guard}, {0x0889742C, select}};
  psp::bind_tables(functions, 2, nullptr, 0);
  psp::sw(0x089F51D0, settings);
  psp::sw(settings + 0x64, 0x2000);
  psp::sw(actor + 0x54, 0);
  psp::controls_init(true);
  psp::Cpu c;
  c.r[4] = actor;
  c.r[5] = 17;
  c.r[2] = 23;
  sample(0x30, 0xC0);
  psp::controls_equipment(c);
  CHECK(selections == 1 && guardCalls == 1);
  CHECK(selectedDirections == 0x2030);  // Native selector retains diagonal priority.
  CHECK(c.r[2] == 23 && c.r[5] == 17 && c.depth == 0);
  psp::controls_equipment(c);
  CHECK(selections == 1);  // Peeking/reading does not repeat the action.
  sample(0x30, 0xC0);
  psp::controls_equipment(c);
  CHECK(selections == 1);  // Holding a direction does not toggle the weapon repeatedly.
  sample(0x70, 0x20);  // Adding Down while holding Up + Right selects only the new direction.
  psp::controls_equipment(c);
  CHECK(selections == 2 && selectedDirections == 0x2040);
  sample(0, 0x10);
  psp::controls_equipment(c);
  CHECK(selections == 2);
  sample(0x80, 0);
  blocked = 1;
  psp::controls_equipment(c);
  CHECK(selections == 2 && guardCalls == 3);
  blocked = 0;
  psp::controls_equipment(c);
  CHECK(selections == 2);  // Rejected menu input is never queued for later gameplay.
  sample(0, 0);
  sample(0x10, 0);
  psp::sw(actor + 0x54, 6);
  psp::controls_equipment(c);
  CHECK(selections == 2);
  psp::sw(actor + 0x54, 0);
  psp::sh(pad + 0x1E, 4);
  sample(0, 0);
  sample(0x20, 0);
  psp::controls_equipment(c);
  CHECK(selections == 2);
  psp::sh(pad + 0x1E, 0);
  sample(0x20, 0);
  psp::controls_equipment(c);
  CHECK(selections == 2);  // Holding menu navigation across unpause isn't a new weapon press.
  sample(0, 0);
  sample(0x10, 0);
  psp::controls_host_sample(0, 0, 0, false);
  psp::controls_equipment(c);
  CHECK(selections == 2);
  psp::controls_init(false);
  sample(0x20, 0x10);
  psp::controls_equipment(c);
  CHECK(selections == 2);
}

void firmwareDriver(psp::Cpu& c) {
  constexpr uint32_t buffer = 0x08AB0000;
  for (uint32_t import = 0; import != 4; ++import) {
    c.r[4] = buffer;
    c.r[5] = 2;
    psp::hle(c, import);
    CHECK(c.r[2] == 2);
    uint32_t expected = import >= 2 ? ~0x4010u : 0x4010u;
    CHECK(psp::lw(buffer + 4) == expected);
    CHECK(psp::lw(buffer + 20) == expected);
    CHECK(psp::lbu(buffer + 8) == 128 && psp::lbu(buffer + 9) == 128);
  }
  psp::sw(source + 4, 0x4010);
  psp::sw(pad, 0x4010);
  psp::controls_guest_sample(pad, source);
  CHECK(gameplay() == 0x4030);
  CHECK(equipment() == 0x4000);
  CHECK((gameplay(0x14) & 0xF0) == 0x30);
  // Firmware peeks update the host sample but must not consume the game's edges.
  c.r[4] = buffer;
  c.r[5] = 1;
  psp::hle(c, 1);
  CHECK((gameplay(0x14) & 0xF0) == 0x30);
  psp::k::display_set_press_script("0:0x80:10:64:-64");
  psp::hle(c, 1);
  CHECK(psp::lw(buffer + 4) == 0x4090);
  CHECK(psp::lbu(buffer + 8) == 192 && psp::lbu(buffer + 9) == 64);
  psp::sw(source + 4, 0x4090);
  psp::sw(pad, 0x4090);
  psp::controls_guest_sample(pad, source);
  CHECK(gameplay() == 0x40B0 && equipment() == 0x4080);
  // Both scripted sticks pass through the same first-person router as XInput.
  psp::k::display_set_gamepad_script("0:0x200:10:0:127:127:0");
  psp::k::display_set_press_script("0:0:10:0:0");
  for (uint32_t import = 0; import != 4; ++import) {
    c.r[4] = buffer;
    c.r[5] = 2;
    psp::hle(c, import);
    uint32_t expected = import >= 2 ? ~0x4290u : 0x4290u;
    CHECK(psp::lw(buffer + 4) == expected && psp::lw(buffer + 20) == expected);
    // Raw --press analog override still wins, without affecting gamepad direction metadata.
    CHECK(psp::lbu(buffer + 8) == 192 && psp::lbu(buffer + 9) == 64);
  }
  psp::sw(source + 4, 0x4290);
  psp::sw(pad, 0x4290);
  psp::controls_guest_sample(pad, source);
  CHECK(gameplay() == 0x42A0 && equipment() == 0x4280);
  CHECK((gameplay(0x18) & 0xF0) == 0x10);
  // Another peek must not consume the routed direction's rising/falling edges.
  c.r[4] = buffer;
  c.r[5] = 1;
  psp::hle(c, 1);
  CHECK((gameplay(0x14) & 0xF0) == 0);
  CHECK((gameplay(0x18) & 0xF0) == 0x10);
  c.r[2] = 0;
  psp::hle(c, 4);
}

void testPrompts() {
  constexpr uint32_t d = 0x08AB0000, h = d + 64, palette = h + 64, original = 0x08AC0000;
  for (uint32_t a = d; a < palette + 64; a += 4) psp::sw(a, 0);
  psp::sw(d, 0x10);
  psp::sw(d + 4, 0xC850DF);
  psp::sw(d + 8, 0xEB5398);
  psp::sw(d + 12, 0x000C000C);
  psp::sw(d + 20, h);
  psp::sw(h, 0x40105020);
  psp::sw(h + 4, 4);
  psp::sw(h + 12, palette);
  for (unsigned i = 0; i < 16; ++i) psp::sw(palette + i * 4, 0xFF202020);
  psp::sw(palette, 0);
  psp::sw(palette + 60, 0xFFFFFFFF);
  psp::prompts_init(false);
  CHECK(psp::prompts_texture(d, original) == original);
  psp::prompts_init(true);
  for (bool modernControls : {false, true}) {
    psp::controls_init(modernControls);
    uint32_t y = psp::prompts_texture(d, original);
    CHECK(y != original);
    CHECK(psp::prompts_texture(d, original) == y);
    CHECK(psp::lw(d + 20) == h && psp::lw(h + 8) == 0);
    for (unsigned row = 0; row < 16; ++row)
      for (unsigned col = 0; col < 32; ++col) {
        uint8_t packed = psp::lbu(y + row / 8 * 128 + row % 8 * 16 + col / 2);
        CHECK(((packed >> (col % 2 * 4)) & 15) == (psp::prompts_pixel('Y', col, row) ? 15 : 0));
      }
    psp::sw(d + 4, 0xB450DB);
    CHECK(psp::prompts_texture(d, original) != y);
    psp::sw(d + 8, 0x295087);
    CHECK(psp::prompts_texture(d, original) == original);
    psp::sw(d + 8, 0xA6753D);
    CHECK(psp::prompts_texture(d, original) != original);
    psp::sw(d + 4, 0xF3DB22);
    CHECK(psp::prompts_texture(d, original) == original);  // D-pad, not a face button.
    psp::sw(d + 4, 0xC850DF);
    psp::sw(d + 12, 0x00100010);
    CHECK(psp::prompts_texture(d, original) == original);
    psp::sw(d + 12, 0x000C000C);
    psp::sw(d + 8, 0xEB5398);
  }
  CHECK(!psp::prompts_pixel('B', 5, 5));
  CHECK(!psp::prompts_pixel('X', 12, 0));
  psp::sw(palette + 60, 0xE7FFFFFF);
  CHECK(psp::prompts_texture(d, original) != original);  // Native antialiased icons need not reach alpha255.
  psp::sw(palette + 60, 0xFF000000);
  CHECK(psp::prompts_texture(d, original) == original);
  CHECK(psp::prompts_texture(0, original) == original);
  psp::prompts_init(false);
  CHECK(psp::prompts_texture(d, original) == original);
}

void testFirmware() {
  psp::KernelConfig cfg;
  cfg.headless = true;
  psp::kernel_init(cfg, 0x08900000, 0);
  testPrompts();
  psp::register_all_hle();
  const psp::FuncEntry functions[] = {{0x08804000, firmwareDriver}};
  const psp::ImportEntry imports[] = {
      {0x08900000, "sceCtrl", 0x1F803938}, {0x08900008, "sceCtrl", 0x3A622550},
      {0x08900010, "sceCtrl", 0x60B81F86}, {0x08900018, "sceCtrl", 0xC152080A},
      {0x08900020, "LoadExecForUser", 0x05572A5F}};
  psp::bind_tables(functions, 1, imports, 5);
  psp::controls_init(true);
  for (const char* invalid : {"", "x:0", "-1:0", "0:0:0", "0:0:1:128:0",
                              "0:0:1:0:1junk", "0:0xZZ", "0:0:1:0", "0:0x40:10,invalid",
                              "0:0:1:0:0:128:0", "0:0:1:0:0:0", "0:0:1:0:0:0:1junk"}) {
    bool rejected = false;
    try { psp::k::display_set_gamepad_script(invalid); }
    catch (const std::invalid_argument&) { rejected = true; }
    catch (const std::out_of_range&) { rejected = true; }
    CHECK(rejected);
  }
  psp::k::display_set_gamepad_script("0:0x4010:10:127:-127");
  CHECK(psp::kernel_run_main(0x08804000) == 0);
}

void testReadCycle() {
  psp::k::CtrlReadCycle reads;
  CHECK(reads.take(0));
  CHECK(!reads.take(0));
  CHECK(!reads.take(0));
  CHECK(reads.take(1));
  CHECK(!reads.take(1));
  CHECK(reads.take(4));  // Several elapsed samples still make one fresh read available.
  CHECK(!reads.take(4));
  CHECK(reads.take(0x100000000ull));
  CHECK(!reads.take(0x100000000ull));
  reads = {};
  CHECK(reads.take(0));
}
}  // namespace

int main() {
  psp::init_memory();
  CHECK(psp::camera_directions(8689, -8689) == 0);
  CHECK(psp::camera_directions(8690, -8690) == 0x60);
  CHECK(psp::camera_directions(-8690, 8690) == 0x90);
  CHECK(psp::camera_directions(-32768, 32767) == 0x90);
  CHECK(psp::camera_directions(32767, -32768) == 0x60);
  testRouting();
  testGamepadMapping();
  testStickRoles();
  testNativeSelection();
  testReadCycle();
  testFirmware();
  psp::free_memory();
  return failures ? 1 : 0;
}
