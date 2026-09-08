// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <wallet/pqcredentials_win.h>
#ifdef _WIN32
#include <support/allocators/secure.h>
#include <windows.h>
#include <aclapi.h>
#include <cstdint>
#include <array>
#include <vector>

namespace pqwallet {
namespace {
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    bool Close() { const HANDLE handle = value; value = INVALID_HANDLE_VALUE; return CloseHandle(handle) != 0; }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

bool Private(HANDLE handle, PSID user, bool directory)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory ||
        (!directory && info.nNumberOfLinks != 1)) return false;
    struct Security {
        PSECURITY_DESCRIPTOR value{nullptr};
        ~Security() { if (value) LocalFree(value); }
    } descriptor;
    PSID owner = nullptr;
    PACL acl = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor.value) != ERROR_SUCCESS ||
        !owner || !IsValidSid(owner) || !EqualSid(owner, user) || !acl || !IsValidAcl(acl) || !acl->AceCount)
        return false;
    // Conservative allow-list, not an approximation of Windows AccessCheck:
    // no other principal (including groups) may read or change these secrets.
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry)) return false;
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
            (ace->Header.AceFlags & INHERIT_ONLY_ACE) || ace->Header.AceSize < sizeof(ACCESS_ALLOWED_ACE)) return false;
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (!IsValidSid(sid) || (!EqualSid(sid, user) && !IsWellKnownSid(sid, WinLocalSystemSid))) return false;
    }
    return true;
}

bool Read(const std::wstring& path, PSID user, Span<unsigned char> bytes)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    LARGE_INTEGER size{};
    if (file.value == INVALID_HANDLE_VALUE || !Private(file.value, user, false) ||
        !GetFileSizeEx(file.value, &size) || size.QuadPart != int64_t(bytes.size())) return false;
    DWORD count = 0;
    // No writer or deletion can share this handle. Fixed sizes bound the read.
    return ReadFile(file.value, bytes.data(), DWORD(bytes.size()), &count, nullptr) && count == bytes.size() &&
        GetFileSizeEx(file.value, &size) && size.QuadPart == int64_t(bytes.size()) && Private(file.value, user, false);
}

bool ValidPath(const std::wstring& directory)
{
    // Ordinary absolute drive paths only. Reject device/UNC/ADS and aliases
    // before any file open; ancestors must still be trusted by the operator.
    if (directory.size() < 4 || !((directory[0] >= L'A' && directory[0] <= L'Z') ||
        (directory[0] >= L'a' && directory[0] <= L'z')) || directory[1] != L':' ||
        (directory[2] != L'\\' && directory[2] != L'/') || directory.find(L'\0') != std::wstring::npos ||
        directory.find(L':', 2) != std::wstring::npos) return false;
    for (size_t begin = 3; begin < directory.size();) {
        size_t end = directory.find_first_of(L"\\/", begin);
        if (end == std::wstring::npos) end = directory.size();
        if (end == begin || directory[end - 1] == L'.' || directory[end - 1] == L' ' ||
            (end != directory.size() && end + 1 == directory.size())) return false;
        begin = end + 1;
    }
    return true;
}

bool LocalNTFS(const std::wstring& directory)
{
    wchar_t volume[MAX_PATH], filesystem[32];
    if (!GetVolumePathNameW(directory.c_str(), volume, MAX_PATH) || GetDriveTypeW(volume) != DRIVE_FIXED ||
        !GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 32) ||
        wcscmp(filesystem, L"NTFS") != 0) return false;
    return true;
}

bool CurrentUser(std::vector<unsigned char>& identity)
{
    Handle token(nullptr);
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return false;
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (!size) return false;
    identity.resize(size);
    return GetTokenInformation(token.value, TokenUser, identity.data(), size, &size) != 0;
}
}

