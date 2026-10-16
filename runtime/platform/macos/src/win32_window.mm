// SPDX-License-Identifier: GPL-3.0-only
//
// Win32 windowing on Cocoa.
//
// Windows belong to the thread that creates them and receive their messages through that
// thread's queue (GetMessage/PeekMessage/DispatchMessage), as on Windows. Each HWND is backed by
// an NSWindow whose content view is layer-backed with a CAMetalLayer (the DXGI swap chain draws
// into it). AppKit runs on the main thread: window operations hop there, and the main thread
// translates NSEvents into WM_* messages that it posts to the owning thread. It never waits on a
// window thread, so a window thread may wait on it.
//
// Coordinates follow a per-monitor DPI-aware Win32 process: client coordinates are backing
// pixels of the content view with a top-left origin; screen coordinates are backing pixels of the
// global desktop, origin at the top-left of the primary display (points scaled by the primary
// display's backing factor).
//
// Keyboard messages carry Windows virtual keys, set-1 scan codes and the extended-key bit, so the
// renderer's key handling (left/right modifiers through MapVirtualKey) works unchanged.

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <QuartzCore/CAMetalLayer.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include <windows.h>
#undef interface

#include "win32_internal.h"
#include "galaxy/gx/window_messages.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using galaxy::compat::utf8_from_wide;
using galaxy::compat::wide_from_utf8;

@class GalaxyWindowView;
@class GalaxyWindowDelegate;

