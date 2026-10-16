// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// User32 subset of the Win32 emulation (included by the compat windows.h): window classes,
// windows, thread message queues, monitors, cursor and keyboard state, implemented on Cocoa in
// runtime/platform/macos/src/win32_window.mm. Coordinates are physical (backing) pixels with a
// top-left origin; key messages use the Windows virtual-key namespace, so key bindings and
// settings files are the same on both platforms.

// Virtual-key codes (winuser.h values).
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F13 0x7C
#define VK_F14 0x7D
#define VK_F15 0x7E
#define VK_F16 0x7F
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F21 0x84
#define VK_F22 0x85
#define VK_F23 0x86
#define VK_F24 0x87
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_8 0xDF
#define VK_OEM_102 0xE2

#define GA_PARENT 1u
#define GA_ROOT 2u
#define GA_ROOTOWNER 3u

using WNDENUMPROC = BOOL (*)(HWND, LPARAM);
using WNDPROC = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);
using MONITORENUMPROC = BOOL (*)(HMONITOR, HDC, LPRECT, LPARAM);
using HGDIOBJ = void*;

// ---------------------------------------------------------------------------------------------
// Window classes, styles and messages (winuser.h values)
// ---------------------------------------------------------------------------------------------

struct WNDCLASSEXW {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
};

struct MSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
};
using LPMSG = MSG*;

#define CS_VREDRAW 0x0001u
#define CS_HREDRAW 0x0002u
#define CS_DBLCLKS 0x0008u
#define CS_OWNDC 0x0020u

#define WS_OVERLAPPED 0x00000000u
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CAPTION 0x00C00000u
#define WS_BORDER 0x00800000u
#define WS_DLGFRAME 0x00400000u
#define WS_SYSMENU 0x00080000u
#define WS_THICKFRAME 0x00040000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_TOPMOST 0x00000008u
#define WS_EX_TOOLWINDOW 0x00000080u
#define WS_EX_APPWINDOW 0x00040000u
#define WS_EX_NOACTIVATE 0x08000000u
#define CW_USEDEFAULT (static_cast<int>(0x80000000u))

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWNA 8
#define SW_RESTORE 9

#define SWP_NOSIZE 0x0001u
#define SWP_NOMOVE 0x0002u
#define SWP_NOZORDER 0x0004u
#define SWP_NOREDRAW 0x0008u
#define SWP_NOACTIVATE 0x0010u
#define SWP_FRAMECHANGED 0x0020u
#define SWP_SHOWWINDOW 0x0040u
#define SWP_HIDEWINDOW 0x0080u
#define SWP_NOOWNERZORDER 0x0200u
#define HWND_TOP (static_cast<HWND>(nullptr))
#define HWND_BOTTOM (reinterpret_cast<HWND>(static_cast<std::intptr_t>(1)))
#define HWND_TOPMOST (reinterpret_cast<HWND>(static_cast<std::intptr_t>(-1)))
#define HWND_NOTOPMOST (reinterpret_cast<HWND>(static_cast<std::intptr_t>(-2)))

#define GWLP_WNDPROC (-4)
#define GWLP_HINSTANCE (-6)
#define GWLP_USERDATA (-21)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)

#define WM_NULL 0x0000u
#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_PAINT 0x000Fu
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ERASEBKGND 0x0014u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_CANCELMODE 0x001Fu
#define WM_SETCURSOR 0x0020u
#define WM_GETMINMAXINFO 0x0024u
#define WM_WINDOWPOSCHANGED 0x0047u
#define WM_NCHITTEST 0x0084u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCHAR 0x0106u
#define WM_SYSCOMMAND 0x0112u
#define WM_TIMER 0x0113u
#define WM_ENTERMENULOOP 0x0211u
#define WM_EXITMENULOOP 0x0212u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_LBUTTONDBLCLK 0x0203u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP 0x0205u
#define WM_RBUTTONDBLCLK 0x0206u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP 0x0208u
#define WM_MBUTTONDBLCLK 0x0209u
#define WM_MOUSEWHEEL 0x020Au
#define WM_XBUTTONDOWN 0x020Bu
#define WM_XBUTTONUP 0x020Cu
#define WM_XBUTTONDBLCLK 0x020Du
#define WM_MOUSEHWHEEL 0x020Eu
#define WM_CAPTURECHANGED 0x0215u
#define WM_ENTERSIZEMOVE 0x0231u
#define WM_EXITSIZEMOVE 0x0232u
#define WM_MOUSELEAVE 0x02A3u
#define WM_DPICHANGED 0x02E0u
#define WM_USER 0x0400u
#define WM_APP 0x8000u

