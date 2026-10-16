// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Shared internals of the macOS Win32 emulation.

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace galaxy::compat {

enum class ObjectKind : std::uint32_t {
    Event,
    Mutex,
    Timer,
    Semaphore,
    File,
    FileMapping,
    StreamSearch,
    FileSearch,
};

// Every HANDLE the emulation returns points at a KernelObject. CloseHandle deletes it.
struct KernelObject {
    static constexpr std::uint64_t kMagic = 0x4E45425548444C45ull;  // "NEBUHDLE"
    explicit KernelObject(ObjectKind object_kind) noexcept : kind(object_kind) {}
    virtual ~KernelObject() = default;
    // Releases the object's host resources; false reports a failed close.
    virtual bool close() noexcept { return true; }
    std::uint64_t magic = kMagic;
    ObjectKind kind;
};

inline const HANDLE kPseudoProcessHandle = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1));
inline const HANDLE kPseudoThreadHandle = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-2));
inline bool is_pseudo_handle(HANDLE handle) noexcept {
    return handle == kPseudoProcessHandle || handle == kPseudoThreadHandle;
}

[[nodiscard]] KernelObject* kernel_object(HANDLE handle) noexcept;
void notify_wait_state_changed() noexcept;

[[nodiscard]] DWORD win32_error_from_errno(int error) noexcept;
void set_last_error_from_errno(int error) noexcept;
// Records ERROR_CALL_NOT_IMPLEMENTED and reports the function once on stderr.
void unsupported(const char* function) noexcept;

[[nodiscard]] std::string utf8_from_wide(std::wstring_view text);
[[nodiscard]] std::wstring wide_from_utf8(std::string_view text);
[[nodiscard]] bool wide_from_utf8(std::string_view text, std::wstring& result, bool strict);
[[nodiscard]] std::string path_from_wide(LPCWSTR path);

// False when opening `path` with this access and share mode conflicts with a handle this process
// already holds on the file (Windows share-mode rules; see win32_files.cpp).
[[nodiscard]] bool share_allows_open(const std::string& path, DWORD access, DWORD share);

}  // namespace galaxy::compat