namespace {

// ---------------------------------------------------------------------------------------------
// Thread message queues
// ---------------------------------------------------------------------------------------------

struct SentMessage {
    HWND window = nullptr;
    UINT message = 0;
    WPARAM wparam = 0;
    LPARAM lparam = 0;
    LRESULT result = 0;
    bool done = false;
};

struct ThreadQueue {
    DWORD thread_id = 0;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<MSG> posted;
    std::deque<std::shared_ptr<SentMessage>> sent;
    bool quit = false;
    int quit_code = 0;
};

std::mutex g_queues_mutex;
std::map<DWORD, std::shared_ptr<ThreadQueue>> g_queues;
thread_local std::shared_ptr<ThreadQueue> t_queue;
thread_local LONG t_message_time = 0;

std::shared_ptr<ThreadQueue> current_queue() {
    if (!t_queue) {
        t_queue = std::make_shared<ThreadQueue>();
        t_queue->thread_id = GetCurrentThreadId();
        std::lock_guard lock(g_queues_mutex);
        g_queues[t_queue->thread_id] = t_queue;
    }
    return t_queue;
}

DWORD message_time_now() noexcept { return static_cast<DWORD>(GetTickCount64()); }

void post_to_queue(ThreadQueue& queue, HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    MSG entry{};
    entry.hwnd = window;
    entry.message = message;
    entry.wParam = wparam;
    entry.lParam = lparam;
    entry.time = message_time_now();
    GetCursorPos(&entry.pt);
    {
        std::lock_guard lock(queue.mutex);
        queue.posted.push_back(entry);
    }
    queue.wake.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Window records
// ---------------------------------------------------------------------------------------------

struct RegisteredClass {
    WNDPROC proc = nullptr;
    UINT style = 0;
    HCURSOR cursor = nullptr;
};

}  // namespace

struct HWND__ {
    static constexpr std::uint64_t kMagic = 0x4E4542574E444F57ull;  // "NEBWNDOW"
    std::uint64_t magic = kMagic;
    WNDPROC proc = nullptr;
    std::wstring class_name;
    std::wstring title;
    UINT class_style = 0;
    HCURSOR class_cursor = nullptr;
    DWORD style = 0;
    DWORD ex_style = 0;
    LONG_PTR user_data = 0;
    std::shared_ptr<ThreadQueue> queue;
    NSWindow* ns_window = nil;
    GalaxyWindowView* view = nil;
    GalaxyWindowDelegate* delegate = nil;
    CAMetalLayer* layer = nil;
    // Geometry and state, guarded by g_mutex; written on the main thread.
    int client_width = 0;
    int client_height = 0;
    int client_x = 0;
    int client_y = 0;
    RECT frame{};
    double scale = 1.0;
    bool visible = false;
    bool minimized = false;
    bool key = false;
    bool destroyed = false;
    bool track_leave = false;
    bool live_resize = false;
    // Native full screen (main thread). A borderless window covering a screen is Win32's
    // borderless fullscreen; on macOS the window goes full screen in its own Space instead.
    bool fullscreen_target = false;      // the runtime (or the user) wants full screen
    bool fullscreen_active = false;      // in full screen
    bool fullscreen_transition = false;  // entering or leaving
    bool fullscreen_by_runtime = false;  // the transition under way was requested by the runtime
    NSRect windowed_frame = NSZeroRect;  // the frame to return to when leaving full screen
    bool restore_pending = false;        // apply windowed_frame once full screen has been left
};

namespace {

std::recursive_mutex g_mutex;
std::map<std::wstring, RegisteredClass> g_classes;
std::vector<HWND> g_windows;
HWND g_capture = nullptr;
std::atomic<bool> g_cursor_hidden_wanted{false};
bool g_cursor_hidden = false;  // main thread
std::array<std::atomic<bool>, 256> g_keys{};
std::atomic<int> g_cursor_x{0};
std::atomic<int> g_cursor_y{0};

struct MonitorRecord {
    RECT bounds{};
    RECT work{};
    double scale = 1.0;
    unsigned refresh_hz = 60;
    CGDirectDisplayID display = 0;
    bool primary = false;
};
std::vector<MonitorRecord> g_monitors;  // guarded by g_mutex, refreshed on the main thread
double g_primary_height_points = 0.0;
double g_primary_scale = 1.0;

bool valid(HWND window) {
    if (window == nullptr) return false;
    std::lock_guard lock(g_mutex);
    for (HWND candidate : g_windows) {
        if (candidate == window) return !window->destroyed;
    }
    return false;
}

void on_main(void (^block)(void)) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

void post(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    std::shared_ptr<ThreadQueue> queue;
    {
        std::lock_guard lock(g_mutex);
        if (window == nullptr || window->destroyed) return;
        queue = window->queue;
    }
    if (queue) post_to_queue(*queue, window, message, wparam, lparam);
}

// --- Coordinates (main thread) ---------------------------------------------------------------

NSPoint points_from_pixels(LONG x, LONG y) {
    return NSMakePoint(static_cast<double>(x) / g_primary_scale,
        g_primary_height_points - static_cast<double>(y) / g_primary_scale);
}

POINT pixels_from_points(NSPoint point) {
    return POINT{static_cast<LONG>(std::lround(point.x * g_primary_scale)),
        static_cast<LONG>(std::lround((g_primary_height_points - point.y) * g_primary_scale))};
}

RECT pixels_from_rect(NSRect rect) {
    const POINT top_left = pixels_from_points(NSMakePoint(NSMinX(rect), NSMaxY(rect)));
    const POINT bottom_right = pixels_from_points(NSMakePoint(NSMaxX(rect), NSMinY(rect)));
    return RECT{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
}

NSRect rect_from_pixels(const RECT& rect) {
    const NSPoint top_left = points_from_pixels(rect.left, rect.top);
    const NSPoint bottom_right = points_from_pixels(rect.right, rect.bottom);
    return NSMakeRect(top_left.x, bottom_right.y, bottom_right.x - top_left.x, top_left.y - bottom_right.y);
}

void refresh_monitors() {
    NSArray<NSScreen*>* screens = NSScreen.screens;
    if (screens.count == 0) return;
    std::lock_guard lock(g_mutex);
    g_primary_height_points = NSMaxY(screens[0].frame);
    g_primary_scale = screens[0].backingScaleFactor;
    g_monitors.clear();
    for (NSUInteger i = 0; i < screens.count; ++i) {
        NSScreen* screen = screens[i];
        MonitorRecord record{};
        record.bounds = pixels_from_rect(screen.frame);
        record.work = pixels_from_rect(screen.visibleFrame);
        record.scale = screen.backingScaleFactor;
        record.primary = i == 0;
        record.display = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        const NSInteger fps = screen.maximumFramesPerSecond;
        record.refresh_hz = fps > 0 ? static_cast<unsigned>(fps) : 60u;
        g_monitors.push_back(record);
    }
}

// The renderer asks for monitors before the main thread's run loop has listed them (and its
// first window is placed from the answer), so the first question lists them.
void ensure_monitors() {
    {
        std::lock_guard lock(g_mutex);
        if (!g_monitors.empty()) return;
    }
    on_main(^{ refresh_monitors(); });
}

// --- Styles -------------------------------------------------------------------------------------

NSWindowStyleMask mask_for_style(DWORD style) {
    if ((style & WS_POPUP) != 0u || (style & WS_CAPTION) != WS_CAPTION) {
        return NSWindowStyleMaskBorderless;
    }
    NSWindowStyleMask mask = NSWindowStyleMaskTitled;
    if ((style & WS_SYSMENU) != 0u) mask |= NSWindowStyleMaskClosable;
    if ((style & WS_MINIMIZEBOX) != 0u) mask |= NSWindowStyleMaskMiniaturizable;
    if ((style & WS_THICKFRAME) != 0u) mask |= NSWindowStyleMaskResizable;
    return mask;
}

// Title bar height in points for a titled window (no side or bottom borders on macOS).
double title_bar_points() {
    static const double height = [] {
        __block double result = 28.0;
        on_main(^{
            const NSRect content = NSMakeRect(0, 0, 640, 480);
            const NSRect frame = [NSWindow frameRectForContentRect:content styleMask:NSWindowStyleMaskTitled];
            result = NSHeight(frame) - NSHeight(content);
        });
        return result;
    }();
    return height;
}

// --- Geometry publication (main thread) -------------------------------------------------------

void update_geometry(HWND window, bool send_size, bool send_move) {
    NSWindow* ns_window = window->ns_window;
    if (ns_window == nil) return;
    const NSRect frame = ns_window.frame;
    const NSRect content = [ns_window contentRectForFrameRect:frame];
    const double scale = ns_window.backingScaleFactor;
    int width = 0;
    int height = 0;
    {
        std::lock_guard lock(g_mutex);
        window->scale = scale;
        window->client_width = static_cast<int>(std::lround(NSWidth(content) * scale));
        window->client_height = static_cast<int>(std::lround(NSHeight(content) * scale));
        const POINT origin = pixels_from_points(NSMakePoint(NSMinX(content), NSMaxY(content)));
        window->client_x = origin.x;
        window->client_y = origin.y;
        window->frame = pixels_from_rect(frame);
        window->visible = ns_window.isVisible;
        window->minimized = ns_window.isMiniaturized;
        window->key = ns_window.isKeyWindow && NSApp.isActive;
        width = window->client_width;
        height = window->client_height;
    }
    if (window->layer != nil) {
        window->layer.contentsScale = scale;
    }
    if (send_size) {
        const WPARAM kind = ns_window.isMiniaturized ? SIZE_MINIMIZED : (ns_window.isZoomed ? SIZE_MAXIMIZED : SIZE_RESTORED);
        post(window, WM_SIZE, kind, MAKELPARAM(width, height));
    }
    if (send_move) {
        std::lock_guard lock(g_mutex);
        post(window, WM_MOVE, 0, MAKELPARAM(window->client_x, window->client_y));
    }
}

// --- Native full screen (main thread) ---------------------------------------------------------

// GALAXY_TRACE_WINDOW=1 traces window placement and full-screen changes.
bool trace_window() {
    static const bool enabled = [] {
        const char* value = std::getenv("GALAXY_TRACE_WINDOW");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return enabled;
}

void trace_frame(const char* what, NSRect frame) {
    if (trace_window()) {
        std::fprintf(stderr, "[window] %s %.0f,%.0f %.0fx%.0f pt\n", what, NSMinX(frame), NSMinY(frame),
                     NSWidth(frame), NSHeight(frame));
    }
}

// The screen a frame covers exactly, if any.
NSScreen* screen_covered_by(NSRect frame) {
    for (NSScreen* screen in NSScreen.screens) {
        const NSRect bounds = screen.frame;
        if (std::fabs(NSMinX(frame) - NSMinX(bounds)) < 1.0 && std::fabs(NSMinY(frame) - NSMinY(bounds)) < 1.0 &&
            std::fabs(NSWidth(frame) - NSWidth(bounds)) < 1.0 && std::fabs(NSHeight(frame) - NSHeight(bounds)) < 1.0) {
            return screen;
        }
    }
    return nil;
}

// Game windows (not tool windows such as the hidden test window) can go full screen.
bool full_screen_capable(HWND window) { return (window->ex_style & WS_EX_TOOLWINDOW) == 0u; }

// The titled style a full-screen window keeps (its title bar shows at the top of the screen).
NSWindowStyleMask windowed_mask(DWORD style) {
    const NSWindowStyleMask mask = mask_for_style(style);
    return mask != NSWindowStyleMaskBorderless ? mask : mask_for_style(WS_OVERLAPPEDWINDOW);
}

// A window started in full screen returns to three quarters of the screen, 16:9, centered.
NSRect default_windowed_frame(NSScreen* screen) {
    const NSRect visible = (screen != nil ? screen : NSScreen.mainScreen).visibleFrame;
    double width = std::round(NSWidth(visible) * 0.75);
    double height = std::round(width * 9.0 / 16.0);
    if (height > NSHeight(visible) * 0.85) {
        height = std::round(NSHeight(visible) * 0.85);
        width = std::round(height * 16.0 / 9.0);
    }
    const NSRect content = NSMakeRect(NSMidX(visible) - width / 2.0, NSMidY(visible) - height / 2.0, width, height);
    return [NSWindow frameRectForContentRect:content styleMask:mask_for_style(WS_OVERLAPPEDWINDOW)];
}

// Test hook: GALAXY_TEST_FULLSCREEN_TOGGLES=<ms>,<ms>,... toggles full screen at those times after
// the window is first shown, as the title bar button would (so the settings follow).
void schedule_test_full_screen_toggles(HWND window) {
    static bool scheduled = false;
    const char* value = std::getenv("GALAXY_TEST_FULLSCREEN_TOGGLES");
    if (scheduled || value == nullptr || *value == '\0') return;
    scheduled = true;
    for (const char* cursor = value; *cursor != '\0';) {
        char* end = nullptr;
        const long ms = std::strtol(cursor, &end, 10);
        if (end == cursor) break;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(ms) * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            if (!valid(window) || window->fullscreen_transition) return;
            [window->ns_window toggleFullScreen:nil];
        });
        cursor = *end == ',' ? end + 1 : end;
    }
}

void enter_full_screen(HWND window, NSScreen* screen) {
    NSWindow* ns_window = window->ns_window;
    window->fullscreen_target = true;
    if (window->fullscreen_active || window->fullscreen_transition || !ns_window.isVisible) return;
    // Full screen starts from a titled window (its title bar, with the button that leaves full
    // screen, shows at the top of the screen) of a size worth returning to.
    if (ns_window.styleMask == NSWindowStyleMaskBorderless || (screen != nil && ns_window.screen != screen)) {
        DWORD style = 0;
        {
            std::lock_guard lock(g_mutex);
            style = window->style;
        }
        ns_window.styleMask = windowed_mask(style);
        [ns_window setFrame:default_windowed_frame(screen != nil ? screen : ns_window.screen) display:NO];
        [ns_window makeFirstResponder:ns_window.contentView];
    }
    window->fullscreen_by_runtime = true;
    [ns_window toggleFullScreen:nil];
}

void leave_full_screen(HWND window, NSRect frame) {
    NSWindow* ns_window = window->ns_window;
    window->fullscreen_target = false;
    window->windowed_frame = frame;
    window->restore_pending = true;
    if (window->fullscreen_transition) return;  // the end of the transition turns back
    if (window->fullscreen_active) {
        window->fullscreen_by_runtime = true;
        [ns_window toggleFullScreen:nil];
        return;
    }
    window->restore_pending = false;
    [ns_window setFrame:frame display:YES];
}

void set_cursor_hidden(bool hidden) {
    if (hidden == g_cursor_hidden) return;
    if (hidden) [NSCursor hide]; else [NSCursor unhide];
    g_cursor_hidden = hidden;
}

// --- Keyboard translation -------------------------------------------------------------------

struct KeyInfo {
    std::uint8_t vk = 0;
    std::uint8_t scan = 0;
    bool extended = false;
};

KeyInfo key_info(unsigned short key_code) {
    switch (key_code) {
    case kVK_ANSI_A: return {'A', 0x1E}; case kVK_ANSI_B: return {'B', 0x30}; case kVK_ANSI_C: return {'C', 0x2E};
    case kVK_ANSI_D: return {'D', 0x20}; case kVK_ANSI_E: return {'E', 0x12}; case kVK_ANSI_F: return {'F', 0x21};
    case kVK_ANSI_G: return {'G', 0x22}; case kVK_ANSI_H: return {'H', 0x23}; case kVK_ANSI_I: return {'I', 0x17};
    case kVK_ANSI_J: return {'J', 0x24}; case kVK_ANSI_K: return {'K', 0x25}; case kVK_ANSI_L: return {'L', 0x26};
    case kVK_ANSI_M: return {'M', 0x32}; case kVK_ANSI_N: return {'N', 0x31}; case kVK_ANSI_O: return {'O', 0x18};
    case kVK_ANSI_P: return {'P', 0x19}; case kVK_ANSI_Q: return {'Q', 0x10}; case kVK_ANSI_R: return {'R', 0x13};
    case kVK_ANSI_S: return {'S', 0x1F}; case kVK_ANSI_T: return {'T', 0x14}; case kVK_ANSI_U: return {'U', 0x16};
    case kVK_ANSI_V: return {'V', 0x2F}; case kVK_ANSI_W: return {'W', 0x11}; case kVK_ANSI_X: return {'X', 0x2D};
    case kVK_ANSI_Y: return {'Y', 0x15}; case kVK_ANSI_Z: return {'Z', 0x2C};
    case kVK_ANSI_0: return {'0', 0x0B}; case kVK_ANSI_1: return {'1', 0x02}; case kVK_ANSI_2: return {'2', 0x03};
    case kVK_ANSI_3: return {'3', 0x04}; case kVK_ANSI_4: return {'4', 0x05}; case kVK_ANSI_5: return {'5', 0x06};
    case kVK_ANSI_6: return {'6', 0x07}; case kVK_ANSI_7: return {'7', 0x08}; case kVK_ANSI_8: return {'8', 0x09};
    case kVK_ANSI_9: return {'9', 0x0A};
    case kVK_ANSI_Equal: return {VK_OEM_PLUS, 0x0D}; case kVK_ANSI_Minus: return {VK_OEM_MINUS, 0x0C};
    case kVK_ANSI_RightBracket: return {VK_OEM_6, 0x1B}; case kVK_ANSI_LeftBracket: return {VK_OEM_4, 0x1A};
    case kVK_ANSI_Quote: return {VK_OEM_7, 0x28}; case kVK_ANSI_Semicolon: return {VK_OEM_1, 0x27};
    case kVK_ANSI_Backslash: return {VK_OEM_5, 0x2B}; case kVK_ANSI_Comma: return {VK_OEM_COMMA, 0x33};
    case kVK_ANSI_Slash: return {VK_OEM_2, 0x35}; case kVK_ANSI_Period: return {VK_OEM_PERIOD, 0x34};
    case kVK_ANSI_Grave: return {VK_OEM_3, 0x29}; case kVK_ISO_Section: return {VK_OEM_102, 0x56};
    case kVK_Return: return {VK_RETURN, 0x1C}; case kVK_Tab: return {VK_TAB, 0x0F};
    case kVK_Space: return {VK_SPACE, 0x39}; case kVK_Delete: return {VK_BACK, 0x0E};
    case kVK_Escape: return {VK_ESCAPE, 0x01}; case kVK_CapsLock: return {VK_CAPITAL, 0x3A};
    case kVK_Command: return {VK_LWIN, 0x5B, true}; case kVK_RightCommand: return {VK_RWIN, 0x5C, true};
    case kVK_Shift: return {VK_SHIFT, 0x2A}; case kVK_RightShift: return {VK_SHIFT, 0x36};
    case kVK_Option: return {VK_MENU, 0x38}; case kVK_RightOption: return {VK_MENU, 0x38, true};
    case kVK_Control: return {VK_CONTROL, 0x1D}; case kVK_RightControl: return {VK_CONTROL, 0x1D, true};
    case kVK_F1: return {VK_F1, 0x3B}; case kVK_F2: return {VK_F2, 0x3C}; case kVK_F3: return {VK_F3, 0x3D};
    case kVK_F4: return {VK_F4, 0x3E}; case kVK_F5: return {VK_F5, 0x3F}; case kVK_F6: return {VK_F6, 0x40};
    case kVK_F7: return {VK_F7, 0x41}; case kVK_F8: return {VK_F8, 0x42}; case kVK_F9: return {VK_F9, 0x43};
    case kVK_F10: return {VK_F10, 0x44}; case kVK_F11: return {VK_F11, 0x57}; case kVK_F12: return {VK_F12, 0x58};
    case kVK_F13: return {VK_F13, 0x64}; case kVK_F14: return {VK_F14, 0x65}; case kVK_F15: return {VK_F15, 0x66};
    case kVK_F16: return {VK_F16, 0x67}; case kVK_F17: return {VK_F17, 0x68}; case kVK_F18: return {VK_F18, 0x69};
    case kVK_F19: return {VK_F19, 0x6A}; case kVK_F20: return {VK_F20, 0x6B};
    case kVK_Help: return {VK_INSERT, 0x52, true}; case kVK_Home: return {VK_HOME, 0x47, true};
    case kVK_PageUp: return {VK_PRIOR, 0x49, true}; case kVK_ForwardDelete: return {VK_DELETE, 0x53, true};
    case kVK_End: return {VK_END, 0x4F, true}; case kVK_PageDown: return {VK_NEXT, 0x51, true};
    case kVK_LeftArrow: return {VK_LEFT, 0x4B, true}; case kVK_RightArrow: return {VK_RIGHT, 0x4D, true};
    case kVK_DownArrow: return {VK_DOWN, 0x50, true}; case kVK_UpArrow: return {VK_UP, 0x48, true};
    case kVK_ANSI_Keypad0: return {VK_NUMPAD0, 0x52}; case kVK_ANSI_Keypad1: return {VK_NUMPAD1, 0x4F};
    case kVK_ANSI_Keypad2: return {VK_NUMPAD2, 0x50}; case kVK_ANSI_Keypad3: return {VK_NUMPAD3, 0x51};
    case kVK_ANSI_Keypad4: return {VK_NUMPAD4, 0x4B}; case kVK_ANSI_Keypad5: return {VK_NUMPAD5, 0x4C};
    case kVK_ANSI_Keypad6: return {VK_NUMPAD6, 0x4D}; case kVK_ANSI_Keypad7: return {VK_NUMPAD7, 0x47};
    case kVK_ANSI_Keypad8: return {VK_NUMPAD8, 0x48}; case kVK_ANSI_Keypad9: return {VK_NUMPAD9, 0x49};
    case kVK_ANSI_KeypadDecimal: return {VK_DECIMAL, 0x53}; case kVK_ANSI_KeypadMultiply: return {VK_MULTIPLY, 0x37};
    case kVK_ANSI_KeypadPlus: return {VK_ADD, 0x4E}; case kVK_ANSI_KeypadMinus: return {VK_SUBTRACT, 0x4A};
    case kVK_ANSI_KeypadDivide: return {VK_DIVIDE, 0x35, true}; case kVK_ANSI_KeypadEnter: return {VK_RETURN, 0x1C, true};
    case kVK_ANSI_KeypadClear: return {VK_NUMLOCK, 0x45}; case kVK_ANSI_KeypadEquals: return {VK_OEM_PLUS, 0x0D};
    default: return {};
    }
}

// The left/right-specific virtual key, for GetAsyncKeyState.
std::uint8_t specific_vk(const KeyInfo& key) {
    switch (key.vk) {
    case VK_SHIFT: return key.scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    case VK_CONTROL: return key.extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU: return key.extended ? VK_RMENU : VK_LMENU;
    default: return key.vk;
    }
}

bool option_held(NSEventModifierFlags flags) { return (flags & NSEventModifierFlagOption) != 0; }

void post_key(HWND window, const KeyInfo& key, bool down, bool repeat, NSEventModifierFlags flags) {
    if (key.vk == 0) return;
    g_keys[specific_vk(key)].store(down, std::memory_order_relaxed);
    if (key.vk == VK_SHIFT || key.vk == VK_CONTROL || key.vk == VK_MENU) g_keys[key.vk].store(down, std::memory_order_relaxed);
    std::uint32_t lparam = 1u | (static_cast<std::uint32_t>(key.scan) << 16u);
    if (key.extended) lparam |= 1u << 24u;
    if (down && repeat) lparam |= 1u << 30u;
    if (!down) lparam |= (1u << 30u) | (1u << 31u);
    const bool alt = option_held(flags) || key.vk == VK_MENU;
    const UINT message = down ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP);
    post(window, message, key.vk, static_cast<LPARAM>(lparam));
}

WPARAM mouse_key_state(NSEvent* event) {
    WPARAM state = 0;
    const NSUInteger buttons = NSEvent.pressedMouseButtons;
    if ((buttons & 1u) != 0u) state |= MK_LBUTTON;
    if ((buttons & 2u) != 0u) state |= MK_RBUTTON;
    if ((buttons & 4u) != 0u) state |= MK_MBUTTON;
    if ((buttons & 8u) != 0u) state |= MK_XBUTTON1;
    if ((buttons & 16u) != 0u) state |= MK_XBUTTON2;
    const NSEventModifierFlags flags = event.modifierFlags;
    if ((flags & NSEventModifierFlagShift) != 0) state |= MK_SHIFT;
    if ((flags & NSEventModifierFlagControl) != 0) state |= MK_CONTROL;
    return state;
}

HWND g_last_mouse_window = nullptr;

}  // namespace

