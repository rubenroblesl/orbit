// SPDX-License-Identifier: GPL-3.0-only
//
// Win32 kernel subset on macOS: last-error state, waitable kernel objects (events, mutexes,
// waitable timers), slim locks, clocks, threads, fibers, modules, environment and strings.

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include "win32_internal.h"

#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <xlocale.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace galaxy::compat {

// ---------------------------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------------------------

std::string utf8_from_wide(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        auto code = static_cast<std::uint32_t>(character);
        if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
            code = 0xFFFDu;
        }
        if (code < 0x80u) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800u) {
            result.push_back(static_cast<char>(0xC0u | (code >> 6u)));
            result.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else if (code < 0x10000u) {
            result.push_back(static_cast<char>(0xE0u | (code >> 12u)));
            result.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
            result.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else {
            result.push_back(static_cast<char>(0xF0u | (code >> 18u)));
            result.push_back(static_cast<char>(0x80u | ((code >> 12u) & 0x3Fu)));
            result.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
            result.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        }
    }
    return result;
}

bool wide_from_utf8(std::string_view text, std::wstring& result, bool strict) {
    result.clear();
    result.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::uint32_t code = 0;
        std::size_t extra = 0;
        if (lead < 0x80u) {
            code = lead;
        } else if ((lead & 0xE0u) == 0xC0u) {
            code = lead & 0x1Fu;
            extra = 1;
        } else if ((lead & 0xF0u) == 0xE0u) {
            code = lead & 0x0Fu;
            extra = 2;
        } else if ((lead & 0xF8u) == 0xF0u) {
            code = lead & 0x07u;
            extra = 3;
        } else {
            if (strict) return false;
            result.push_back(static_cast<wchar_t>(0xFFFDu));
            ++i;
            continue;
        }
        bool valid = true;
        for (std::size_t k = 1; k <= extra; ++k) {
            if (i + k >= text.size()) {
                valid = false;
                break;
            }
            const auto continuation = static_cast<unsigned char>(text[i + k]);
            if ((continuation & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
            code = (code << 6u) | (continuation & 0x3Fu);
        }
        const std::uint32_t minimum[] = {0u, 0x80u, 0x800u, 0x10000u};
        if (!valid || code < minimum[extra] || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
            if (strict) return false;
            result.push_back(static_cast<wchar_t>(0xFFFDu));
            i += valid ? extra + 1u : 1u;
            continue;
        }
        result.push_back(static_cast<wchar_t>(code));
        i += extra + 1u;
    }
    return true;
}

std::wstring wide_from_utf8(std::string_view text) {
    std::wstring result;
    (void)wide_from_utf8(text, result, false);
    return result;
}

std::string path_from_wide(LPCWSTR path) {
    return path == nullptr ? std::string() : utf8_from_wide(path);
}

// ---------------------------------------------------------------------------------------------
// Last error
// ---------------------------------------------------------------------------------------------

namespace {
thread_local DWORD t_last_error = ERROR_SUCCESS;
}  // namespace

DWORD win32_error_from_errno(int error) noexcept {
    switch (error) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EMFILE:
    case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case EACCES:
    case EPERM:
    case EROFS: return ERROR_ACCESS_DENIED;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case ENOSPC: return ERROR_DISK_FULL;
    case EDQUOT: return ERROR_HANDLE_DISK_FULL;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EBUSY: return ERROR_BUSY;
    case EAGAIN: return ERROR_LOCK_VIOLATION;
    case EIO: return ERROR_GEN_FAILURE;
    case EISDIR: return ERROR_ACCESS_DENIED;
    case ELOOP: return ERROR_CANT_ACCESS_FILE;
    case EXDEV: return ERROR_NOT_SUPPORTED;
    case ENOTSUP: return ERROR_NOT_SUPPORTED;
    case ENOATTR: return ERROR_FILE_NOT_FOUND;
    case ETIMEDOUT: return ERROR_TIMEOUT;
    case ECANCELED: return ERROR_OPERATION_ABORTED;
    case EPIPE: return ERROR_BROKEN_PIPE;
    default: return ERROR_GEN_FAILURE;
    }
}

void set_last_error_from_errno(int error) noexcept {
    t_last_error = win32_error_from_errno(error);
}

void unsupported(const char* function) noexcept {
    static std::mutex mutex;
    static std::vector<std::string> reported;
    std::lock_guard lock(mutex);
    if (std::find(reported.begin(), reported.end(), function) == reported.end()) {
        reported.emplace_back(function);
        std::fprintf(stderr, "[macos-compat] %s is not available on macOS\n", function);
    }
    t_last_error = ERROR_CALL_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------------------------
// Kernel objects
// ---------------------------------------------------------------------------------------------

namespace {

// Every waitable state change happens under this lock and is announced on this condition, so
// one wait can cover several objects of different kinds. The runtime waits on few objects at
// low rates (pacing, worker hand-off); contention is negligible.
std::mutex g_wait_mutex;
std::condition_variable g_wait_changed;

using Clock = std::chrono::steady_clock;

struct EventObject final : KernelObject {
    EventObject(bool manual, bool initial) : KernelObject(ObjectKind::Event), manual_reset(manual), signaled(initial) {}
    bool manual_reset;
    bool signaled;
};

// Every handle of one named mutex shares its ownership state, as Windows mutex handles do.
struct MutexState {
    pthread_t owner{};
    bool owned = false;
    unsigned recursion = 0;
    std::wstring name;
    int lock_fd = -1;  // flock()ed file that makes a named mutex visible to other processes
    unsigned handles = 0;
};

struct MutexObject final : KernelObject {
    explicit MutexObject(MutexState* mutex_state) : KernelObject(ObjectKind::Mutex), state(mutex_state) {}
    MutexState* state;
};

struct TimerObject final : KernelObject {
    explicit TimerObject(bool manual) : KernelObject(ObjectKind::Timer), manual_reset(manual) {}
    bool manual_reset;
    bool armed = false;
    bool signaled = false;
    Clock::time_point due{};
    std::chrono::milliseconds period{0};
};

struct SemaphoreObject final : KernelObject {
    SemaphoreObject(LONG initial, LONG maximum_count)
        : KernelObject(ObjectKind::Semaphore), count(initial), maximum(maximum_count) {}
    LONG count;
    LONG maximum;
};

std::map<std::wstring, MutexState*> g_named_mutexes;

// Brings a timer's signaled state up to date. Caller holds g_wait_mutex.
void refresh_timer(TimerObject& timer, Clock::time_point now) {
    if (timer.armed && now >= timer.due) {
        timer.signaled = true;
        if (timer.period.count() > 0) {
            while (timer.due <= now) timer.due += timer.period;
        } else {
            timer.armed = false;
        }
    }
}

bool object_signaled(KernelObject* object, Clock::time_point now) {
    switch (object->kind) {
    case ObjectKind::Event:
        return static_cast<EventObject*>(object)->signaled;
    case ObjectKind::Mutex: {
        const MutexState* mutex = static_cast<MutexObject*>(object)->state;
        return !mutex->owned || pthread_equal(mutex->owner, pthread_self());
    }
    case ObjectKind::Timer: {
        auto* timer = static_cast<TimerObject*>(object);
        refresh_timer(*timer, now);
        return timer->signaled;
    }
    case ObjectKind::Semaphore:
        return static_cast<SemaphoreObject*>(object)->count > 0;
    default:
        return false;
    }
}

void consume_signal(KernelObject* object) {
    switch (object->kind) {
    case ObjectKind::Event: {
        auto* event = static_cast<EventObject*>(object);
        if (!event->manual_reset) event->signaled = false;
        break;
    }
    case ObjectKind::Mutex: {
        MutexState* mutex = static_cast<MutexObject*>(object)->state;
        mutex->owned = true;
        mutex->owner = pthread_self();
        ++mutex->recursion;
        break;
    }
    case ObjectKind::Timer: {
        auto* timer = static_cast<TimerObject*>(object);
        if (!timer->manual_reset) timer->signaled = false;
        break;
    }
    case ObjectKind::Semaphore:
        --static_cast<SemaphoreObject*>(object)->count;
        break;
    default:
        break;
    }
}

std::optional<Clock::time_point> next_timer_due(KernelObject* object) {
    if (object->kind == ObjectKind::Timer) {
        auto* timer = static_cast<TimerObject*>(object);
        if (timer->armed) return timer->due;
    }
    return std::nullopt;
}

bool waitable(KernelObject* object) {
    return object != nullptr &&
        (object->kind == ObjectKind::Event || object->kind == ObjectKind::Mutex ||
         object->kind == ObjectKind::Timer || object->kind == ObjectKind::Semaphore);
}

std::string lock_file_path_for(std::wstring_view name) {
    std::string sanitized;
    for (const wchar_t character : name) {
        sanitized.push_back(std::iswalnum(static_cast<wint_t>(character)) || character == L'.' ? static_cast<char>(character) : '_');
    }
    const char* tmp = std::getenv("TMPDIR");
    std::string directory = tmp != nullptr && *tmp != '\0' ? tmp : "/tmp/";
    if (directory.back() != '/') directory.push_back('/');
    return directory + sanitized + ".lock";
}

}  // namespace

KernelObject* kernel_object(HANDLE handle) noexcept {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || is_pseudo_handle(handle)) {
        return nullptr;
    }
    auto* object = static_cast<KernelObject*>(handle);
    return object->magic == KernelObject::kMagic ? object : nullptr;
}

