// SPDX-License-Identifier: GPL-3.0-only
//
// XInput over the Game Controller framework.
//
// Extended gamepads occupy user slots 0-3 in connection order and keep their slot until they
// disconnect. GetState reads the current element values (the framework updates them from its own
// thread; the values are plain properties that are safe to read anywhere). Background monitoring
// is enabled so the game keeps receiving input when another window is focused, as XInput does.
// SetState drives the controller's haptics engine with one continuous event whose intensity
// follows the stronger of the two XInput motors.

#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <xinput.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace galaxy::host {
bool controller_hid_owns_switch_pro() noexcept;
bool controller_hid_owns_switch_pro2() noexcept;
}

namespace {

struct Slot {
    GCController* controller = nil;
    DWORD packet = 0;
    XINPUT_GAMEPAD last{};
    CHHapticEngine* haptics = nil;
    id<CHHapticAdvancedPatternPlayer> player = nil;
    float intensity = 0.0f;
};

std::mutex g_mutex;
std::array<Slot, XUSER_MAX_COUNT> g_slots;
bool g_started = false;
bool g_discovery_scheduled = false;

bool names(NSString* text, NSString* needle) {
    return text != nil && [text rangeOfString:needle options:NSCaseInsensitiveSearch].location != NSNotFound;
}

// A Switch Pro Controller or Pro Controller 2 that controller_hid_macos.cpp drives over HID (with
// its gyro and HD rumble) is not also an XInput pad. The framework identifies controllers only by
// name: "Switch Pro Controller" for the Switch Pro; the Pro Controller 2 has no category of its own
// (macOS 27), so any "Switch 2" or "Pro Controller 2" name counts as one.
bool hid_owned(GCController* controller) {
    NSString* category = controller.productCategory;
    NSString* vendor = controller.vendorName;
    if (names(category, @"Switch 2") || names(category, @"Pro Controller 2") || names(vendor, @"Switch 2") ||
        names(vendor, @"Pro Controller 2")) {
        return galaxy::host::controller_hid_owns_switch_pro2();
    }
    return names(category, @"Switch Pro") && galaxy::host::controller_hid_owns_switch_pro();
}

void assign(GCController* controller) {
    if (controller.extendedGamepad == nil) return;
    std::lock_guard lock(g_mutex);
    for (const Slot& slot : g_slots) {
        if (slot.controller == controller) return;
    }
    for (Slot& slot : g_slots) {
        if (slot.controller == nil) {
            slot = Slot{};
            slot.controller = controller;
            return;
        }
    }
}

void release(GCController* controller) {
    std::lock_guard lock(g_mutex);
    for (Slot& slot : g_slots) {
        if (slot.controller == controller) {
            if (slot.player != nil) [slot.player stopAtTime:0 error:nil];
            if (slot.haptics != nil) [slot.haptics stopWithCompletionHandler:nil];
            slot = Slot{};
        }
    }
}

void start_discovery() {
    if (g_started) return;
    g_started = true;
    GCController.shouldMonitorBackgroundEvents = YES;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
                    usingBlock:^(NSNotification* note) { assign(note.object); }];
    [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil
                    usingBlock:^(NSNotification* note) { release(note.object); }];
    for (GCController* controller in GCController.controllers) assign(controller);
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
}

SHORT axis(float value) {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<SHORT>(std::lround(clamped >= 0.0f ? clamped * 32767.0f : clamped * 32768.0f));
}

