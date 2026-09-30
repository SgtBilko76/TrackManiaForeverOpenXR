#pragma once

#include <cstdint>

namespace tmoxr {
// XrAPI controller button indices, in WinlatorXR's order.
enum ControllerButton : uint32_t {
    kLeftGrip = 0,
    kLeftMenu = 1,
    kLeftThumbstickPress = 2,
    kLeftTrigger = 7,
    kButtonX = 8,
    kButtonY = 9,
    kButtonA = 10,
    kButtonB = 11,
    kRightGrip = 12,
    kRightThumbstickPress = 13,
    kRightTrigger = 18,
};

// Latest Quest controller state received from WinlatorXR.
struct ControllerState {
    float leftStick[2]{};   // x right, y up, -1..1
    float rightStick[2]{};
    uint32_t buttons = 0;   // bit n = ControllerButton n
    bool connected = false; // false when no fresh sample arrived recently
    bool race = false;      // true while a race is shown in stereo 3D, false in menus

    bool Pressed(ControllerButton button) const { return (buttons >> button) & 1u; }
};

// Thread-safe; returns a neutral, disconnected state before tracking starts.
ControllerState GetControllerState();

// Installs the virtual "Quest Controllers" DirectInput joypad into the game's
// DirectInput8Create import and mutes DirectInput mouse input.
void InstallVirtualJoypad();
} // namespace tmoxr
