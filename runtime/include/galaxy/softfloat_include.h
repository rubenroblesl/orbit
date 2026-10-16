// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Berkeley SoftFloat for C++ translation units. On AArch64, <arm_neon.h> (pulled in by the
// native fast paths) declares float16_t/float32_t/float64_t as host floating types, while
// SoftFloat declares structs of the same names. In C++ the SoftFloat structs are renamed for
// the rest of the including file; the library's extern "C" symbols and layouts are unchanged.

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define float16_t galaxy_softfloat_float16_t
#define float32_t galaxy_softfloat_float32_t
#define float64_t galaxy_softfloat_float64_t
#endif

extern "C" {
#include "platform.h"
#include "softfloat.h"
}