// ---------------------------------------------------------------------------------------------
// Cocoa classes
// ---------------------------------------------------------------------------------------------

@interface GalaxyWindowView : NSView
@property(nonatomic, assign) HWND window_handle;
@end

@implementation GalaxyWindowView {
    NSTrackingArea* _tracking;
}

- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.opaque = YES;
    layer.framebufferOnly = NO;
    return layer;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (BOOL)isOpaque { return YES; }

- (void)updateTrackingAreas {
    if (_tracking != nil) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

- (LPARAM)clientPoint:(NSEvent*)event {
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const double scale = self.window.backingScaleFactor;
    const int x = static_cast<int>(std::lround(point.x * scale));
    const int y = static_cast<int>(std::lround((NSHeight(self.bounds) - point.y) * scale));
    return MAKELPARAM(x, y);
}

- (void)recordCursor:(NSEvent*)event {
    const POINT screen = pixels_from_points(NSEvent.mouseLocation);
    g_cursor_x.store(screen.x, std::memory_order_relaxed);
    g_cursor_y.store(screen.y, std::memory_order_relaxed);
}

- (void)moved:(NSEvent*)event {
    [self recordCursor:event];
    HWND window = self.window_handle;
    g_last_mouse_window = window;
    if (g_cursor_hidden_wanted.load(std::memory_order_relaxed) && self.window.isKeyWindow) set_cursor_hidden(true);
    post(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    post(window, WM_MOUSEMOVE, mouse_key_state(event), [self clientPoint:event]);
}

- (void)mouseMoved:(NSEvent*)event { [self moved:event]; }
- (void)mouseDragged:(NSEvent*)event { [self moved:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self moved:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self moved:event]; }
- (void)mouseEntered:(NSEvent*)event { [self moved:event]; }

- (void)mouseExited:(NSEvent*)event {
    [self recordCursor:event];
    set_cursor_hidden(false);
    HWND window = self.window_handle;
    bool tracking = false;
    {
        std::lock_guard lock(g_mutex);
        tracking = window != nullptr && window->track_leave;
        if (tracking) window->track_leave = false;
    }
    if (tracking) post(window, WM_MOUSELEAVE, 0, 0);
}

- (void)button:(NSEvent*)event down:(BOOL)down {
    [self recordCursor:event];
    HWND window = self.window_handle;
    const LPARAM point = [self clientPoint:event];
    const WPARAM state = mouse_key_state(event);
    switch (event.buttonNumber) {
    case 0: post(window, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, state, point); break;
    case 1: post(window, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, state, point); break;
    case 2: post(window, down ? WM_MBUTTONDOWN : WM_MBUTTONUP, state, point); break;
    case 3: post(window, down ? WM_XBUTTONDOWN : WM_XBUTTONUP, MAKEWPARAM(state, XBUTTON1), point); break;
    case 4: post(window, down ? WM_XBUTTONDOWN : WM_XBUTTONUP, MAKEWPARAM(state, XBUTTON2), point); break;
    default: break;
    }
}

- (void)mouseDown:(NSEvent*)event { [self button:event down:YES]; }
- (void)mouseUp:(NSEvent*)event { [self button:event down:NO]; }
- (void)rightMouseDown:(NSEvent*)event { [self button:event down:YES]; }
- (void)rightMouseUp:(NSEvent*)event { [self button:event down:NO]; }
- (void)otherMouseDown:(NSEvent*)event { [self button:event down:YES]; }
- (void)otherMouseUp:(NSEvent*)event { [self button:event down:NO]; }

- (void)scrollWheel:(NSEvent*)event {
    const double delta = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY / 10.0 : event.scrollingDeltaY;
    const auto wheel = static_cast<short>(std::clamp(std::lround(delta * WHEEL_DELTA), -32768l, 32767l));
    if (wheel == 0) return;
    const POINT screen = pixels_from_points(NSEvent.mouseLocation);
    post(self.window_handle, WM_MOUSEWHEEL, MAKEWPARAM(mouse_key_state(event), static_cast<WORD>(wheel)),
        MAKELPARAM(screen.x, screen.y));
}

- (void)keyDown:(NSEvent*)event {
    post_key(self.window_handle, key_info(event.keyCode), true, event.isARepeat, event.modifierFlags);
}

- (void)keyUp:(NSEvent*)event {
    post_key(self.window_handle, key_info(event.keyCode), false, false, event.modifierFlags);
}

- (void)flagsChanged:(NSEvent*)event {
    const KeyInfo key = key_info(event.keyCode);
    if (key.vk == 0) return;
    // The device-dependent flag of the key that changed tells press from release.
    std::uint64_t bit = 0;
    switch (event.keyCode) {
    case kVK_Shift: bit = NX_DEVICELSHIFTKEYMASK; break;
    case kVK_RightShift: bit = NX_DEVICERSHIFTKEYMASK; break;
    case kVK_Control: bit = NX_DEVICELCTLKEYMASK; break;
    case kVK_RightControl: bit = NX_DEVICERCTLKEYMASK; break;
    case kVK_Option: bit = NX_DEVICELALTKEYMASK; break;
    case kVK_RightOption: bit = NX_DEVICERALTKEYMASK; break;
    case kVK_Command: bit = NX_DEVICELCMDKEYMASK; break;
    case kVK_RightCommand: bit = NX_DEVICERCMDKEYMASK; break;
    case kVK_CapsLock: bit = NSEventModifierFlagCapsLock; break;
    default: return;
    }
    const bool down = (event.modifierFlags & bit) != 0;
    post_key(self.window_handle, key, down, false, event.modifierFlags);
}

// Command shortcuts stay with the application menu (Quit, Hide); nothing else reaches the game.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) return [super performKeyEquivalent:event];
    return NO;
}
@end

@interface GalaxyWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) HWND window_handle;
@end

