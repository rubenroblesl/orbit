#pragma once

namespace galaxy::gx {

// Posted to the game window when the operating system, not the runtime, changed its display
// mode: on macOS the window entered or left full screen from its title bar or the Window menu.
// wParam != 0: full screen (the runtime's borderless mode); 0: windowed. The renderer's window
// procedure makes the settings follow. WM_USER + 0x45, next to the renderer's own requests.
inline constexpr unsigned kSystemDisplayModeMessage = 0x0400u + 0x45u;

}  // namespace galaxy::gx
