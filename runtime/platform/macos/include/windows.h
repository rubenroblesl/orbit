// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The Win32 subset the Nebula runtime uses, implemented on macOS (runtime/platform/macos/src).
//
// This directory is on the include path of the macOS build only. Kernel objects (files, events,
// mutexes, waitable timers, modules) are emulated over POSIX and Mach; window queries refer to the
// one game window that the Cocoa layer owns. Functions report failure through GetLastError with
// the Win32 error codes the call sites test. Anything the emulation cannot honour fails loudly
// instead of pretending to succeed.
//
// Paths: the W functions accept wide strings (UTF-32 on macOS) and, as overloads, the UTF-8
// `const char*` that std::filesystem::path::c_str() yields on this platform.
//
// NTFS alternate data streams ("file:name") are stored as extended attributes of the file.

#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cwchar>
#include <pthread.h>
#include <type_traits>

#include "galaxy/platform_compat.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

// ---------------------------------------------------------------------------------------------
// Base types
// ---------------------------------------------------------------------------------------------

#define WINAPI
#define APIENTRY
#define CALLBACK
#define __stdcall
#define __cdecl
#define FAR
#define NEAR
#define CONST const
#define VOID void

// Objective-C (arm64) declares BOOL as bool; Win32 BOOL is used here only as a truth value, so
// the two declarations are made identical and Cocoa/Metal code can include this header.
using BOOL = bool;
using BOOLEAN = std::uint8_t;
using BYTE = std::uint8_t;
using UCHAR = std::uint8_t;
using CHAR = char;
using WCHAR = wchar_t;
using SHORT = std::int16_t;
using USHORT = std::uint16_t;
using WORD = std::uint16_t;
using INT = int;
using UINT = unsigned int;
using LONG = std::int32_t;
using ULONG = std::uint32_t;
using DWORD = std::uint32_t;
using LONGLONG = std::int64_t;
using ULONGLONG = std::uint64_t;
using DWORD64 = std::uint64_t;
using ULONG64 = std::uint64_t;
using LONG64 = std::int64_t;
using INT8 = std::int8_t;
using UINT8 = std::uint8_t;
using INT16 = std::int16_t;
using UINT16 = std::uint16_t;
using INT32 = std::int32_t;
using UINT32 = std::uint32_t;
using INT64 = std::int64_t;
using UINT64 = std::uint64_t;
using __int64 = std::int64_t;
using SIZE_T = std::size_t;
using SSIZE_T = std::ptrdiff_t;
using ULONG_PTR = std::uintptr_t;
using LONG_PTR = std::intptr_t;
using UINT_PTR = std::uintptr_t;
using INT_PTR = std::intptr_t;
using DWORD_PTR = std::uintptr_t;
using DWORDLONG = std::uint64_t;
using HRESULT = std::int32_t;
using FLOAT = float;
using DOUBLE = double;
using RPC_IF_HANDLE = void*;
using NTSTATUS = std::int32_t;
using LSTATUS = std::int32_t;
using ATOM = WORD;
using PVOID = void*;
using LPVOID = void*;
using LPCVOID = const void*;
using PBYTE = BYTE*;
using LPBYTE = BYTE*;
using PUCHAR = UCHAR*;
using PDWORD = DWORD*;
using PDWORD_PTR = DWORD_PTR*;
using PULONG64 = ULONG64*;
using LPDWORD = DWORD*;
using PULONG = ULONG*;
using PLONG = LONG*;
using LPLONG = LONG*;
using LPSTR = char*;
using LPCSTR = const char*;
using PSTR = char*;
using PCSTR = const char*;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using PWSTR = wchar_t*;
using PCWSTR = const wchar_t*;
using PWCHAR = wchar_t*;

using HANDLE = void*;
using PHANDLE = HANDLE*;
using LPHANDLE = HANDLE*;
using HMODULE = void*;
using HINSTANCE = void*;
using FARPROC = void (*)();
using HKEY = struct HKEY__*;
using HICON = struct HICON__*;
using HCURSOR = HICON;
using HBRUSH = struct HBRUSH__*;
using HMONITOR = struct HMONITOR__*;
using HDC = struct HDC__*;
using HMENU = struct HMENU__*;
struct HWND__;
using HWND = HWND__*;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1)))
#define INVALID_FILE_ATTRIBUTES (static_cast<DWORD>(0xFFFFFFFFu))
#define INVALID_FILE_SIZE (static_cast<DWORD>(0xFFFFFFFFu))

#define LOWORD(l) (static_cast<WORD>(static_cast<DWORD_PTR>(l) & 0xFFFFu))
#define HIWORD(l) (static_cast<WORD>((static_cast<DWORD_PTR>(l) >> 16u) & 0xFFFFu))
#define LOBYTE(w) (static_cast<BYTE>(static_cast<DWORD_PTR>(w) & 0xFFu))
#define HIBYTE(w) (static_cast<BYTE>((static_cast<DWORD_PTR>(w) >> 8u) & 0xFFu))
#define MAKELONG(a, b) (static_cast<LONG>((static_cast<WORD>(a)) | (static_cast<DWORD>(static_cast<WORD>(b)) << 16u)))
#define MAKEINTRESOURCEW(i) (reinterpret_cast<LPWSTR>(static_cast<ULONG_PTR>(static_cast<WORD>(i))))
#define GET_X_LPARAM(lp) (static_cast<int>(static_cast<short>(LOWORD(lp))))
#define GET_Y_LPARAM(lp) (static_cast<int>(static_cast<short>(HIWORD(lp))))

union LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
};
using PLARGE_INTEGER = LARGE_INTEGER*;

union ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    struct {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    ULONGLONG QuadPart;
};

struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};
using LPFILETIME = FILETIME*;

struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};

struct GUID {
    std::uint32_t Data1;
    std::uint16_t Data2;
    std::uint16_t Data3;
    std::uint8_t Data4[8];
};
using IID = GUID;
using UUID = GUID;
using LPCGUID = const GUID*;
using CLSID = GUID;
using REFIID = const IID&;
using REFCLSID = const CLSID&;
using REFGUID = const GUID&;
inline bool operator==(const GUID& a, const GUID& b) noexcept {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           __builtin_memcmp(a.Data4, b.Data4, 8) == 0;
}

struct LUID {
    DWORD LowPart;
    LONG HighPart;
};

struct POINT {
    LONG x;
    LONG y;
};
using LPPOINT = POINT*;

struct RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};
using LPRECT = RECT*;

struct SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
};
using LPSECURITY_ATTRIBUTES = SECURITY_ATTRIBUTES*;

struct OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
};
using LPOVERLAPPED = OVERLAPPED*;

// ---------------------------------------------------------------------------------------------
// HRESULT and error codes
// ---------------------------------------------------------------------------------------------

#define S_OK (static_cast<HRESULT>(0))
#define S_FALSE (static_cast<HRESULT>(1))
#define NOERROR 0
#define E_NOTIMPL (static_cast<HRESULT>(0x80004001u))
#define E_NOINTERFACE (static_cast<HRESULT>(0x80004002u))
#define E_POINTER (static_cast<HRESULT>(0x80004003u))
#define E_ABORT (static_cast<HRESULT>(0x80004004u))
#define E_FAIL (static_cast<HRESULT>(0x80004005u))
#define E_UNEXPECTED (static_cast<HRESULT>(0x8000FFFFu))
#define E_ACCESSDENIED (static_cast<HRESULT>(0x80070005u))
#define E_HANDLE (static_cast<HRESULT>(0x80070006u))
#define E_OUTOFMEMORY (static_cast<HRESULT>(0x8007000Eu))
#define E_INVALIDARG (static_cast<HRESULT>(0x80070057u))
#define RPC_E_CHANGED_MODE (static_cast<HRESULT>(0x80010106u))
#define SUCCEEDED(hr) ((static_cast<HRESULT>(hr)) >= 0)
#define FAILED(hr) ((static_cast<HRESULT>(hr)) < 0)
#define HRESULT_FROM_WIN32(x) \
    (static_cast<HRESULT>(x) <= 0 ? static_cast<HRESULT>(x) \
                                  : static_cast<HRESULT>((static_cast<DWORD>(x) & 0x0000FFFFu) | 0x80070000u))

#define ERROR_SUCCESS 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_INVALID_DATA 13L
#define ERROR_OUTOFMEMORY 14L
#define ERROR_NOT_READY 21L
#define ERROR_CRC 23L
#define ERROR_WRITE_FAULT 29L
#define ERROR_READ_FAULT 30L
#define ERROR_GEN_FAILURE 31L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_LOCK_VIOLATION 33L
#define ERROR_HANDLE_EOF 38L
#define ERROR_HANDLE_DISK_FULL 39L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_TOO_MANY_POSTS 298L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_DISK_FULL 112L
#define ERROR_CALL_NOT_IMPLEMENTED 120L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_INVALID_NAME 123L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_BAD_PATHNAME 161L
#define ERROR_BUSY 170L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_FILENAME_EXCED_RANGE 206L
#define ERROR_ENVVAR_NOT_FOUND 203L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_MORE_ITEMS 259L
#define ERROR_DIRECTORY 267L
#define ERROR_OPERATION_ABORTED 995L
#define ERROR_IO_INCOMPLETE 996L
#define ERROR_IO_PENDING 997L
#define ERROR_NOACCESS 998L
#define ERROR_FILE_INVALID 1006L
#define ERROR_DEVICE_NOT_CONNECTED 1167L
#define ERROR_NOT_FOUND 1168L
#define ERROR_PRIVILEGE_NOT_HELD 1314L
#define ERROR_TIMEOUT 1460L
#define ERROR_INVALID_STATE 5023L
#define ERROR_CANT_ACCESS_FILE 1920L

DWORD GetLastError() noexcept;
void SetLastError(DWORD error) noexcept;

// ---------------------------------------------------------------------------------------------
// Handles, waits and synchronization objects
// ---------------------------------------------------------------------------------------------

#define WAIT_OBJECT_0 0x00000000u
#define WAIT_ABANDONED 0x00000080u
#define WAIT_ABANDONED_0 0x00000080u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define MAXIMUM_WAIT_OBJECTS 64u

#define SYNCHRONIZE 0x00100000u
#define EVENT_MODIFY_STATE 0x0002u
#define EVENT_ALL_ACCESS 0x1F0003u
#define MUTEX_ALL_ACCESS 0x1F0001u
#define MUTEX_MODIFY_STATE 0x0001u
#define TIMER_MODIFY_STATE 0x0002u
#define TIMER_ALL_ACCESS 0x1F0003u
#define CREATE_EVENT_MANUAL_RESET 0x00000001u
#define CREATE_EVENT_INITIAL_SET 0x00000002u
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001u
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002u
#define HANDLE_FLAG_INHERIT 0x00000001u

