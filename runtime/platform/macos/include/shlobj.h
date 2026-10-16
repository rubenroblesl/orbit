// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Known folders. CSIDL_LOCAL_APPDATA is ~/Library/Application Support, where macOS keeps
// per-user application data (settings, saves and caches live in an "Orbit" folder under it (galaxy/product.h)).

#include <windows.h>

#define CSIDL_APPDATA 0x001A
#define CSIDL_LOCAL_APPDATA 0x001C
#define CSIDL_FLAG_CREATE 0x8000
#define SHGFP_TYPE_CURRENT 0
#define KF_FLAG_CREATE 0x00008000u
#define KF_FLAG_DEFAULT 0x00000000u

using KNOWNFOLDERID = GUID;
using REFKNOWNFOLDERID = const KNOWNFOLDERID&;
inline constexpr KNOWNFOLDERID FOLDERID_LocalAppData{
    0xF1B32785u, 0x6FBAu, 0x4FCFu, {0x9Du, 0x55u, 0x7Bu, 0x8Eu, 0x7Fu, 0x15u, 0x70u, 0x91u}};
inline constexpr KNOWNFOLDERID FOLDERID_RoamingAppData{
    0x3EB685DBu, 0x65F9u, 0x4CF6u, {0xA0u, 0x3Au, 0xE3u, 0xEFu, 0x65u, 0x72u, 0x9Fu, 0x3Du}};

HRESULT SHGetFolderPathW(HWND owner, int folder, HANDLE token, DWORD flags, LPWSTR path) noexcept;
HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID folder, DWORD flags, HANDLE token, PWSTR* path) noexcept;