@implementation GalaxyWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    post(self.window_handle, WM_CLOSE, 0, 0);
    return NO;
}
- (void)windowDidResize:(NSNotification*)notification { update_geometry(self.window_handle, true, false); }
- (void)windowDidMove:(NSNotification*)notification { update_geometry(self.window_handle, false, true); }
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    refresh_monitors();
    update_geometry(self.window_handle, true, true);
}
- (void)windowDidChangeScreen:(NSNotification*)notification { update_geometry(self.window_handle, true, true); }
- (void)windowDidMiniaturize:(NSNotification*)notification { update_geometry(self.window_handle, true, false); }
- (void)windowDidDeminiaturize:(NSNotification*)notification { update_geometry(self.window_handle, true, false); }
- (void)windowWillStartLiveResize:(NSNotification*)notification {
    // Full-screen transitions resize the window too; only the user's own resizing counts.
    if (self.window_handle->fullscreen_transition || self.window_handle->fullscreen_active) return;
    {
        std::lock_guard lock(g_mutex);
        self.window_handle->live_resize = true;
    }
    post(self.window_handle, WM_ENTERSIZEMOVE, 0, 0);
}
- (void)windowDidEndLiveResize:(NSNotification*)notification {
    {
        std::lock_guard lock(g_mutex);
        if (!self.window_handle->live_resize) return;
        self.window_handle->live_resize = false;
    }
    update_geometry(self.window_handle, true, false);
    post(self.window_handle, WM_EXITSIZEMOVE, 0, 0);
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
    update_geometry(self.window_handle, false, false);
    post(self.window_handle, WM_ACTIVATEAPP, TRUE, 0);
    post(self.window_handle, WM_SETFOCUS, 0, 0);
}
- (void)windowDidResignKey:(NSNotification*)notification {
    update_geometry(self.window_handle, false, false);
    set_cursor_hidden(false);
    for (auto& key : g_keys) key.store(false, std::memory_order_relaxed);
    post(self.window_handle, WM_KILLFOCUS, 0, 0);
    post(self.window_handle, WM_ACTIVATEAPP, FALSE, 0);
}
// Full screen started by the runtime follows its display mode; full screen entered or left by the
// user (title bar button, Window menu, Ctrl+Cmd+F) becomes the runtime's display mode.
- (void)windowWillEnterFullScreen:(NSNotification*)notification {
    HWND window = self.window_handle;
    trace_frame("will enter full screen from", window->ns_window.frame);
    window->fullscreen_transition = true;
    if (!window->restore_pending) window->windowed_frame = window->ns_window.frame;
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification {
    HWND window = self.window_handle;
    const bool by_user = !window->fullscreen_by_runtime;
    window->fullscreen_transition = false;
    window->fullscreen_active = true;
    window->fullscreen_by_runtime = false;
    update_geometry(window, true, true);
    std::fprintf(stderr, "[window] full screen entered (%s) client=%dx%d windowed=%.0fx%.0f pt\n",
                 by_user ? "user" : "runtime", window->client_width, window->client_height,
                 NSWidth(window->windowed_frame), NSHeight(window->windowed_frame));
    if (by_user) {
        window->fullscreen_target = true;
        {
            std::lock_guard lock(g_mutex);
            window->style = (window->style & WS_VISIBLE) | WS_POPUP;
        }
        post(window, galaxy::gx::kSystemDisplayModeMessage, 1, 0);
    } else if (!window->fullscreen_target) {
        window->fullscreen_by_runtime = true;
        [window->ns_window toggleFullScreen:nil];
    }
}
- (void)windowWillExitFullScreen:(NSNotification*)notification {
    self.window_handle->fullscreen_transition = true;
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
    HWND window = self.window_handle;
    NSWindow* ns_window = window->ns_window;
    const bool by_user = !window->fullscreen_by_runtime;
    window->fullscreen_transition = false;
    window->fullscreen_active = false;
    window->fullscreen_by_runtime = false;
    DWORD style = 0;
    {
        std::lock_guard lock(g_mutex);
        if (by_user) window->style = (window->style & WS_VISIBLE) | WS_OVERLAPPEDWINDOW;
        style = window->style;
    }
    ns_window.styleMask = windowed_mask(style);
    [ns_window makeFirstResponder:window->view];
    if (window->restore_pending) {
        window->restore_pending = false;
        [ns_window setFrame:window->windowed_frame display:YES];
    }
    update_geometry(window, true, true);
    std::fprintf(stderr, "[window] full screen left (%s) client=%dx%d\n", by_user ? "user" : "runtime",
                 window->client_width, window->client_height);
    if (by_user) {
        window->fullscreen_target = false;
        post(window, galaxy::gx::kSystemDisplayModeMessage, 0, 0);
    } else if (window->fullscreen_target) {
        window->fullscreen_by_runtime = true;
        [ns_window toggleFullScreen:nil];
    }
}
- (void)windowDidFailToEnterFullScreen:(NSWindow*)window {
    self.window_handle->fullscreen_transition = false;
    self.window_handle->fullscreen_by_runtime = false;
}
- (void)windowDidFailToExitFullScreen:(NSWindow*)window {
    self.window_handle->fullscreen_transition = false;
    self.window_handle->fullscreen_by_runtime = false;
}
@end

@interface GalaxyApplicationDelegate : NSObject <NSApplicationDelegate>
@end

@implementation GalaxyApplicationDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    // Quitting closes the game window; the runtime shuts down and exits the process itself.
    HWND window = galaxy::macos::game_window_handle();
    if (window == nullptr) return NSTerminateNow;
    post(window, WM_CLOSE, 0, 0);
    return NSTerminateCancel;
}
- (void)applicationDidChangeScreenParameters:(NSNotification*)notification { refresh_monitors(); }
@end

// ---------------------------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------------------------

namespace galaxy::macos {

void run_application() {
    @autoreleasepool {
        NSApplication* application = [NSApplication sharedApplication];
        static GalaxyApplicationDelegate* delegate = [[GalaxyApplicationDelegate alloc] init];
        application.delegate = delegate;
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu* menu_bar = [[NSMenu alloc] init];
        NSMenuItem* app_item = [[NSMenuItem alloc] init];
        [menu_bar addItem:app_item];
        // The app's own name (the bundle's CFBundleName), as macOS shows it in the menu bar.
        NSString* name = NSRunningApplication.currentApplication.localizedName ?: @"Orbit";
        NSMenu* app_menu = [[NSMenu alloc] initWithTitle:name];
        [app_menu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
        [app_menu addItem:[NSMenuItem separatorItem]];
        [app_menu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(terminate:) keyEquivalent:@"q"];
        app_item.submenu = app_menu;
        NSMenuItem* window_item = [[NSMenuItem alloc] init];
        [menu_bar addItem:window_item];
        NSMenu* window_menu = [[NSMenu alloc] initWithTitle:@"Window"];
        [window_menu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        NSMenuItem* full_screen = [window_menu addItemWithTitle:@"Enter Full Screen"
                                                         action:@selector(toggleFullScreen:)
                                                  keyEquivalent:@"f"];
        full_screen.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        window_item.submenu = window_menu;
        // The item above is the only full-screen item (AppKit would add its own as well).
        [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"NSFullScreenMenuItemEverywhere"];
        application.mainMenu = menu_bar;
        application.windowsMenu = window_menu;
        refresh_monitors();
        [application finishLaunching];
        [application activateIgnoringOtherApps:YES];
        [application run];
    }
}

void stop_application() noexcept {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp stop:nil];
        // stop: takes effect after the next event; post one.
        NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                                       modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
        [NSApp postEvent:event atStart:YES];
    });
}