BOOL CloseHandle(HANDLE handle) noexcept;
BOOL GetHandleInformation(HANDLE handle, LPDWORD flags) noexcept;
BOOL DuplicateHandle(HANDLE source_process, HANDLE source, HANDLE target_process, LPHANDLE target,
    DWORD access, BOOL inherit, DWORD options) noexcept;
#define DUPLICATE_SAME_ACCESS 0x00000002u

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial_state,
    LPCWSTR name) noexcept;
HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags,
    DWORD access) noexcept;
BOOL SetEvent(HANDLE event) noexcept;
BOOL ResetEvent(HANDLE event) noexcept;
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, LPCWSTR name) noexcept;
BOOL ReleaseSemaphore(HANDLE semaphore, LONG count, LPLONG previous) noexcept;
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES attributes, BOOL initial_owner, LPCWSTR name) noexcept;
BOOL ReleaseMutex(HANDLE mutex) noexcept;
HANDLE OpenMutexW(DWORD access, BOOL inherit, LPCWSTR name) noexcept;
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags,
    DWORD access) noexcept;
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset,
    LPCWSTR name) noexcept;
using PTIMERAPCROUTINE = void (*)(LPVOID, DWORD, DWORD);
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due_time, LONG period,
    PTIMERAPCROUTINE completion, LPVOID argument, BOOL resume) noexcept;
BOOL CancelWaitableTimer(HANDLE timer) noexcept;
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) noexcept;
DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable) noexcept;
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all,
    DWORD milliseconds) noexcept;

struct SRWLOCK {
    pthread_rwlock_t lock;
};
using PSRWLOCK = SRWLOCK*;
#define SRWLOCK_INIT {PTHREAD_RWLOCK_INITIALIZER}
void InitializeSRWLock(PSRWLOCK lock) noexcept;
void AcquireSRWLockExclusive(PSRWLOCK lock) noexcept;
void ReleaseSRWLockExclusive(PSRWLOCK lock) noexcept;
void AcquireSRWLockShared(PSRWLOCK lock) noexcept;
void ReleaseSRWLockShared(PSRWLOCK lock) noexcept;
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock) noexcept;
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock) noexcept;

struct CRITICAL_SECTION {
    pthread_mutex_t mutex;
};
using LPCRITICAL_SECTION = CRITICAL_SECTION*;
void InitializeCriticalSection(LPCRITICAL_SECTION section) noexcept;
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION section, DWORD spin) noexcept;
void DeleteCriticalSection(LPCRITICAL_SECTION section) noexcept;
void EnterCriticalSection(LPCRITICAL_SECTION section) noexcept;
void LeaveCriticalSection(LPCRITICAL_SECTION section) noexcept;
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION section) noexcept;

// ---------------------------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------------------------

BOOL QueryPerformanceCounter(LARGE_INTEGER* counter) noexcept;
BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) noexcept;
ULONGLONG GetTickCount64() noexcept;
DWORD GetTickCount() noexcept;
void Sleep(DWORD milliseconds) noexcept;
DWORD SleepEx(DWORD milliseconds, BOOL alertable) noexcept;
void GetSystemTimeAsFileTime(LPFILETIME time) noexcept;
void GetSystemTimePreciseAsFileTime(LPFILETIME time) noexcept;
void GetLocalTime(SYSTEMTIME* time) noexcept;
void GetSystemTime(SYSTEMTIME* time) noexcept;
using MMRESULT = UINT;
#define TIMERR_NOERROR 0u
MMRESULT timeBeginPeriod(UINT period) noexcept;
MMRESULT timeEndPeriod(UINT period) noexcept;

// ---------------------------------------------------------------------------------------------
// Processes and threads
// ---------------------------------------------------------------------------------------------

#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_ERROR_RETURN 0x7FFFFFFF
#define IDLE_PRIORITY_CLASS 0x00000040u
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000u
#define NORMAL_PRIORITY_CLASS 0x00000020u
#define REALTIME_PRIORITY_CLASS 0x00000100u
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
#define THREAD_MODE_BACKGROUND_END 0x00020000
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000u
#define HIGH_PRIORITY_CLASS 0x00000080u
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000u
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800u
#define THREAD_SUSPEND_RESUME 0x0002u
#define THREAD_GET_CONTEXT 0x0008u
#define THREAD_QUERY_INFORMATION 0x0040u

HANDLE GetCurrentProcess() noexcept;
HANDLE GetCurrentThread() noexcept;
DWORD GetCurrentProcessId() noexcept;
DWORD GetCurrentThreadId() noexcept;
BOOL SwitchToThread() noexcept;
BOOL SetThreadPriority(HANDLE thread, int priority) noexcept;
int GetThreadPriority(HANDLE thread) noexcept;
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask) noexcept;
BOOL GetProcessAffinityMask(HANDLE process, PDWORD_PTR process_mask, PDWORD_PTR system_mask) noexcept;
BOOL SetPriorityClass(HANDLE process, DWORD priority_class) noexcept;
DWORD GetPriorityClass(HANDLE process) noexcept;
BOOL GetThreadTimes(HANDLE thread, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel,
    LPFILETIME user) noexcept;
BOOL GetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel,
    LPFILETIME user) noexcept;
