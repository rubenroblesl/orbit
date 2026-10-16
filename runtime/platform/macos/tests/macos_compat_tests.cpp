// SPDX-License-Identifier: GPL-3.0-only
//
// Behavioural checks of the macOS Win32 emulation that the runtime relies on.

#include <windows.h>
#include <bcrypt.h>

#include "galaxy/host_fpu.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (GetLastError=%u)\n", what, static_cast<unsigned>(GetLastError()));
        ++g_failures;
    }
}

// --- Fibers ---------------------------------------------------------------------------------

struct FiberTest {
    void* main_fiber = nullptr;
    void* worker_fiber = nullptr;
    int steps = 0;
    std::uint64_t worker_fp_control = 0;
    bool caught = false;
};

void fiber_body(void* parameter) {
    auto* test = static_cast<FiberTest*>(parameter);
    test->steps += 1;
    galaxy::host_fpu::enable_flush_to_zero();
    test->worker_fp_control = galaxy::host_fpu::read();
    try {
        throw std::runtime_error("inside fiber");
    } catch (const std::exception&) {
        test->caught = true;
    }
    SwitchToFiber(test->main_fiber);
    test->steps += 10;
    // The flush-to-zero mode survived the switch away and back.
    check(galaxy::host_fpu::read() == test->worker_fp_control, "fiber keeps its own FPCR");
    SwitchToFiber(test->main_fiber);
    std::abort();  // never resumed again
}

void test_fibers() {
    FiberTest test;
    const auto main_control = galaxy::host_fpu::read();
    test.main_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    check(test.main_fiber != nullptr && GetCurrentFiber() == test.main_fiber, "ConvertThreadToFiberEx");
    test.worker_fiber = CreateFiberEx(64 * 1024, 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, &fiber_body, &test);
    check(test.worker_fiber != nullptr, "CreateFiberEx");
    SwitchToFiber(test.worker_fiber);
    check(test.steps == 1 && test.caught, "fiber ran and caught its exception");
    check(GetCurrentFiber() == test.main_fiber, "current fiber restored");
    check(galaxy::host_fpu::read() == main_control, "main fiber FPCR untouched by the worker");
    SwitchToFiber(test.worker_fiber);
    check(test.steps == 11, "fiber resumed where it switched away");
    DeleteFiber(test.worker_fiber);
    check(ConvertFiberToThread() != FALSE, "ConvertFiberToThread");
}

// --- Events, waits, timers --------------------------------------------------------------------

void test_waits() {
    HANDLE automatic = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE manual = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(WaitForSingleObject(automatic, 0) == WAIT_TIMEOUT, "unsignaled event times out");
    std::thread setter([automatic] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        SetEvent(automatic);
    });
    check(WaitForSingleObject(automatic, 2000) == WAIT_OBJECT_0, "auto-reset event wakes a waiter");
    setter.join();
    check(WaitForSingleObject(automatic, 0) == WAIT_TIMEOUT, "auto-reset event resets after one wait");
    SetEvent(manual);
    check(WaitForSingleObject(manual, 0) == WAIT_OBJECT_0 && WaitForSingleObject(manual, 0) == WAIT_OBJECT_0,
        "manual-reset event stays signaled");
    const HANDLE both[2] = {automatic, manual};
    check(WaitForMultipleObjects(2, both, FALSE, 0) == WAIT_OBJECT_0 + 1, "wait-any reports the signaled index");
    ResetEvent(manual);

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    due.QuadPart = -50'000;  // 5 ms relative
    const auto start = std::chrono::steady_clock::now();
    check(SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE, "SetWaitableTimer");
    check(WaitForSingleObject(timer, 1000) == WAIT_OBJECT_0, "timer fires");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check(elapsed >= std::chrono::milliseconds(5) && elapsed < std::chrono::milliseconds(200), "timer fires on time");
    CloseHandle(timer);
    CloseHandle(automatic);
    CloseHandle(manual);

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    check(frequency.QuadPart > 1'000'000, "performance counter frequency");
}

// --- Files and alternate data streams ----------------------------------------------------------

std::wstring wide(const std::filesystem::path& path) {
    return path.wstring();
}