#define MK_LBUTTON 0x0001u
#define MK_RBUTTON 0x0002u
#define MK_SHIFT 0x0004u
#define MK_CONTROL 0x0008u
#define MK_MBUTTON 0x0010u
#define MK_XBUTTON1 0x0020u
#define MK_XBUTTON2 0x0040u
#define XBUTTON1 0x0001u
#define XBUTTON2 0x0002u
#define WHEEL_DELTA 120
#define GET_XBUTTON_WPARAM(w) (HIWORD(w))
#define GET_WHEEL_DELTA_WPARAM(w) (static_cast<short>(HIWORD(w)))
#define GET_KEYSTATE_WPARAM(w) (LOWORD(w))
#define MAKEWPARAM(l, h) (static_cast<WPARAM>(static_cast<DWORD>(MAKELONG(l, h))))
#define MAKELPARAM(l, h) (static_cast<LPARAM>(static_cast<DWORD>(MAKELONG(l, h))))

#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define HTCLIENT 1

#define PM_NOREMOVE 0x0000u
#define PM_REMOVE 0x0001u
#define PM_NOYIELD 0x0002u
#define SMTO_NORMAL 0x0000u
#define SMTO_BLOCK 0x0001u
#define SMTO_ABORTIFHUNG 0x0002u

#define TME_HOVER 0x00000001u
#define TME_LEAVE 0x00000002u
#define TME_CANCEL 0x80000000u
struct TRACKMOUSEEVENT {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
};

enum INPUT_MESSAGE_DEVICE_TYPE { IMDT_UNAVAILABLE = 0, IMDT_KEYBOARD = 1, IMDT_MOUSE = 2, IMDT_TOUCH = 4, IMDT_PEN = 8 };
enum INPUT_MESSAGE_ORIGIN_ID { IMO_UNAVAILABLE = 0, IMO_HARDWARE = 1, IMO_INJECTED = 2, IMO_SYSTEM = 4 };
struct INPUT_MESSAGE_SOURCE {
    INPUT_MESSAGE_DEVICE_TYPE deviceType;
    INPUT_MESSAGE_ORIGIN_ID originId;
};

#define MAPVK_VK_TO_VSC 0u
#define MAPVK_VSC_TO_VK 1u
#define MAPVK_VK_TO_CHAR 2u
#define MAPVK_VSC_TO_VK_EX 3u
#define MAPVK_VK_TO_VSC_EX 4u

#define IDC_ARROW (reinterpret_cast<const char*>(static_cast<std::uintptr_t>(32512)))
#define IMAGE_ICON 1u
#define IMAGE_CURSOR 2u
#define LR_DEFAULTCOLOR 0x0000u
#define LR_DEFAULTSIZE 0x0040u
#define BLACK_BRUSH 4

#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define SM_CMONITORS 80
#define SM_REMOTESESSION 0x1000

#define MONITOR_DEFAULTTONULL 0x00000000u
#define MONITOR_DEFAULTTOPRIMARY 0x00000001u
#define MONITOR_DEFAULTTONEAREST 0x00000002u
#define MONITORINFOF_PRIMARY 0x00000001u
#define CCHDEVICENAME 32
struct MONITORINFO {
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
};
struct MONITORINFOEXW : MONITORINFO {
    WCHAR szDevice[CCHDEVICENAME];
};
using LPMONITORINFO = MONITORINFO*;

#define ENUM_CURRENT_SETTINGS (static_cast<DWORD>(-1))
#define ENUM_REGISTRY_SETTINGS (static_cast<DWORD>(-2))
#define DM_BITSPERPEL 0x00040000u
#define DM_PELSWIDTH 0x00080000u
#define DM_PELSHEIGHT 0x00100000u
#define DM_DISPLAYFLAGS 0x00200000u
#define DM_DISPLAYFREQUENCY 0x00400000u
#define DM_INTERLACED 0x00000002u
struct DEVMODEW {
    WCHAR dmDeviceName[32];
    WORD dmSpecVersion;
    WORD dmDriverVersion;
    WORD dmSize;
    WORD dmDriverExtra;
    DWORD dmFields;
    POINT dmPosition;
    DWORD dmDisplayOrientation;
    DWORD dmDisplayFixedOutput;
    short dmColor;
    short dmDuplex;
    short dmYResolution;
    short dmTTOption;
    short dmCollate;
    WCHAR dmFormName[32];
    WORD dmLogPixels;
    DWORD dmBitsPerPel;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    DWORD dmDisplayFlags;
    DWORD dmDisplayFrequency;
    DWORD dmICMMethod;
    DWORD dmICMIntent;
    DWORD dmMediaType;
    DWORD dmDitherType;
    DWORD dmReserved1;
    DWORD dmReserved2;
    DWORD dmPanningWidth;
    DWORD dmPanningHeight;
};