BOOL QueryThreadCycleTime(HANDLE thread, PULONG64 cycles) noexcept;
HRESULT SetThreadDescription(HANDLE thread, PCWSTR description) noexcept;
HRESULT GetThreadDescription(HANDLE thread, PWSTR* description) noexcept;
[[noreturn]] void ExitProcess(UINT code) noexcept;
BOOL TerminateProcess(HANDLE process, UINT code) noexcept;
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD thread_id) noexcept;
DWORD GetActiveProcessorCount(WORD group) noexcept;
#define ALL_PROCESSOR_GROUPS 0xFFFFu

// Fibers (win32_fiber.cpp): stackful coroutines over an AArch64 context switch. The switch
// saves and restores FPCR with the callee-saved registers, so every fiber keeps its own
// floating-point mode, as FIBER_FLAG_FLOAT_SWITCH requests.
#define FIBER_FLAG_FLOAT_SWITCH 0x1u
using LPFIBER_START_ROUTINE = void (*)(LPVOID);
LPVOID ConvertThreadToFiber(LPVOID parameter) noexcept;
LPVOID ConvertThreadToFiberEx(LPVOID parameter, DWORD flags) noexcept;
BOOL ConvertFiberToThread() noexcept;
LPVOID CreateFiber(SIZE_T stack_size, LPFIBER_START_ROUTINE start, LPVOID parameter) noexcept;
LPVOID CreateFiberEx(SIZE_T commit_size, SIZE_T reserve_size, DWORD flags, LPFIBER_START_ROUTINE start,
    LPVOID parameter) noexcept;
void SwitchToFiber(LPVOID fiber) noexcept;
void DeleteFiber(LPVOID fiber) noexcept;
LPVOID GetCurrentFiber() noexcept;
LPVOID GetFiberData() noexcept;
BOOL IsThreadAFiber() noexcept;

// ---------------------------------------------------------------------------------------------
// Modules and environment
// ---------------------------------------------------------------------------------------------

#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100u
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800u
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000u
#define LOAD_LIBRARY_SEARCH_APPLICATION_DIR 0x00000200u
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008u
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004u
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002u

// A Windows module name ("RMGE01_game.dll") loads the macOS image of the same stem (".dylib").
// System DLLs the runtime probes optionally (avrt, ntdll, shell32, dwmapi) are absent.
HMODULE LoadLibraryW(LPCWSTR path) noexcept;
HMODULE LoadLibraryW(const char* path) noexcept;
HMODULE LoadLibraryA(LPCSTR path) noexcept;
HMODULE LoadLibraryExW(LPCWSTR path, HANDLE reserved, DWORD flags) noexcept;
HMODULE LoadLibraryExW(const char* path, HANDLE reserved, DWORD flags) noexcept;
BOOL FreeLibrary(HMODULE module) noexcept;
FARPROC GetProcAddress(HMODULE module, LPCSTR name) noexcept;
HMODULE GetModuleHandleW(LPCWSTR name) noexcept;
BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module) noexcept;
DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size) noexcept;
DWORD GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size) noexcept;

DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size) noexcept;
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size) noexcept;
BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) noexcept;
errno_t _wputenv_s(const wchar_t* name, const wchar_t* value) noexcept;
errno_t _wputenv_s(const wchar_t* name, const char* value) noexcept;
#define _ENABLE_PER_THREAD_LOCALE 0x1
#define _DISABLE_PER_THREAD_LOCALE 0x2
int _configthreadlocale(int mode) noexcept;
errno_t _wdupenv_s(wchar_t** buffer, std::size_t* length, const wchar_t* name) noexcept;
wchar_t* _wgetenv(const wchar_t* name) noexcept;
errno_t _wfopen_s(std::FILE** file, const wchar_t* path, const wchar_t* mode) noexcept;
errno_t _wfopen_s(std::FILE** file, const char* path, const wchar_t* mode) noexcept;
std::FILE* _wfopen(const wchar_t* path, const wchar_t* mode) noexcept;
std::FILE* _wfopen(const char* path, const wchar_t* mode) noexcept;
std::FILE* _wfsopen(const wchar_t* path, const wchar_t* mode, int share) noexcept;
std::FILE* _wfsopen(const char* path, const wchar_t* mode, int share) noexcept;
#define _SH_DENYNO 0x40
#define _SH_DENYWR 0x20
#define _SH_DENYRW 0x10
int _wcsicmp(const wchar_t* left, const wchar_t* right) noexcept;
int _wcsnicmp(const wchar_t* left, const wchar_t* right, std::size_t count) noexcept;
int lstrcmpW(LPCWSTR left, LPCWSTR right) noexcept;
int lstrcmpiW(LPCWSTR left, LPCWSTR right) noexcept;
int swprintf_s(wchar_t* buffer, std::size_t count, const wchar_t* format, ...) noexcept;
int vswprintf_s(wchar_t* buffer, std::size_t count, const wchar_t* format, va_list arguments) noexcept;
template <std::size_t N>
int swprintf_s(wchar_t (&buffer)[N], const wchar_t* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    const int result = vswprintf_s(buffer, N, format, arguments);
    va_end(arguments);
    return result;
}
template <std::size_t N>
errno_t wcsncpy_s(wchar_t (&destination)[N], const wchar_t* source, std::size_t count) noexcept {
    if (source == nullptr) return EINVAL;
    const std::size_t length = wcsnlen(source, count == _TRUNCATE ? N : count);
    const std::size_t copied = length < N ? length : N - 1u;
    std::wmemcpy(destination, source, copied);
    destination[copied] = L'\0';
    return length < N ? 0 : (count == _TRUNCATE ? 80 : ERANGE);
}
errno_t wcscpy_s(wchar_t* destination, std::size_t size, const wchar_t* source) noexcept;
inline std::size_t strnlen_s(const char* text, std::size_t size) noexcept {
    return text == nullptr ? 0u : strnlen(text, size);
}
inline std::size_t wcsnlen_s(const wchar_t* text, std::size_t size) noexcept {
    return text == nullptr ? 0u : wcsnlen(text, size);
}