void notify_wait_state_changed() noexcept {
    std::lock_guard lock(g_wait_mutex);
    g_wait_changed.notify_all();
}

}  // namespace galaxy::compat

using namespace galaxy::compat;

DWORD GetLastError() noexcept { return t_last_error; }
void SetLastError(DWORD error) noexcept { t_last_error = error; }

BOOL CloseHandle(HANDLE handle) noexcept {
    if (is_pseudo_handle(handle)) return TRUE;
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (object->kind == ObjectKind::Mutex) {
        std::lock_guard lock(g_wait_mutex);
        MutexState* mutex = static_cast<MutexObject*>(object)->state;
        if (--mutex->handles == 0u) {
            if (!mutex->name.empty()) {
                const auto it = g_named_mutexes.find(mutex->name);
                if (it != g_named_mutexes.end() && it->second == mutex) g_named_mutexes.erase(it);
            }
            if (mutex->lock_fd >= 0) {
                (void)flock(mutex->lock_fd, LOCK_UN);
                (void)close(mutex->lock_fd);
            }
            delete mutex;
        }
    }
    const bool closed = object->close();
    object->magic = 0;
    delete object;
    g_wait_changed.notify_all();
    return closed ? TRUE : FALSE;
}

BOOL GetHandleInformation(HANDLE handle, LPDWORD flags) noexcept {
    if (kernel_object(handle) == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (flags != nullptr) *flags = 0;
    return TRUE;
}

BOOL DuplicateHandle(HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD) noexcept {
    unsupported("DuplicateHandle");
    return FALSE;
}

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL manual_reset, BOOL initial_state, LPCWSTR name) noexcept {
    if (name != nullptr && *name != L'\0') {
        unsupported("CreateEventW (named)");
        return nullptr;
    }
    SetLastError(ERROR_SUCCESS);
    return new EventObject(manual_reset != FALSE, initial_state != FALSE);
}

HANDLE CreateEventExW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD) noexcept {
    return CreateEventW(attributes, (flags & CREATE_EVENT_MANUAL_RESET) != 0u,
        (flags & CREATE_EVENT_INITIAL_SET) != 0u, name);
}

BOOL SetEvent(HANDLE handle) noexcept {
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr || object->kind != ObjectKind::Event) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    {
        std::lock_guard lock(g_wait_mutex);
        static_cast<EventObject*>(object)->signaled = true;
    }
    g_wait_changed.notify_all();
    return TRUE;
}

BOOL ResetEvent(HANDLE handle) noexcept {
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr || object->kind != ObjectKind::Event) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::lock_guard lock(g_wait_mutex);
    static_cast<EventObject*>(object)->signaled = false;
    return TRUE;
}

HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES, LONG initial, LONG maximum, LPCWSTR name) noexcept {
    if (name != nullptr && *name != L'\0') {
        unsupported("CreateSemaphoreW (named)");
        return nullptr;
    }
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    SetLastError(ERROR_SUCCESS);
    return new SemaphoreObject(initial, maximum);
}

BOOL ReleaseSemaphore(HANDLE handle, LONG count, LPLONG previous) noexcept {
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr || object->kind != ObjectKind::Semaphore) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    {
        std::lock_guard lock(g_wait_mutex);
        auto* semaphore = static_cast<SemaphoreObject*>(object);
        if (count <= 0 || semaphore->count > semaphore->maximum - count) {
            SetLastError(ERROR_TOO_MANY_POSTS);
            return FALSE;
        }
        if (previous != nullptr) *previous = semaphore->count;
        semaphore->count += count;
    }
    g_wait_changed.notify_all();
    return TRUE;
}

HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL initial_owner, LPCWSTR name) noexcept {
    std::lock_guard lock(g_wait_mutex);
    bool already_exists = false;
    MutexState* state = nullptr;
    if (name != nullptr && *name != L'\0') {
        const auto existing = g_named_mutexes.find(name);
        if (existing != g_named_mutexes.end()) {
            state = existing->second;
            already_exists = true;
        } else {
            // Another process holding the same name has the lock file locked.
            const std::string path = lock_file_path_for(name);
            const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (fd < 0) {
                set_last_error_from_errno(errno);
                return nullptr;
            }
            state = new MutexState();
            state->name = name;
            if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
                (void)close(fd);
                already_exists = true;
            } else {
                state->lock_fd = fd;
                g_named_mutexes.emplace(state->name, state);
            }
        }
    } else {
        state = new MutexState();
    }
    ++state->handles;
    if (initial_owner != FALSE && !already_exists) {
        state->owned = true;
        state->owner = pthread_self();
        state->recursion = 1;
    }
    SetLastError(already_exists ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return new MutexObject(state);
}