// ---------------------------------------------------------------------------------------------
// Functions (win32_window.mm)
// ---------------------------------------------------------------------------------------------

ATOM RegisterClassExW(const WNDCLASSEXW* window_class) noexcept;
BOOL UnregisterClassW(LPCWSTR class_name, HINSTANCE instance) noexcept;
HWND CreateWindowExW(DWORD ex_style, LPCWSTR class_name, LPCWSTR title, DWORD style, int x, int y, int width,
    int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) noexcept;
BOOL DestroyWindow(HWND window) noexcept;
BOOL ShowWindow(HWND window, int command) noexcept;
BOOL UpdateWindow(HWND window) noexcept;
BOOL SetForegroundWindow(HWND window) noexcept;
HWND SetActiveWindow(HWND window) noexcept;
HWND GetActiveWindow() noexcept;
HWND SetFocus(HWND window) noexcept;
HWND GetFocus() noexcept;
BOOL SetWindowTextW(HWND window, LPCWSTR text) noexcept;
BOOL InvalidateRect(HWND window, const RECT* rect, BOOL erase) noexcept;
LRESULT DefWindowProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
BOOL GetMessageW(LPMSG message, HWND window, UINT filter_min, UINT filter_max) noexcept;
BOOL PeekMessageW(LPMSG message, HWND window, UINT filter_min, UINT filter_max, UINT remove) noexcept;
BOOL TranslateMessage(const MSG* message) noexcept;
LRESULT DispatchMessageW(const MSG* message) noexcept;
BOOL PostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
BOOL PostThreadMessageW(DWORD thread_id, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
LRESULT SendMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
LRESULT SendMessageTimeoutW(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT flags, UINT timeout_ms,
    PDWORD_PTR result) noexcept;
void PostQuitMessage(int exit_code) noexcept;
LONG GetMessageTime() noexcept;
BOOL WaitMessage() noexcept;
DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD milliseconds, DWORD wake_mask) noexcept;
#define QS_ALLINPUT 0x04FFu
#define QS_ALLEVENTS 0x04BFu

LONG_PTR GetWindowLongPtrW(HWND window, int index) noexcept;
LONG_PTR SetWindowLongPtrW(HWND window, int index, LONG_PTR value) noexcept;
BOOL SetWindowPos(HWND window, HWND insert_after, int x, int y, int width, int height, UINT flags) noexcept;
BOOL AdjustWindowRectEx(LPRECT rect, DWORD style, BOOL menu, DWORD ex_style) noexcept;
BOOL AdjustWindowRectExForDpi(LPRECT rect, DWORD style, BOOL menu, DWORD ex_style, UINT dpi) noexcept;

HMONITOR MonitorFromWindow(HWND window, DWORD flags) noexcept;
HMONITOR MonitorFromPoint(POINT point, DWORD flags) noexcept;
HMONITOR MonitorFromRect(const RECT* rect, DWORD flags) noexcept;
BOOL GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info) noexcept;
BOOL EnumDisplayMonitors(HDC dc, const RECT* clip, MONITORENUMPROC callback, LPARAM user) noexcept;
BOOL EnumDisplaySettingsW(LPCWSTR device, DWORD mode_number, DEVMODEW* mode) noexcept;
BOOL EnumDisplaySettingsExW(LPCWSTR device, DWORD mode_number, DEVMODEW* mode, DWORD flags) noexcept;
BOOL ShowWindowAsync(HWND window, int command) noexcept;
int GetSystemMetrics(int index) noexcept;

