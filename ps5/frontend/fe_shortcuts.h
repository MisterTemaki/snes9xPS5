// Configurable, non-overlapping in-game DualSense shortcuts.
// SPDX-License-Identifier: MIT
#pragma once
#include "ProsperoSce.h"
#include <cstdint>

namespace fe {
namespace shortcuts {
struct Combo { uint32_t buttons; const char* label; };
// All combinations are made from physical buttons (not synthesized stick directions).
// Keeping alternatives as presets prevents accidental single-button SNES input remapping.
constexpr Combo kChoices[] = {
    {SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3, "L3 + R3"},
    {SCE_PAD_BUTTON_L2 | SCE_PAD_BUTTON_R3, "L2 + R3"},
    {SCE_PAD_BUTTON_R2 | SCE_PAD_BUTTON_L3, "R2 + L3"},
    {SCE_PAD_BUTTON_TOUCH_PAD | SCE_PAD_BUTTON_OPTIONS, "Touchpad + Options"},
    {SCE_PAD_BUTTON_L1 | SCE_PAD_BUTTON_R1 | SCE_PAD_BUTTON_L3, "L1 + R1 + L3"},
    {SCE_PAD_BUTTON_L2 | SCE_PAD_BUTTON_TRIANGLE, "L2 + Triangle"},
    {SCE_PAD_BUTTON_R2 | SCE_PAD_BUTTON_TRIANGLE, "R2 + Triangle"},
    {SCE_PAD_BUTTON_OPTIONS | SCE_PAD_BUTTON_TRIANGLE, "Options + Triangle"},
    {SCE_PAD_BUTTON_TOUCH_PAD | SCE_PAD_BUTTON_TRIANGLE, "Touchpad + Triangle"},
    {SCE_PAD_BUTTON_R3 | SCE_PAD_BUTTON_OPTIONS, "R3 + Options"},
};
constexpr int kCount = sizeof(kChoices) / sizeof(kChoices[0]);
inline bool Valid(int index) { return index >= 0 && index < kCount; }
inline bool Held(uint32_t physical, int index) {
    return Valid(index) && (physical & kChoices[index].buttons) == kChoices[index].buttons;
}
inline bool JustPressed(uint32_t physical, uint32_t pressed, int index) {
    return Held(physical, index) && (pressed & kChoices[index].buttons) != 0;
}
inline const char* Label(int index) {
    return Valid(index) ? kChoices[index].label : "(invalid)";
}
} // namespace shortcuts
} // namespace fe