// Locale-independent number parsing (the "C" locale), as _create_locale(LC_NUMERIC, "C") gives.
struct __crt_locale_pointers;
using _locale_t = __crt_locale_pointers*;
_locale_t _create_locale(int category, const char* locale) noexcept;
void _free_locale(_locale_t locale) noexcept;
float _strtof_l(const char* text, char** end, _locale_t locale) noexcept;
double _strtod_l(const char* text, char** end, _locale_t locale) noexcept;
int _snprintf_s_l(char* buffer, std::size_t size, std::size_t count, const char* format, _locale_t locale, ...) noexcept;

#define CP_ACP 0u
#define CP_UTF8 65001u
#define MB_ERR_INVALID_CHARS 0x00000008u
#define WC_ERR_INVALID_CHARS 0x00000080u
int MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int source_length,
    LPWSTR destination, int destination_length) noexcept;
int WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int source_length,
    LPSTR destination, int destination_length, LPCSTR default_char, BOOL* used_default) noexcept;

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u
#define DELETE 0x00010000u
#define READ_CONTROL 0x00020000u
#define FILE_READ_DATA 0x0001u
#define FILE_WRITE_DATA 0x0002u
#define FILE_APPEND_DATA 0x0004u
#define FILE_READ_ATTRIBUTES 0x0080u
#define FILE_WRITE_ATTRIBUTES 0x0100u
#define FILE_SHARE_READ 0x00000001u
#define FILE_SHARE_WRITE 0x00000002u
#define FILE_SHARE_DELETE 0x00000004u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u

#define FILE_ATTRIBUTE_READONLY 0x00000001u
#define FILE_ATTRIBUTE_HIDDEN 0x00000002u
#define FILE_ATTRIBUTE_SYSTEM 0x00000004u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020u
#define FILE_ATTRIBUTE_DEVICE 0x00000040u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100u
#define FILE_ATTRIBUTE_SPARSE_FILE 0x00000200u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400u
#define FILE_ATTRIBUTE_COMPRESSED 0x00000800u
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define FILE_FLAG_NO_BUFFERING 0x20000000u
#define FILE_FLAG_RANDOM_ACCESS 0x10000000u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u

#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u

#define MOVEFILE_REPLACE_EXISTING 0x00000001u
#define MOVEFILE_COPY_ALLOWED 0x00000002u
#define MOVEFILE_WRITE_THROUGH 0x00000008u

#define FILE_CASE_SENSITIVE_SEARCH 0x00000001u
#define FILE_CASE_PRESERVED_NAMES 0x00000002u
#define FILE_PERSISTENT_ACLS 0x00000008u
#define FILE_SUPPORTS_SPARSE_FILES 0x00000040u
#define FILE_SUPPORTS_REPARSE_POINTS 0x00000080u
#define FILE_NAMED_STREAMS 0x00040000u
#define FILE_SUPPORTS_HARD_LINKS 0x00400000u

struct BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
};
using LPBY_HANDLE_FILE_INFORMATION = BY_HANDLE_FILE_INFORMATION*;

enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel };
struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
};

enum FILE_INFO_BY_HANDLE_CLASS {
    FileBasicInfo = 0,
    FileStandardInfo = 1,
    FileNameInfo = 2,
    FileRenameInfo = 3,
    FileDispositionInfo = 4,
    FileAllocationInfo = 5,
    FileEndOfFileInfo = 6,
    FileStreamInfo = 7,
    FileIdInfo = 18,
    FileDispositionInfoEx = 21,
    FileCaseSensitiveInfo = 23,
};
struct FILE_DISPOSITION_INFO {
    BOOLEAN DeleteFile;
};
struct FILE_STANDARD_INFO {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    DWORD NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
};
struct FILE_END_OF_FILE_INFO {
    LARGE_INTEGER EndOfFile;
};
#define FILE_CS_FLAG_CASE_SENSITIVE_DIR 0x00000001u
struct FILE_CASE_SENSITIVE_INFO {
    ULONG Flags;
};

enum STREAM_INFO_LEVELS { FindStreamInfoStandard, FindStreamInfoMaxInfoLevel };
struct WIN32_FIND_STREAM_DATA {
    LARGE_INTEGER StreamSize;
    WCHAR cStreamName[MAX_PATH + 36];
};

struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
};

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
    DWORD disposition, DWORD flags, HANDLE template_file) noexcept;
HANDLE CreateFileW(const char* path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
    DWORD disposition, DWORD flags, HANDLE template_file) noexcept;
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
    DWORD disposition, DWORD flags, HANDLE template_file) noexcept;
BOOL ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped) noexcept;
BOOL WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written,
    LPOVERLAPPED overlapped) noexcept;
BOOL GetOverlappedResult(HANDLE file, LPOVERLAPPED overlapped, LPDWORD transferred,
    BOOL wait) noexcept;