HANDLE OpenMutexW(DWORD, BOOL, LPCWSTR name) noexcept {
    std::lock_guard lock(g_wait_mutex);
    const auto existing = name != nullptr ? g_named_mutexes.find(name) : g_named_mutexes.end();
    if (existing == g_named_mutexes.end()) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return nullptr;
    }
    ++existing->second->handles;
    return new MutexObject(existing->second);
}

BOOL ReleaseMutex(HANDLE handle) noexcept {
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr || object->kind != ObjectKind::Mutex) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    MutexState* mutex = static_cast<MutexObject*>(object)->state;
    {
        std::lock_guard lock(g_wait_mutex);
        if (!mutex->owned || !pthread_equal(mutex->owner, pthread_self())) {
            SetLastError(288u);  // ERROR_NOT_OWNER
            return FALSE;
        }
        if (--mutex->recursion == 0) mutex->owned = false;
    }
    g_wait_changed.notify_all();
    return TRUE;
}

HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR name, DWORD flags, DWORD) noexcept {
    if (name != nullptr && *name != L'\0') {
        unsupported("CreateWaitableTimerExW (named)");
        return nullptr;
    }
    // Every timer wait is high resolution: the condition wait below has sub-millisecond precision.
    return new TimerObject((flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0u);
}

HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, LPCWSTR name) noexcept {
    return CreateWaitableTimerExW(attributes, name, manual_reset ? CREATE_WAITABLE_TIMER_MANUAL_RESET : 0u, 0u);
}

BOOL SetWaitableTimer(HANDLE handle, const LARGE_INTEGER* due_time, LONG period, PTIMERAPCROUTINE completion,
    LPVOID, BOOL) noexcept {
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr || object->kind != ObjectKind::Timer || due_time == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (completion != nullptr) {
        unsupported("SetWaitableTimer completion routine");
        return FALSE;
    }
    auto* timer = static_cast<TimerObject*>(object);
    const auto now = Clock::now();
    Clock::time_point due;
    if (due_time->QuadPart < 0) {
        due = now + std::chrono::nanoseconds(-due_time->QuadPart * 100);
    } else {
        // Absolute UTC in 100 ns units since 1601.
        constexpr std::int64_t kUnixEpochIn100ns = 116444736000000000ll;
        const auto system_now = std::chrono::system_clock::now();
        const auto target = std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::nanoseconds((due_time->QuadPart - kUnixEpochIn100ns) * 100)));
        due = now + std::chrono::duration_cast<Clock::duration>(target - system_now);
    }
    {
        std::lock_guard lock(g_wait_mutex);
        timer->due = due;
        timer->period = std::chrono::milliseconds(period);
        timer->armed = true;
        timer->signaled = false;
    }
    g_wait_changed.notify_all();
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE handle) noexcept {
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr || object->kind != ObjectKind::Timer) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::lock_guard lock(g_wait_mutex);
    static_cast<TimerObject*>(object)->armed = false;
    return TRUE;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD milliseconds) noexcept {
    if (count == 0u || count > MAXIMUM_WAIT_OBJECTS || handles == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    KernelObject* objects[MAXIMUM_WAIT_OBJECTS];
    for (DWORD i = 0; i < count; ++i) {
        objects[i] = kernel_object(handles[i]);
        if (!waitable(objects[i])) {
            SetLastError(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
    }
    const bool infinite = milliseconds == INFINITE;
    const auto deadline = Clock::now() + std::chrono::milliseconds(infinite ? 0u : milliseconds);
    std::unique_lock lock(g_wait_mutex);
    for (;;) {
        const auto now = Clock::now();
        if (wait_all != FALSE) {
            bool all = true;
            for (DWORD i = 0; i < count; ++i) all = object_signaled(objects[i], now) && all;
            if (all) {
                for (DWORD i = 0; i < count; ++i) consume_signal(objects[i]);
                return WAIT_OBJECT_0;
            }
        } else {
            for (DWORD i = 0; i < count; ++i) {
                if (object_signaled(objects[i], now)) {
                    consume_signal(objects[i]);
                    return WAIT_OBJECT_0 + i;
                }
            }
        }
        if (!infinite && now >= deadline) return WAIT_TIMEOUT;
        auto wake = infinite ? Clock::time_point::max() : deadline;
        for (DWORD i = 0; i < count; ++i) {
            if (const auto due = next_timer_due(objects[i])) wake = std::min(wake, *due);
        }
        if (wake == Clock::time_point::max()) {
            g_wait_changed.wait(lock);
        } else {
            g_wait_changed.wait_until(lock, wake);
        }
    }
}

DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) noexcept {
    return WaitForMultipleObjects(1u, &handle, FALSE, milliseconds);
}

DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL) noexcept {
    return WaitForSingleObject(handle, milliseconds);
}

// ---------------------------------------------------------------------------------------------
// Slim locks and critical sections
// ---------------------------------------------------------------------------------------------

void InitializeSRWLock(PSRWLOCK lock) noexcept { pthread_rwlock_init(&lock->lock, nullptr); }
void AcquireSRWLockExclusive(PSRWLOCK lock) noexcept { pthread_rwlock_wrlock(&lock->lock); }
void ReleaseSRWLockExclusive(PSRWLOCK lock) noexcept { pthread_rwlock_unlock(&lock->lock); }
void AcquireSRWLockShared(PSRWLOCK lock) noexcept { pthread_rwlock_rdlock(&lock->lock); }
void ReleaseSRWLockShared(PSRWLOCK lock) noexcept { pthread_rwlock_unlock(&lock->lock); }
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock) noexcept { return pthread_rwlock_trywrlock(&lock->lock) == 0; }
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock) noexcept { return pthread_rwlock_tryrdlock(&lock->lock) == 0; }

void InitializeCriticalSection(LPCRITICAL_SECTION section) noexcept {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&section->mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
}
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION section, DWORD) noexcept {
    InitializeCriticalSection(section);
    return TRUE;
}
void DeleteCriticalSection(LPCRITICAL_SECTION section) noexcept { pthread_mutex_destroy(&section->mutex); }
void EnterCriticalSection(LPCRITICAL_SECTION section) noexcept { pthread_mutex_lock(&section->mutex); }
void LeaveCriticalSection(LPCRITICAL_SECTION section) noexcept { pthread_mutex_unlock(&section->mutex); }
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION section) noexcept {
    return pthread_mutex_trylock(&section->mutex) == 0 ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------------------------

namespace {

const mach_timebase_info_data_t& timebase() noexcept {
    static const mach_timebase_info_data_t info = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    return info;
}

FILETIME filetime_from_100ns(std::uint64_t value) noexcept {
    return FILETIME{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32u)};
}

}  // namespace

