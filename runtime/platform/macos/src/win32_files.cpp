// SPDX-License-Identifier: GPL-3.0-only
//
// Win32 file APIs on macOS: handles over file descriptors, attributes from lstat/fstat, volumes
// from statfs, file mappings and virtual memory over mmap.
//
// NTFS alternate data streams ("C:\dir\file:name[:$DATA]") are stored as extended attributes
// named "com.nebula.ads.<name>" on the base file. A stream handle holds the whole value in
// memory; writes reach the attribute on FlushFileBuffers and CloseHandle (fsetxattr replaces the
// value atomically), and the base file is fsync'ed so the attribute is durable. Renames and hard
// links carry the attributes with the inode, as NTFS carries streams with the file.

#include <windows.h>

#include "win32_internal.h"

#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace galaxy::compat;

namespace {

constexpr std::string_view kStreamAttributePrefix = "com.nebula.ads.";

struct StreamPath {
    std::string base;
    std::string stream;  // empty for the default data stream
};

// Splits "path:stream[:$DATA]" at the first ':' of the last component.
StreamPath split_stream_path(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t colon = path.find(':', slash == std::string::npos ? 0u : slash + 1u);
    if (colon == std::string::npos) {
        return {path, {}};
    }
    std::string stream = path.substr(colon + 1u);
    if (const std::size_t type = stream.find(':'); type != std::string::npos) {
        stream.resize(type);  // ":$DATA"
    }
    return {path.substr(0, colon), stream};
}

std::string stream_attribute(const std::string& stream) {
    return std::string(kStreamAttributePrefix) + stream;
}

std::string host_path(const char* path) {
    if (path == nullptr) return {};
    std::string result(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::string host_path(LPCWSTR path) {
    return host_path(path_from_wide(path).c_str());
}

FILETIME filetime_from_timespec(const timespec& value) noexcept {
    constexpr std::uint64_t kUnixEpochIn100ns = 116444736000000000ull;
    const std::uint64_t ticks = kUnixEpochIn100ns + static_cast<std::uint64_t>(value.tv_sec) * 10'000'000ull +
        static_cast<std::uint64_t>(value.tv_nsec) / 100u;
    return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32u)};
}

DWORD attributes_from_stat(const struct stat& status) noexcept {
    DWORD attributes = 0;
    if (S_ISDIR(status.st_mode)) {
        attributes |= FILE_ATTRIBUTE_DIRECTORY;
    } else if (S_ISLNK(status.st_mode)) {
        attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    } else {
        attributes |= FILE_ATTRIBUTE_ARCHIVE;
    }
    if ((status.st_mode & S_IWUSR) == 0) {
        attributes |= FILE_ATTRIBUTE_READONLY;
    }
    return attributes;
}

bool fsync_full(int fd) noexcept {
    // F_FULLFSYNC reaches stable storage on APFS; fsync alone may stop at the drive cache.
    if (fcntl(fd, F_FULLFSYNC) == 0) return true;
    return fsync(fd) == 0;
}

void sync_directory_of(const std::string& path) noexcept {
    const std::string parent = std::filesystem::path(path).parent_path().string();
    const int fd = open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        (void)fsync_full(fd);
        (void)close(fd);
    }
}

// --- Share modes ------------------------------------------------------------------------------
// Windows refuses an open, rename or delete that conflicts with the share mode of a handle already
// open on the same file. The emulation applies that rule among this process's handles (POSIX has
// no mandatory equivalent across processes). Objects are keyed by device, inode and stream.

struct ShareKey {
    dev_t device;
    ino_t inode;
    std::string stream;
    bool operator<(const ShareKey& other) const {
        if (device != other.device) return device < other.device;
        if (inode != other.inode) return inode < other.inode;
        return stream < other.stream;
    }
};

struct ShareEntry {
    const void* owner;
    DWORD access;
    DWORD share;
};

std::mutex g_share_mutex;
std::map<ShareKey, std::vector<ShareEntry>> g_shares;