void* window_metal_layer(HWND window) noexcept {
    if (!valid(window)) return nullptr;
    return (__bridge void*)window->layer;
}

bool window_client_pixels(HWND window, int& width, int& height, double& scale) noexcept {
    if (!valid(window)) return false;
    std::lock_guard lock(g_mutex);
    width = window->client_width;
    height = window->client_height;
    scale = window->scale;
    return true;
}

HWND game_window_handle() noexcept {
    std::lock_guard lock(g_mutex);
    for (HWND window : g_windows) {
        if (!window->destroyed && (window->ex_style & WS_EX_TOOLWINDOW) == 0u) return window;
    }
    for (HWND window : g_windows) {
        if (!window->destroyed) return window;
    }
    return nullptr;
}

}  // namespace galaxy::macos

// ---------------------------------------------------------------------------------------------
// Classes and windows
// ---------------------------------------------------------------------------------------------

ATOM RegisterClassExW(const WNDCLASSEXW* window_class) noexcept {
    if (window_class == nullptr || window_class->lpszClassName == nullptr || window_class->lpfnWndProc == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::lock_guard lock(g_mutex);
    RegisteredClass& entry = g_classes[window_class->lpszClassName];
    entry.proc = window_class->lpfnWndProc;
    entry.style = window_class->style;
    entry.cursor = window_class->hCursor;
    return static_cast<ATOM>(g_classes.size());
}

BOOL UnregisterClassW(LPCWSTR class_name, HINSTANCE) noexcept {
    std::lock_guard lock(g_mutex);
    return class_name != nullptr && g_classes.erase(class_name) != 0u;
}

HWND CreateWindowExW(DWORD ex_style, LPCWSTR class_name, LPCWSTR title, DWORD style, int x, int y, int width,
    int height, HWND, HMENU, HINSTANCE, LPVOID) noexcept {
    RegisteredClass window_class{};
    {
        std::lock_guard lock(g_mutex);
        const auto it = class_name != nullptr ? g_classes.find(class_name) : g_classes.end();
        if (it == g_classes.end()) {
            SetLastError(1407u);  // ERROR_CANNOT_FIND_WND_CLASS
            return nullptr;
        }
        window_class = it->second;
    }
    auto* window = new HWND__();
    window->proc = window_class.proc;
    window->class_name = class_name;
    window->title = title != nullptr ? title : L"";
    window->class_style = window_class.style;
    window->class_cursor = window_class.cursor;
    window->style = style;
    window->ex_style = ex_style;
    window->queue = current_queue();
    on_main(^{
        refresh_monitors();
        const NSWindowStyleMask mask = mask_for_style(style);
        NSRect frame;
        if (x == CW_USEDEFAULT || y == CW_USEDEFAULT) {
            const NSRect screen = NSScreen.mainScreen.visibleFrame;
            const double w = (width == CW_USEDEFAULT ? 1280.0 * g_primary_scale : width) / g_primary_scale;
            const double h = (height == CW_USEDEFAULT ? 720.0 * g_primary_scale : height) / g_primary_scale;
            frame = NSMakeRect(NSMidX(screen) - w / 2.0, NSMidY(screen) - h / 2.0, w, h);
        } else {
            frame = rect_from_pixels(RECT{x, y, x + width, y + height});
        }
        NSWindowStyleMask create_mask = mask;
        const bool capable = full_screen_capable(window);
        NSScreen* covered = capable && mask == NSWindowStyleMaskBorderless ? screen_covered_by(frame) : nil;
        if (covered != nil) {
            // Borderless fullscreen: a normal window that enters full screen once it is shown.
            create_mask = windowed_mask(style);
            frame = default_windowed_frame(covered);
            window->fullscreen_target = true;
        }
        window->windowed_frame = frame;
        trace_frame(covered != nil ? "create (full screen)" : "create", frame);
        const NSRect content = [NSWindow contentRectForFrameRect:frame styleMask:create_mask];
        NSWindow* ns_window = [[NSWindow alloc] initWithContentRect:content
                                                          styleMask:create_mask
                                                            backing:NSBackingStoreBuffered
                                                              defer:NO];
        ns_window.releasedWhenClosed = NO;
        ns_window.title = [NSString stringWithUTF8String:utf8_from_wide(window->title).c_str()];
        ns_window.acceptsMouseMovedEvents = YES;
        ns_window.collectionBehavior = capable ? NSWindowCollectionBehaviorFullScreenPrimary
                                               : NSWindowCollectionBehaviorFullScreenNone;
        GalaxyWindowView* view = [[GalaxyWindowView alloc] initWithFrame:NSMakeRect(0, 0, NSWidth(content), NSHeight(content))];
        view.window_handle = window;
        view.wantsLayer = YES;
        view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
        ns_window.contentView = view;
        [ns_window makeFirstResponder:view];
        GalaxyWindowDelegate* delegate = [[GalaxyWindowDelegate alloc] init];
        delegate.window_handle = window;
        ns_window.delegate = delegate;
        window->ns_window = ns_window;
        window->view = view;
        window->delegate = delegate;
        window->layer = (CAMetalLayer*)view.layer;
        {
            std::lock_guard lock(g_mutex);
            g_windows.push_back(window);
        }
        update_geometry(window, false, false);
    });
    (void)window->proc(window, WM_CREATE, 0, 0);
    if ((style & WS_VISIBLE) != 0u) ShowWindow(window, SW_SHOW);
    return window;
}

BOOL DestroyWindow(HWND window) noexcept {
    if (!valid(window)) {
        SetLastError(1400u);  // ERROR_INVALID_WINDOW_HANDLE
        return FALSE;
    }
    (void)window->proc(window, WM_DESTROY, 0, 0);
    {
        std::lock_guard lock(g_mutex);
        window->destroyed = true;
        if (g_capture == window) g_capture = nullptr;
    }
    on_main(^{
        NSWindow* ns_window = window->ns_window;
        ns_window.delegate = nil;
        window->view.window_handle = nullptr;
        [ns_window orderOut:nil];
        [ns_window close];
        set_cursor_hidden(false);
    });
    // The record stays allocated (destroyed) so late messages and queries fail cleanly.
    return TRUE;
}

BOOL ShowWindow(HWND window, int command) noexcept {
    if (!valid(window)) return FALSE;
    const bool was_visible = IsWindowVisible(window);
    on_main(^{
        NSWindow* ns_window = window->ns_window;
        const bool activate = (window->ex_style & WS_EX_NOACTIVATE) == 0u;
        switch (command) {
        case SW_HIDE:
            [ns_window orderOut:nil];
            break;
        case SW_MINIMIZE:
        case SW_SHOWMINIMIZED:
            [ns_window miniaturize:nil];
            break;
        case SW_MAXIMIZE:
            if (!ns_window.isZoomed) [ns_window zoom:nil];
            [ns_window makeKeyAndOrderFront:nil];
            break;
        case SW_RESTORE:
            if (ns_window.isMiniaturized) [ns_window deminiaturize:nil];
            [ns_window makeKeyAndOrderFront:nil];
            break;
        default:
            if (activate && command != SW_SHOWNOACTIVATE && command != SW_SHOWNA) {
                [ns_window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
            } else {
                [ns_window orderFront:nil];
            }
            break;
        }
        {
            std::lock_guard lock(g_mutex);
            if (command == SW_HIDE) window->style &= ~WS_VISIBLE; else window->style |= WS_VISIBLE;
        }
        update_geometry(window, command != SW_HIDE, command != SW_HIDE);
        if (command != SW_HIDE && full_screen_capable(window)) schedule_test_full_screen_toggles(window);
        if (command != SW_HIDE && window->fullscreen_target && !window->fullscreen_active) {
            // On the next turn of the run loop, when the window is on screen.
            dispatch_async(dispatch_get_main_queue(), ^{
                if (valid(window) && window->fullscreen_target) enter_full_screen(window, nil);
            });
        }
    });
    return was_visible;
}

BOOL UpdateWindow(HWND window) noexcept { return valid(window); }

BOOL ShowWindowAsync(HWND window, int command) noexcept {
    if (!valid(window)) return FALSE;
    dispatch_async(dispatch_get_main_queue(), ^{ (void)ShowWindow(window, command); });
    return TRUE;
}

BOOL SetForegroundWindow(HWND window) noexcept {
    if (!valid(window)) return FALSE;
    on_main(^{
        [window->ns_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    });
    return TRUE;
}

HWND SetActiveWindow(HWND window) noexcept {
    SetForegroundWindow(window);
    return window;
}

HWND GetActiveWindow() noexcept { return GetForegroundWindow(); }

HWND SetFocus(HWND window) noexcept {
    if (!valid(window)) return nullptr;
    on_main(^{ [window->ns_window makeFirstResponder:window->view]; });
    return window;
}

HWND GetFocus() noexcept { return GetForegroundWindow(); }

BOOL SetWindowTextW(HWND window, LPCWSTR text) noexcept {
    if (!valid(window)) return FALSE;
    const std::string title = utf8_from_wide(text != nullptr ? text : L"");
    on_main(^{ window->ns_window.title = [NSString stringWithUTF8String:title.c_str()]; });
    return TRUE;
}

BOOL InvalidateRect(HWND window, const RECT*, BOOL) noexcept { return valid(window); }

LRESULT DefWindowProcW(HWND window, UINT message, WPARAM, LPARAM) noexcept {
    switch (message) {
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_SETCURSOR: {
        HCURSOR cursor = nullptr;
        {
            std::lock_guard lock(g_mutex);
            if (window != nullptr) cursor = window->class_cursor;
        }
        SetCursor(cursor);
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------------------------

namespace {

bool process_sent(ThreadQueue& queue) {
    std::shared_ptr<SentMessage> sent;
    {
        std::lock_guard lock(queue.mutex);
        if (queue.sent.empty()) return false;
        sent = queue.sent.front();
        queue.sent.pop_front();
    }
    LRESULT result = 0;
    if (valid(sent->window)) result = sent->window->proc(sent->window, sent->message, sent->wparam, sent->lparam);
    {
        std::lock_guard lock(queue.mutex);
        sent->result = result;
        sent->done = true;
    }
    queue.wake.notify_all();
    return true;
}

bool take_message(ThreadQueue& queue, MSG* message, bool remove) {
    std::lock_guard lock(queue.mutex);
    if (!queue.posted.empty()) {
        *message = queue.posted.front();
        if (remove) queue.posted.pop_front();
        t_message_time = static_cast<LONG>(message->time);
        return true;
    }
    if (queue.quit) {
        *message = MSG{};
        message->message = WM_QUIT;
        message->wParam = static_cast<WPARAM>(queue.quit_code);
        message->time = message_time_now();
        if (remove) queue.quit = false;
        return true;
    }
    return false;
}

}  // namespace

BOOL GetMessageW(LPMSG message, HWND, UINT, UINT) noexcept {
    const std::shared_ptr<ThreadQueue> queue = current_queue();
    for (;;) {
        while (process_sent(*queue)) {}
        if (take_message(*queue, message, true)) return message->message != WM_QUIT;
        std::unique_lock lock(queue->mutex);
        queue->wake.wait(lock, [&] { return !queue->posted.empty() || !queue->sent.empty() || queue->quit; });
    }
}

BOOL PeekMessageW(LPMSG message, HWND, UINT, UINT, UINT remove) noexcept {
    const std::shared_ptr<ThreadQueue> queue = current_queue();
    while (process_sent(*queue)) {}
    return take_message(*queue, message, (remove & PM_REMOVE) != 0u);
}

BOOL TranslateMessage(const MSG*) noexcept { return FALSE; }

LRESULT DispatchMessageW(const MSG* message) noexcept {
    if (message == nullptr || !valid(message->hwnd)) return 0;
    return message->hwnd->proc(message->hwnd, message->message, message->wParam, message->lParam);
}

BOOL PostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!valid(window)) {
        SetLastError(1400u);
        return FALSE;
    }
    post(window, message, wparam, lparam);
    return TRUE;
}

BOOL PostThreadMessageW(DWORD thread_id, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    std::shared_ptr<ThreadQueue> queue;
    {
        std::lock_guard lock(g_queues_mutex);
        const auto it = g_queues.find(thread_id);
        if (it != g_queues.end()) queue = it->second;
    }
    if (!queue) {
        SetLastError(1444u);  // ERROR_INVALID_THREAD_ID
        return FALSE;
    }
    if (message == WM_QUIT) {
        {
            std::lock_guard lock(queue->mutex);
            queue->quit = true;
            queue->quit_code = static_cast<int>(wparam);
        }
        queue->wake.notify_all();
        return TRUE;
    }
    post_to_queue(*queue, nullptr, message, wparam, lparam);
    return TRUE;
}

LRESULT SendMessageTimeoutW(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT, UINT timeout_ms,
    PDWORD_PTR result) noexcept {
    if (!valid(window)) {
        SetLastError(1400u);
        return 0;
    }
    std::shared_ptr<ThreadQueue> queue;
    {
        std::lock_guard lock(g_mutex);
        queue = window->queue;
    }
    if (queue == t_queue) {
        const LRESULT value = window->proc(window, message, wparam, lparam);
        if (result != nullptr) *result = static_cast<DWORD_PTR>(value);
        return 1;
    }
    auto sent = std::make_shared<SentMessage>();
    sent->window = window;
    sent->message = message;
    sent->wparam = wparam;
    sent->lparam = lparam;
    {
        std::lock_guard lock(queue->mutex);
        queue->sent.push_back(sent);
    }
    queue->wake.notify_all();
    std::unique_lock lock(queue->mutex);
    const bool done = timeout_ms == INFINITE
        ? (queue->wake.wait(lock, [&] { return sent->done; }), true)
        : queue->wake.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return sent->done; });
    if (!done) {
        SetLastError(ERROR_TIMEOUT);
        return 0;
    }
    if (result != nullptr) *result = static_cast<DWORD_PTR>(sent->result);
    return 1;
}

LRESULT SendMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    DWORD_PTR result = 0;
    (void)SendMessageTimeoutW(window, message, wparam, lparam, SMTO_NORMAL, INFINITE, &result);
    return static_cast<LRESULT>(result);
}

void PostQuitMessage(int exit_code) noexcept {
    const std::shared_ptr<ThreadQueue> queue = current_queue();
    {
        std::lock_guard lock(queue->mutex);
        queue->quit = true;
        queue->quit_code = exit_code;
    }
    queue->wake.notify_all();
}

LONG GetMessageTime() noexcept { return t_message_time; }

BOOL WaitMessage() noexcept {
    const std::shared_ptr<ThreadQueue> queue = current_queue();
    std::unique_lock lock(queue->mutex);
    queue->wake.wait(lock, [&] { return !queue->posted.empty() || !queue->sent.empty() || queue->quit; });
    return TRUE;
}

DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD milliseconds, DWORD) noexcept {
    // Polls the objects and the message queue in short slices.
    const std::shared_ptr<ThreadQueue> queue = current_queue();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds == INFINITE ? 0u : milliseconds);
    for (;;) {
        if (count != 0u) {
            const DWORD result = WaitForMultipleObjects(count, handles, wait_all, 0);
            if (result != WAIT_TIMEOUT) return result;
        }
        {
            std::unique_lock lock(queue->mutex);
            if (!queue->posted.empty() || !queue->sent.empty() || queue->quit) return WAIT_OBJECT_0 + count;
            queue->wake.wait_for(lock, std::chrono::milliseconds(1));
        }
        if (milliseconds != INFINITE && std::chrono::steady_clock::now() >= deadline) return WAIT_TIMEOUT;
    }
}

