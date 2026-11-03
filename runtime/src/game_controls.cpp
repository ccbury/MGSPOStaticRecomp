#include <psp/game_controls.h>
#include <psp/runtime.h>

#include <cstdio>

namespace psp {
namespace {
constexpr uint32_t kDirections = 0xF0;
constexpr uint32_t kPad = 0x08A793DC;
constexpr uint32_t kSettings = 0x089F51D0;
constexpr uint32_t kEquipmentGuard = 0x08894C04;
constexpr uint32_t kSelectDirection = 0x0889742C;

struct Buttons {
  uint32_t held = 0, pressed = 0, released = 0;
  void update(uint32_t next) {
    pressed = next & ~held;
    released = held & ~next;
    held = next;
  }
};

bool g_modern = false, g_trace = false;
uint32_t g_dpad = 0, g_camera = 0, g_other = 0;
bool g_active = false;
uint32_t g_lastGameplayDirections[2] = {UINT32_MAX, UINT32_MAX};
Buttons g_gameplay, g_equipment, g_physical;
uint32_t g_pending = 0;
// Guest threads have separate host stacks; never temporarily overwrite the shared PSP pad.
thread_local bool g_selecting = false;
thread_local uint32_t g_selectDirections = 0, g_selectButton = 0;

uint32_t invoke(Cpu& c, uint32_t function, uint32_t actor) {
  Cpu saved = c;
  c.r[4] = actor;
  call(c, function);
  uint32_t result = c.r[2];
  c = saved;
  return result;
}
}  // namespace

uint32_t camera_directions(int16_t x, int16_t y) {
  // XInput's recommended right-stick deadzone, applied independently to permit diagonals.
  constexpr int deadzone = 8689;
  uint32_t directions = 0;
  if (x > deadzone) directions |= 0x20;
  if (x < -deadzone) directions |= 0x80;
  if (y > deadzone) directions |= 0x10;
  if (y < -deadzone) directions |= 0x40;
  return directions;
}

void controls_init(bool modern, bool trace) {
  g_modern = modern;
  g_trace = trace;
  g_dpad = g_camera = g_other = g_pending = 0;
  g_active = false;
  g_gameplay = g_equipment = g_physical = {};
  g_selecting = false;
  g_lastGameplayDirections[0] = g_lastGameplayDirections[1] = UINT32_MAX;
}

void controls_host_sample(uint32_t dpad, uint32_t camera, uint32_t otherDirections, bool active) {
  g_active = active;
  g_dpad = active ? dpad & kDirections : 0;
  g_camera = active ? camera & kDirections : 0;
  g_other = otherDirections & kDirections;
  if (!active) g_pending = 0;
}

void controls_guest_sample(uint32_t destination, uint32_t source) {
  if (!g_modern || destination != kPad) return;
  uint32_t raw = lw(source + 4);
  // Flags 0..2 are the game's input inhibition flags (pause, scripted input, etc.).
  bool inhibited = (lhu(kPad + 0x1E) & 7) != 0;
  uint32_t base = (raw & ~g_dpad) | (raw & g_other);
  g_equipment.update(base);
  g_gameplay.update(base | (inhibited ? 0 : g_camera));
  g_physical.update(g_dpad);
  // A tap is consumed by the equipment actor, not by firmware reads/peeks.
  g_pending = g_physical.pressed;
  if (g_trace && (g_pending || g_gameplay.pressed || g_gameplay.released))
    std::fprintf(stderr, "[controls] sample dpad=%02X camera=%02X inhibited=%u\n",
                 g_dpad, g_camera, inhibited ? 1u : 0u);
  if (inhibited) g_pending = 0;
}

uint32_t controls_load(uint32_t address, ControlConsumer consumer) {
  uint32_t original = lw(address);
  if (!g_modern || (address != kPad && address != kPad + 0x14 && address != kPad + 0x18))
    return original;
  const Buttons& buttons = consumer == ControlConsumer::Equipment ? g_equipment : g_gameplay;
  uint32_t routed = address == kPad ? buttons.held :
                    address == kPad + 0x14 ? buttons.pressed : buttons.released;
  uint32_t result = (original & ~kDirections) | (routed & kDirections);
  if (consumer == ControlConsumer::Equipment && g_selecting) {
    result &= ~kDirections;
    if (address != kPad + 0x18) result |= g_selectDirections;
    if (address == kPad) result |= g_selectButton;
  }
  unsigned traceIndex = consumer == ControlConsumer::Camera ? 1 : 0;
  if (g_trace && consumer != ControlConsumer::Equipment && address == kPad &&
      (result & kDirections) != g_lastGameplayDirections[traceIndex]) {
    g_lastGameplayDirections[traceIndex] = result & kDirections;
    std::fprintf(stderr, "[controls] %s dpad=%02X camera=%02X directions=%02X\n",
                 traceIndex ? "camera" : "player", g_dpad, g_camera, g_lastGameplayDirections[traceIndex]);
  }
  return result;
}

void controls_equipment(Cpu& c) {
  if (!g_modern || !g_active || !g_pending) return;
  uint32_t directions = g_pending;
  g_pending = 0;
  uint32_t actor = c.r[4];
  uint32_t state = lw(actor + 0x54);
  // States 0..5 are idle/opening/directional selection; 6..12 are other UI operations.
  // The game's own guard rejects pause, dialogue, cutscenes and restricted player actions.
  if (state > 5) {
    if (g_trace) std::fprintf(stderr, "[controls] equipment ignored in UI state %u\n", state);
    return;
  }
  uint32_t restricted = invoke(c, kEquipmentGuard, actor);
  if (restricted != 0) {
    if (g_trace) std::fprintf(stderr, "[controls] equipment rejected by native guard (%08X)\n", restricted);
    return;
  }
  uint32_t settings = lw(kSettings);
  g_selectButton = lw(settings + 0x64);
  g_selectDirections = directions;
  g_selecting = true;
  struct SelectionScope {
    ~SelectionScope() { g_selecting = false; }
  } selection;
  uint32_t result = invoke(c, kSelectDirection, actor);
  if (g_trace)
    std::fprintf(stderr, "[controls] equipment directions=%02X requested=%d selected=%d state=%u result=%d\n",
                 directions, static_cast<int32_t>(lw(actor + 0x48)), static_cast<int32_t>(lw(actor + 0x4C)),
                 state, static_cast<int32_t>(result));
}
}  // namespace psp
