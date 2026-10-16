// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// MSVC intrinsics the runtime calls, on AArch64 clang. __cpuid reports no x86 leaves, so every
// x86-specific feature probe (invariant TSC, AVX2) declines and the portable path is taken.

#include <cstdint>

#include "galaxy/platform_compat.h"

#include <sse2neon.h>

inline void __cpuid(int registers[4], int) noexcept {
    registers[0] = registers[1] = registers[2] = registers[3] = 0;
}

inline void __cpuidex(int registers[4], int, int) noexcept {
    registers[0] = registers[1] = registers[2] = registers[3] = 0;
}

inline std::uint64_t _umul128(std::uint64_t a, std::uint64_t b, std::uint64_t* high) noexcept {
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    *high = static_cast<std::uint64_t>(product >> 64u);
    return static_cast<std::uint64_t>(product);
}

// Divides the 128-bit value high:low; the quotient must fit in 64 bits, as on x64.
inline std::uint64_t _udiv128(
    std::uint64_t high, std::uint64_t low, std::uint64_t divisor, std::uint64_t* remainder) noexcept {
    const unsigned __int128 dividend = (static_cast<unsigned __int128>(high) << 64u) | low;
    *remainder = static_cast<std::uint64_t>(dividend % divisor);
    return static_cast<std::uint64_t>(dividend / divisor);
}

inline unsigned char _BitScanReverse(unsigned long* index, unsigned long mask) noexcept {
    if (static_cast<std::uint32_t>(mask) == 0u) return 0u;
    *index = 31u - static_cast<unsigned long>(__builtin_clz(static_cast<std::uint32_t>(mask)));
    return 1u;
}

inline unsigned char _BitScanForward(unsigned long* index, unsigned long mask) noexcept {
    if (static_cast<std::uint32_t>(mask) == 0u) return 0u;
    *index = static_cast<unsigned long>(__builtin_ctz(static_cast<std::uint32_t>(mask)));
    return 1u;
}

inline unsigned char _BitScanReverse64(unsigned long* index, std::uint64_t mask) noexcept {
    if (mask == 0u) return 0u;
    *index = 63u - static_cast<unsigned long>(__builtin_clzll(mask));
    return 1u;
}

inline unsigned char _BitScanForward64(unsigned long* index, std::uint64_t mask) noexcept {
    if (mask == 0u) return 0u;
    *index = static_cast<unsigned long>(__builtin_ctzll(mask));
    return 1u;
}

inline unsigned short _byteswap_ushort(unsigned short value) noexcept { return __builtin_bswap16(value); }
inline std::uint32_t _byteswap_ulong(std::uint32_t value) noexcept { return __builtin_bswap32(value); }
inline std::uint64_t _byteswap_uint64(std::uint64_t value) noexcept { return __builtin_bswap64(value); }
inline unsigned int __popcnt(unsigned int value) noexcept { return static_cast<unsigned int>(__builtin_popcount(value)); }
inline std::uint64_t __popcnt64(std::uint64_t value) noexcept { return static_cast<std::uint64_t>(__builtin_popcountll(value)); }
inline unsigned int _rotl(unsigned int value, int shift) noexcept { return (value << (shift & 31)) | (value >> ((32 - shift) & 31)); }
inline unsigned int _rotr(unsigned int value, int shift) noexcept { return (value >> (shift & 31)) | (value << ((32 - shift) & 31)); }
inline void __debugbreak() noexcept { __builtin_debugtrap(); }
