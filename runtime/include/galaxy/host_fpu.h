// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Host floating-point control state: the mode the cooperative scheduler hands to new fibers, and
// the Gekko's non-IEEE mode (denormal inputs and results flushed to zero) that fast-float modules
// run in. x86-64 keeps it in MXCSR (FTZ and DAZ), AArch64 in FPCR (FZ covers both).

#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace galaxy::host_fpu {

using Control = std::uint64_t;

inline Control read() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    return _mm_getcsr();
#elif defined(__aarch64__)
    std::uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    return fpcr;
#else
#error "unsupported host"
#endif
}

inline void write(Control control) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_setcsr(static_cast<unsigned int>(control));
#elif defined(__aarch64__)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(control));
#endif
}

inline void enable_flush_to_zero() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    write(read() | 0x8040u);  // FTZ | DAZ
#elif defined(__aarch64__)
    write(read() | (Control{1} << 24u));  // FZ
#endif
}

}  // namespace galaxy::host_fpu
