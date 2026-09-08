// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
// Standalone GNU-linker test. Wraps only the journal I/O boundary; no production
// fault switch, wallet, node, or real signing key. See doc/pq-only.md for build.
#ifdef NDEBUG
#error Journal fault checks require assertions enabled
#endif
#include <pqjournal.h>
#include <pqjournal_io.h>
#include <logging.h>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

// Journal, framing, hashes and native I/O are real. Disable logging and abort
// if this storage-only check ever attempts out-of-scope crypto verification.
static BCLog::Logger logger;
BCLog::Logger* const g_logger = &logger;
void BCLog::Logger::LogPrintStr(const std::string&) {}
bool mldsa44::Verify(Span<const unsigned char>, Span<const unsigned char>,
                    Span<const unsigned char>, Span<const unsigned char>) { std::abort(); }

enum class Fault { NONE, SYNC, PARENT, REOPEN, REPLACE, PARTIAL, CRASH_SYNC, CRASH_REPLACE };
static Fault fault = Fault::NONE;
static int countdown = 0;
static bool Hit(Fault point)
{
    if (fault != point || --countdown != 0) return false;
    fault = Fault::NONE;
    return true;
}

extern "C" {
bool __real__ZN9pqjournal2io4SyncEi(int);
bool __wrap__ZN9pqjournal2io4SyncEi(int fd)
{
    if (Hit(Fault::SYNC)) return false;
    const bool ok = __real__ZN9pqjournal2io4SyncEi(fd);
    if (Hit(Fault::CRASH_SYNC)) { assert(ok); std::_Exit(87); }
    return ok;
}
bool __real__ZN9pqjournal2io10SyncParentERKN5boost10filesystem4pathE(const fs::path&);
bool __wrap__ZN9pqjournal2io10SyncParentERKN5boost10filesystem4pathE(const fs::path& path)
{
    return !Hit(Fault::PARENT) && __real__ZN9pqjournal2io10SyncParentERKN5boost10filesystem4pathE(path);
}
int __real__ZN9pqjournal2io4OpenERKN5boost10filesystem4pathENS0_4ModeE(const fs::path&, pqjournal::io::Mode);
int __wrap__ZN9pqjournal2io4OpenERKN5boost10filesystem4pathENS0_4ModeE(const fs::path& path, pqjournal::io::Mode mode)
{
    if (mode == pqjournal::io::Mode::APPEND && Hit(Fault::REOPEN)) return -1;
    return __real__ZN9pqjournal2io4OpenERKN5boost10filesystem4pathENS0_4ModeE(path, mode);
}
bool __real__ZN9pqjournal2io7ReplaceERKN5boost10filesystem4pathES5_(const fs::path&, const fs::path&);
bool __wrap__ZN9pqjournal2io7ReplaceERKN5boost10filesystem4pathES5_(const fs::path& source, const fs::path& destination)
{
    if (Hit(Fault::REPLACE)) return false;
    const bool ok = __real__ZN9pqjournal2io7ReplaceERKN5boost10filesystem4pathES5_(source, destination);
    if (Hit(Fault::CRASH_REPLACE)) { assert(ok); std::_Exit(87); }
    return ok;
}
// size_t is unsigned long on Linux and unsigned long long on 64-bit MinGW.
#ifdef _WIN32
#define REAL_WRITE __real__ZN9pqjournal2io8WriteAllEiPKhy
#define WRAP_WRITE __wrap__ZN9pqjournal2io8WriteAllEiPKhy
#else
#define REAL_WRITE __real__ZN9pqjournal2io8WriteAllEiPKhm
#define WRAP_WRITE __wrap__ZN9pqjournal2io8WriteAllEiPKhm
#endif
bool REAL_WRITE(int, const unsigned char*, size_t);
bool WRAP_WRITE(int fd, const unsigned char* data, size_t size)
{
    if (Hit(Fault::PARTIAL)) {
        assert(size > 1 && REAL_WRITE(fd, data, size / 2));
        return false;
    }
    return REAL_WRITE(fd, data, size);
}
}