// The performance counter is mach_absolute_time: 24 MHz on Apple silicon, an exact integer
// frequency (1e9 * denom / numer).
BOOL QueryPerformanceCounter(LARGE_INTEGER* counter) noexcept {
    counter->QuadPart = static_cast<LONGLONG>(mach_absolute_time());
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) noexcept {
    const auto& info = timebase();
    frequency->QuadPart = static_cast<LONGLONG>(1'000'000'000ull * info.denom / info.numer);
    return TRUE;
}

ULONGLONG GetTickCount64() noexcept {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1'000'000ull;
}

DWORD GetTickCount() noexcept { return static_cast<DWORD>(GetTickCount64()); }

void Sleep(DWORD milliseconds) noexcept {
    if (milliseconds == 0u) {
        sched_yield();
        return;
    }
    if (milliseconds == INFINITE) {
        for (;;) pause();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

DWORD SleepEx(DWORD milliseconds, BOOL) noexcept {
    Sleep(milliseconds);
    return 0;
}

void GetSystemTimeAsFileTime(LPFILETIME time) noexcept {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    constexpr std::uint64_t kUnixEpochIn100ns = 116444736000000000ull;
    *time = filetime_from_100ns(kUnixEpochIn100ns + static_cast<std::uint64_t>(now.tv_sec) * 10'000'000ull +
        static_cast<std::uint64_t>(now.tv_nsec) / 100u);
}

void GetSystemTimePreciseAsFileTime(LPFILETIME time) noexcept { GetSystemTimeAsFileTime(time); }

namespace {
void fill_system_time(SYSTEMTIME* out, bool local) noexcept {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    std::tm parts{};
    if (local) localtime_r(&now.tv_sec, &parts); else gmtime_r(&now.tv_sec, &parts);
    out->wYear = static_cast<WORD>(parts.tm_year + 1900);
    out->wMonth = static_cast<WORD>(parts.tm_mon + 1);
    out->wDayOfWeek = static_cast<WORD>(parts.tm_wday);
    out->wDay = static_cast<WORD>(parts.tm_mday);
    out->wHour = static_cast<WORD>(parts.tm_hour);
    out->wMinute = static_cast<WORD>(parts.tm_min);
    out->wSecond = static_cast<WORD>(parts.tm_sec);
    out->wMilliseconds = static_cast<WORD>(now.tv_nsec / 1'000'000);
}
}  // namespace

void GetLocalTime(SYSTEMTIME* time) noexcept { fill_system_time(time, true); }
void GetSystemTime(SYSTEMTIME* time) noexcept { fill_system_time(time, false); }
MMRESULT timeBeginPeriod(UINT) noexcept { return TIMERR_NOERROR; }
MMRESULT timeEndPeriod(UINT) noexcept { return TIMERR_NOERROR; }

// ---------------------------------------------------------------------------------------------
// Processes and threads
// ---------------------------------------------------------------------------------------------

namespace {
thread_local int t_thread_priority = THREAD_PRIORITY_NORMAL;

bool is_current_thread(HANDLE thread) noexcept {
    return thread == GetCurrentThread();
}

FILETIME filetime_from_timeval(const time_value_t& value) noexcept {
    return filetime_from_100ns(static_cast<std::uint64_t>(value.seconds) * 10'000'000ull +
        static_cast<std::uint64_t>(value.microseconds) * 10ull);
}
}  // namespace

HANDLE GetCurrentProcess() noexcept { return kPseudoProcessHandle; }
HANDLE GetCurrentThread() noexcept { return kPseudoThreadHandle; }
DWORD GetCurrentProcessId() noexcept { return static_cast<DWORD>(getpid()); }

DWORD GetCurrentThreadId() noexcept {
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<DWORD>(id);
}

BOOL SwitchToThread() noexcept {
    sched_yield();
    return TRUE;
}

// Windows priorities map to the quality-of-service classes the macOS scheduler uses for the
// calling thread. Other threads' priorities are left to the scheduler.
BOOL SetThreadPriority(HANDLE thread, int priority) noexcept {
    if (!is_current_thread(thread)) {
        return TRUE;
    }
    qos_class_t qos = QOS_CLASS_USER_INITIATED;
    if (priority == THREAD_MODE_BACKGROUND_BEGIN) {
        qos = QOS_CLASS_BACKGROUND;
    } else if (priority == THREAD_MODE_BACKGROUND_END) {
        qos = QOS_CLASS_USER_INITIATED;
    } else if (priority >= THREAD_PRIORITY_ABOVE_NORMAL) {
        qos = QOS_CLASS_USER_INTERACTIVE;
    } else if (priority == THREAD_PRIORITY_IDLE) {
        qos = QOS_CLASS_BACKGROUND;
    } else if (priority < THREAD_PRIORITY_NORMAL) {
        qos = QOS_CLASS_UTILITY;
    }
    // Fails on threads not created by pthreads (none here) and when already pinned; not fatal.
    (void)pthread_set_qos_class_self_np(qos, 0);
    t_thread_priority = priority;
    return TRUE;
}

int GetThreadPriority(HANDLE thread) noexcept {
    return is_current_thread(thread) ? t_thread_priority : THREAD_PRIORITY_NORMAL;
}

// Apple silicon has no user-controllable affinity; the scheduler places threads on P and E cores.
DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR mask) noexcept { return mask != 0u ? ~DWORD_PTR{0} : 0u; }

BOOL GetProcessAffinityMask(HANDLE, PDWORD_PTR process_mask, PDWORD_PTR system_mask) noexcept {
    const unsigned count = std::min(64u, std::max(1u, std::thread::hardware_concurrency()));
    const DWORD_PTR mask = count >= 64u ? ~DWORD_PTR{0} : ((DWORD_PTR{1} << count) - 1u);
    if (process_mask != nullptr) *process_mask = mask;
    if (system_mask != nullptr) *system_mask = mask;
    return TRUE;
}

BOOL SetPriorityClass(HANDLE, DWORD) noexcept { return TRUE; }
DWORD GetPriorityClass(HANDLE) noexcept { return NORMAL_PRIORITY_CLASS; }

BOOL GetThreadTimes(HANDLE thread, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user) noexcept {
    if (!is_current_thread(thread)) {
        unsupported("GetThreadTimes (other thread)");
        return FALSE;
    }
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const mach_port_t port = pthread_mach_thread_np(pthread_self());
    if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    if (creation != nullptr) *creation = FILETIME{};
    if (exit != nullptr) *exit = FILETIME{};
    if (kernel != nullptr) *kernel = filetime_from_timeval(info.system_time);
    if (user != nullptr) *user = filetime_from_timeval(info.user_time);
    return TRUE;
}

BOOL GetProcessTimes(HANDLE, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user) noexcept {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto to_filetime = [](const timeval& value) {
        return filetime_from_100ns(static_cast<std::uint64_t>(value.tv_sec) * 10'000'000ull +
            static_cast<std::uint64_t>(value.tv_usec) * 10ull);
    };
    if (creation != nullptr) {
        *creation = FILETIME{};
        kinfo_proc info{};
        std::size_t size = sizeof(info);
        int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
        if (sysctl(name, 4, &info, &size, nullptr, 0) == 0) {
            constexpr std::uint64_t kUnixEpochIn100ns = 116444736000000000ull;
            const timeval& start = info.kp_proc.p_starttime;
            *creation = filetime_from_100ns(kUnixEpochIn100ns + static_cast<std::uint64_t>(start.tv_sec) * 10'000'000ull +
                static_cast<std::uint64_t>(start.tv_usec) * 10ull);
        }
    }
    if (exit != nullptr) *exit = FILETIME{};
    if (kernel != nullptr) *kernel = to_filetime(usage.ru_stime);
    if (user != nullptr) *user = to_filetime(usage.ru_utime);
    return TRUE;
}

// Thread CPU time in nanoseconds stands in for the cycle count: callers compare deltas.
BOOL QueryThreadCycleTime(HANDLE thread, PULONG64 cycles) noexcept {
    if (!is_current_thread(thread)) {
        unsupported("QueryThreadCycleTime (other thread)");
        return FALSE;
    }
    *cycles = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    return TRUE;
}

HRESULT SetThreadDescription(HANDLE thread, PCWSTR description) noexcept {
    if (is_current_thread(thread) && description != nullptr) {
        const std::string name = utf8_from_wide(description);
        (void)pthread_setname_np(name.substr(0, 63).c_str());
    }
    return S_OK;
}

HRESULT GetThreadDescription(HANDLE thread, PWSTR* description) noexcept {
    char name[64]{};
    if (is_current_thread(thread)) (void)pthread_getname_np(pthread_self(), name, sizeof(name));
    const std::wstring wide = wide_from_utf8(name);
    auto* copy = static_cast<wchar_t*>(std::malloc((wide.size() + 1u) * sizeof(wchar_t)));
    if (copy == nullptr) return E_OUTOFMEMORY;
    std::wmemcpy(copy, wide.c_str(), wide.size() + 1u);
    *description = copy;
    return S_OK;
}

void ExitProcess(UINT code) noexcept {
    std::fflush(nullptr);
    std::_Exit(static_cast<int>(code));
}

BOOL TerminateProcess(HANDLE process, UINT code) noexcept {
    if (process == GetCurrentProcess()) ExitProcess(code);
    unsupported("TerminateProcess (other process)");
    return FALSE;
}

HANDLE OpenThread(DWORD, BOOL, DWORD) noexcept {
    unsupported("OpenThread");
    return nullptr;
}

DWORD GetActiveProcessorCount(WORD) noexcept {
    return std::max(1u, std::thread::hardware_concurrency());
}

// ---------------------------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------------------------

namespace {

std::string module_path_for_macos(std::string path) {
    // Windows system DLLs that the runtime probes optionally have no macOS image.
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const std::string name = lower(std::filesystem::path(path).filename().string());
    if (name.size() > 4 && name.ends_with(".dll")) {
        path.replace(path.size() - 4u, 4u, ".dylib");
    }
    return path;
}

bool is_windows_system_module(const std::string& path) {
    const std::filesystem::path name = std::filesystem::path(path).filename();
    std::string lower = name.string();
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr const char* kSystem[] = {
        "kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll", "shell32.dll", "avrt.dll",
        "dwmapi.dll", "shcore.dll", "winmm.dll", "dbghelp.dll", "advapi32.dll", "powrprof.dll",
        "d3d12.dll", "dxgi.dll", "xinput1_4.dll", "xaudio2_9.dll", "hid.dll", "setupapi.dll"};
    return std::any_of(std::begin(kSystem), std::end(kSystem), [&](const char* system) { return lower == system; });
}

HMODULE load_module(const std::string& path) noexcept {
    if (is_windows_system_module(path)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    const std::string macos_path = module_path_for_macos(path);
    void* const module = dlopen(macos_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        const char* error = dlerror();
        std::fprintf(stderr, "[macos-compat] dlopen(%s) failed: %s\n", macos_path.c_str(), error != nullptr ? error : "?");
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    return module;
}

}  // namespace

HMODULE LoadLibraryW(const char* path) noexcept { return path != nullptr ? load_module(path) : nullptr; }
HMODULE LoadLibraryW(LPCWSTR path) noexcept { return path != nullptr ? load_module(utf8_from_wide(path)) : nullptr; }
HMODULE LoadLibraryA(LPCSTR path) noexcept { return LoadLibraryW(path); }
HMODULE LoadLibraryExW(LPCWSTR path, HANDLE, DWORD) noexcept { return LoadLibraryW(path); }
HMODULE LoadLibraryExW(const char* path, HANDLE, DWORD) noexcept { return LoadLibraryW(path); }

BOOL FreeLibrary(HMODULE module) noexcept {
    if (module == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return dlclose(module) == 0 ? TRUE : FALSE;
}

FARPROC GetProcAddress(HMODULE module, LPCSTR name) noexcept {
    if (module == nullptr || name == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    void* const symbol = dlsym(module, name);
    if (symbol == nullptr) SetLastError(ERROR_PROC_NOT_FOUND);
    return reinterpret_cast<FARPROC>(symbol);
}

HMODULE GetModuleHandleW(LPCWSTR name) noexcept {
    if (name == nullptr) {
        return dlopen(nullptr, RTLD_NOW | RTLD_NOLOAD);
    }
    const std::string path = utf8_from_wide(name);
    if (is_windows_system_module(path)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    void* const module = dlopen(module_path_for_macos(path).c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (module == nullptr) SetLastError(ERROR_MOD_NOT_FOUND);
    return module;
}

BOOL GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module) noexcept {
    if (module == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) != 0u) {
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void*>(name), &info) == 0 || info.dli_fname == nullptr) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            return FALSE;
        }
        *module = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
        return *module != nullptr ? TRUE : FALSE;
    }
    *module = GetModuleHandleW(name);
    return *module != nullptr ? TRUE : FALSE;
}

namespace {
std::string module_file_name(HMODULE module) {
    if (module == nullptr || module == dlopen(nullptr, RTLD_NOW | RTLD_NOLOAD)) {
        std::uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string path(size, '\0');
        if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
        path.resize(std::strlen(path.c_str()));
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(path, error);
        return error ? path : canonical.string();
    }
    // Find the image whose handle matches.
    for (std::uint32_t i = 0, count = _dyld_image_count(); i < count; ++i) {
        const char* image = _dyld_get_image_name(i);
        void* const handle = dlopen(image, RTLD_NOW | RTLD_NOLOAD);
        if (handle != nullptr) {
            dlclose(handle);
            if (handle == module) return image;
        }
    }
    return {};
}
}  // namespace

DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size) noexcept {
    const std::wstring path = wide_from_utf8(module_file_name(module));
    if (path.empty()) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if (size == 0u) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const std::size_t copied = std::min<std::size_t>(path.size(), size - 1u);
    std::wmemcpy(buffer, path.c_str(), copied);
    buffer[copied] = L'\0';
    if (copied < path.size()) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    SetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(copied);
}

DWORD GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size) noexcept {
    const std::string path = module_file_name(module);
    if (path.empty() || size == 0u) {
        SetLastError(path.empty() ? ERROR_MOD_NOT_FOUND : ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const std::size_t copied = std::min<std::size_t>(path.size(), size - 1u);
    std::memcpy(buffer, path.c_str(), copied);
    buffer[copied] = '\0';
    return static_cast<DWORD>(copied < path.size() ? size : copied);
}

// ---------------------------------------------------------------------------------------------
// Environment and strings
// ---------------------------------------------------------------------------------------------

namespace {
template <typename Char>
DWORD copy_environment_value(const std::basic_string<Char>& value, Char* buffer, DWORD size) noexcept {
    if (buffer == nullptr || size <= value.size()) {
        return static_cast<DWORD>(value.size() + 1u);
    }
    std::copy(value.begin(), value.end(), buffer);
    buffer[value.size()] = Char{};
    return static_cast<DWORD>(value.size());
}
}  // namespace

namespace {
std::string application_support_directory();
// Windows' per-user data variables point at Application Support when the process environment
// does not set them.
const char* environment_value(const std::string& name, std::string& storage) {
    if (const char* value = std::getenv(name.c_str())) return value;
    if (name == "LOCALAPPDATA" || name == "APPDATA") {
        storage = application_support_directory();
        if (!storage.empty()) return storage.c_str();
    }
    return nullptr;
}
}  // namespace

DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size) noexcept {
    std::string storage;
    const char* value = name != nullptr ? environment_value(utf8_from_wide(name), storage) : nullptr;
    if (value == nullptr) {
        SetLastError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    SetLastError(ERROR_SUCCESS);
    return copy_environment_value(wide_from_utf8(value), buffer, size);
}

DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size) noexcept {
    std::string storage;
    const char* value = name != nullptr ? environment_value(name, storage) : nullptr;
    if (value == nullptr) {
        SetLastError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    SetLastError(ERROR_SUCCESS);
    return copy_environment_value(std::string(value), buffer, size);
}

BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) noexcept {
    if (name == nullptr) return FALSE;
    const std::string key = utf8_from_wide(name);
    const int result = value == nullptr ? unsetenv(key.c_str()) : setenv(key.c_str(), utf8_from_wide(value).c_str(), 1);
    return result == 0 ? TRUE : FALSE;
}

errno_t _wputenv_s(const wchar_t* name, const wchar_t* value) noexcept {
    if (name == nullptr || value == nullptr) return EINVAL;
    return _putenv_s(utf8_from_wide(name).c_str(), utf8_from_wide(value).c_str());
}

errno_t _wputenv_s(const wchar_t* name, const char* value) noexcept {
    if (name == nullptr || value == nullptr) return EINVAL;
    return _putenv_s(utf8_from_wide(name).c_str(), value);
}

int _configthreadlocale(int) noexcept {
    // Numeric parsing in the runtime uses explicit "C" locales (_strtof_l); report the
    // process-wide mode, which is what macOS offers through setlocale.
    return _DISABLE_PER_THREAD_LOCALE;
}

SIZE_T VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION* information, SIZE_T size) noexcept {
    if (information == nullptr || size < sizeof(MEMORY_BASIC_INFORMATION)) return 0;
    mach_vm_address_t region = reinterpret_cast<mach_vm_address_t>(address);
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t basic{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    *information = {};
    if (mach_vm_region(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&basic), &count, &object) != KERN_SUCCESS ||
        region > reinterpret_cast<mach_vm_address_t>(address)) {
        information->BaseAddress = const_cast<void*>(address);
        information->State = MEM_FREE;
        information->Protect = PAGE_NOACCESS;
        return sizeof(MEMORY_BASIC_INFORMATION);
    }
    information->BaseAddress = reinterpret_cast<void*>(region);
    information->AllocationBase = reinterpret_cast<void*>(region);
    information->RegionSize = static_cast<SIZE_T>(region_size);
    const bool accessible = (basic.protection & VM_PROT_READ) != 0;
    information->State = accessible ? MEM_COMMIT : MEM_RESERVE;
    information->Protect = !accessible ? PAGE_NOACCESS : (basic.protection & VM_PROT_WRITE) != 0 ? PAGE_READWRITE : PAGE_READONLY;
    information->Type = basic.shared ? MEM_MAPPED : MEM_PRIVATE;
    return sizeof(MEMORY_BASIC_INFORMATION);
}

BOOL ReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size, SIZE_T* read) noexcept {
    if (read != nullptr) *read = 0;
    if (process != GetCurrentProcess()) {
        unsupported("ReadProcessMemory (other process)");
        return FALSE;
    }
    mach_vm_size_t copied = 0;
    if (mach_vm_read_overwrite(mach_task_self(), reinterpret_cast<mach_vm_address_t>(address), size,
            reinterpret_cast<mach_vm_address_t>(buffer), &copied) != KERN_SUCCESS) {
        SetLastError(299u);  // ERROR_PARTIAL_COPY
        return FALSE;
    }
    if (read != nullptr) *read = static_cast<SIZE_T>(copied);
    return TRUE;
}