BYTE trigger(float value) {
    return static_cast<BYTE>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

XINPUT_GAMEPAD read_gamepad(GCExtendedGamepad* pad) {
    XINPUT_GAMEPAD state{};
    WORD buttons = 0;
    const auto set = [&buttons](GCControllerButtonInput* button, WORD mask) {
        if (button != nil && button.isPressed) buttons |= mask;
    };
    set(pad.dpad.up, XINPUT_GAMEPAD_DPAD_UP);
    set(pad.dpad.down, XINPUT_GAMEPAD_DPAD_DOWN);
    set(pad.dpad.left, XINPUT_GAMEPAD_DPAD_LEFT);
    set(pad.dpad.right, XINPUT_GAMEPAD_DPAD_RIGHT);
    set(pad.buttonMenu, XINPUT_GAMEPAD_START);
    set(pad.buttonOptions, XINPUT_GAMEPAD_BACK);
    set(pad.leftThumbstickButton, XINPUT_GAMEPAD_LEFT_THUMB);
    set(pad.rightThumbstickButton, XINPUT_GAMEPAD_RIGHT_THUMB);
    set(pad.leftShoulder, XINPUT_GAMEPAD_LEFT_SHOULDER);
    set(pad.rightShoulder, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    set(pad.buttonA, XINPUT_GAMEPAD_A);
    set(pad.buttonB, XINPUT_GAMEPAD_B);
    set(pad.buttonX, XINPUT_GAMEPAD_X);
    set(pad.buttonY, XINPUT_GAMEPAD_Y);
    state.wButtons = buttons;
    state.bLeftTrigger = trigger(pad.leftTrigger.value);
    state.bRightTrigger = trigger(pad.rightTrigger.value);
    state.sThumbLX = axis(pad.leftThumbstick.xAxis.value);
    state.sThumbLY = axis(pad.leftThumbstick.yAxis.value);
    state.sThumbRX = axis(pad.rightThumbstick.xAxis.value);
    state.sThumbRY = axis(pad.rightThumbstick.yAxis.value);
    return state;
}

void apply_haptics(Slot& slot, float intensity) {
    if (std::fabs(intensity - slot.intensity) < 0.01f) return;
    slot.intensity = intensity;
    if (slot.controller.haptics == nil) return;
    @autoreleasepool {
        if (slot.haptics == nil) {
            slot.haptics = [slot.controller.haptics createEngineWithLocality:GCHapticsLocalityDefault];
            if (slot.haptics == nil) return;
            slot.haptics.playsHapticsOnly = YES;
            slot.haptics.autoShutdownEnabled = YES;
            if (![slot.haptics startAndReturnError:nil]) {
                slot.haptics = nil;
                return;
            }
        }
        if (intensity <= 0.0f) {
            if (slot.player != nil) [slot.player stopAtTime:0 error:nil];
            slot.player = nil;
            return;
        }
        if (slot.player == nil) {
            CHHapticEventParameter* strength =
                [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
            CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                                 parameters:@[ strength ]
                                                               relativeTime:0
                                                                   duration:60.0];
            CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:nil];
            if (pattern == nil) return;
            slot.player = [slot.haptics createAdvancedPlayerWithPattern:pattern error:nil];
            slot.player.loopEnabled = YES;
            [slot.player startAtTime:0 error:nil];
        }
        CHHapticDynamicParameter* level =
            [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                                                            value:intensity
                                                     relativeTime:0];
        [slot.player sendParameters:@[ level ] atTime:0 error:nil];
    }
}

}  // namespace

DWORD XInputGetState(DWORD user_index, XINPUT_STATE* state) noexcept {
    if (state == nullptr || user_index >= XUSER_MAX_COUNT) return ERROR_INVALID_PARAMETER;
    std::lock_guard lock(g_mutex);
    if (!g_discovery_scheduled) {
        // Discovery starts on the main queue at the first poll; notification blocks run on any thread.
        g_discovery_scheduled = true;
        dispatch_async(dispatch_get_main_queue(), ^{ start_discovery(); });
    }
    Slot& slot = g_slots[user_index];
    GCExtendedGamepad* pad = slot.controller != nil ? slot.controller.extendedGamepad : nil;
    if (pad == nil || hid_owned(slot.controller)) {
        *state = {};
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    const XINPUT_GAMEPAD current = read_gamepad(pad);
    if (std::memcmp(&current, &slot.last, sizeof(current)) != 0) {
        slot.last = current;
        ++slot.packet;
    }
    state->dwPacketNumber = slot.packet;
    state->Gamepad = current;
    return ERROR_SUCCESS;
}

DWORD XInputSetState(DWORD user_index, XINPUT_VIBRATION* vibration) noexcept {
    if (vibration == nullptr || user_index >= XUSER_MAX_COUNT) return ERROR_INVALID_PARAMETER;
    std::lock_guard lock(g_mutex);
    Slot& slot = g_slots[user_index];
    if (slot.controller == nil || hid_owned(slot.controller)) return ERROR_DEVICE_NOT_CONNECTED;
    const float intensity =
        static_cast<float>(std::max(vibration->wLeftMotorSpeed, vibration->wRightMotorSpeed)) / 65535.0f;
    apply_haptics(slot, intensity);
    return ERROR_SUCCESS;
}
