// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Direct3D 12 on macOS: the interface, structure and enumeration definitions are Microsoft's
// DirectX-Headers (MIT); the implementation is the Metal translation layer in
// runtime/platform/macos/src/d3d12. Interface IDs come from DirectX-Headers' dxguids.h.

#include <windows.h>

#include_next <d3d12.h>

#include <dxguids.h>