// ---------------------------------------------------------------------------------------------
// Window properties and placement
// ---------------------------------------------------------------------------------------------

LONG_PTR GetWindowLongPtrW(HWND window, int index) noexcept {
    if (!valid(window)) return 0;
    std::lock_guard lock(g_mutex);
    switch (index) {
    case GWL_STYLE: return static_cast<LONG_PTR>(window->style);
    case GWL_EXSTYLE: return static_cast<LONG_PTR>(window->ex_style);
    case GWLP_USERDATA: return window->user_data;
    case GWLP_WNDPROC: return reinterpret_cast<LONG_PTR>(window->proc);
    default: return 0;
    }
}

LONG_PTR SetWindowLongPtrW(HWND window, int index, LONG_PTR value) noexcept {
    if (!valid(window)) return 0;
    std::lock_guard lock(g_mutex);
    LONG_PTR previous = 0;
    switch (index) {
    case GWL_STYLE:
        previous = static_cast<LONG_PTR>(window->style);
        window->style = static_cast<DWORD>(value);
        break;
    case GWL_EXSTYLE:
        previous = static_cast<LONG_PTR>(window->ex_style);
        window->ex_style = static_cast<DWORD>(value);
        break;
    case GWLP_USERDATA:
        previous = window->user_data;
        window->user_data = value;
        break;
    case GWLP_WNDPROC:
        previous = reinterpret_cast<LONG_PTR>(window->proc);
        window->proc = reinterpret_cast<WNDPROC>(value);
        break;
    default:
        break;
    }
    return previous;
}

BOOL SetWindowPos(HWND window, HWND insert_after, int x, int y, int width, int height, UINT flags) noexcept {
    if (!valid(window)) return FALSE;
    DWORD style = 0;
    {
        std::lock_guard lock(g_mutex);
        style = window->style;
    }
    on_main(^{
        NSWindow* ns_window = window->ns_window;
        const NSWindowStyleMask mask = mask_for_style(style);
        const bool capable = full_screen_capable(window);
        const bool full_screen = capable &&
            (window->fullscreen_active || window->fullscreen_transition || window->fullscreen_target);
        // In full screen, sizes and positions are relative to the windowed frame.
        NSRect frame = full_screen ? window->windowed_frame : ns_window.frame;
        RECT pixels = pixels_from_rect(frame);
        if ((flags & SWP_NOMOVE) == 0u) {
            const LONG w = pixels.right - pixels.left;
            const LONG h = pixels.bottom - pixels.top;
            pixels.left = x;
            pixels.top = y;
            pixels.right = x + w;
            pixels.bottom = y + h;
        }
        if ((flags & SWP_NOSIZE) == 0u) {
            pixels.right = pixels.left + width;
            pixels.bottom = pixels.top + height;
        }
        frame = rect_from_pixels(pixels);
        if (trace_window()) {
            std::fprintf(stderr, "[window] SetWindowPos style=%08x flags=%04x full_screen=%d\n",
                         static_cast<unsigned>(style), flags, full_screen ? 1 : 0);
            trace_frame("  frame", frame);
        }
        if ((flags & SWP_HIDEWINDOW) != 0u) [ns_window orderOut:nil];
        if ((flags & SWP_SHOWWINDOW) != 0u) {
            if ((flags & SWP_NOACTIVATE) != 0u) [ns_window orderFront:nil]; else [ns_window makeKeyAndOrderFront:nil];
        }
        NSScreen* covered = capable && mask == NSWindowStyleMaskBorderless ? screen_covered_by(frame) : nil;
        if (covered != nil) {
            enter_full_screen(window, covered);
        } else if (full_screen) {
            leave_full_screen(window, frame);
        } else {
            if ((flags & SWP_FRAMECHANGED) != 0u && ns_window.styleMask != mask) {
                ns_window.styleMask = mask;
                [ns_window makeFirstResponder:window->view];
            }
            if (insert_after == HWND_TOPMOST) ns_window.level = NSFloatingWindowLevel;
            else if (insert_after == HWND_NOTOPMOST) ns_window.level = NSNormalWindowLevel;
            [ns_window setFrame:frame display:YES];
        }
        update_geometry(window, true, true);
    });
    return TRUE;
}