errno_t _wdupenv_s(wchar_t** buffer, std::size_t* length, const wchar_t* name) noexcept {
    if (buffer == nullptr || name == nullptr) return EINVAL;
    *buffer = nullptr;
    if (length != nullptr) *length = 0;
    std::string storage;
    const char* value = environment_value(utf8_from_wide(name), storage);
    if (value == nullptr) return 0;
    const std::wstring wide = wide_from_utf8(value);
    auto* copy = static_cast<wchar_t*>(std::malloc((wide.size() + 1u) * sizeof(wchar_t)));
    if (copy == nullptr) return ENOMEM;
    std::wmemcpy(copy, wide.c_str(), wide.size() + 1u);
    *buffer = copy;
    if (length != nullptr) *length = wide.size() + 1u;
    return 0;
}

wchar_t* _wgetenv(const wchar_t* name) noexcept {
    // Values are interned per name so the returned pointer stays valid, as the CRT's does.
    static std::mutex mutex;
    static std::map<std::wstring, std::wstring> values;
    std::string storage;
    const char* value = name != nullptr ? environment_value(utf8_from_wide(name), storage) : nullptr;
    if (value == nullptr) return nullptr;
    std::lock_guard lock(mutex);
    std::wstring& slot = values[name];
    slot = wide_from_utf8(value);
    return slot.data();
}

