// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
// Standalone native Windows check. Uses only disposable, synthetic credentials.
#ifdef NDEBUG
#error Credential checks require assertions enabled
#endif
#include <compat.h>
#include <wallet/pqcredentials_win.h>
#include <sddl.h>
#include <aclapi.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <vector>
#ifdef PQ_CREDENTIAL_CRYPTO_TEST
#include <wallet/pqkey.h>
#include <streams.h>
#endif

#ifdef PQ_CREDENTIAL_FAULT_TEST
// GNU linker interception exists only in this native test executable.
enum class Fault { NONE, SHORT_WRITE, FIRST_FLUSH, SECOND_FLUSH, MOVE };
static Fault fault = Fault::NONE;
static unsigned flushes = 0;
extern "C" {
extern decltype(&WriteFile) __real___imp_WriteFile;
extern decltype(&FlushFileBuffers) __real___imp_FlushFileBuffers;
extern decltype(&MoveFileExW) __real___imp_MoveFileExW;
}
static BOOL WINAPI FaultWrite(HANDLE file, LPCVOID bytes, DWORD size, LPDWORD written, LPOVERLAPPED overlapped)
{
    return __real___imp_WriteFile(file, bytes, fault == Fault::SHORT_WRITE ? size / 2 : size, written, overlapped);
}
static BOOL WINAPI FaultFlush(HANDLE file)
{
    if ((fault == Fault::FIRST_FLUSH || fault == Fault::SECOND_FLUSH) &&
        ++flushes == (fault == Fault::FIRST_FLUSH ? 1U : 2U)) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return __real___imp_FlushFileBuffers(file);
}
static BOOL WINAPI FaultMove(LPCWSTR source, LPCWSTR destination, DWORD flags)
{
    if (fault == Fault::MOVE) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    return __real___imp_MoveFileExW(source, destination, flags);
}
extern "C" {
decltype(&WriteFile) __wrap___imp_WriteFile = FaultWrite;
decltype(&FlushFileBuffers) __wrap___imp_FlushFileBuffers = FaultFlush;
decltype(&MoveFileExW) __wrap___imp_MoveFileExW = FaultMove;
}
#endif

static void SetACL(const std::wstring& path, const std::wstring& sddl)
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
    BOOL present, defaulted;
    PACL acl = nullptr;
    PSID owner = nullptr;
    assert(GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present);
    assert(GetSecurityDescriptorOwner(descriptor, &owner, &defaulted));
    assert(SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION | (owner ? OWNER_SECURITY_INFORMATION : 0),
        owner, nullptr, acl, nullptr) == ERROR_SUCCESS);
    LocalFree(descriptor);
}

static void Write(const std::wstring& path, Span<const unsigned char> bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    assert(WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size());
    assert(CloseHandle(file));
}

static void Write(const std::wstring& path, size_t size)
{
    std::array<unsigned char, 1386> bytes{};
    bytes.fill(42);
    assert(size <= bytes.size());
    Write(path, Span<const unsigned char>(bytes.data(), size));
}

