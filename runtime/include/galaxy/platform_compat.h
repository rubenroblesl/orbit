// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// POSIX (macOS) equivalents of the Microsoft CRT and compiler intrinsics the runtime and the
// generated modules call. On Windows this header is empty: every call site keeps the MSVC CRT.
// Semantics follow the Microsoft documentation of each function, including the return codes
// the call sites test.

#if !defined(_WIN32)

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <arm_acle.h>
#endif

using errno_t = int;

// getenv_s: `*required` receives the value's size including the terminator (0 when unset).
// A null buffer with size 0 only queries the size. A buffer that is too small yields ERANGE.
inline errno_t getenv_s(
    std::size_t* required, char* buffer, std::size_t size, const char* name) noexcept {
    if (required == nullptr || name == nullptr || (buffer == nullptr && size != 0)) {
        return EINVAL;
    }
    const char* value = std::getenv(name);
    if (value == nullptr) {
        *required = 0;
        if (buffer != nullptr && size != 0) {
            buffer[0] = '\0';
        }
        return 0;
    }
    const std::size_t needed = std::strlen(value) + 1u;
    *required = needed;
    if (buffer == nullptr) {
        return 0;
    }
    if (needed > size) {
        buffer[0] = '\0';
        return ERANGE;
    }
    std::memcpy(buffer, value, needed);
    return 0;
}

// _dupenv_s: a malloc'd copy the caller frees, or null with length 0 when unset.
inline errno_t _dupenv_s(char** buffer, std::size_t* length, const char* name) noexcept {
    if (buffer == nullptr || name == nullptr) {
        return EINVAL;
    }
    *buffer = nullptr;
    if (length != nullptr) {
        *length = 0;
    }
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return 0;
    }
    const std::size_t needed = std::strlen(value) + 1u;
    char* copy = static_cast<char*>(std::malloc(needed));
    if (copy == nullptr) {
        return ENOMEM;
    }
    std::memcpy(copy, value, needed);
    *buffer = copy;
    if (length != nullptr) {
        *length = needed;
    }
    return 0;
}

// _putenv_s: an empty value removes the variable, as on Windows.
inline errno_t _putenv_s(const char* name, const char* value) noexcept {
    if (name == nullptr || value == nullptr) {
        return EINVAL;
    }
    const int result = value[0] == '\0' ? unsetenv(name) : setenv(name, value, 1);
    return result == 0 ? 0 : errno;
}

// The runtime's sscanf_s calls parse numbers only, which need no buffer-size arguments.
#define sscanf_s sscanf

inline errno_t fopen_s(std::FILE** file, const char* path, const char* mode) noexcept {
    if (file == nullptr || path == nullptr || mode == nullptr) {
        return EINVAL;
    }
    *file = std::fopen(path, mode);
    return *file != nullptr ? 0 : errno;
}

inline int _fseeki64(std::FILE* file, std::int64_t offset, int origin) noexcept {
    return fseeko(file, static_cast<off_t>(offset), origin);
}

inline std::int64_t _ftelli64(std::FILE* file) noexcept {
    return static_cast<std::int64_t>(ftello(file));
}

inline int _access(const char* path, int mode) noexcept {
    return access(path, mode);
}

inline int _getpid() noexcept {
    return static_cast<int>(getpid());
}

inline int _stricmp(const char* left, const char* right) noexcept {
    return strcasecmp(left, right);
}

inline int _strnicmp(const char* left, const char* right, std::size_t count) noexcept {
    return strncasecmp(left, right, count);
}

// strncpy_s with _TRUNCATE (the only count the runtime passes) or an exact count.
#ifndef _TRUNCATE
#define _TRUNCATE (static_cast<std::size_t>(-1))
#endif
inline errno_t strncpy_s(
    char* destination, std::size_t size, const char* source, std::size_t count) noexcept {
    if (destination == nullptr || size == 0 || source == nullptr) {
        return EINVAL;
    }
    const std::size_t length = strnlen(source, count == _TRUNCATE ? size : count);
    if (length >= size) {
        if (count != _TRUNCATE) {
            destination[0] = '\0';
            return ERANGE;
        }
        std::memcpy(destination, source, size - 1u);
        destination[size - 1u] = '\0';
        return 80;  // STRUNCATE
    }
    std::memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

// Cycle counter for profiling and short calibrated spins. AArch64 reads the virtual counter
// (24 MHz on Apple silicon); callers only compare or scale differences against a calibration.
inline std::uint64_t __rdtsc() noexcept {
#if defined(__aarch64__)
    std::uint64_t value;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(value) : : "memory");
    return value;
#else
    return __builtin_readcyclecounter();
#endif
}

// _mm_pause comes from <intrin.h>/<immintrin.h> (sse2neon on AArch64).
inline void YieldProcessor() noexcept {
#if defined(__aarch64__)
    __yield();
#endif
}

#ifndef _ReadWriteBarrier
#define _ReadWriteBarrier() __asm__ __volatile__("" : : : "memory")
#endif

#endif  // !defined(_WIN32)