void test_files() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("nebula-compat-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    check(CreateDirectoryW(wide(root).c_str(), nullptr) != FALSE, "CreateDirectoryW");
    check(CreateDirectoryW(wide(root).c_str(), nullptr) == FALSE && GetLastError() == ERROR_ALREADY_EXISTS,
        "CreateDirectoryW reports ERROR_ALREADY_EXISTS");
    const std::filesystem::path file = root / "data.bin";
    HANDLE handle = CreateFileW(wide(file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "CreateFileW CREATE_NEW");
    const char payload[] = "galaxy";
    DWORD written = 0;
    check(WriteFile(handle, payload, sizeof(payload), &written, nullptr) != FALSE && written == sizeof(payload), "WriteFile");
    check(FlushFileBuffers(handle) != FALSE, "FlushFileBuffers");
    CloseHandle(handle);
    check(CreateFileW(wide(file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr) == INVALID_HANDLE_VALUE &&
              GetLastError() == ERROR_FILE_EXISTS,
        "CREATE_NEW on an existing file fails with ERROR_FILE_EXISTS");
    const DWORD attributes = GetFileAttributesW(wide(file).c_str());
    check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u, "GetFileAttributesW");
    check(GetFileAttributesW(wide(root / "missing").c_str()) == INVALID_FILE_ATTRIBUTES &&
              GetLastError() == ERROR_FILE_NOT_FOUND,
        "missing file reports ERROR_FILE_NOT_FOUND");

    // Alternate data stream round trip.
    const std::wstring stream = wide(file) + L":galaxy.isfs.meta";
    handle = CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "create stream");
    const char meta[] = "metadata";
    check(WriteFile(handle, meta, sizeof(meta), &written, nullptr) != FALSE, "write stream");
    CloseHandle(handle);
    handle = CreateFileW(stream.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    char read_back[32]{};
    DWORD read = 0;
    check(handle != INVALID_HANDLE_VALUE && ReadFile(handle, read_back, sizeof(read_back), &read, nullptr) != FALSE &&
              read == sizeof(meta) && std::memcmp(read_back, meta, sizeof(meta)) == 0,
        "read stream");
    CloseHandle(handle);
    WIN32_FIND_STREAM_DATA found{};
    HANDLE search = FindFirstStreamW(wide(file).c_str(), FindStreamInfoStandard, &found, 0);
    std::vector<std::wstring> names;
    if (search != INVALID_HANDLE_VALUE) {
        do names.emplace_back(found.cStreamName); while (FindNextStreamW(search, &found) != FALSE);
        FindClose(search);
    }
    check(names.size() == 2u && names[0] == L"::$DATA" && names[1] == L":galaxy.isfs.meta:$DATA",
        "stream enumeration lists the default and named streams");
    check(DeleteFileW(stream.c_str()) != FALSE, "delete stream");
    check(GetFileAttributesW(stream.c_str()) == INVALID_FILE_ATTRIBUTES, "deleted stream is gone");

    // Rename semantics.
    const std::filesystem::path other = root / "other.bin";
    handle = CreateFileW(wide(other).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    CloseHandle(handle);
    check(MoveFileExW(wide(file).c_str(), wide(other).c_str(), 0) == FALSE && GetLastError() == ERROR_ALREADY_EXISTS,
        "MoveFileExW without replace refuses an existing target");
    check(MoveFileExW(wide(file).c_str(), wide(other).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
        "MoveFileExW replaces");
    BY_HANDLE_FILE_INFORMATION info{};
    handle = CreateFileW(wide(other).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(GetFileInformationByHandle(handle, &info) != FALSE && info.nFileSizeLow == sizeof(payload) &&
              info.nNumberOfLinks == 1u,
        "GetFileInformationByHandle");
    CloseHandle(handle);
    DWORD serial = 0;
    wchar_t volume[MAX_PATH]{};
    check(GetVolumePathNameW(wide(other).c_str(), volume, MAX_PATH) != FALSE &&
              GetVolumeInformationW(volume, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) != FALSE &&
              serial == info.dwVolumeSerialNumber,
        "volume serial matches the file's");

    // Delete on close through the disposition.
    handle = CreateFileW(wide(other).c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    FILE_DISPOSITION_INFO disposition{TRUE};
    check(SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE,
        "FileDispositionInfo");
    CloseHandle(handle);
    check(GetFileAttributesW(wide(other).c_str()) == INVALID_FILE_ATTRIBUTES, "file deleted on close");
    check(RemoveDirectoryW(wide(root).c_str()) != FALSE, "RemoveDirectoryW");
}

// --- Named mutex, environment, strings, hashing --------------------------------------------------

void test_misc() {
    SetLastError(ERROR_SUCCESS);
    HANDLE first = CreateMutexW(nullptr, TRUE, L"Local\\Nebula.CompatTest");
    check(first != nullptr && GetLastError() == ERROR_SUCCESS, "named mutex created");
    HANDLE second = CreateMutexW(nullptr, TRUE, L"Local\\Nebula.CompatTest");
    check(second != nullptr && GetLastError() == ERROR_ALREADY_EXISTS, "second named mutex reports ERROR_ALREADY_EXISTS");
    CloseHandle(second);
    ReleaseMutex(first);
    CloseHandle(first);

    _putenv_s("NEBULA_COMPAT_TEST", "value");
    wchar_t buffer[3]{};
    check(GetEnvironmentVariableW(L"NEBULA_COMPAT_TEST", buffer, 3) == 6u, "too-small buffer reports the size");
    wchar_t full[16]{};
    check(GetEnvironmentVariableW(L"NEBULA_COMPAT_TEST", full, 16) == 5u && std::wcscmp(full, L"value") == 0,
        "GetEnvironmentVariableW");
    check(GetEnvironmentVariableW(L"NEBULA_COMPAT_MISSING", full, 16) == 0u && GetLastError() == ERROR_ENVVAR_NOT_FOUND,
        "missing variable");

    const char utf8[] = "Gal\xC3\xA1xia \xE2\x98\x85";
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    std::wstring converted(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, converted.data(), length);
    check(converted.size() == 10u && converted[3] == L'\x00E1' && converted[8] == L'\x2605', "UTF-8 to wide");
    char back[32]{};
    WideCharToMultiByte(CP_UTF8, 0, converted.c_str(), -1, back, sizeof(back), nullptr, nullptr);
    check(std::strcmp(back, utf8) == 0, "wide to UTF-8");

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<UCHAR, 20> digest{};
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == STATUS_SUCCESS &&
              BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == STATUS_SUCCESS &&
              BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>("abc")), 3, 0) == STATUS_SUCCESS &&
              BCryptFinishHash(hash, digest.data(), 20, 0) == STATUS_SUCCESS,
        "BCrypt SHA-1");
    static constexpr std::array<UCHAR, 20> kAbcSha1{0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E,
        0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};
    check(digest == kAbcSha1, "SHA-1(abc)");
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
}

}  // namespace

int main() {
    test_fibers();
    test_waits();
    test_files();
    test_misc();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d macOS compat check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("macOS compat checks passed");
    return 0;
}