static const uint256 GENESIS = uint256S("aa"), REGISTRATION = uint256S("bb");
static mldsa44::PublicKey PublicKey() { mldsa44::PublicKey key{}; key[0] = 1; return key; }
static pqquorum::Statement Vote(uint32_t height)
{
    pqquorum::Statement vote;
    vote.purpose = pqquorum::Purpose::PREVOTE;
    vote.genesis = GENESIS; vote.anchor = uint256S("cc"); vote.committee = uint256S("dd");
    vote.height = height; vote.value = uint256S("ee");
    return vote;
}
static bool Sign(const pqquorum::Statement&, pqjournal::Journal::Signature& signature)
{
    signature.fill(0x5a); // Deterministic public test bytes, not a cryptographic test.
    return true;
}
static std::unique_ptr<pqjournal::Journal> Load(const fs::path& directory)
{
    std::string reason;
    return pqjournal::Journal::Load(directory, GENESIS, REGISTRATION, PublicKey(), reason);
}
static void CheckRetained(pqjournal::Journal& journal, uint32_t height)
{
    bool called = false;
    auto signer = [&](const pqquorum::Statement&, pqjournal::Journal::Signature&) { called = true; return true; };
    std::string reason;
    pqjournal::Journal::Signature signature;
    assert(journal.GetOrSignVote(Vote(height), signer, signature, reason) && !called);
    assert(std::all_of(signature.begin(), signature.end(), [](unsigned char c) { return c == 0x5a; }));
    auto conflicting = Vote(height); conflicting.value = uint256S("ff");
    assert(!journal.GetOrSignVote(conflicting, signer, signature, reason) && !called);
}
static void Crash(const fs::path& directory, bool replacement)
{
    auto journal = Load(directory);
    assert(journal);
    fault = replacement ? Fault::CRASH_REPLACE : Fault::CRASH_SYNC;
    countdown = 1;
    std::string reason;
    pqjournal::Journal::Signature signature;
    if (replacement) journal->RecordFinalized(9, uint256S("09"), uint256S("19"), reason);
    else journal->GetOrSignVote(Vote(11), Sign, signature, reason);
    std::_Exit(88); // The injection MUST terminate the process first.
}
static void RunCrash(const fs::path& directory, bool replacement)
{
#ifdef _WIN32
    wchar_t executable[32768];
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    assert(length && length < 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " +
        (replacement ? L"--crash-replace \"" : L"--crash-sync \"") + directory.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    assert(CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child));
    assert(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD status; assert(GetExitCodeProcess(child.hProcess, &status));
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    assert(status == 87);
#else
    const pid_t child = fork(); assert(child >= 0);
    if (child == 0) Crash(directory, replacement);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 87);
#endif
}

int main(int argc, char** argv)
{
    if (argc == 3) Crash(fs::absolute(argv[2]), std::string(argv[1]) == "--crash-replace");
    assert(argc == 1);
    const auto root = fs::temp_directory_path() / fs::unique_path("olc-journal-fault-%%%%-%%%%-%%%%");
    assert(!fs::exists(root) && fs::create_directory(root));
    const Fault cases[] = {Fault::SYNC, Fault::PARTIAL, Fault::SYNC, Fault::PARENT,
        Fault::REOPEN, Fault::REPLACE, Fault::CRASH_SYNC, Fault::CRASH_REPLACE};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const auto directory = root / std::to_string(i);
        assert(fs::create_directory(directory));
        std::string reason;
        auto journal = pqjournal::Journal::Initialize(directory, GENESIS, REGISTRATION, PublicKey(), reason);
        assert(journal);
        pqjournal::Journal::Signature signature;
        assert(journal->GetOrSignVote(Vote(10), Sign, signature, reason));
        assert(journal->RecordLock(10, 0, Vote(10).value, reason));
        pqquorum::Certificate proof; proof.statement = Vote(10); proof.signatures.resize(1);
        proof.signatures[0].member = 0; proof.signatures[0].bytes.fill(0x5a);
        assert(journal->RecordProof(proof, reason));
        if (i >= 6) {
            journal.reset();
            RunCrash(directory, i == 7);
        } else {
            fault = cases[i]; countdown = i == 2 ? 2 : 1; // Finalized append succeeds; temp flush fails.
            const bool ok = i < 2 ? journal->GetOrSignVote(Vote(11), Sign, signature, reason) :
                journal->RecordFinalized(9, uint256S("09"), uint256S("19"), reason);
            assert(!ok && fault == Fault::NONE && countdown == 0);
            assert(!Load(directory)); // Poisoning must not release the lifetime lease.
            bool called = false;
            assert(!journal->GetOrSignVote(Vote(10), [&](const pqquorum::Statement&, pqjournal::Journal::Signature&) {
                called = true; return true;
            }, signature, reason) && !called);
            assert(std::all_of(signature.begin(), signature.end(), [](unsigned char c) { return c == 0; }));
            journal.reset();
        }
        if (i == 0) {
            // Complete bytes can remain in the OS cache after failed flush.
            // Reopening must durably flush them before exposing cached votes.
            fault = Fault::SYNC; countdown = 2; // Marker first, journal second.
            assert(!Load(directory) && fault == Fault::NONE && countdown == 0);
        }
        auto reopened = Load(directory); assert(reopened);
        CheckRetained(*reopened, 10);
        if (i == 0 || i == 6) CheckRetained(*reopened, 11);
        uint256 locked; uint32_t round;
        assert(reopened->GetLock(10, locked, round) && locked == Vote(10).value && round == 0);
        pqquorum::Certificate restored;
        assert(reopened->GetProof(10, restored) && pqquorum::Encode(restored) == pqquorum::Encode(proof));
        if (i >= 2 && i != 6) assert(reopened->FinalizedHeight() == 9);
        reopened.reset();
        std::cout << "journal failure/crash case " << i << " passed\n";
    }
    fs::remove_all(root); // Only fixtures created above, never user directories.
}