#ifdef PQ_CREDENTIAL_CRYPTO_TEST
static void EncryptedCredentials(const std::wstring& directory)
{
    const pqwallet::SecureBytes seed(32, 11), wrapping(32, 27);
    const uint256 genesis = uint256S("1234");
    mldsa44::Key loaded;
    std::string reason;
    const auto store = [&](const pqwallet::Record& record, const pqwallet::SecureBytes& master) {
        CDataStream encoded(SER_DISK, 0);
        encoded << record;
        Write(directory + L"\\olc-pq-operator-record", Span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()));
        Write(directory + L"\\olc-pq-operator-key", master);
    };
    const auto reject = [&](const std::string& network, const uint256& chain) {
        assert(loaded.SetSeed(seed)); // A failed reload must erase an existing key.
        reason = "stale";
        assert(!pqwallet::LoadOperatorCredentials(fs::path(directory), network, chain, loaded, reason));
        assert(!loaded.IsValid() && !reason.empty());
    };
    for (const std::string network : {"regtest", "test"}) {
        pqwallet::Record record;
        assert(pqwallet::EncryptOperatorSeed(seed, wrapping, network, genesis, record));
        store(record, wrapping);
        assert(pqwallet::LoadOperatorCredentials(fs::path(directory), network, genesis, loaded, reason));
        assert(reason.empty() && loaded.GetPublicKey() == record.public_key);
        const std::array<unsigned char, 3> message{{1, 2, 3}};
        std::vector<unsigned char> signature;
        assert(loaded.Sign(message, {}, signature));
        assert(mldsa44::Verify(record.public_key, message, {}, signature));
        reject(network == "test" ? "regtest" : "test", genesis);
        reject("main", genesis);
        reject(network, uint256S("1235"));
        reject(network, uint256{});
        auto wrongKey = wrapping;
        wrongKey[0] ^= 1;
        store(record, wrongKey);
        reject(network, genesis);
        for (int field = 0; field < 4; ++field) {
            auto bad = record;
            if (field == 0) bad.version = 9;
            if (field == 1) bad.public_key.back() ^= 1;
            if (field == 2) bad.nonce.back() ^= 1;
            if (field == 3) bad.encrypted_seed.back() ^= 1;
            store(bad, wrapping);
            reject(network, genesis);
        }
        pqwallet::Record wrongDomain;
        assert(pqwallet::EncryptSeed(seed, wrapping, network, wrongDomain));
        store(wrongDomain, wrapping);
        reject(network, genesis);
        assert(pqwallet::EncryptOperatorRecovery(seed, wrapping, network, genesis, wrongDomain));
        store(wrongDomain, wrapping);
        reject(network, genesis);
        store(record, wrapping);
        assert(pqwallet::LoadOperatorCredentials(fs::path(directory), network, genesis, loaded, reason));
        assert(reason.empty() && loaded.GetPublicKey() == record.public_key);
    }
    std::cout << "Native Windows authenticated credential load/sign/rejection checks passed\n";
}