namespace {
std::string mode_from_wide(const wchar_t* mode) {
    std::string result = utf8_from_wide(mode);
    // The CRT's ccs= and N/T/S/R flags have no stdio meaning here.
    std::string filtered;
    for (const char c : result) {
        if (c == ',') break;
        if (c == 'r' || c == 'w' || c == 'a' || c == '+' || c == 'b' || c == 'x') filtered.push_back(c);
    }
    return filtered;
}
}  // namespace

errno_t _wfopen_s(std::FILE** file, const char* path, const wchar_t* mode) noexcept {
    if (file == nullptr || path == nullptr || mode == nullptr) return EINVAL;
    *file = nullptr;
    // The CRT opens with _SH_DENYNO: read and write sharing, refused by a handle that denies it.
    const std::string stdio_mode = mode_from_wide(mode);
    DWORD access = stdio_mode.find('r') != std::string::npos ? GENERIC_READ : GENERIC_WRITE;
    if (stdio_mode.find('+') != std::string::npos) access = GENERIC_READ | GENERIC_WRITE;
    if (!share_allows_open(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE)) {
        errno = EACCES;
        return EACCES;
    }
    *file = std::fopen(path, stdio_mode.c_str());
    return *file != nullptr ? 0 : errno;
}

errno_t _wfopen_s(std::FILE** file, const wchar_t* path, const wchar_t* mode) noexcept {
    if (path == nullptr) return EINVAL;
    return _wfopen_s(file, utf8_from_wide(path).c_str(), mode);
}

