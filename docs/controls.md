# Gamepad controls

The default `--controls modern` layout changes only XInput directional controls:

| Input | Gameplay | Menus and dialogue |
|---|---|---|
| Left stick | Original movement | Original behavior |
| Right stick | Original D-pad camera directions | No action |
| D-pad | Direct selection of the existing directional equipment slots | Original navigation |
| B | Original Circle behavior; not needed for D-pad equipment selection | Original behavior |
| Other buttons and keyboard | Unchanged | Unchanged |

`--controls legacy` restores the original mapping. With the launcher, append
`-- --controls legacy` to the launch command. No save data or in-game control settings are modified.
Empty/restricted slots retain the game's own rejection behavior. Repeated presses retain the native
slot selection/toggle behavior; holding a direction does not repeatedly select it.

The right stick uses XInput's recommended 8689 deadzone on each axis. Outside it, the stick supplies
digital camera directions, matching the original camera speed. Both axes may be active together.
Physical D-pad diagonals retain the native selector's priority. Only newly pressed directions
participate, so adding a direction while another is held does not reselect the held slot.
Camera and weapon selection can be used simultaneously; camera directions never reach the selector,
even while B is held.

## Why this isn't a firmware-level B + D-pad macro

The PSP has no right stick and exposes only one D-pad bitfield. Synthesizing Circle and directions in
`sceCtrl` would also affect menus, crouching and the camera. It would miss short taps because the native
equipment popup opens only after holding Circle.

The lifter instead adds narrowly scoped input hooks for the supported US `ULUS10202` main module.
Instruction signatures gate hook emission; unsupported images can use `--controls legacy` and get an
explicit error if modern controls are requested.

The firmware still writes the unmodified PSP input. Separate host metadata records physical D-pad,
right-stick and keyboard/script direction sources. At the game's final pad conversion
(`0x0884072C`, player-zero pad at `0x08A793DC`), the runtime calculates independent held, pressed and
released direction sets. Firmware polling and peeking do not consume those edges.

Only the gameplay camera direction reader (`0x089142E0`) and player input reader (`0x088B52F8`) see
right-stick directions in place of physical D-pad directions. The equipment update (`0x0889814C`)
consumes a physical D-pad rising edge, calls the game's native restriction check (`0x08894C04`), and
then invokes the same directional selection routine (`0x0889742C`) used by its Circle-held selector.
That invocation alone sees the directional press and the configured equipment modifier from
`0x089F51D0`'s settings pointer, offset `0x64`. The normal equipment update still runs. This avoids
both the popup hold timer and any guessed frame delay, while retaining native inventory validation
and equipment changes. Temporary CPU registers are restored; shared guest pad memory is never swapped.

The game's pad inhibition flags and native restriction check reject paused/dialogue/cutscene input.
Rejected presses are not queued into later gameplay. Focus loss and controller disconnect also
cancel pending selection. Raw menu input, left-stick values, and unrelated button bits are unchanged.
Re-lift generated sources after updating the lifter; the launcher invalidates its lift cache when
the lifter executable changes.

## Diagnostics

`--controls-trace` logs camera/player direction routing and native direct-selection results,
including the requested and selected native slot indices.
`--gamepad-press "time:mask:hold:rx:ry,..."` injects gamepad input for unattended checks, including
headless runs. Times are in game seconds, masks are **PSP** bits (not XInput bits), and right-stick
axes are -128..127 with negative Y pointing up. Example:

```
--gamepad-press "70:0x20:0.5,72:0:1:127:0,74:0x10:1:0:-127" --controls-trace
```

This presses D-pad Right, then camera Right, then D-pad Up and camera Up together. Existing `--press`
and `--auto-press` remain raw PSP/keyboard-style input and are not remapped. Scripted times are based
on vblanks, not wall-clock seconds; use `--dump-frame` alongside logs to confirm the game is playable
rather than paused or in a cutscene. Keep screenshots, logs and generated code outside tracked files.

`mgspo-controls-tests` covers source separation, deadzone boundaries, diagonals, simultaneous B/camera/
D-pad input, held/release edges, keyboard overlap, menu inhibition, focus/disconnect cancellation,
native guard invocation and register preservation. The lifter tests cover signature gating, consumer
ranges, and overlay exclusion. Firmware integration checks exercise all four positive/negative
read/peek variants, multi-sample buffers, script-only input, and overlapping keyboard input.
These tests do not require game assets or a controller.

An isolated headless run of the first playable prison stage (`st04vo`) exercised all four D-pad
directions through the real native selector (slots 0..3) and observed native rejection during
pause/dialogue. D-pad-only selection left the camera unchanged; scripted right-stick input visibly
changed pitch and yaw, including while B or the D-pad was held, without extra selector calls.
An Up + Right diagonal requested slot 0; adding Down while holding that diagonal requested slot 2.
The prison inventory was empty, so this does not verify equipping an owned weapon.
An XInput controller was detected, but physical button/stick operation was not exercised;
the live checks used `--gamepad-press`. Physical playtesting with populated equipment slots
remains necessary.
