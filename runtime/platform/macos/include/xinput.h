// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// XInput over the Game Controller framework (runtime/platform/macos/src/xinput_gamecontroller.mm).
// Extended gamepads (Xbox, DualSense, DualShock, Switch Pro, MFi) occupy user slots in connection
// order and report the XInput layout: A/B/X/Y by position (Xbox naming), sticks as signed 16-bit
// axes, triggers as 0-255. XInputSetState drives the controller's haptics when it has any.

#include <windows.h>

#define XUSER_MAX_COUNT 4u
#define XUSER_INDEX_ANY 0x000000FFu

#define XINPUT_GAMEPAD_DPAD_UP 0x0001u
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002u
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004u
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008u
#define XINPUT_GAMEPAD_START 0x0010u
#define XINPUT_GAMEPAD_BACK 0x0020u
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040u
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080u
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100u
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200u
#define XINPUT_GAMEPAD_A 0x1000u
#define XINPUT_GAMEPAD_B 0x2000u
#define XINPUT_GAMEPAD_X 0x4000u
#define XINPUT_GAMEPAD_Y 0x8000u

#define XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE 7849
#define XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE 8689
#define XINPUT_GAMEPAD_TRIGGER_THRESHOLD 30

struct XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XINPUT_STATE {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};

struct XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};

DWORD XInputGetState(DWORD user_index, XINPUT_STATE* state) noexcept;
DWORD XInputSetState(DWORD user_index, XINPUT_VIBRATION* vibration) noexcept;