HCURSOR LoadCursorA(HINSTANCE instance, LPCSTR name) noexcept;
HCURSOR LoadCursorW(HINSTANCE instance, const char* name) noexcept;
HANDLE LoadImageW(HINSTANCE instance, LPCWSTR name, UINT type, int width, int height, UINT flags) noexcept;
HCURSOR SetCursor(HCURSOR cursor) noexcept;
int ShowCursor(BOOL show) noexcept;
BOOL ClipCursor(const RECT* rect) noexcept;
BOOL SetCursorPos(int x, int y) noexcept;
HWND SetCapture(HWND window) noexcept;
BOOL ReleaseCapture() noexcept;
HWND GetCapture() noexcept;
BOOL TrackMouseEvent(TRACKMOUSEEVENT* event) noexcept;
BOOL GetCurrentInputMessageSource(INPUT_MESSAGE_SOURCE* source) noexcept;
UINT MapVirtualKeyA(UINT code, UINT map_type) noexcept;
UINT MapVirtualKeyW(UINT code, UINT map_type) noexcept;
HGDIOBJ GetStockObject(int object) noexcept;
int FillRect(HDC dc, const RECT* rect, HBRUSH brush) noexcept;

HWND GetForegroundWindow() noexcept;
BOOL IsWindow(HWND window) noexcept;
BOOL IsWindowVisible(HWND window) noexcept;
BOOL IsIconic(HWND window) noexcept;
DWORD GetWindowThreadProcessId(HWND window, LPDWORD process_id) noexcept;
BOOL GetCursorPos(LPPOINT point) noexcept;
HWND WindowFromPoint(POINT point) noexcept;
HWND GetAncestor(HWND window, UINT flags) noexcept;
BOOL EnumWindows(WNDENUMPROC callback, LPARAM user) noexcept;
BOOL GetClientRect(HWND window, LPRECT rect) noexcept;
BOOL GetWindowRect(HWND window, LPRECT rect) noexcept;
BOOL ScreenToClient(HWND window, LPPOINT point) noexcept;
BOOL ClientToScreen(HWND window, LPPOINT point) noexcept;
int GetClassNameW(HWND window, LPWSTR buffer, int size) noexcept;
SHORT GetAsyncKeyState(int virtual_key) noexcept;
SHORT GetKeyState(int virtual_key) noexcept;

// ---------------------------------------------------------------------------------------------
// Power requests (keep the display awake while playing): IOKit power assertions.
// ---------------------------------------------------------------------------------------------

enum POWER_REQUEST_TYPE {
    PowerRequestDisplayRequired = 0,
    PowerRequestSystemRequired = 1,
    PowerRequestAwayModeRequired = 2,
    PowerRequestExecutionRequired = 3,
};
#define POWER_REQUEST_CONTEXT_VERSION 0u
#define POWER_REQUEST_CONTEXT_SIMPLE_STRING 0x1u
#define POWER_REQUEST_CONTEXT_DETAILED_STRING 0x2u
struct REASON_CONTEXT {
    ULONG Version;
    DWORD Flags;
    union {
        struct {
            HMODULE LocalizedReasonModule;
            ULONG LocalizedReasonId;
            ULONG ReasonStringCount;
            LPWSTR* ReasonStrings;
        } Detailed;
        LPWSTR SimpleReasonString;
    } Reason;
};
using PREASON_CONTEXT = REASON_CONTEXT*;
HANDLE PowerCreateRequest(PREASON_CONTEXT context) noexcept;
BOOL PowerSetRequest(HANDLE request, POWER_REQUEST_TYPE type) noexcept;
BOOL PowerClearRequest(HANDLE request, POWER_REQUEST_TYPE type) noexcept;

#include <map>
#include <string>

namespace galaxy::macos {

// Display name of each monitor by its GDI-style device name (\\.\DISPLAYn).
[[nodiscard]] std::map<std::wstring, std::wstring> monitor_names();

// The CAMetalLayer of a window's content view (a CAMetalLayer*), for the DXGI swap chain.
[[nodiscard]] void* window_metal_layer(HWND window) noexcept;
// Client size in backing pixels and the backing scale, read from any thread.
[[nodiscard]] bool window_client_pixels(HWND window, int& width, int& height, double& scale) noexcept;
// The first visible top-level window (the game window), or null.
[[nodiscard]] HWND game_window_handle() noexcept;
// Runs the Cocoa application on the calling (main) thread until `stop_application` is called.
void run_application();
void stop_application() noexcept;

}  // namespace galaxy::macos
