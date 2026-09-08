// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqjournal_io.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

namespace pqjournal { namespace io {
bool DirectorySafe(const fs::path& directory)
{
#ifdef _WIN32
    const auto path = fs::absolute(directory).wstring();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    wchar_t volume[MAX_PATH], filesystem[32];
    // Bound the Windows durability contract to local NTFS, not remote shares
    // or filesystems with unqualified replacement/metadata semantics.
    return GetVolumePathNameW(path.c_str(), volume, MAX_PATH) && GetDriveTypeW(volume) == DRIVE_FIXED &&
        GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 32) &&
        wcscmp(filesystem, L"NTFS") == 0;
#else
    struct stat info{};
    return lstat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

int Open(const fs::path& path, Mode mode)
{
    const bool lease = mode == Mode::LOCK_EXISTING || mode == Mode::LOCK_CREATE;
    const bool create = mode == Mode::CREATE || mode == Mode::LOCK_CREATE;
#ifdef _WIN32
    if (!DirectorySafe(path.parent_path())) return -1;
    const DWORD disposition = create ? CREATE_NEW : mode == Mode::REWRITE ? OPEN_ALWAYS : OPEN_EXISTING;
    const HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
        lease ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1) {
        CloseHandle(handle);
        return -1;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
        _O_RDWR | _O_BINARY | _O_NOINHERIT | (mode == Mode::APPEND || create ? _O_APPEND : 0));
    if (fd < 0) { CloseHandle(handle); return -1; }
#else
    const int flags = O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
        (create ? O_CREAT | O_EXCL : mode == Mode::REWRITE ? O_CREAT : 0) |
        (mode == Mode::APPEND || create ? O_APPEND : 0);
    const int fd = open(path.c_str(), flags, 0600);
    if (fd < 0) return -1;
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        (lease && flock(fd, LOCK_EX | LOCK_NB) != 0) ||
        (mode == Mode::REWRITE && fchmod(fd, 0600) != 0)) {
        Close(fd);
        return -1;
    }
#endif
    // Inspect the actual handle BEFORE truncating an existing temporary file.
    if (mode == Mode::REWRITE && !Truncate(fd, 0)) { Close(fd); return -1; }
    return fd;
}

void Close(int fd)
{
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

FILE* Stream(int fd, const char* mode)
{
#ifdef _WIN32
    return _fdopen(fd, mode);
#else
    return fdopen(fd, mode);
#endif
}

int Descriptor(FILE* file)
{
#ifdef _WIN32
    return _fileno(file);
#else
    return fileno(file);
#endif
}

int64_t Size(int fd)
{
#ifdef _WIN32
    return _filelengthi64(fd);
#else
    struct stat info{};
    return fstat(fd, &info) == 0 && S_ISREG(info.st_mode) ? int64_t(info.st_size) : -1;
#endif
}

bool WriteAll(int fd, const unsigned char* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
        const int count = _write(fd, data + offset, static_cast<unsigned>(std::min<size_t>(size - offset, INT_MAX)));
#else
        const ssize_t count = write(fd, data + offset, size - offset);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += size_t(count);
    }
    return true;
}

bool Sync(int fd)
{
#ifdef _WIN32
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(fd))) != 0;
#elif defined(__APPLE__) && defined(F_FULLFSYNC)
    return fcntl(fd, F_FULLFSYNC, 0) != -1;
#else
    return fsync(fd) == 0;
#endif
}

bool Truncate(int fd, size_t size)
{
#ifdef _WIN32
    return _chsize_s(fd, size) == 0;
#else
    return ftruncate(fd, size) == 0;
#endif
}

bool Replace(const fs::path& source, const fs::path& destination)
{
#ifdef _WIN32
    return MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(source.c_str(), destination.c_str()) == 0;
#endif
}

bool SyncParent(const fs::path& file)
{
#ifdef _WIN32
    return DirectorySafe(file.parent_path()); // File Sync/Replace persist NTFS metadata.
#else
    const int fd = open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    Close(fd);
    return ok;
#endif
}
} }