bool reads(DWORD access) noexcept {
    return (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0u;
}
bool writes(DWORD access) noexcept {
    return (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0u;
}
bool deletes(DWORD access) noexcept {
    return (access & (DELETE | GENERIC_ALL)) != 0u;
}

bool share_conflict(const std::vector<ShareEntry>& entries, DWORD access, DWORD share) noexcept {
    for (const ShareEntry& entry : entries) {
        if ((reads(access) && (entry.share & FILE_SHARE_READ) == 0u) ||
            (writes(access) && (entry.share & FILE_SHARE_WRITE) == 0u) ||
            (deletes(access) && (entry.share & FILE_SHARE_DELETE) == 0u) ||
            (reads(entry.access) && (share & FILE_SHARE_READ) == 0u) ||
            (writes(entry.access) && (share & FILE_SHARE_WRITE) == 0u) ||
            (deletes(entry.access) && (share & FILE_SHARE_DELETE) == 0u)) {
            return true;
        }
    }
    return false;
}

std::optional<ShareKey> share_key_for_path(const std::string& path, const std::string& stream) {
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0) return std::nullopt;
    return ShareKey{status.st_dev, status.st_ino, stream};
}

// True when deleting or renaming `path` conflicts with an open handle (no FILE_SHARE_DELETE).
bool delete_blocked(const std::string& path) {
    const auto key = share_key_for_path(path, {});
    if (!key) return false;
    std::lock_guard lock(g_share_mutex);
    const auto it = g_shares.find(*key);
    return it != g_shares.end() && share_conflict(it->second, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
}

}  // namespace

bool galaxy::compat::share_allows_open(const std::string& path, DWORD access, DWORD share) {
    const auto key = share_key_for_path(host_path(path.c_str()), {});
    if (!key) return true;
    std::lock_guard lock(g_share_mutex);
    const auto it = g_shares.find(*key);
    return it == g_shares.end() || !share_conflict(it->second, access, share);
}

namespace {

struct FileObject final : galaxy::compat::KernelObject {
    FileObject() : KernelObject(ObjectKind::File) {}
    int fd = -1;
    std::string path;  // base path for streams
    DWORD access = 0;
    DWORD flags = 0;
    bool directory = false;
    bool delete_on_close = false;
    bool write_through = false;
    // Stream emulation.
    bool stream = false;
    std::string attribute;
    std::vector<std::byte> data;
    std::uint64_t position = 0;
    bool dirty = false;

    bool flush_stream() noexcept {
        if (!stream || !dirty) return true;
        if (fsetxattr(fd, attribute.c_str(), data.data(), data.size(), 0, 0) != 0) {
            set_last_error_from_errno(errno);
            return false;
        }
        dirty = false;
        return true;
    }

    std::optional<ShareKey> share_key;

    bool close() noexcept override {
        if (share_key) {
            std::lock_guard lock(g_share_mutex);
            const auto it = g_shares.find(*share_key);
            if (it != g_shares.end()) {
                std::erase_if(it->second, [this](const ShareEntry& entry) { return entry.owner == this; });
                if (it->second.empty()) g_shares.erase(it);
            }
        }
        bool ok = flush_stream();
        if (delete_on_close) {
            const int result = stream ? removexattr(path.c_str(), attribute.c_str(), XATTR_NOFOLLOW)
                : (directory ? rmdir(path.c_str()) : unlink(path.c_str()));
            if (result != 0) {
                set_last_error_from_errno(errno);
                ok = false;
            }
        }
        if (fd >= 0 && ::close(fd) != 0) {
            set_last_error_from_errno(errno);
            ok = false;
        }
        fd = -1;
        return ok;
    }
};

FileObject* file_object(HANDLE handle) noexcept {
    KernelObject* const object = kernel_object(handle);
    return object != nullptr && object->kind == ObjectKind::File ? static_cast<FileObject*>(object) : nullptr;
}

int open_flags_for(DWORD access, DWORD disposition, DWORD flags, bool& valid) noexcept {
    valid = true;
    int open_flags = O_CLOEXEC;
    const bool read = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0u;
    const bool write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0u;
    open_flags |= write ? (read ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: open_flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: open_flags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: open_flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: open_flags |= O_TRUNC; break;
    default: valid = false; break;
    }
    if ((flags & FILE_FLAG_OPEN_REPARSE_POINT) != 0u) {
        open_flags |= O_SYMLINK;
    }
    return open_flags;
}

bool register_share(FileObject& file, DWORD share, const std::string& stream) {
    struct stat status {};
    if (fstat(file.fd, &status) != 0) return true;
    const ShareKey key{status.st_dev, status.st_ino, stream};
    std::lock_guard lock(g_share_mutex);
    std::vector<ShareEntry>& entries = g_shares[key];
    if (share_conflict(entries, file.access, share)) {
        if (entries.empty()) g_shares.erase(key);
        return false;
    }
    entries.push_back(ShareEntry{&file, file.access, share});
    file.share_key = key;
    return true;
}

HANDLE create_stream_handle(const StreamPath& target, DWORD access, DWORD share, DWORD disposition, DWORD flags) noexcept {
    const bool write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0u;
    // The base file must exist; creating a stream on a missing file creates the file, as on NTFS.
    int fd = open(target.base.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && disposition != OPEN_EXISTING && disposition != TRUNCATE_EXISTING) {
        fd = open(target.base.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
        set_last_error_from_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    auto* file = new FileObject();
    file->fd = fd;
    file->path = target.base;
    file->access = access;
    file->flags = flags;
    file->stream = true;
    file->attribute = stream_attribute(target.stream);
    if (!register_share(*file, share, target.stream)) {
        file->close();
        delete file;
        SetLastError(ERROR_SHARING_VIOLATION);
        return INVALID_HANDLE_VALUE;
    }
    const ssize_t size = fgetxattr(fd, file->attribute.c_str(), nullptr, 0, 0, 0);
    const bool exists = size >= 0;
    if (!exists && errno != ENOATTR) {
        set_last_error_from_errno(errno);
        file->close();
        delete file;
        return INVALID_HANDLE_VALUE;
    }
    if ((disposition == CREATE_NEW && exists) || ((disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING) && !exists)) {
        SetLastError(exists ? ERROR_FILE_EXISTS : ERROR_FILE_NOT_FOUND);
        file->close();
        delete file;
        return INVALID_HANDLE_VALUE;
    }
    const bool truncate = disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING;
    if (exists && !truncate) {
        file->data.resize(static_cast<std::size_t>(size));
        if (size > 0 && fgetxattr(fd, file->attribute.c_str(), file->data.data(), file->data.size(), 0, 0) != size) {
            set_last_error_from_errno(errno);
            file->close();
            delete file;
            return INVALID_HANDLE_VALUE;
        }
    }
    file->dirty = write && (!exists || truncate);
    if (file->dirty && !file->flush_stream()) {
        file->close();
        delete file;
        return INVALID_HANDLE_VALUE;
    }
    SetLastError(exists && (disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return file;
}

HANDLE create_file(const std::string& path, DWORD access, DWORD share, DWORD disposition, DWORD flags) noexcept {
    const StreamPath target = split_stream_path(path);
    if (!target.stream.empty()) {
        return create_stream_handle(target, access, share, disposition, flags);
    }
    bool valid = false;
    int open_flags = open_flags_for(access, disposition, flags, valid);
    if (!valid || path.empty()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    struct stat before {};
    const bool existed = lstat(path.c_str(), &before) == 0;
    const bool directory = existed && S_ISDIR(before.st_mode);
    if (directory) {
        if ((flags & FILE_FLAG_BACKUP_SEMANTICS) == 0u) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    }
    const int fd = open(path.c_str(), open_flags, 0644);
    if (fd < 0) {
        const int error = errno;
        if (error == EEXIST && disposition == CREATE_NEW) {
            SetLastError(ERROR_FILE_EXISTS);
        } else if (error == ENOENT && !std::filesystem::exists(std::filesystem::path(path).parent_path())) {
            SetLastError(ERROR_PATH_NOT_FOUND);
        } else {
            set_last_error_from_errno(error);
        }
        return INVALID_HANDLE_VALUE;
    }
    auto* file = new FileObject();
    file->fd = fd;
    file->path = path;
    file->access = access;
    file->flags = flags;
    file->directory = directory;
    if (!register_share(*file, share, {})) {
        file->close();
        delete file;
        if (!existed) (void)unlink(path.c_str());
        SetLastError(ERROR_SHARING_VIOLATION);
        return INVALID_HANDLE_VALUE;
    }
    file->write_through = (flags & FILE_FLAG_WRITE_THROUGH) != 0u;
    file->delete_on_close = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0u;
    SetLastError(existed && (disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return file;
}

std::map<const void*, std::size_t>& mapping_sizes() {
    static std::map<const void*, std::size_t> sizes;
    return sizes;
}
std::mutex g_mapping_mutex;

struct MappingObject final : galaxy::compat::KernelObject {
    MappingObject() : KernelObject(ObjectKind::FileMapping) {}
    int fd = -1;            // file or shared-memory descriptor; -1 for an anonymous mapping
    bool owns_fd = false;
    std::uint64_t size = 0;
    bool writable = false;
    bool close() noexcept override {
        if (owns_fd && fd >= 0) ::close(fd);
        fd = -1;
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------------------------

HANDLE CreateFileW(const char* path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD flags,
    HANDLE) noexcept {
    return create_file(host_path(path), access, share, disposition, flags);
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
    DWORD flags, HANDLE template_file) noexcept {
    return CreateFileW(host_path(path).c_str(), access, share, attributes, disposition, flags, template_file);
}

HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
    DWORD flags, HANDLE template_file) noexcept {
    return CreateFileW(path, access, share, attributes, disposition, flags, template_file);
}

BOOL ReadFile(HANDLE handle, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::uint64_t transferred = 0;
    if (file->stream) {
        const std::uint64_t offset = overlapped != nullptr
            ? (static_cast<std::uint64_t>(overlapped->OffsetHigh) << 32u) | overlapped->Offset : file->position;
        if (offset < file->data.size()) {
            transferred = std::min<std::uint64_t>(size, file->data.size() - offset);
            std::memcpy(buffer, file->data.data() + offset, transferred);
        }
        if (overlapped == nullptr) file->position = offset + transferred;
    } else {
        auto* bytes = static_cast<std::byte*>(buffer);
        while (transferred < size) {
            const ssize_t result = overlapped != nullptr
                ? pread(file->fd, bytes + transferred, size - transferred,
                      static_cast<off_t>(((static_cast<std::uint64_t>(overlapped->OffsetHigh) << 32u) | overlapped->Offset) + transferred))
                : ::read(file->fd, bytes + transferred, size - transferred);
            if (result < 0) {
                if (errno == EINTR) continue;
                set_last_error_from_errno(errno);
                if (read != nullptr) *read = static_cast<DWORD>(transferred);
                return FALSE;
            }
            if (result == 0) break;
            transferred += static_cast<std::uint64_t>(result);
        }
    }
    if (read != nullptr) *read = static_cast<DWORD>(transferred);
    if (overlapped != nullptr) {
        // Completed synchronously; GetOverlappedResult reports it.
        overlapped->Internal = 0;
        overlapped->InternalHigh = transferred;
        if (overlapped->hEvent != nullptr) SetEvent(overlapped->hEvent);
    }
    return TRUE;
}

BOOL WriteFile(HANDLE handle, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    std::uint64_t transferred = 0;
    if (file->stream) {
        const std::uint64_t offset = overlapped != nullptr
            ? (static_cast<std::uint64_t>(overlapped->OffsetHigh) << 32u) | overlapped->Offset : file->position;
        if (file->data.size() < offset + size) file->data.resize(offset + size);
        std::memcpy(file->data.data() + offset, buffer, size);
        transferred = size;
        if (overlapped == nullptr) file->position = offset + size;
        file->dirty = true;
        if (file->write_through && !file->flush_stream()) return FALSE;
    } else {
        const auto* bytes = static_cast<const std::byte*>(buffer);
        while (transferred < size) {
            const ssize_t result = overlapped != nullptr
                ? pwrite(file->fd, bytes + transferred, size - transferred,
                      static_cast<off_t>(((static_cast<std::uint64_t>(overlapped->OffsetHigh) << 32u) | overlapped->Offset) + transferred))
                : ::write(file->fd, bytes + transferred, size - transferred);
            if (result < 0) {
                if (errno == EINTR) continue;
                set_last_error_from_errno(errno);
                if (written != nullptr) *written = static_cast<DWORD>(transferred);
                return FALSE;
            }
            transferred += static_cast<std::uint64_t>(result);
        }
        if (file->write_through && !fsync_full(file->fd)) {
            set_last_error_from_errno(errno);
            return FALSE;
        }
    }
    if (written != nullptr) *written = static_cast<DWORD>(transferred);
    if (overlapped != nullptr) {
        overlapped->Internal = 0;
        overlapped->InternalHigh = transferred;
        if (overlapped->hEvent != nullptr) SetEvent(overlapped->hEvent);
    }
    return TRUE;
}

BOOL GetOverlappedResult(HANDLE, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL) noexcept {
    if (overlapped == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (transferred != nullptr) *transferred = static_cast<DWORD>(overlapped->InternalHigh);
    if (overlapped->Internal != 0) {
        SetLastError(static_cast<DWORD>(overlapped->Internal));
        return FALSE;
    }
    return TRUE;
}

BOOL CancelIoEx(HANDLE, LPOVERLAPPED) noexcept {
    // Every operation completes synchronously, so there is never anything to cancel.
    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

BOOL CancelIo(HANDLE) noexcept { return TRUE; }

BOOL FlushFileBuffers(HANDLE handle) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (file->stream && !file->flush_stream()) return FALSE;
    if (!fsync_full(file->fd)) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL SetFilePointerEx(HANDLE handle, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (file->stream) {
        const std::int64_t base = method == FILE_BEGIN ? 0
            : method == FILE_CURRENT ? static_cast<std::int64_t>(file->position) : static_cast<std::int64_t>(file->data.size());
        const std::int64_t target = base + distance.QuadPart;
        if (target < 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        file->position = static_cast<std::uint64_t>(target);
        if (position != nullptr) position->QuadPart = target;
        return TRUE;
    }
    const int whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    const off_t result = lseek(file->fd, static_cast<off_t>(distance.QuadPart), whence);
    if (result < 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    if (position != nullptr) position->QuadPart = result;
    return TRUE;
}

BOOL SetEndOfFile(HANDLE handle) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (file->stream) {
        file->data.resize(file->position);
        file->dirty = true;
        return TRUE;
    }
    const off_t position = lseek(file->fd, 0, SEEK_CUR);
    if (position < 0 || ftruncate(file->fd, position) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE handle, PLARGE_INTEGER size) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (file->stream) {
        size->QuadPart = static_cast<LONGLONG>(file->data.size());
        return TRUE;
    }
    struct stat status {};
    if (fstat(file->fd, &status) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    size->QuadPart = status.st_size;
    return TRUE;
}

DWORD GetFileType(HANDLE handle) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr) return FILE_TYPE_UNKNOWN;
    struct stat status {};
    if (fstat(file->fd, &status) != 0) return FILE_TYPE_UNKNOWN;
    if (S_ISCHR(status.st_mode)) return FILE_TYPE_CHAR;
    if (S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode)) return FILE_TYPE_PIPE;
    return FILE_TYPE_DISK;
}

BOOL GetFileInformationByHandle(HANDLE handle, LPBY_HANDLE_FILE_INFORMATION information) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr || information == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    struct stat status {};
    if (fstat(file->fd, &status) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    *information = {};
    information->dwFileAttributes = attributes_from_stat(status);
    information->ftCreationTime = filetime_from_timespec(status.st_birthtimespec);
    information->ftLastAccessTime = filetime_from_timespec(status.st_atimespec);
    information->ftLastWriteTime = filetime_from_timespec(status.st_mtimespec);
    information->dwVolumeSerialNumber = static_cast<DWORD>(status.st_dev);
    const std::uint64_t size = file->stream ? file->data.size() : static_cast<std::uint64_t>(status.st_size);
    information->nFileSizeHigh = static_cast<DWORD>(size >> 32u);
    information->nFileSizeLow = static_cast<DWORD>(size);
    // APFS counts a directory's subdirectories in st_nlink; NTFS reports one link per directory.
    information->nNumberOfLinks = S_ISDIR(status.st_mode) ? 1u : static_cast<DWORD>(status.st_nlink);
    information->nFileIndexHigh = static_cast<DWORD>(static_cast<std::uint64_t>(status.st_ino) >> 32u);
    information->nFileIndexLow = static_cast<DWORD>(status.st_ino);
    return TRUE;
}

BOOL GetFileInformationByHandleEx(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS information_class, LPVOID information,
    DWORD size) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr || information == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    switch (information_class) {
    case FileStandardInfo: {
        if (size < sizeof(FILE_STANDARD_INFO)) break;
        struct stat status {};
        if (fstat(file->fd, &status) != 0) {
            set_last_error_from_errno(errno);
            return FALSE;
        }
        auto* standard = static_cast<FILE_STANDARD_INFO*>(information);
        *standard = {};
        standard->EndOfFile.QuadPart = file->stream ? static_cast<LONGLONG>(file->data.size()) : status.st_size;
        standard->AllocationSize.QuadPart = static_cast<LONGLONG>(status.st_blocks) * 512;
        standard->NumberOfLinks = S_ISDIR(status.st_mode) ? 1u : static_cast<DWORD>(status.st_nlink);
        standard->DeletePending = file->delete_on_close;
        standard->Directory = S_ISDIR(status.st_mode);
        return TRUE;
    }
    case FileCaseSensitiveInfo: {
        if (size < sizeof(FILE_CASE_SENSITIVE_INFO)) break;
        // macOS case sensitivity is a property of the volume.
        const long sensitive = fpathconf(file->fd, _PC_CASE_SENSITIVE);
        static_cast<FILE_CASE_SENSITIVE_INFO*>(information)->Flags = sensitive == 1 ? FILE_CS_FLAG_CASE_SENSITIVE_DIR : 0u;
        return TRUE;
    }
    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
}

BOOL SetFileInformationByHandle(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS information_class, LPVOID information,
    DWORD size) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr || information == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    switch (information_class) {
    case FileDispositionInfo:
        if (size < sizeof(FILE_DISPOSITION_INFO)) break;
        if ((file->access & (DELETE | GENERIC_ALL)) == 0u) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        file->delete_on_close = static_cast<FILE_DISPOSITION_INFO*>(information)->DeleteFile != 0;
        return TRUE;
    case FileEndOfFileInfo: {
        if (size < sizeof(FILE_END_OF_FILE_INFO)) break;
        const LONGLONG end = static_cast<FILE_END_OF_FILE_INFO*>(information)->EndOfFile.QuadPart;
        if (file->stream) {
            file->data.resize(static_cast<std::size_t>(end));
            file->dirty = true;
            return TRUE;
        }
        if (ftruncate(file->fd, static_cast<off_t>(end)) != 0) {
            set_last_error_from_errno(errno);
            return FALSE;
        }
        return TRUE;
    }
    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
}

DWORD GetFinalPathNameByHandleW(HANDLE handle, LPWSTR buffer, DWORD size, DWORD) noexcept {
    FileObject* const file = file_object(handle);
    char path[MAXPATHLEN]{};
    if (file == nullptr || fcntl(file->fd, F_GETPATH, path) != 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    const std::wstring wide = wide_from_utf8(path);
    if (size <= wide.size()) return static_cast<DWORD>(wide.size() + 1u);
    std::wmemcpy(buffer, wide.c_str(), wide.size() + 1u);
    return static_cast<DWORD>(wide.size());
}

BOOL SetFileTime(HANDLE handle, const FILETIME* creation, const FILETIME* access, const FILETIME* write) noexcept {
    FileObject* const file = file_object(handle);
    if (file == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    (void)creation;
    const auto to_timespec = [](const FILETIME* time) {
        timespec value{0, UTIME_OMIT};
        if (time != nullptr) {
            constexpr std::uint64_t kUnixEpochIn100ns = 116444736000000000ull;
            const std::uint64_t ticks = (static_cast<std::uint64_t>(time->dwHighDateTime) << 32u) | time->dwLowDateTime;
            const std::uint64_t unix_ticks = ticks > kUnixEpochIn100ns ? ticks - kUnixEpochIn100ns : 0u;
            value.tv_sec = static_cast<time_t>(unix_ticks / 10'000'000ull);
            value.tv_nsec = static_cast<long>((unix_ticks % 10'000'000ull) * 100u);
        }
        return value;
    };
    const timespec times[2] = {to_timespec(access), to_timespec(write)};
    if (futimens(file->fd, times) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL GetFileTime(HANDLE handle, LPFILETIME creation, LPFILETIME access, LPFILETIME write) noexcept {
    FileObject* const file = file_object(handle);
    struct stat status {};
    if (file == nullptr || fstat(file->fd, &status) != 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (creation != nullptr) *creation = filetime_from_timespec(status.st_birthtimespec);
    if (access != nullptr) *access = filetime_from_timespec(status.st_atimespec);
    if (write != nullptr) *write = filetime_from_timespec(status.st_mtimespec);
    return TRUE;
}

// ---------------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------------

BOOL DeleteFileW(const char* raw_path) noexcept {
    const StreamPath target = split_stream_path(host_path(raw_path));
    if (!target.stream.empty()) {
        if (removexattr(target.base.c_str(), stream_attribute(target.stream).c_str(), XATTR_NOFOLLOW) != 0) {
            set_last_error_from_errno(errno);
            return FALSE;
        }
        return TRUE;
    }
    struct stat status {};
    if (lstat(target.base.c_str(), &status) == 0 && S_ISDIR(status.st_mode)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (delete_blocked(target.base)) {
        SetLastError(ERROR_SHARING_VIOLATION);
        return FALSE;
    }
    if (unlink(target.base.c_str()) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL DeleteFileW(LPCWSTR path) noexcept { return DeleteFileW(host_path(path).c_str()); }

BOOL RemoveDirectoryW(const char* raw_path) noexcept {
    if (rmdir(host_path(raw_path).c_str()) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL RemoveDirectoryW(LPCWSTR path) noexcept { return RemoveDirectoryW(host_path(path).c_str()); }

BOOL CreateDirectoryW(const char* raw_path, LPSECURITY_ATTRIBUTES) noexcept {
    const std::string path = host_path(raw_path);
    if (mkdir(path.c_str(), 0755) != 0) {
        const int error = errno;
        SetLastError(error == EEXIST ? ERROR_ALREADY_EXISTS : win32_error_from_errno(error));
        if (error == ENOENT) SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES attributes) noexcept {
    return CreateDirectoryW(host_path(path).c_str(), attributes);
}

BOOL MoveFileExW(const char* raw_source, const char* raw_destination, DWORD flags) noexcept {
    const std::string source = host_path(raw_source);
    const std::string destination = host_path(raw_destination);
    if (!split_stream_path(source).stream.empty() || !split_stream_path(destination).stream.empty()) {
        unsupported("MoveFileExW (alternate data stream)");
        return FALSE;
    }
    if (delete_blocked(source) || ((flags & MOVEFILE_REPLACE_EXISTING) != 0u && delete_blocked(destination))) {
        SetLastError(ERROR_SHARING_VIOLATION);
        return FALSE;
    }
    const unsigned rename_flags = (flags & MOVEFILE_REPLACE_EXISTING) != 0u ? 0u : RENAME_EXCL;
    if (renamex_np(source.c_str(), destination.c_str(), rename_flags) != 0) {
        const int error = errno;
        SetLastError(error == EEXIST ? ERROR_ALREADY_EXISTS : win32_error_from_errno(error));
        return FALSE;
    }
    if ((flags & MOVEFILE_WRITE_THROUGH) != 0u) {
        sync_directory_of(destination);
        if (std::filesystem::path(source).parent_path() != std::filesystem::path(destination).parent_path()) {
            sync_directory_of(source);
        }
    }
    return TRUE;
}

BOOL MoveFileExW(LPCWSTR source, LPCWSTR destination, DWORD flags) noexcept {
    return MoveFileExW(host_path(source).c_str(), host_path(destination).c_str(), flags);
}

BOOL MoveFileW(LPCWSTR source, LPCWSTR destination) noexcept { return MoveFileExW(source, destination, 0u); }

BOOL CopyFileW(LPCWSTR source, LPCWSTR destination, BOOL fail_if_exists) noexcept {
    const std::string from = host_path(source);
    const std::string to = host_path(destination);
    if (fail_if_exists != FALSE && access(to.c_str(), F_OK) == 0) {
        SetLastError(ERROR_FILE_EXISTS);
        return FALSE;
    }
    if (copyfile(from.c_str(), to.c_str(), nullptr, COPYFILE_ALL) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL CreateHardLinkW(const char* link_path, const char* existing, LPSECURITY_ATTRIBUTES) noexcept {
    if (link(host_path(existing).c_str(), host_path(link_path).c_str()) != 0) {
        const int error = errno;
        SetLastError(error == EEXIST ? ERROR_ALREADY_EXISTS : win32_error_from_errno(error));
        return FALSE;
    }
    return TRUE;
}

BOOL CreateHardLinkW(LPCWSTR link_path, LPCWSTR existing, LPSECURITY_ATTRIBUTES attributes) noexcept {
    return CreateHardLinkW(host_path(link_path).c_str(), host_path(existing).c_str(), attributes);
}

DWORD GetFileAttributesW(const char* raw_path) noexcept {
    const StreamPath target = split_stream_path(host_path(raw_path));
    struct stat status {};
    if (lstat(target.base.c_str(), &status) != 0) {
        const int error = errno;
        SetLastError(error == ENOENT && !std::filesystem::exists(std::filesystem::path(target.base).parent_path())
            ? ERROR_PATH_NOT_FOUND : win32_error_from_errno(error));
        return INVALID_FILE_ATTRIBUTES;
    }
    if (!target.stream.empty() &&
        getxattr(target.base.c_str(), stream_attribute(target.stream).c_str(), nullptr, 0, 0, XATTR_NOFOLLOW) < 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return attributes_from_stat(status);
}

DWORD GetFileAttributesW(LPCWSTR path) noexcept { return GetFileAttributesW(host_path(path).c_str()); }
DWORD GetFileAttributesA(LPCSTR path) noexcept { return GetFileAttributesW(path); }

BOOL GetFileAttributesExW(const char* raw_path, GET_FILEEX_INFO_LEVELS level, LPVOID information) noexcept {
    if (level != GetFileExInfoStandard || information == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const StreamPath target = split_stream_path(host_path(raw_path));
    struct stat status {};
    if (lstat(target.base.c_str(), &status) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    std::uint64_t size = static_cast<std::uint64_t>(status.st_size);
    if (!target.stream.empty()) {
        const ssize_t stream_size =
            getxattr(target.base.c_str(), stream_attribute(target.stream).c_str(), nullptr, 0, 0, XATTR_NOFOLLOW);
        if (stream_size < 0) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
        size = static_cast<std::uint64_t>(stream_size);
    }
    auto* data = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(information);
    data->dwFileAttributes = attributes_from_stat(status);
    data->ftCreationTime = filetime_from_timespec(status.st_birthtimespec);
    data->ftLastAccessTime = filetime_from_timespec(status.st_atimespec);
    data->ftLastWriteTime = filetime_from_timespec(status.st_mtimespec);
    data->nFileSizeHigh = static_cast<DWORD>(size >> 32u);
    data->nFileSizeLow = static_cast<DWORD>(size);
    return TRUE;
}

BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID information) noexcept {
    return GetFileAttributesExW(host_path(path).c_str(), level, information);
}

BOOL SetFileAttributesW(LPCWSTR path, DWORD attributes) noexcept {
    const std::string target = host_path(path);
    struct stat status {};
    if (stat(target.c_str(), &status) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    mode_t mode = status.st_mode & 07777;
    mode = (attributes & FILE_ATTRIBUTE_READONLY) != 0u ? (mode & ~mode_t{0222}) : (mode | S_IWUSR);
    if (chmod(target.c_str(), mode) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL GetVolumePathNameW(const char* raw_path, LPWSTR volume, DWORD size) noexcept {
    std::string path = host_path(raw_path);
    struct statfs info {};
    // The path may not exist yet; its nearest existing ancestor is on the same volume.
    std::filesystem::path probe(split_stream_path(path).base);
    while (statfs(probe.c_str(), &info) != 0) {
        if (!probe.has_parent_path() || probe.parent_path() == probe) {
            set_last_error_from_errno(errno);
            return FALSE;
        }
        probe = probe.parent_path();
    }
    std::wstring root = wide_from_utf8(info.f_mntonname);
    if (root.empty() || root.back() != L'/') root.push_back(L'/');
    if (size <= root.size()) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    std::wmemcpy(volume, root.c_str(), root.size() + 1u);
    return TRUE;
}

BOOL GetVolumePathNameW(LPCWSTR path, LPWSTR volume, DWORD size) noexcept {
    return GetVolumePathNameW(host_path(path).c_str(), volume, size);
}

BOOL GetVolumeInformationW(LPCWSTR root, LPWSTR name, DWORD name_size, LPDWORD serial, LPDWORD max_component,
    LPDWORD flags, LPWSTR file_system, DWORD file_system_size) noexcept {
    const std::string path = host_path(root);
    struct statfs info {};
    struct stat status {};
    if (statfs(path.c_str(), &info) != 0 || stat(path.c_str(), &status) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    if (name != nullptr && name_size > 0u) name[0] = L'\0';
    // Matches BY_HANDLE_FILE_INFORMATION::dwVolumeSerialNumber of files on this volume.
    if (serial != nullptr) *serial = static_cast<DWORD>(status.st_dev);
    if (max_component != nullptr) *max_component = static_cast<DWORD>(pathconf(path.c_str(), _PC_NAME_MAX));
    if (flags != nullptr) {
        DWORD value = FILE_CASE_PRESERVED_NAMES | FILE_SUPPORTS_HARD_LINKS | FILE_SUPPORTS_SPARSE_FILES;
        if (pathconf(path.c_str(), _PC_CASE_SENSITIVE) == 1) value |= FILE_CASE_SENSITIVE_SEARCH;
        if (pathconf(path.c_str(), _PC_XATTR_SIZE_BITS) > 0) value |= FILE_NAMED_STREAMS;
        *flags = value;
    }
    if (file_system != nullptr && file_system_size > 0u) {
        const std::wstring type = wide_from_utf8(info.f_fstypename);
        const std::size_t copied = std::min<std::size_t>(type.size(), file_system_size - 1u);
        std::wmemcpy(file_system, type.c_str(), copied);
        file_system[copied] = L'\0';
    }
    return TRUE;
}

// ---------------------------------------------------------------------------------------------
// Stream and file enumeration
// ---------------------------------------------------------------------------------------------

namespace {

struct StreamSearchObject final : galaxy::compat::KernelObject {
    StreamSearchObject() : KernelObject(ObjectKind::StreamSearch) {}
    std::vector<WIN32_FIND_STREAM_DATA> entries;
    std::size_t next = 0;
};

struct FileSearchObject final : galaxy::compat::KernelObject {
    FileSearchObject() : KernelObject(ObjectKind::FileSearch) {}
    std::vector<WIN32_FIND_DATAW> entries;
    std::size_t next = 0;
};

void set_stream_name(WIN32_FIND_STREAM_DATA& entry, const std::wstring& name) {
    const std::size_t copied = std::min<std::size_t>(name.size(), std::size(entry.cStreamName) - 1u);
    std::wmemcpy(entry.cStreamName, name.c_str(), copied);
    entry.cStreamName[copied] = L'\0';
}

}  // namespace

HANDLE FindFirstStreamW(const char* raw_path, STREAM_INFO_LEVELS level, LPVOID data, DWORD) noexcept {
    if (level != FindStreamInfoStandard || data == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    const std::string path = host_path(raw_path);
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0) {
        set_last_error_from_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    auto* search = new StreamSearchObject();
    if (S_ISREG(status.st_mode)) {
        WIN32_FIND_STREAM_DATA entry{};
        entry.StreamSize.QuadPart = status.st_size;
        set_stream_name(entry, L"::$DATA");
        search->entries.push_back(entry);
    }
    const ssize_t names_size = listxattr(path.c_str(), nullptr, 0, XATTR_NOFOLLOW);
    if (names_size > 0) {
        std::vector<char> names(static_cast<std::size_t>(names_size));
        const ssize_t listed = listxattr(path.c_str(), names.data(), names.size(), XATTR_NOFOLLOW);
        for (ssize_t offset = 0; offset < listed;) {
            const std::string attribute(names.data() + offset);
            offset += static_cast<ssize_t>(attribute.size()) + 1;
            if (!attribute.starts_with(kStreamAttributePrefix)) continue;
            WIN32_FIND_STREAM_DATA entry{};
            entry.StreamSize.QuadPart = getxattr(path.c_str(), attribute.c_str(), nullptr, 0, 0, XATTR_NOFOLLOW);
            set_stream_name(entry, L":" + wide_from_utf8(attribute.substr(kStreamAttributePrefix.size())) + L":$DATA");
            search->entries.push_back(entry);
        }
    }
    if (search->entries.empty()) {
        delete search;
        SetLastError(ERROR_HANDLE_EOF);
        return INVALID_HANDLE_VALUE;
    }
    *static_cast<WIN32_FIND_STREAM_DATA*>(data) = search->entries[0];
    search->next = 1;
    return search;
}

HANDLE FindFirstStreamW(LPCWSTR path, STREAM_INFO_LEVELS level, LPVOID data, DWORD flags) noexcept {
    return FindFirstStreamW(host_path(path).c_str(), level, data, flags);
}

BOOL FindNextStreamW(HANDLE handle, LPVOID data) noexcept {
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr || object->kind != ObjectKind::StreamSearch) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    auto* search = static_cast<StreamSearchObject*>(object);
    if (search->next >= search->entries.size()) {
        SetLastError(ERROR_HANDLE_EOF);
        return FALSE;
    }
    *static_cast<WIN32_FIND_STREAM_DATA*>(data) = search->entries[search->next++];
    return TRUE;
}

HANDLE FindFirstFileW(LPCWSTR raw_pattern, WIN32_FIND_DATAW* data) noexcept {
    const std::filesystem::path pattern(host_path(raw_pattern));
    const std::string directory = pattern.has_parent_path() ? pattern.parent_path().string() : ".";
    const std::string mask = pattern.filename().string();
    DIR* const stream = opendir(directory.c_str());
    if (stream == nullptr) {
        set_last_error_from_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    auto* search = new FileSearchObject();
    while (dirent* entry = readdir(stream)) {
        if (fnmatch(mask.c_str(), entry->d_name, 0) != 0) continue;
        struct stat status {};
        const std::string full = directory + "/" + entry->d_name;
        if (lstat(full.c_str(), &status) != 0) continue;
        WIN32_FIND_DATAW found{};
        found.dwFileAttributes = attributes_from_stat(status);
        found.ftCreationTime = filetime_from_timespec(status.st_birthtimespec);
        found.ftLastAccessTime = filetime_from_timespec(status.st_atimespec);
        found.ftLastWriteTime = filetime_from_timespec(status.st_mtimespec);
        found.nFileSizeHigh = static_cast<DWORD>(static_cast<std::uint64_t>(status.st_size) >> 32u);
        found.nFileSizeLow = static_cast<DWORD>(status.st_size);
        const std::wstring name = wide_from_utf8(entry->d_name);
        const std::size_t copied = std::min<std::size_t>(name.size(), MAX_PATH - 1u);
        std::wmemcpy(found.cFileName, name.c_str(), copied);
        found.cFileName[copied] = L'\0';
        search->entries.push_back(found);
    }
    closedir(stream);
    if (search->entries.empty()) {
        delete search;
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    *data = search->entries[0];
    search->next = 1;
    return search;
}

BOOL FindNextFileW(HANDLE handle, WIN32_FIND_DATAW* data) noexcept {
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr || object->kind != ObjectKind::FileSearch) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    auto* search = static_cast<FileSearchObject*>(object);
    if (search->next >= search->entries.size()) {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    *data = search->entries[search->next++];
    return TRUE;
}

BOOL FindClose(HANDLE handle) noexcept { return CloseHandle(handle); }

DWORD GetTempPathW(DWORD size, LPWSTR buffer) noexcept {
    const char* tmp = std::getenv("TMPDIR");
    std::wstring path = wide_from_utf8(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp/");
    if (path.back() != L'/') path.push_back(L'/');
    if (size <= path.size()) return static_cast<DWORD>(path.size() + 1u);
    std::wmemcpy(buffer, path.c_str(), path.size() + 1u);
    return static_cast<DWORD>(path.size());
}

DWORD GetFullPathNameW(LPCWSTR path, DWORD size, LPWSTR buffer, LPWSTR* file_part) noexcept {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(host_path(path), error);
    if (error) {
        SetLastError(ERROR_INVALID_NAME);
        return 0;
    }
    const std::wstring wide = wide_from_utf8(absolute.lexically_normal().string());
    if (size <= wide.size()) return static_cast<DWORD>(wide.size() + 1u);
    std::wmemcpy(buffer, wide.c_str(), wide.size() + 1u);
    if (file_part != nullptr) {
        const std::size_t slash = wide.find_last_of(L'/');
        *file_part = slash == std::wstring::npos ? buffer : buffer + slash + 1u;
    }
    return static_cast<DWORD>(wide.size());
}

// ---------------------------------------------------------------------------------------------
// File mappings and virtual memory
// ---------------------------------------------------------------------------------------------

HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD protect, DWORD size_high, DWORD size_low,
    LPCWSTR name) noexcept {
    auto* mapping = new MappingObject();
    mapping->size = (static_cast<std::uint64_t>(size_high) << 32u) | size_low;
    mapping->writable = (protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0u;
    if (file == INVALID_HANDLE_VALUE) {
        if (name != nullptr && *name != L'\0') {
            // POSIX shared-memory names are short on macOS (31 bytes): use a hash of the name.
            const std::size_t digest = std::hash<std::wstring>{}(name);
            char shm_name[32];
            std::snprintf(shm_name, sizeof(shm_name), "/nebula.%016zx", digest);
            const int fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
            if (fd < 0 || ftruncate(fd, static_cast<off_t>(mapping->size)) != 0) {
                set_last_error_from_errno(errno);
                if (fd >= 0) ::close(fd);
                delete mapping;
                return nullptr;
            }
            mapping->fd = fd;
            mapping->owns_fd = true;
        }
    } else {
        FileObject* const source = file_object(file);
        if (source == nullptr || source->stream) {
            SetLastError(ERROR_INVALID_HANDLE);
            delete mapping;
            return nullptr;
        }
        mapping->fd = dup(source->fd);
        mapping->owns_fd = true;
        if (mapping->size == 0) {
            struct stat status {};
            fstat(mapping->fd, &status);
            mapping->size = static_cast<std::uint64_t>(status.st_size);
        }
    }
    SetLastError(ERROR_SUCCESS);
    return mapping;
}

HANDLE OpenFileMappingW(DWORD access, BOOL, LPCWSTR name) noexcept {
    if (name == nullptr || *name == L'\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const std::size_t digest = std::hash<std::wstring>{}(name);
    char shm_name[32];
    std::snprintf(shm_name, sizeof(shm_name), "/nebula.%016zx", digest);
    const bool write = (access & (FILE_MAP_WRITE | FILE_MAP_ALL_ACCESS)) != 0u;
    const int fd = shm_open(shm_name, write ? O_RDWR : O_RDONLY, 0);
    if (fd < 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return nullptr;
    }
    struct stat status {};
    fstat(fd, &status);
    auto* mapping = new MappingObject();
    mapping->fd = fd;
    mapping->owns_fd = true;
    mapping->size = static_cast<std::uint64_t>(status.st_size);
    mapping->writable = write;
    return mapping;
}

LPVOID MapViewOfFile(HANDLE handle, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T size) noexcept {
    KernelObject* const object = kernel_object(handle);
    if (object == nullptr || object->kind != ObjectKind::FileMapping) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    auto* mapping = static_cast<MappingObject*>(object);
    const std::uint64_t offset = (static_cast<std::uint64_t>(offset_high) << 32u) | offset_low;
    const std::size_t length = size != 0 ? size : static_cast<std::size_t>(mapping->size - offset);
    const bool write = (access & (FILE_MAP_WRITE | FILE_MAP_ALL_ACCESS)) != 0u && mapping->writable;
    void* const view = mmap(nullptr, length, PROT_READ | (write ? PROT_WRITE : 0),
        mapping->fd >= 0 ? MAP_SHARED : (MAP_SHARED | MAP_ANON), mapping->fd, static_cast<off_t>(offset));
    if (view == MAP_FAILED) {
        set_last_error_from_errno(errno);
        return nullptr;
    }
    std::lock_guard lock(g_mapping_mutex);
    mapping_sizes()[view] = length;
    return view;
}

BOOL UnmapViewOfFile(LPCVOID address) noexcept {
    std::size_t length = 0;
    {
        std::lock_guard lock(g_mapping_mutex);
        const auto it = mapping_sizes().find(address);
        if (it == mapping_sizes().end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        length = it->second;
        mapping_sizes().erase(it);
    }
    return munmap(const_cast<void*>(address), length) == 0 ? TRUE : FALSE;
}

namespace {
int protection_from(DWORD protect) noexcept {
    switch (protect & 0xFFu) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}
}  // namespace

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) noexcept {
    if (address != nullptr && (type & MEM_COMMIT) != 0u && (type & MEM_RESERVE) == 0u) {
        // Commit inside an earlier reservation.
        if (mprotect(address, size, protection_from(protect)) != 0) {
            set_last_error_from_errno(errno);
            return nullptr;
        }
        return address;
    }
    const int protection = (type & MEM_COMMIT) != 0u ? protection_from(protect) : PROT_NONE;
    void* const memory = mmap(address, size, protection, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        set_last_error_from_errno(errno);
        return nullptr;
    }
    if (address != nullptr && memory != address) {
        munmap(memory, size);
        SetLastError(487u);  // ERROR_INVALID_ADDRESS
        return nullptr;
    }
    std::lock_guard lock(g_mapping_mutex);
    mapping_sizes()[memory] = size;
    return memory;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type) noexcept {
    if ((type & MEM_RELEASE) != 0u) {
        std::size_t length = 0;
        {
            std::lock_guard lock(g_mapping_mutex);
            const auto it = mapping_sizes().find(address);
            if (it == mapping_sizes().end()) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            length = it->second;
            mapping_sizes().erase(it);
        }
        return munmap(address, length) == 0 ? TRUE : FALSE;
    }
    if ((type & MEM_DECOMMIT) != 0u) {
        (void)madvise(address, size, MADV_FREE);
        return mprotect(address, size, PROT_NONE) == 0 ? TRUE : FALSE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old_protect) noexcept {
    if (old_protect != nullptr) *old_protect = PAGE_READWRITE;
    if (mprotect(address, size, protection_from(protect)) != 0) {
        set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL PrefetchVirtualMemory(HANDLE, ULONG_PTR count, WIN32_MEMORY_RANGE_ENTRY* entries, ULONG) noexcept {
    for (ULONG_PTR i = 0; i < count; ++i) {
        (void)madvise(entries[i].VirtualAddress, entries[i].NumberOfBytes, MADV_WILLNEED);
    }
    return TRUE;
}