BOOL CancelIoEx(HANDLE file, LPOVERLAPPED overlapped) noexcept;
BOOL CancelIo(HANDLE file) noexcept;
BOOL FlushFileBuffers(HANDLE file) noexcept;
BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position,
    DWORD method) noexcept;
BOOL SetEndOfFile(HANDLE file) noexcept;
BOOL GetFileSizeEx(HANDLE file, PLARGE_INTEGER size) noexcept;
DWORD GetFileType(HANDLE file) noexcept;
#define FILE_TYPE_UNKNOWN 0x0000u
#define FILE_TYPE_DISK 0x0001u
#define FILE_TYPE_CHAR 0x0002u
#define FILE_TYPE_PIPE 0x0003u
BOOL GetFileInformationByHandle(HANDLE file, LPBY_HANDLE_FILE_INFORMATION information) noexcept;
BOOL GetFileInformationByHandleEx(HANDLE file, FILE_INFO_BY_HANDLE_CLASS information_class,
    LPVOID information, DWORD size) noexcept;
BOOL SetFileInformationByHandle(HANDLE file, FILE_INFO_BY_HANDLE_CLASS information_class,
    LPVOID information, DWORD size) noexcept;
DWORD GetFinalPathNameByHandleW(HANDLE file, LPWSTR buffer, DWORD size, DWORD flags) noexcept;
BOOL SetFileTime(HANDLE file, const FILETIME* creation, const FILETIME* access, const FILETIME* write) noexcept;
BOOL GetFileTime(HANDLE file, LPFILETIME creation, LPFILETIME access, LPFILETIME write) noexcept;

BOOL DeleteFileW(LPCWSTR path) noexcept;
BOOL DeleteFileW(const char* path) noexcept;
BOOL RemoveDirectoryW(LPCWSTR path) noexcept;
BOOL RemoveDirectoryW(const char* path) noexcept;
BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES attributes) noexcept;
BOOL CreateDirectoryW(const char* path, LPSECURITY_ATTRIBUTES attributes) noexcept;
BOOL MoveFileExW(LPCWSTR source, LPCWSTR destination, DWORD flags) noexcept;
BOOL MoveFileExW(const char* source, const char* destination, DWORD flags) noexcept;
BOOL MoveFileW(LPCWSTR source, LPCWSTR destination) noexcept;
BOOL CopyFileW(LPCWSTR source, LPCWSTR destination, BOOL fail_if_exists) noexcept;
BOOL CreateHardLinkW(LPCWSTR link, LPCWSTR existing, LPSECURITY_ATTRIBUTES attributes) noexcept;
BOOL CreateHardLinkW(const char* link, const char* existing, LPSECURITY_ATTRIBUTES attributes) noexcept;
DWORD GetFileAttributesW(LPCWSTR path) noexcept;
DWORD GetFileAttributesW(const char* path) noexcept;
DWORD GetFileAttributesA(LPCSTR path) noexcept;
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID information) noexcept;
BOOL GetFileAttributesExW(const char* path, GET_FILEEX_INFO_LEVELS level, LPVOID information) noexcept;
BOOL SetFileAttributesW(LPCWSTR path, DWORD attributes) noexcept;
BOOL GetVolumePathNameW(LPCWSTR path, LPWSTR volume, DWORD size) noexcept;
BOOL GetVolumePathNameW(const char* path, LPWSTR volume, DWORD size) noexcept;
BOOL GetVolumeInformationW(LPCWSTR root, LPWSTR name, DWORD name_size, LPDWORD serial,
    LPDWORD max_component, LPDWORD flags, LPWSTR file_system, DWORD file_system_size) noexcept;
HANDLE FindFirstStreamW(LPCWSTR path, STREAM_INFO_LEVELS level, LPVOID data, DWORD flags) noexcept;
HANDLE FindFirstStreamW(const char* path, STREAM_INFO_LEVELS level, LPVOID data, DWORD flags) noexcept;
BOOL FindNextStreamW(HANDLE search, LPVOID data) noexcept;
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data) noexcept;
BOOL FindNextFileW(HANDLE search, WIN32_FIND_DATAW* data) noexcept;
BOOL FindClose(HANDLE search) noexcept;
DWORD GetTempPathW(DWORD size, LPWSTR buffer) noexcept;
DWORD GetFullPathNameW(LPCWSTR path, DWORD size, LPWSTR buffer, LPWSTR* file_part) noexcept;

// File mappings (anonymous or named shared memory; file-backed mappings of an open file).
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_DECOMMIT 0x00004000u
#define MEM_RELEASE 0x00008000u
#define FILE_MAP_WRITE 0x0002u
#define FILE_MAP_READ 0x0004u
#define FILE_MAP_ALL_ACCESS 0x000F001Fu
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect,
    DWORD size_high, DWORD size_low, LPCWSTR name) noexcept;
HANDLE OpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name) noexcept;
LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low,
    SIZE_T size) noexcept;
BOOL UnmapViewOfFile(LPCVOID address) noexcept;
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) noexcept;
BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type) noexcept;
BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old_protect) noexcept;
struct WIN32_MEMORY_RANGE_ENTRY {
    PVOID VirtualAddress;
    SIZE_T NumberOfBytes;
};
BOOL PrefetchVirtualMemory(HANDLE process, ULONG_PTR count, WIN32_MEMORY_RANGE_ENTRY* entries,
    ULONG flags) noexcept;
