// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// macOS platform helpers the runtime calls where the Windows build reads PE structures or owns
// Win32 objects directly. Included by the compat windows.h.

#include <cstddef>

namespace galaxy::host {

// The loaded image's __TEXT segment (translated code) for a module handle from LoadLibraryW.
[[nodiscard]] bool macos_module_text_range(void* module, const unsigned char*& base, std::size_t& size) noexcept;

}  // namespace galaxy::host