// Window rectangles are in pixels of the main display (points times its scale) whatever DPI is
// asked for (GetDpiFor* report 96), so the title bar is added in those pixels too.
BOOL AdjustWindowRectExForDpi(LPRECT rect, DWORD style, BOOL, DWORD, UINT) noexcept {
    if (rect == nullptr) return FALSE;
    if (mask_for_style(style) != NSWindowStyleMaskBorderless) {
        ensure_monitors();
        double scale = 1.0;
        {
            std::lock_guard lock(g_mutex);
            scale = g_primary_scale;
        }
        rect->top -= static_cast<LONG>(std::lround(title_bar_points() * scale));
    }
    return TRUE;
}

BOOL AdjustWindowRectEx(LPRECT rect, DWORD style, BOOL menu, DWORD ex_style) noexcept {
    return AdjustWindowRectExForDpi(rect, style, menu, ex_style, GetDpiForSystem());
}

// ---------------------------------------------------------------------------------------------
// Monitors
// ---------------------------------------------------------------------------------------------

namespace {

HMONITOR monitor_handle(std::size_t index) { return reinterpret_cast<HMONITOR>(index + 1u); }

std::optional<MonitorRecord> monitor_record(HMONITOR monitor) {
    ensure_monitors();
    std::lock_guard lock(g_mutex);
    const auto index = reinterpret_cast<std::uintptr_t>(monitor);
    if (index == 0u || index > g_monitors.size()) return std::nullopt;
    return g_monitors[index - 1u];
}

HMONITOR monitor_at(POINT point, DWORD flags) {
    ensure_monitors();
    std::lock_guard lock(g_mutex);
    if (g_monitors.empty()) return nullptr;
    std::size_t nearest = 0;
    long long best = -1;
    for (std::size_t i = 0; i < g_monitors.size(); ++i) {
        const RECT& r = g_monitors[i].bounds;
        if (point.x >= r.left && point.x < r.right && point.y >= r.top && point.y < r.bottom) return monitor_handle(i);
        const long long dx = point.x < r.left ? r.left - point.x : (point.x >= r.right ? point.x - r.right + 1 : 0);
        const long long dy = point.y < r.top ? r.top - point.y : (point.y >= r.bottom ? point.y - r.bottom + 1 : 0);
        const long long distance = dx * dx + dy * dy;
        if (best < 0 || distance < best) {
            best = distance;
            nearest = i;
        }
    }
    if (flags == MONITOR_DEFAULTTONULL) return nullptr;
    return monitor_handle(flags == MONITOR_DEFAULTTOPRIMARY ? 0u : nearest);
}

std::wstring device_name(std::size_t index) { return L"\\\\.\\DISPLAY" + std::to_wstring(index + 1u); }

}  // namespace

HMONITOR MonitorFromPoint(POINT point, DWORD flags) noexcept { return monitor_at(point, flags); }

HMONITOR MonitorFromRect(const RECT* rect, DWORD flags) noexcept {
    if (rect == nullptr) return nullptr;
    return monitor_at(POINT{(rect->left + rect->right) / 2, (rect->top + rect->bottom) / 2}, flags);
}

HMONITOR MonitorFromWindow(HWND window, DWORD flags) noexcept {
    if (!valid(window)) return flags == MONITOR_DEFAULTTONULL ? nullptr : monitor_handle(0);
    RECT frame{};
    {
        std::lock_guard lock(g_mutex);
        frame = window->frame;
    }
    return MonitorFromRect(&frame, flags);
}

BOOL GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info) noexcept {
    const auto record = monitor_record(monitor);
    if (!record || info == nullptr) return FALSE;
    info->rcMonitor = record->bounds;
    info->rcWork = record->work;
    info->dwFlags = record->primary ? MONITORINFOF_PRIMARY : 0u;
    if (info->cbSize >= sizeof(MONITORINFOEXW)) {
        const std::wstring name = device_name(reinterpret_cast<std::uintptr_t>(monitor) - 1u);
        std::wmemcpy(static_cast<MONITORINFOEXW*>(info)->szDevice, name.c_str(), name.size() + 1u);
    }
    return TRUE;
}

BOOL EnumDisplayMonitors(HDC, const RECT*, MONITORENUMPROC callback, LPARAM user) noexcept {
    ensure_monitors();
    std::vector<MonitorRecord> monitors;
    {
        std::lock_guard lock(g_mutex);
        monitors = g_monitors;
    }
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        RECT bounds = monitors[i].bounds;
        if (callback != nullptr && !callback(monitor_handle(i), nullptr, &bounds, user)) break;
    }
    return TRUE;
}

BOOL EnumDisplaySettingsW(LPCWSTR device, DWORD mode_number, DEVMODEW* mode) noexcept {
    if (mode == nullptr) return FALSE;
    ensure_monitors();
    std::optional<MonitorRecord> record;
    {
        std::lock_guard lock(g_mutex);
        for (std::size_t i = 0; i < g_monitors.size(); ++i) {
            if (device == nullptr ? g_monitors[i].primary : device_name(i) == device) record = g_monitors[i];
        }
    }
    if (!record) return FALSE;
    const WORD size = mode->dmSize;
    *mode = DEVMODEW{};
    mode->dmSize = size;
    mode->dmBitsPerPel = 32;
    mode->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
    if (mode_number == ENUM_CURRENT_SETTINGS || mode_number == ENUM_REGISTRY_SETTINGS) {
        mode->dmPelsWidth = static_cast<DWORD>(record->bounds.right - record->bounds.left);
        mode->dmPelsHeight = static_cast<DWORD>(record->bounds.bottom - record->bounds.top);
        mode->dmDisplayFrequency = record->refresh_hz;
        mode->dmPosition = POINT{record->bounds.left, record->bounds.top};
        return TRUE;
    }
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(record->display, nullptr);
    if (modes == nullptr) return FALSE;
    const bool found = mode_number < static_cast<DWORD>(CFArrayGetCount(modes));
    if (found) {
        auto display_mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, mode_number);
        mode->dmPelsWidth = static_cast<DWORD>(CGDisplayModeGetPixelWidth(display_mode));
        mode->dmPelsHeight = static_cast<DWORD>(CGDisplayModeGetPixelHeight(display_mode));
        const double refresh = CGDisplayModeGetRefreshRate(display_mode);
        mode->dmDisplayFrequency = refresh > 0.0 ? static_cast<DWORD>(std::lround(refresh)) : record->refresh_hz;
    }
    CFRelease(modes);
    return found;
}

BOOL EnumDisplaySettingsExW(LPCWSTR device, DWORD mode_number, DEVMODEW* mode, DWORD) noexcept {
    return EnumDisplaySettingsW(device, mode_number, mode);
}

std::map<std::wstring, std::wstring> galaxy::macos::monitor_names() {
    __block std::map<std::wstring, std::wstring> names;
    on_main(^{
        NSArray<NSScreen*>* screens = NSScreen.screens;
        for (NSUInteger i = 0; i < screens.count; ++i) {
            names[device_name(i)] = wide_from_utf8(screens[i].localizedName.UTF8String);
        }
    });
    return names;
}