struct MEMORYSTATUSEX {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    DWORDLONG ullTotalPhys;
    DWORDLONG ullAvailPhys;
    DWORDLONG ullTotalPageFile;
    DWORDLONG ullAvailPageFile;
    DWORDLONG ullTotalVirtual;
    DWORDLONG ullAvailVirtual;
    DWORDLONG ullAvailExtendedVirtual;
};
BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* status) noexcept;
#define MEM_FREE 0x00010000u
#define MEM_PRIVATE 0x00020000u
#define MEM_MAPPED 0x00040000u
struct MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
};
SIZE_T VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION* information, SIZE_T size) noexcept;
BOOL ReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size, SIZE_T* read) noexcept;

// ---------------------------------------------------------------------------------------------
// COM
// ---------------------------------------------------------------------------------------------

#define COINIT_APARTMENTTHREADED 0x2u
#define COINIT_MULTITHREADED 0x0u
#define COINIT_DISABLE_OLE1DDE 0x4u
#define CLSCTX_INPROC_SERVER 0x1u
#define CLSCTX_ALL 0x17u
HRESULT CoInitializeEx(LPVOID reserved, DWORD flags) noexcept;
void CoUninitialize() noexcept;
void CoTaskMemFree(LPVOID memory) noexcept;
LPVOID CoTaskMemAlloc(SIZE_T size) noexcept;

// MIDL-generated headers (the DirectX-Headers d3d12.h, the compat XAudio2/MMDevice headers) use
// these declarations. Interface IDs are bound to types with __CRT_UUID_DECL, the scheme the
// DirectX-Headers WSL adapter uses, so __uuidof and IID_PPV_ARGS work on types and expressions.
#define interface struct
#define MIDL_INTERFACE(x) struct
#define DECLSPEC_UUID(x)
#define DECLSPEC_NOVTABLE
#define DECLSPEC_SELECTANY
#define DECLSPEC_XFGVIRT(base, func)
#define BEGIN_INTERFACE
#define END_INTERFACE
#define STDMETHODCALLTYPE
#define STDAPICALLTYPE
#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif
#define STDAPI EXTERN_C HRESULT STDAPICALLTYPE
#define STDMETHOD(method) virtual HRESULT STDMETHODCALLTYPE method
#define STDMETHOD_(type, method) virtual type STDMETHODCALLTYPE method
#define IFACEMETHOD(method) STDMETHOD(method)
#define IFACEMETHOD_(type, method) STDMETHOD_(type, method)
#define PURE = 0
#define THIS_
#define THIS void
#define DECLARE_INTERFACE(iface) interface DECLSPEC_NOVTABLE iface
#define DECLARE_INTERFACE_(iface, baseiface) interface DECLSPEC_NOVTABLE iface : public baseiface
#define C_ASSERT(expr) static_assert((expr))
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof(*(a)))
#endif
#define UNREFERENCED_PARAMETER(P) (void)(P)

// SAL annotations carry no meaning for clang.
#define _In_
#define _In_z_
#define _In_opt_
#define _In_opt_z_
#define _In_reads_(x)
#define _In_reads_opt_(x)
#define _In_reads_bytes_(x)
#define _In_reads_bytes_opt_(x)
#define _In_range_(x, y)
#define _In_bytecount_(x)
#define _Out_
#define _Out_opt_
#define _Outptr_
#define _Outptr_opt_
#define _Outptr_opt_result_z_
#define _Outptr_opt_result_bytebuffer_(x)
#define _Outptr_result_maybenull_
#define _Outptr_result_nullonfailure_
#define _COM_Outptr_
#define _COM_Outptr_result_maybenull_
#define _COM_Outptr_opt_
#define _COM_Outptr_opt_result_maybenull_
#define _Out_writes_(x)
#define _Out_writes_z_(x)
#define _Out_writes_opt_(x)
#define _Out_writes_all_(x)
#define _Out_writes_all_opt_(x)
#define _Out_writes_to_opt_(x, y)
#define _Out_writes_bytes_(x)
#define _Out_writes_bytes_all_(x)
#define _Out_writes_bytes_all_opt_(x)
#define _Out_writes_bytes_opt_(x)
#define _Inout_
#define _Inout_opt_
#define _Inout_updates_(x)
#define _Inout_updates_bytes_(x)
#define _Field_size_(x)
#define _Field_size_opt_(x)
#define _Field_size_bytes_(x)
#define _Field_size_full_(x)
#define _Field_size_full_opt_(x)
#define _Field_size_bytes_full_(x)
#define _Field_size_bytes_full_opt_(x)
#define _Field_size_bytes_part_(x, y)
#define _Field_range_(x, y)
#define _Field_z_
#define _Check_return_
#define _Must_inspect_result_
#define _IRQL_requires_(x)
#define _IRQL_requires_min_(x)
#define _IRQL_requires_max_(x)
#define _At_(x, y)
#define _Always_(x)
#define _Return_type_success_(x)
#define _Translates_Win32_to_HRESULT_(x)
#define _Maybenull_
#define _Analysis_assume_(x)
#define __analysis_assume(x)
#define _Success_(x)
#define _In_count_(x)
#define _In_opt_count_(x)
#define _Use_decl_annotations_
#define _Null_terminated_
#define _Frees_ptr_opt_
#define _Ret_maybenull_
#define _Post_writable_byte_size_(x)
#define __RPC__in
#define __RPC__out
#define __RPC__deref_out
#define __RPC__deref_out_opt
#define __RPC__in_opt