std::FILE* _wfopen(const char* path, const wchar_t* mode) noexcept {
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path, mode) == 0 ? file : nullptr;
}

std::FILE* _wfopen(const wchar_t* path, const wchar_t* mode) noexcept {
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path, mode) == 0 ? file : nullptr;
}

std::FILE* _wfsopen(const wchar_t* path, const wchar_t* mode, int) noexcept { return _wfopen(path, mode); }
std::FILE* _wfsopen(const char* path, const wchar_t* mode, int) noexcept { return _wfopen(path, mode); }

int _wcsicmp(const wchar_t* left, const wchar_t* right) noexcept { return wcscasecmp(left, right); }
int _wcsnicmp(const wchar_t* left, const wchar_t* right, std::size_t count) noexcept {
    return wcsncasecmp(left, right, count);
}
int lstrcmpW(LPCWSTR left, LPCWSTR right) noexcept { return std::wcscmp(left, right); }
int lstrcmpiW(LPCWSTR left, LPCWSTR right) noexcept { return wcscasecmp(left, right); }

int vswprintf_s(wchar_t* buffer, std::size_t count, const wchar_t* format, va_list arguments) noexcept {
    return std::vswprintf(buffer, count, format, arguments);
}

int swprintf_s(wchar_t* buffer, std::size_t count, const wchar_t* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    const int result = std::vswprintf(buffer, count, format, arguments);
    va_end(arguments);
    return result;
}

errno_t wcscpy_s(wchar_t* destination, std::size_t size, const wchar_t* source) noexcept {
    if (destination == nullptr || source == nullptr || size == 0) return EINVAL;
    const std::size_t length = std::wcslen(source);
    if (length >= size) {
        destination[0] = L'\0';
        return ERANGE;
    }
    std::wmemcpy(destination, source, length + 1u);
    return 0;
}

struct __crt_locale_pointers {
    locale_t locale;
};

_locale_t _create_locale(int, const char* name) noexcept {
    locale_t locale = newlocale(LC_ALL_MASK, name != nullptr ? name : "C", nullptr);
    if (locale == nullptr) return nullptr;
    return new __crt_locale_pointers{locale};
}

void _free_locale(_locale_t locale) noexcept {
    if (locale == nullptr) return;
    freelocale(locale->locale);
    delete locale;
}

float _strtof_l(const char* text, char** end, _locale_t locale) noexcept {
    return strtof_l(text, end, locale != nullptr ? locale->locale : LC_C_LOCALE);
}

double _strtod_l(const char* text, char** end, _locale_t locale) noexcept {
    return strtod_l(text, end, locale != nullptr ? locale->locale : LC_C_LOCALE);
}

int _snprintf_s_l(char* buffer, std::size_t size, std::size_t count, const char* format, _locale_t locale, ...) noexcept {
    if (buffer == nullptr || size == 0) return -1;
    va_list arguments;
    va_start(arguments, locale);
    const std::size_t limit = count == _TRUNCATE ? size : std::min(size, count + 1u);
    const int result = vsnprintf_l(buffer, limit, locale != nullptr ? locale->locale : LC_C_LOCALE, format, arguments);
    va_end(arguments);
    if (result < 0) return -1;
    return static_cast<std::size_t>(result) >= limit ? -1 : result;
}

int MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR source, int source_length, LPWSTR destination,
    int destination_length) noexcept {
    if (code_page != CP_UTF8 && code_page != CP_ACP) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const std::size_t length = source_length < 0 ? std::strlen(source) + 1u : static_cast<std::size_t>(source_length);
    std::wstring wide;
    if (!wide_from_utf8(std::string_view(source, length), wide, (flags & MB_ERR_INVALID_CHARS) != 0u)) {
        SetLastError(1113u);  // ERROR_NO_UNICODE_TRANSLATION
        return 0;
    }
    if (destination_length == 0) return static_cast<int>(wide.size());
    if (static_cast<std::size_t>(destination_length) < wide.size()) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::wmemcpy(destination, wide.data(), wide.size());
    return static_cast<int>(wide.size());
}

int WideCharToMultiByte(UINT code_page, DWORD, LPCWSTR source, int source_length, LPSTR destination,
    int destination_length, LPCSTR, BOOL* used_default) noexcept {
    if (code_page != CP_UTF8 && code_page != CP_ACP) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (used_default != nullptr) *used_default = FALSE;
    const std::size_t length = source_length < 0 ? std::wcslen(source) + 1u : static_cast<std::size_t>(source_length);
    const std::string utf8 = utf8_from_wide(std::wstring_view(source, length));
    if (destination_length == 0) return static_cast<int>(utf8.size());
    if (static_cast<std::size_t>(destination_length) < utf8.size()) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::memcpy(destination, utf8.data(), utf8.size());
    return static_cast<int>(utf8.size());
}

// ---------------------------------------------------------------------------------------------
// COM, DPI, console, diagnostics
// ---------------------------------------------------------------------------------------------

HRESULT CoInitializeEx(LPVOID, DWORD) noexcept { return S_OK; }
void CoUninitialize() noexcept {}
void CoTaskMemFree(LPVOID memory) noexcept { std::free(memory); }
LPVOID CoTaskMemAlloc(SIZE_T size) noexcept { return std::malloc(size); }

BOOL SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT) noexcept { return TRUE; }
DPI_AWARENESS_CONTEXT GetThreadDpiAwarenessContext() noexcept { return DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2; }
BOOL AreDpiAwarenessContextsEqual(DPI_AWARENESS_CONTEXT a, DPI_AWARENESS_CONTEXT b) noexcept { return a == b; }
UINT GetDpiForSystem() noexcept { return 96u; }
UINT GetDpiForWindow(HWND) noexcept { return 96u; }
BOOL SetConsoleTitleW(LPCWSTR) noexcept { return TRUE; }
UINT SetErrorMode(UINT) noexcept { return 0u; }
void OutputDebugStringA(LPCSTR text) noexcept {
    if (text != nullptr) std::fputs(text, stderr);
}
void OutputDebugStringW(LPCWSTR text) noexcept {
    if (text != nullptr) std::fputs(utf8_from_wide(text).c_str(), stderr);
}