bool ReadWindowsOperatorCredentials(const std::wstring& directory,
    Span<unsigned char> record, Span<unsigned char> wrappingKey)
{
    bool success = false;
    struct Output {
        Span<unsigned char> record, key;
        const bool& success;
        ~Output() {
            if (!success) {
                if (record.data()) SecureZeroMemory(record.data(), record.size());
                if (key.data()) SecureZeroMemory(key.data(), key.size());
            }
        }
    } output{record, wrappingKey, success};
    if (record.size() != 1385 || wrappingKey.size() != 32 || !record.data() || !wrappingKey.data() ||
        !ValidPath(directory) || !LocalNTFS(directory)) return false;
    std::vector<unsigned char> identity;
    if (!CurrentUser(identity)) return false;
    PSID user = reinterpret_cast<TOKEN_USER*>(identity.data())->User.Sid;
    Handle parent(CreateFileW(directory.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (parent.value == INVALID_HANDLE_VALUE || !Private(parent.value, user, true)) return false;
    success = Read(directory + L"\\olc-pq-operator-record", user, record) &&
        Read(directory + L"\\olc-pq-operator-key", user, wrappingKey) && Private(parent.value, user, true);
    return success;
}

bool IsPrivateWindowsConfigFile(const std::wstring& path, const std::string& expected)
{
    if (expected.size() > 1024 * 1024 || !ValidPath(path) || !LocalNTFS(path)) return false;
    std::vector<unsigned char> identity;
    if (!CurrentUser(identity)) return false;
    PSID user = reinterpret_cast<TOKEN_USER*>(identity.data())->User.Sid;
    std::vector<unsigned char, secure_allocator<unsigned char>> actual(expected.size());
    return Read(path, user, actual) &&
           std::equal(actual.begin(), actual.end(), reinterpret_cast<const unsigned char*>(expected.data()));
}

bool WriteWindowsOperatorCredentials(const std::wstring& directory,
    Span<const unsigned char> record, Span<const unsigned char> wrappingKey,
    Span<const unsigned char> random)
{
    if (record.size() != 1385 || wrappingKey.size() != 32 || random.size() != 16 ||
        !record.data() || !wrappingKey.data() || !random.data() || !ValidPath(directory)) return false;
    const auto split = directory.find_last_of(L"\\/");
    const auto parentPath = directory.substr(0, split == 2 ? 3 : split);
    if (!LocalNTFS(parentPath)) return false;
    std::vector<unsigned char> identity;
    if (!CurrentUser(identity)) return false;
    PSID user = reinterpret_cast<TOKEN_USER*>(identity.data())->User.Sid;
    Handle parent(CreateFileW(parentPath.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (parent.value == INVALID_HANDLE_VALUE || !Private(parent.value, user, true)) return false;

    // Apply a protected owner-only descriptor AT creation, not afterwards.
    struct Security {
        SECURITY_DESCRIPTOR descriptor{};
        PACL acl{nullptr};
        ~Security() { if (acl) LocalFree(acl); }
    } security;
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<wchar_t*>(user);
    if (SetEntriesInAclW(1, &access, nullptr, &security.acl) != ERROR_SUCCESS ||
        !InitializeSecurityDescriptor(&security.descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&security.descriptor, user, FALSE) ||
        !SetSecurityDescriptorDacl(&security.descriptor, TRUE, security.acl, FALSE) ||
        !SetSecurityDescriptorControl(&security.descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) return false;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &security.descriptor, FALSE};
    std::wstring staging = directory.substr(0, split + 1) + L".olc-pq-";
    for (const unsigned char byte : random) {
        staging += L"0123456789abcdef"[byte >> 4];
        staging += L"0123456789abcdef"[byte & 15];
    }
    if (!CreateDirectoryW(staging.c_str(), &attributes)) return false;
    struct Pending {
        const std::wstring& path;
        bool keep{false};
        ~Pending() {
            if (!keep) {
                DeleteFileW((path + L"\\olc-pq-operator-record").c_str());
                DeleteFileW((path + L"\\olc-pq-operator-key").c_str());
                RemoveDirectoryW(path.c_str());
            }
        }
    } pending{staging};
    Handle temporary(CreateFileW(staging.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (temporary.value == INVALID_HANDLE_VALUE || !Private(temporary.value, user, true)) return false;
    const auto write = [&](const wchar_t* name, Span<const unsigned char> bytes) {
        Handle file(CreateFileW((staging + name).c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
            FILE_SHARE_READ, &attributes, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
        DWORD written = 0;
        return file.value != INVALID_HANDLE_VALUE && Private(file.value, user, false) &&
            WriteFile(file.value, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size() &&
            FlushFileBuffers(file.value) && file.Close();
    };
    if (!write(L"\\olc-pq-operator-record", record) || !write(L"\\olc-pq-operator-key", wrappingKey)) return false;
    std::array<unsigned char, 1385> readbackRecord{};
    std::vector<unsigned char, secure_allocator<unsigned char>> readbackKey(32);
    if (!ReadWindowsOperatorCredentials(staging, readbackRecord, readbackKey) ||
        !std::equal(record.begin(), record.end(), readbackRecord.begin()) ||
        !std::equal(wrappingKey.begin(), wrappingKey.end(), readbackKey.begin()) ||
        !Private(parent.value, user, true) || !temporary.Close() || !parent.Close()) return false;
    // Same parent/local NTFS only. No replace, cross-volume copy or reboot queue.
    // Both directory read handles must close before Windows can move children;
    // their paths/ancestors must remain trusted, as for credential loading.
    if (!MoveFileExW(staging.c_str(), directory.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
    pending.keep = true;
    return true;
}
}
#endif