#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    EXTERN_C const GUID name
#define InlineIsEqualGUID(a, b) ((a) == (b))
#define IsEqualGUID(a, b) ((a) == (b))
#define IsEqualIID(a, b) ((a) == (b))
inline bool operator!=(const GUID& a, const GUID& b) noexcept { return !(a == b); }

extern "C++" {
template <typename T>
struct __wsl_stub_uuidof_s;
template <typename T>
constexpr const GUID& __wsl_stub_uuidof();
}
#define __CRT_UUID_DECL(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)          \
    extern "C++" {                                                               \
    template <>                                                                  \
    struct __wsl_stub_uuidof_s<type> {                                           \
        static constexpr IID __uuid_inst = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}; \
    };                                                                           \
    template <>                                                                  \
    constexpr const GUID& __wsl_stub_uuidof<type>() {                            \
        return __wsl_stub_uuidof_s<type>::__uuid_inst;                           \
    }                                                                            \
    template <>                                                                  \
    constexpr const GUID& __wsl_stub_uuidof<type*>() {                           \
        return __wsl_stub_uuidof_s<type>::__uuid_inst;                           \
    }                                                                            \
    }
#define __uuidof(type) __wsl_stub_uuidof<__typeof(type)>()

struct IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
    template <class Q>
    HRESULT STDMETHODCALLTYPE QueryInterface(Q** pointer) {
        return QueryInterface(__uuidof(Q), reinterpret_cast<void**>(pointer));
    }

protected:
    ~IUnknown() = default;
};
using LPUNKNOWN = IUnknown*;
__CRT_UUID_DECL(IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)

extern "C++" {
template <typename T>
void** IID_PPV_ARGS_Helper(T** pointer) {
    (void)static_cast<IUnknown*>(*pointer);
    return reinterpret_cast<void**>(pointer);
}
}
#define IID_PPV_ARGS(pointer) __uuidof(**(pointer)), IID_PPV_ARGS_Helper(pointer)

extern "C++" {
template <std::size_t S>
struct _ENUM_FLAG_INTEGER_FOR_SIZE;
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<1> { using type = std::int8_t; };
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<2> { using type = std::int16_t; };
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<4> { using type = std::int32_t; };
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<8> { using type = std::int64_t; };
template <class T>
struct _ENUM_FLAG_SIZED_INTEGER { using type = typename _ENUM_FLAG_INTEGER_FOR_SIZE<sizeof(T)>::type; };
}
#define DEFINE_ENUM_FLAG_OPERATORS(ENUMTYPE)                                                              \
    extern "C++" {                                                                                       \
    inline constexpr ENUMTYPE operator|(ENUMTYPE a, ENUMTYPE b) {                                         \
        return ENUMTYPE(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a) | ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    }                                                                                                    \
    inline ENUMTYPE& operator|=(ENUMTYPE& a, ENUMTYPE b) {                                                \
        return (ENUMTYPE&)(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type&)a) |= ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    }                                                                                                    \
    inline constexpr ENUMTYPE operator&(ENUMTYPE a, ENUMTYPE b) {                                         \
        return ENUMTYPE(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a) & ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    }                                                                                                    \
    inline ENUMTYPE& operator&=(ENUMTYPE& a, ENUMTYPE b) {                                                \
        return (ENUMTYPE&)(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type&)a) &= ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    }                                                                                                    \
    inline constexpr ENUMTYPE operator~(ENUMTYPE a) {                                                     \
        return ENUMTYPE(~((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a));                                  \
    }                                                                                                    \
    inline constexpr ENUMTYPE operator^(ENUMTYPE a, ENUMTYPE b) {                                         \
        return ENUMTYPE(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a) ^ ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    }                                                                                                    \
    inline ENUMTYPE& operator^=(ENUMTYPE& a, ENUMTYPE b) {                                                \
        return (ENUMTYPE&)(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type&)a) ^= ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    }                                                                                                    \
    }

// ---------------------------------------------------------------------------------------------
// Power, DPI, console, error modes: no macOS counterpart is needed for these calls' effects.
// ---------------------------------------------------------------------------------------------

using DPI_AWARENESS_CONTEXT = struct DPI_AWARENESS_CONTEXT__*;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (reinterpret_cast<DPI_AWARENESS_CONTEXT>(static_cast<std::intptr_t>(-4)))
BOOL SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT context) noexcept;
DPI_AWARENESS_CONTEXT GetThreadDpiAwarenessContext() noexcept;
BOOL AreDpiAwarenessContextsEqual(DPI_AWARENESS_CONTEXT a, DPI_AWARENESS_CONTEXT b) noexcept;
UINT GetDpiForSystem() noexcept;
UINT GetDpiForWindow(HWND window) noexcept;
BOOL SetConsoleTitleW(LPCWSTR title) noexcept;
#define SEM_FAILCRITICALERRORS 0x0001u
#define SEM_NOGPFAULTERRORBOX 0x0002u
#define SEM_NOOPENFILEERRORBOX 0x8000u
UINT SetErrorMode(UINT mode) noexcept;
void OutputDebugStringA(LPCSTR text) noexcept;
void OutputDebugStringW(LPCWSTR text) noexcept;
BOOL IsDebuggerPresent() noexcept;
void DebugBreak() noexcept;

#include "galaxy_compat_user32.h"

#include "galaxy_macos.h"