BOOL IsDebuggerPresent() noexcept {
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(name, 4, &info, &size, nullptr, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) != 0 ? TRUE : FALSE;
}

void DebugBreak() noexcept { __builtin_debugtrap(); }

BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* status) noexcept {
    std::uint64_t total = 0;
    std::size_t size = sizeof(total);
    sysctlbyname("hw.memsize", &total, &size, nullptr, 0);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count);
    const std::uint64_t page = static_cast<std::uint64_t>(vm_page_size);
    const std::uint64_t available = (static_cast<std::uint64_t>(vm.free_count) + vm.inactive_count) * page;
    status->dwMemoryLoad = total != 0 ? static_cast<DWORD>(100u - std::min<std::uint64_t>(100u, available * 100u / total)) : 0u;
    status->ullTotalPhys = total;
    status->ullAvailPhys = available;
    status->ullTotalPageFile = total;
    status->ullAvailPageFile = available;
    status->ullTotalVirtual = 1ull << 47u;
    status->ullAvailVirtual = 1ull << 47u;
    status->ullAvailExtendedVirtual = 0;
    return TRUE;
}

// ---------------------------------------------------------------------------------------------
// Known folders
// ---------------------------------------------------------------------------------------------

namespace {
std::string application_support_directory() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return std::string(home) + "/Library/Application Support";
}
}  // namespace

HRESULT SHGetFolderPathW(HWND, int folder, HANDLE, DWORD, LPWSTR path) noexcept {
    const int base = folder & 0xFF;
    if (base != CSIDL_LOCAL_APPDATA && base != CSIDL_APPDATA) return E_INVALIDARG;
    const std::wstring wide = wide_from_utf8(application_support_directory());
    if (wide.empty() || wide.size() >= MAX_PATH) return E_FAIL;
    std::wmemcpy(path, wide.c_str(), wide.size() + 1u);
    return S_OK;
}

HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID folder, DWORD, HANDLE, PWSTR* path) noexcept {
    if (!(folder == FOLDERID_LocalAppData) && !(folder == FOLDERID_RoamingAppData)) return E_INVALIDARG;
    const std::wstring wide = wide_from_utf8(application_support_directory());
    if (wide.empty()) return E_FAIL;
    auto* copy = static_cast<wchar_t*>(CoTaskMemAlloc((wide.size() + 1u) * sizeof(wchar_t)));
    if (copy == nullptr) return E_OUTOFMEMORY;
    std::wmemcpy(copy, wide.c_str(), wide.size() + 1u);
    *path = copy;
    return S_OK;
}

// ---------------------------------------------------------------------------------------------
// BCrypt hashing
// ---------------------------------------------------------------------------------------------

namespace {
enum class HashAlgorithm { Sha1, Sha256 };
struct AlgorithmObject {
    HashAlgorithm algorithm;
};
struct HashObject {
    HashAlgorithm algorithm;
    CC_SHA1_CTX sha1;
    CC_SHA256_CTX sha256;
};
ULONG digest_length(HashAlgorithm algorithm) noexcept {
    return algorithm == HashAlgorithm::Sha1 ? CC_SHA1_DIGEST_LENGTH : CC_SHA256_DIGEST_LENGTH;
}
}  // namespace

NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* algorithm, LPCWSTR id, LPCWSTR, ULONG flags) noexcept {
    if (algorithm == nullptr || id == nullptr || flags != 0u) return STATUS_INVALID_PARAMETER;
    if (std::wcscmp(id, BCRYPT_SHA1_ALGORITHM) == 0) {
        *algorithm = new AlgorithmObject{HashAlgorithm::Sha1};
    } else if (std::wcscmp(id, BCRYPT_SHA256_ALGORITHM) == 0) {
        *algorithm = new AlgorithmObject{HashAlgorithm::Sha256};
    } else {
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE algorithm, ULONG) noexcept {
    delete static_cast<AlgorithmObject*>(algorithm);
    return STATUS_SUCCESS;
}

NTSTATUS BCryptGetProperty(BCRYPT_HANDLE object, LPCWSTR property, PUCHAR output, ULONG size, ULONG* result, ULONG) noexcept {
    if (object == nullptr || property == nullptr) return STATUS_INVALID_PARAMETER;
    const auto* algorithm = static_cast<AlgorithmObject*>(object);
    ULONG value = 0;
    if (std::wcscmp(property, BCRYPT_OBJECT_LENGTH) == 0) {
        value = sizeof(HashObject);
    } else if (std::wcscmp(property, BCRYPT_HASH_LENGTH) == 0) {
        value = digest_length(algorithm->algorithm);
    } else {
        return STATUS_NOT_SUPPORTED;
    }
    if (result != nullptr) *result = sizeof(ULONG);
    if (output == nullptr || size < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
    std::memcpy(output, &value, sizeof(value));
    return STATUS_SUCCESS;
}

NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE algorithm, BCRYPT_HASH_HANDLE* hash, PUCHAR, ULONG, PUCHAR secret,
    ULONG secret_size, ULONG) noexcept {
    if (algorithm == nullptr || hash == nullptr || (secret != nullptr && secret_size != 0)) return STATUS_INVALID_PARAMETER;
    auto* object = new HashObject{};
    object->algorithm = static_cast<AlgorithmObject*>(algorithm)->algorithm;
    if (object->algorithm == HashAlgorithm::Sha1) CC_SHA1_Init(&object->sha1); else CC_SHA256_Init(&object->sha256);
    *hash = object;
    return STATUS_SUCCESS;
}

NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hash, PUCHAR input, ULONG size, ULONG) noexcept {
    auto* object = static_cast<HashObject*>(hash);
    if (object == nullptr) return STATUS_INVALID_PARAMETER;
    if (object->algorithm == HashAlgorithm::Sha1) CC_SHA1_Update(&object->sha1, input, size);
    else CC_SHA256_Update(&object->sha256, input, size);
    return STATUS_SUCCESS;
}

NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hash, PUCHAR output, ULONG size, ULONG) noexcept {
    auto* object = static_cast<HashObject*>(hash);
    if (object == nullptr || size != digest_length(object->algorithm)) return STATUS_INVALID_PARAMETER;
    if (object->algorithm == HashAlgorithm::Sha1) CC_SHA1_Final(output, &object->sha1);
    else CC_SHA256_Final(output, &object->sha256);
    return STATUS_SUCCESS;
}

NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hash) noexcept {
    delete static_cast<HashObject*>(hash);
    return STATUS_SUCCESS;
}
