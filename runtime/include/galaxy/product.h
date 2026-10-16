#pragma once

namespace galaxy {

// The name the runtime shows (window and menus) and the folder its per-user data lives in, under
// the local application data folder: config.ini, runtime_settings.ini, the shader cache and
// Load/Textures. The macOS port ships as Orbit; other builds keep Nebula.
#if defined(__APPLE__)
inline constexpr wchar_t kProductName[] = L"Orbit";
#else
inline constexpr wchar_t kProductName[] = L"Nebula";
#endif
inline constexpr const wchar_t* kAppDataFolder = kProductName;

}  // namespace galaxy