int GetSystemMetrics(int index) noexcept {
    std::lock_guard lock(g_mutex);
    const RECT primary = g_monitors.empty() ? RECT{0, 0, 1920, 1080} : g_monitors[0].bounds;
    switch (index) {
    case SM_CXSCREEN: return primary.right - primary.left;
    case SM_CYSCREEN: return primary.bottom - primary.top;
    case SM_CXICON:
    case SM_CYICON: return 32;
    case SM_CXSMICON:
    case SM_CYSMICON: return 16;
    case SM_CMONITORS: return static_cast<int>(g_monitors.size());
    case SM_REMOTESESSION: return 0;
    case SM_XVIRTUALSCREEN:
    case SM_YVIRTUALSCREEN: return 0;
    case SM_CXVIRTUALSCREEN: return primary.right - primary.left;
    case SM_CYVIRTUALSCREEN: return primary.bottom - primary.top;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Cursor, capture, input
// ---------------------------------------------------------------------------------------------

HCURSOR LoadCursorA(HINSTANCE, LPCSTR name) noexcept {
    return reinterpret_cast<HCURSOR>(const_cast<char*>(name != nullptr ? name : IDC_ARROW));
}
HCURSOR LoadCursorW(HINSTANCE instance, const char* name) noexcept { return LoadCursorA(instance, name); }
HANDLE LoadImageW(HINSTANCE, LPCWSTR, UINT, int, int, UINT) noexcept { return nullptr; }

HCURSOR SetCursor(HCURSOR cursor) noexcept {
    const bool hide = cursor == nullptr;
    g_cursor_hidden_wanted.store(hide, std::memory_order_relaxed);
    dispatch_async(dispatch_get_main_queue(), ^{
        HWND window = galaxy::macos::game_window_handle();
        const bool inside = window != nullptr && window->ns_window.isKeyWindow &&
            NSPointInRect([window->ns_window mouseLocationOutsideOfEventStream], window->view.frame);
        set_cursor_hidden(hide && inside);
    });
    return nullptr;
}

int ShowCursor(BOOL show) noexcept {
    SetCursor(show ? LoadCursorA(nullptr, IDC_ARROW) : nullptr);
    return show ? 0 : -1;
}

BOOL ClipCursor(const RECT*) noexcept { return TRUE; }

BOOL SetCursorPos(int x, int y) noexcept {
    const NSPoint point = points_from_pixels(x, y);
    CGWarpMouseCursorPosition(CGPointMake(point.x, g_primary_height_points - point.y));
    g_cursor_x.store(x, std::memory_order_relaxed);
    g_cursor_y.store(y, std::memory_order_relaxed);
    return TRUE;
}

HWND SetCapture(HWND window) noexcept {
    std::lock_guard lock(g_mutex);
    const HWND previous = g_capture;
    g_capture = window;
    return previous;
}

BOOL ReleaseCapture() noexcept {
    HWND previous = nullptr;
    {
        std::lock_guard lock(g_mutex);
        previous = g_capture;
        g_capture = nullptr;
    }
    if (previous != nullptr) post(previous, WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}

HWND GetCapture() noexcept {
    std::lock_guard lock(g_mutex);
    return g_capture;
}

BOOL TrackMouseEvent(TRACKMOUSEEVENT* event) noexcept {
    if (event == nullptr || !valid(event->hwndTrack)) return FALSE;
    std::lock_guard lock(g_mutex);
    if ((event->dwFlags & TME_LEAVE) != 0u) event->hwndTrack->track_leave = (event->dwFlags & TME_CANCEL) == 0u;
    return TRUE;
}

BOOL GetCurrentInputMessageSource(INPUT_MESSAGE_SOURCE* source) noexcept {
    if (source == nullptr) return FALSE;
    source->deviceType = IMDT_MOUSE;
    source->originId = IMO_HARDWARE;
    return TRUE;
}

UINT MapVirtualKeyW(UINT code, UINT map_type) noexcept {
    if (map_type == MAPVK_VSC_TO_VK_EX || map_type == MAPVK_VSC_TO_VK) {
        switch (code & 0xFFu) {
        case 0x2A: return map_type == MAPVK_VSC_TO_VK_EX ? VK_LSHIFT : VK_SHIFT;
        case 0x36: return map_type == MAPVK_VSC_TO_VK_EX ? VK_RSHIFT : VK_SHIFT;
        case 0x1D: return map_type == MAPVK_VSC_TO_VK_EX ? VK_LCONTROL : VK_CONTROL;
        case 0x38: return map_type == MAPVK_VSC_TO_VK_EX ? VK_LMENU : VK_MENU;
        default: break;
        }
        for (unsigned short key_code = 0; key_code < 0x80; ++key_code) {
            const KeyInfo info = key_info(key_code);
            if (info.vk != 0 && info.scan == (code & 0xFFu)) return info.vk;
        }
        return 0;
    }
    if (map_type == MAPVK_VK_TO_VSC || map_type == MAPVK_VK_TO_VSC_EX) {
        for (unsigned short key_code = 0; key_code < 0x80; ++key_code) {
            const KeyInfo info = key_info(key_code);
            if (info.vk == code || specific_vk(info) == code) return info.scan;
        }
    }
    return 0;
}

UINT MapVirtualKeyA(UINT code, UINT map_type) noexcept { return MapVirtualKeyW(code, map_type); }

HGDIOBJ GetStockObject(int) noexcept { return nullptr; }
int FillRect(HDC, const RECT*, HBRUSH) noexcept { return 1; }

// ---------------------------------------------------------------------------------------------
// Window queries
// ---------------------------------------------------------------------------------------------

HWND GetForegroundWindow() noexcept {
    std::lock_guard lock(g_mutex);
    for (HWND window : g_windows) {
        if (!window->destroyed && window->key) return window;
    }
    return nullptr;
}

BOOL IsWindow(HWND window) noexcept { return valid(window); }

BOOL IsWindowVisible(HWND window) noexcept {
    if (!valid(window)) return FALSE;
    std::lock_guard lock(g_mutex);
    return window->visible;
}

BOOL IsIconic(HWND window) noexcept {
    if (!valid(window)) return FALSE;
    std::lock_guard lock(g_mutex);
    return window->minimized;
}

DWORD GetWindowThreadProcessId(HWND window, LPDWORD process_id) noexcept {
    if (!valid(window)) {
        if (process_id != nullptr) *process_id = 0;
        return 0;
    }
    if (process_id != nullptr) *process_id = GetCurrentProcessId();
    std::lock_guard lock(g_mutex);
    return window->queue ? window->queue->thread_id : 0;
}

BOOL GetCursorPos(LPPOINT point) noexcept {
    if (point == nullptr) return FALSE;
    point->x = g_cursor_x.load(std::memory_order_relaxed);
    point->y = g_cursor_y.load(std::memory_order_relaxed);
    return TRUE;
}

HWND WindowFromPoint(POINT point) noexcept {
    std::lock_guard lock(g_mutex);
    for (HWND window : g_windows) {
        if (window->destroyed || !window->visible) continue;
        const RECT& frame = window->frame;
        if (point.x >= frame.left && point.x < frame.right && point.y >= frame.top && point.y < frame.bottom) return window;
    }
    return nullptr;
}

HWND GetAncestor(HWND window, UINT) noexcept { return valid(window) ? window : nullptr; }

BOOL EnumWindows(WNDENUMPROC callback, LPARAM user) noexcept {
    std::vector<HWND> windows;
    {
        std::lock_guard lock(g_mutex);
        for (HWND window : g_windows) {
            if (!window->destroyed) windows.push_back(window);
        }
    }
    for (HWND window : windows) {
        if (callback != nullptr && !callback(window, user)) break;
    }
    return TRUE;
}

BOOL GetClientRect(HWND window, LPRECT rect) noexcept {
    if (rect == nullptr || !valid(window)) {
        SetLastError(1400u);
        return FALSE;
    }
    std::lock_guard lock(g_mutex);
    *rect = RECT{0, 0, window->client_width, window->client_height};
    return TRUE;
}

BOOL GetWindowRect(HWND window, LPRECT rect) noexcept {
    if (rect == nullptr || !valid(window)) {
        SetLastError(1400u);
        return FALSE;
    }
    std::lock_guard lock(g_mutex);
    *rect = window->frame;
    return TRUE;
}

BOOL ScreenToClient(HWND window, LPPOINT point) noexcept {
    if (point == nullptr || !valid(window)) return FALSE;
    std::lock_guard lock(g_mutex);
    point->x -= window->client_x;
    point->y -= window->client_y;
    return TRUE;
}

BOOL ClientToScreen(HWND window, LPPOINT point) noexcept {
    if (point == nullptr || !valid(window)) return FALSE;
    std::lock_guard lock(g_mutex);
    point->x += window->client_x;
    point->y += window->client_y;
    return TRUE;
}

int GetClassNameW(HWND window, LPWSTR buffer, int size) noexcept {
    if (buffer == nullptr || size <= 0 || !valid(window)) return 0;
    std::lock_guard lock(g_mutex);
    const std::size_t copied = std::min<std::size_t>(window->class_name.size(), static_cast<std::size_t>(size) - 1u);
    std::wmemcpy(buffer, window->class_name.c_str(), copied);
    buffer[copied] = L'\0';
    return static_cast<int>(copied);
}

SHORT GetAsyncKeyState(int virtual_key) noexcept {
    if (virtual_key <= 0 || virtual_key >= 256) return 0;
    const auto down = [](int key) { return g_keys[static_cast<std::size_t>(key)].load(std::memory_order_relaxed); };
    bool pressed = down(virtual_key);
    if (virtual_key == VK_LBUTTON) pressed = (NSEvent.pressedMouseButtons & 1u) != 0u;
    if (virtual_key == VK_RBUTTON) pressed = (NSEvent.pressedMouseButtons & 2u) != 0u;
    if (virtual_key == VK_MBUTTON) pressed = (NSEvent.pressedMouseButtons & 4u) != 0u;
    return pressed ? static_cast<SHORT>(0x8000) : 0;
}

SHORT GetKeyState(int virtual_key) noexcept { return GetAsyncKeyState(virtual_key); }

// ---------------------------------------------------------------------------------------------
// Power requests
// ---------------------------------------------------------------------------------------------

namespace {
struct PowerRequest {
    std::string reason;
    IOPMAssertionID display = kIOPMNullAssertionID;
    IOPMAssertionID system = kIOPMNullAssertionID;
};
}  // namespace

HANDLE PowerCreateRequest(PREASON_CONTEXT context) noexcept {
    auto* request = new PowerRequest();
    request->reason = context != nullptr && (context->Flags & POWER_REQUEST_CONTEXT_SIMPLE_STRING) != 0u &&
            context->Reason.SimpleReasonString != nullptr
        ? utf8_from_wide(context->Reason.SimpleReasonString)
        : "Orbit";
    return request;
}

BOOL PowerSetRequest(HANDLE handle, POWER_REQUEST_TYPE type) noexcept {
    auto* request = static_cast<PowerRequest*>(handle);
    if (request == nullptr || handle == INVALID_HANDLE_VALUE) return FALSE;
    IOPMAssertionID& assertion = type == PowerRequestDisplayRequired ? request->display : request->system;
    if (assertion != kIOPMNullAssertionID) return TRUE;
    CFStringRef reason = CFStringCreateWithCString(nullptr, request->reason.c_str(), kCFStringEncodingUTF8);
    const IOReturn result = IOPMAssertionCreateWithName(
        type == PowerRequestDisplayRequired ? kIOPMAssertionTypePreventUserIdleDisplaySleep
                                            : kIOPMAssertionTypePreventUserIdleSystemSleep,
        kIOPMAssertionLevelOn, reason, &assertion);
    CFRelease(reason);
    return result == kIOReturnSuccess;
}

BOOL PowerClearRequest(HANDLE handle, POWER_REQUEST_TYPE type) noexcept {
    auto* request = static_cast<PowerRequest*>(handle);
    if (request == nullptr || handle == INVALID_HANDLE_VALUE) return FALSE;
    IOPMAssertionID& assertion = type == PowerRequestDisplayRequired ? request->display : request->system;
    if (assertion == kIOPMNullAssertionID) return TRUE;
    IOPMAssertionRelease(assertion);
    assertion = kIOPMNullAssertionID;
    return TRUE;
}
