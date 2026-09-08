// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
// Standalone native-platform check: compile with pqjournal_io.cpp, assertions enabled.
#ifdef NDEBUG
#error Journal storage checks require assertions enabled
#endif
#include <pqjournal_io.h>
#include <cassert>
#include <cstring>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

static void Probe(const fs::path& directory, bool locked)
{
#ifdef _WIN32
    wchar_t executable[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    assert(length && length < 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --probe";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    assert(CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, 0, nullptr,
                          directory.wstring().c_str(), &startup, &child));
    assert(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD status;
    assert(GetExitCodeProcess(child.hProcess, &status));
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
#else
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        const int lease = pqjournal::io::Open(directory / "journal.lock", pqjournal::io::Mode::LOCK_EXISTING);
        if (lease >= 0) pqjournal::io::Close(lease);
        _exit(lease < 0 ? 1 : 0);
    }
    int result = 0;
    assert(waitpid(child, &result, 0) == child && WIFEXITED(result));
    const int status = WEXITSTATUS(result);
#endif
    assert(status == (locked ? 1 : 0));
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--probe") == 0) {
        const int lease = pqjournal::io::Open(fs::current_path() / "journal.lock", pqjournal::io::Mode::LOCK_EXISTING);
        if (lease >= 0) pqjournal::io::Close(lease);
        return lease < 0 ? 1 : 0;
    }
    assert(argc <= 2);
    const fs::path root = argc == 2 ? fs::absolute(argv[1]) :
        fs::temp_directory_path() / fs::unique_path("olc-journal-%%%%-%%%%-%%%%");
    assert(!fs::exists(root)); // Never touch an existing directory.
    assert(fs::create_directory(root));
#ifdef _WIN32
    const auto directory = root / L"\u017elu\u0165ou\u010dk\u00fd";
#else
    const auto directory = root / "\xc5\xbelu\xc5\xa5ou\xc4\x8d k\xc3\xbd";
#endif
    assert(fs::create_directory(directory));
    assert(pqjournal::io::DirectorySafe(directory));
    using namespace pqjournal::io;
    const auto path = directory / "journal";
    const auto marker = directory / "journal.lock";
    const int lease = Open(marker, Mode::LOCK_CREATE);
    assert(lease >= 0 && Sync(lease));
    assert(Open(marker, Mode::LOCK_EXISTING) < 0);
    assert(Open(marker, Mode::LOCK_CREATE) < 0);
    Probe(directory, true);
    assert(Open(path, Mode::APPEND) < 0);
    const int file = Open(path, Mode::CREATE);
    assert(file >= 0);
    assert(Open(path, Mode::CREATE) < 0);
    const unsigned char original[] = {0, 1, 2, 3, '\n', 255};
    assert(WriteAll(file, original, sizeof(original)) && Sync(file));
    assert(Size(file) == sizeof(original));
    Close(file);
    const int append = Open(path, Mode::APPEND);
    assert(append >= 0);
    assert(WriteAll(append, original, sizeof(original)) && Sync(append));
    assert(Size(append) == 2 * sizeof(original));
    assert(Truncate(append, sizeof(original)) && Sync(append));
    const auto temporary = directory / "journal.tmp";
    const int replacement = Open(temporary, Mode::REWRITE);
    assert(replacement >= 0 && WriteAll(replacement, original, 2) && Sync(replacement));
    Close(replacement);
    Close(append); // Windows replacement requires closing the old append target.
    Probe(directory, true); // This must not create a competing-writer gap.
    const bool replaced = Replace(temporary, path);
#ifdef _WIN32
    if (!replaced) std::cerr << "Replace failed: " << GetLastError() << '\n';
#endif
    assert(replaced && SyncParent(path));
    const int reopened = Open(path, Mode::APPEND);
    assert(reopened >= 0 && Size(reopened) == 2);
    Close(reopened);
    assert(Open(marker, Mode::LOCK_EXISTING) < 0); // Replacement never releases the lease.
    Probe(directory, true);
    Close(lease);
    const int nextLease = Open(marker, Mode::LOCK_EXISTING);
    assert(nextLease >= 0);
    Close(nextLease);
    Probe(directory, false);
    assert(Open(directory, Mode::REWRITE) < 0);
    const auto hardlink = directory / "hardlink";
    fs::create_hard_link(path, hardlink);
    assert(Open(hardlink, Mode::REWRITE) < 0); // Must inspect BEFORE truncating.
    assert(fs::file_size(path) == 2);
    assert(Open(path, Mode::APPEND) < 0);
    fs::remove(hardlink);
    const auto symlink = directory / "symlink";
    fs::create_symlink(path, symlink);
    assert(Open(symlink, Mode::APPEND) < 0);
    assert(Open(symlink, Mode::REWRITE) < 0 && fs::file_size(path) == 2);
    const auto linkedDirectory = root / "linked-directory";
    fs::create_directory_symlink(directory, linkedDirectory);
    assert(!DirectorySafe(linkedDirectory));
    fs::remove(linkedDirectory);
    fs::remove_all(root); // Only the newly created isolated fixture.
    std::cout << "journal native I/O checks passed\n";
}