static void PublishCredentials(const std::wstring& directory, const std::wstring& privateACL)
{
    const pqwallet::SecureBytes seed(32, 11), wrapping(32, 27);
    const uint256 genesis = uint256S("1234");
    pqwallet::Record record;
    assert(pqwallet::EncryptOperatorSeed(seed, wrapping, "regtest", genesis, record));
    const auto destination = directory + L"\\published";
    const auto next = directory + L"\\next";
    std::string reason;
    assert(pqwallet::WriteOperatorCredentials(fs::path(destination), record, wrapping, "regtest", genesis, reason));
    assert(reason.empty());
    mldsa44::Key loaded;
    assert(pqwallet::LoadOperatorCredentials(fs::path(destination), "regtest", genesis, loaded, reason));
    assert(loaded.GetPublicKey() == record.public_key);
    // Final publication conflict must clean its staging files, not overwrite.
    pqwallet::Record replacement;
    assert(pqwallet::EncryptOperatorSeed(pqwallet::SecureBytes(32, 12), wrapping, "regtest", genesis, replacement));
    assert(!pqwallet::WriteOperatorCredentials(fs::path(destination), replacement, wrapping, "regtest", genesis, reason));
    assert(pqwallet::LoadOperatorCredentials(fs::path(destination), "regtest", genesis, loaded, reason));
    assert(loaded.GetPublicKey() == record.public_key);
    for (const std::string network : {"test", "main"})
        assert(!pqwallet::WriteOperatorCredentials(fs::path(next), record, wrapping, network, genesis, reason));
    assert(!pqwallet::WriteOperatorCredentials(fs::path(next), record, pqwallet::SecureBytes(32, 28), "regtest", genesis, reason));
    assert(GetFileAttributesW(next.c_str()) == INVALID_FILE_ATTRIBUTES);
#ifdef PQ_CREDENTIAL_FAULT_TEST
    for (Fault inject : {Fault::SHORT_WRITE, Fault::FIRST_FLUSH, Fault::SECOND_FLUSH, Fault::MOVE}) {
        fault = inject;
        flushes = 0;
        const bool published = pqwallet::WriteOperatorCredentials(fs::path(next), record, wrapping, "regtest", genesis, reason);
        fault = Fault::NONE;
        assert(!published && !reason.empty());
        assert(GetFileAttributesW(next.c_str()) == INVALID_FILE_ATTRIBUTES);
        assert(pqwallet::LoadOperatorCredentials(fs::path(destination), "regtest", genesis, loaded, reason));
        assert(loaded.GetPublicKey() == record.public_key);
    }
    std::cout << "Native Windows partial-write, both flush failures and move-failure checks passed\n";
#endif
    SetACL(directory, privateACL + L"(A;;FR;;;WD)");
    assert(!pqwallet::WriteOperatorCredentials(fs::path(next), record, wrapping, "regtest", genesis, reason));
    SetACL(directory, privateACL);
    assert(GetFileAttributesW(next.c_str()) == INVALID_FILE_ATTRIBUTES);
    assert(CreateDirectoryW(next.c_str(), nullptr));
    assert(!pqwallet::WriteOperatorCredentials(fs::path(next), record, wrapping, "regtest", genesis, reason));
    assert(RemoveDirectoryW(next.c_str())); // It stayed empty.
    Write(next, 1);
    assert(!pqwallet::WriteOperatorCredentials(fs::path(next), record, wrapping, "regtest", genesis, reason));
    WIN32_FILE_ATTRIBUTE_DATA info{};
    assert(GetFileAttributesExW(next.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow == 1);
    assert(DeleteFileW(next.c_str()));
    if (CreateSymbolicLinkW(next.c_str(), destination.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) ||
        CreateSymbolicLinkW(next.c_str(), destination.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        assert(!pqwallet::WriteOperatorCredentials(fs::path(next), record, wrapping, "regtest", genesis, reason));
        assert(GetFileAttributesW(next.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT);
        assert(RemoveDirectoryW(next.c_str()));
    } else {
        std::cout << "SKIP publication symlink conflict: Windows privilege unavailable\n";
    }
    for (const auto& invalid : {std::wstring(L"relative"), next + L"\\", next + L":stream", directory + L"\\..\\next"})
        assert(!pqwallet::WriteOperatorCredentials(fs::path(invalid), record, wrapping, "regtest", genesis, reason));
    assert(DeleteFileW((destination + L"\\olc-pq-operator-record").c_str()));
    assert(DeleteFileW((destination + L"\\olc-pq-operator-key").c_str()));
    assert(RemoveDirectoryW(destination.c_str()));
    WIN32_FIND_DATAW entry{};
    HANDLE scan = FindFirstFileW((directory + L"\\.olc-pq-*").c_str(), &entry);
    if (scan != INVALID_HANDLE_VALUE) FindClose(scan);
    assert(scan == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);
    std::cout << "Native Windows exclusive private credential publication checks passed\n";
}
#endif

int main()
{
    HANDLE token = nullptr;
    assert(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    assert(GetTokenInformation(token, TokenUser, user.data(), size, &size));
    assert(CloseHandle(token));
    LPWSTR sid = nullptr;
    assert(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid));
    const std::wstring privateACL = L"O:" + std::wstring(sid) + L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    wchar_t temporary[MAX_PATH], unique[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, temporary));
    assert(GetTempFileNameW(temporary, L"pqk", 0, unique));
    assert(DeleteFileW(unique));
    const std::wstring root = unique;
    assert(CreateDirectoryW(root.c_str(), nullptr));
    const std::wstring directory = root + L"\\\u017elu\u0165ou\u010dk\u00fd";
    assert(CreateDirectoryW(directory.c_str(), nullptr));
    SetACL(directory, privateACL);
    const auto recordFile = directory + L"\\olc-pq-operator-record";
    const auto keyFile = directory + L"\\olc-pq-operator-key";
    Write(recordFile, 1385); Write(keyFile, 32);
    SetACL(recordFile, privateACL); SetACL(keyFile, privateACL);
    std::array<unsigned char, 1385> record{};
    std::array<unsigned char, 32> key{};
    const auto read = [&](const std::wstring& path) {
        record.fill(99); key.fill(99);
        const bool ok = pqwallet::ReadWindowsOperatorCredentials(path, record, key);
        if (!ok) {
            assert(std::all_of(record.begin(), record.end(), [](unsigned char c) { return c == 0; }));
            assert(std::all_of(key.begin(), key.end(), [](unsigned char c) { return c == 0; }));
        }
        return ok;
    };
    assert(read(directory));
    assert(record.front() == 42 && record.back() == 42 && key.front() == 42 && key.back() == 42);
    for (const auto& path : {std::wstring(), std::wstring(L"relative"), directory + L"\\",
            directory + L"\\.", directory + L"\\..", directory + L":stream",
            L"\\\\?\\" + directory, directory + std::wstring(L"\0ignored", 8)}) assert(!read(path));
    assert(!pqwallet::ReadWindowsOperatorCredentials(directory, Span<unsigned char>(record.data(), 1), key));
    for (const auto& path : {directory, recordFile, keyFile}) {
        SetACL(path, privateACL + L"(A;;FR;;;WD)");
        assert(!read(directory));
        SetACL(path, privateACL);
        assert(read(directory));
        SetACL(path, privateACL + L"(A;;FA;;;SY)");
        assert(read(directory));
        SetACL(path, L"D:P");
        assert(!read(directory));
        SetACL(path, privateACL);
        // A NULL DACL grants everyone access; it is not an empty private ACL.
        assert(SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS);
        assert(!read(directory));
        SetACL(path, privateACL);
        // An elevated fixture may initially create Administrators-owned files.
        // A group owner must not be accepted as the process user's identity.
        BYTE administrators[SECURITY_MAX_SID_SIZE];
        DWORD sidSize = sizeof(administrators);
        assert(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &sidSize));
        if (SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION, administrators, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            assert(!read(directory));
            SetACL(path, privateACL);
        } else {
            std::cout << "SKIP alternate owner assignment: Windows privilege unavailable\n";
        }
    }
    for (const auto& path : {recordFile, keyFile}) {
        const size_t expected = path == recordFile ? 1385 : 32;
        for (size_t length : {size_t(0), expected - 1, expected + 1}) {
            Write(path, length);
            assert(!read(directory));
        }
        Write(path, expected);
        const auto link = directory + L"\\alias";
        assert(CreateHardLinkW(link.c_str(), path.c_str(), nullptr));
        assert(!read(directory));
        assert(DeleteFileW(link.c_str()));
        HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(writer != INVALID_HANDLE_VALUE);
        assert(!read(directory));
        assert(CloseHandle(writer));
        assert(MoveFileW(path.c_str(), link.c_str()));
        assert(!read(directory));
        if (CreateSymbolicLinkW(path.c_str(), link.c_str(), 0x2) ||
            CreateSymbolicLinkW(path.c_str(), link.c_str(), 0)) {
            assert(!read(directory));
            assert(DeleteFileW(path.c_str()));
        } else {
            std::cout << "SKIP file symlink creation: Windows privilege/developer mode unavailable\n";
        }
        assert(CreateDirectoryW(path.c_str(), nullptr));
        SetACL(path, privateACL);
        assert(!read(directory));
        assert(RemoveDirectoryW(path.c_str()));
        assert(MoveFileW(link.c_str(), path.c_str()));
        assert(read(directory));
    }
    const auto alias = root + L"\\alias";
    if (CreateSymbolicLinkW(alias.c_str(), directory.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) ||
        CreateSymbolicLinkW(alias.c_str(), directory.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY)) {
        assert(!read(alias));
        assert(RemoveDirectoryW(alias.c_str()));
    } else {
        std::cout << "SKIP directory symlink creation: Windows privilege/developer mode unavailable\n";
    }
#ifdef PQ_CREDENTIAL_CRYPTO_TEST
    EncryptedCredentials(directory);
    PublishCredentials(directory, privateACL);
#endif
    assert(DeleteFileW(recordFile.c_str()) && DeleteFileW(keyFile.c_str()));
    assert(RemoveDirectoryW(directory.c_str()) && RemoveDirectoryW(root.c_str()));
    std::cout << "Native Windows private credential reader checks passed\n";
}
