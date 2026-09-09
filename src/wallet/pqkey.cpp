// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <wallet/pqkey.h>
#include <sodium.h>
#include <algorithm>
#include <cstring>
#include <streams.h>
#include <support/cleanse.h>
#include <utilstrencodings.h>
#ifdef WIN32
#include <wallet/pqcredentials_win.h>
#endif
#ifndef WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#endif
#ifdef __linux__
#include <crypto/common.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <sys/xattr.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#elif defined(__APPLE__)
#include <sys/acl.h>
#endif

namespace pqwallet {
namespace {
#ifndef WIN32
struct Descriptor {
    const int fd;
    explicit Descriptor(int value) : fd(value) {}
    ~Descriptor() { if (fd >= 0) close(fd); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};

bool PrivateCredential(int fd, const struct stat& info, bool directory)
{
    if (info.st_mode & (S_ISUID | S_ISGID | S_ISVTX | S_IRWXO)) return false;
    const mode_t required = S_IRUSR | (directory ? S_IXUSR : 0);
    const bool owner_only = info.st_uid == geteuid() && !(info.st_mode & S_IRWXG) &&
                            (info.st_mode & required) == required;
#ifdef __linux__
    // systemd's root-owned credential mount grants only the service UID access.
    // Accept exactly that ACL, not arbitrary group or named-user access.
    std::array<unsigned char, 45> acl{};
    const auto size = fgetxattr(fd, "system.posix_acl_access", acl.data(), acl.size());
    if (size < 0) return errno == ENODATA && owner_only;
    if (info.st_uid != 0 || (info.st_mode & 0777) != (directory ? 0550 : 0440) || size != 44)
        return false;
    std::array<unsigned char, 44> expected{};
    WriteLE32(expected.data(), POSIX_ACL_XATTR_VERSION);
    const uint16_t tags[] = {ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_MASK, ACL_OTHER};
    const uint16_t access = ACL_READ | (directory ? ACL_EXECUTE : 0);
    for (size_t i = 0; i < 5; ++i) {
        auto* entry = expected.data() + 4 + i * 8;
        WriteLE16(entry, tags[i]);
        WriteLE16(entry + 2, i == 2 || i == 4 ? 0 : access);
        WriteLE32(entry + 4, i == 1 ? geteuid() : static_cast<uint32_t>(ACL_UNDEFINED_ID));
    }
    return std::equal(expected.begin(), expected.end(), acl.begin());
#elif defined(__APPLE__)
    // Darwin extended ACLs can grant access independently of the mode bits.
    acl_t acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (!acl) return owner_only && errno == ENOENT;
    acl_entry_t entry;
    const bool empty = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) == -1 && errno == EINVAL;
    acl_free(acl);
    return owner_only && empty;
#else
    return false; // No unverified ACL policy on other POSIX platforms.
#endif
}

bool ReadCredential(int directory, const char* name, size_t expected, SecureBytes& bytes)
{
    Descriptor file(openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    struct stat info;
    const auto valid = [&] {
        return file.fd >= 0 && fstat(file.fd, &info) == 0 && S_ISREG(info.st_mode) &&
               info.st_nlink == 1 && info.st_size == static_cast<off_t>(expected) && PrivateCredential(file.fd, info, false);
    };
    if (!valid()) return false;
    bytes.resize(expected + 1); // Fixed bound; catches growth after the metadata check.
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = read(file.fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return false;
        if (count == 0) break;
        offset += count;
    }
    if (offset != expected || !valid()) return false;
    bytes.resize(expected);
    return true;
}
#endif

constexpr char KDF_CONTEXT[] = "OLCPQ001";
constexpr char OPERATOR_KDF_CONTEXT[] = "OLCPQOP1";
constexpr char RECOVERY_KDF_CONTEXT[] = "OLCPQRC1";
static_assert(sizeof(KDF_CONTEXT) - 1 == crypto_kdf_CONTEXTBYTES, "PQ KDF context size mismatch");
static_assert(sizeof(OPERATOR_KDF_CONTEXT) - 1 == crypto_kdf_CONTEXTBYTES, "PQ operator KDF context size mismatch");
static_assert(sizeof(RECOVERY_KDF_CONTEXT) - 1 == crypto_kdf_CONTEXTBYTES, "PQ recovery KDF context size mismatch");
static_assert(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES == 24 &&
              mldsa44::SEED_SIZE + crypto_aead_xchacha20poly1305_ietf_ABYTES == 48,
              "PQ encrypted record size mismatch");

bool StorageKey(const SecureBytes& master, SecureBytes& key, uint8_t version)
{
    if (master.size() != crypto_kdf_KEYBYTES || sodium_init() < 0) return false;
    key.resize(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    return crypto_kdf_derive_from_key(key.data(), key.size(), 0,
                                    version == 3 ? RECOVERY_KDF_CONTEXT :
                                    version == 2 ? OPERATOR_KDF_CONTEXT : KDF_CONTEXT, master.data()) == 0;
}

std::vector<unsigned char> AssociatedData(const Record& record, const std::string& network, const uint256* genesis)
{
    if (genesis && genesis->IsNull()) return {};
    const char* domain = record.version == 3 ?
        (network == "regtest" ? "OLC/PQ/ML-DSA-44/regtest/operator-recovery/v1" :
         network == "test" ? "OLC/PQ/ML-DSA-44/testnet/operator-recovery/v1" :
         network == "main" ? "OLC/PQ/ML-DSA-44/mainnet/operator-recovery/v1" : nullptr) : genesis ?
        (network == "regtest" ? "OLC/PQ/ML-DSA-44/regtest/operator-seed/v1" :
         network == "test" ? "OLC/PQ/ML-DSA-44/testnet/operator-seed/v1" :
         network == "main" ? "OLC/PQ/ML-DSA-44/mainnet/operator-seed/v1" : nullptr) :
        (network == "regtest" ? "OLC/PQ/ML-DSA-44/regtest/seed/v1" :
         network == "test" ? "OLC/PQ/ML-DSA-44/testnet/seed/v1" :
         network == "main" ? "OLC/PQ/ML-DSA-44/mainnet/seed/v1" : nullptr);
    if (!domain) return {};
    std::vector<unsigned char> data(domain, domain + std::strlen(domain));
    data.push_back(record.version);
    data.insert(data.end(), record.public_key.begin(), record.public_key.end());
    if (genesis) data.insert(data.end(), genesis->begin(), genesis->end());
    return data;
}

bool Encrypt(const SecureBytes& seed, const SecureBytes& master_key, const std::string& network,
             const uint256* genesis, Record& record, uint8_t version)
{
    record = {};
    SecureBytes encryption_key;
    mldsa44::Key key;
    if (seed.size() != mldsa44::SEED_SIZE || !StorageKey(master_key, encryption_key, version) || !key.SetSeed(seed)) return false;
    Record result;
    result.version = version;
    result.public_key = key.GetPublicKey();
    randombytes_buf(result.nonce.data(), result.nonce.size());
    const auto ad = AssociatedData(result, network, genesis);
    if (ad.empty()) return false;
    unsigned long long size = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(result.encrypted_seed.data(), &size,
            seed.data(), seed.size(), ad.data(), ad.size(), nullptr,
            result.nonce.data(), encryption_key.data()) != 0 || size != result.encrypted_seed.size()) return false;
    record = result;
    return true;
}

bool Decrypt(const SecureBytes& master_key, const Record& record, const std::string& network,
             const uint256* genesis, mldsa44::Key& key, uint8_t version, SecureBytes* rewrap_seed = nullptr)
{
    key.Clear();
    if (rewrap_seed) rewrap_seed->clear();
    SecureBytes encryption_key;
    if (record.version != version || !StorageKey(master_key, encryption_key, version)) return false;
    SecureBytes seed(mldsa44::SEED_SIZE);
    const auto ad = AssociatedData(record, network, genesis);
    if (ad.empty()) return false;
    unsigned long long size = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(seed.data(), &size, nullptr,
            record.encrypted_seed.data(), record.encrypted_seed.size(), ad.data(), ad.size(),
            record.nonce.data(), encryption_key.data()) != 0 || size != seed.size() || !key.SetSeed(seed)) return false;
    if (key.GetPublicKey() != record.public_key) {
        key.Clear();
        return false;
    }
    if (rewrap_seed) *rewrap_seed = std::move(seed);
    return true;
}
} // namespace

bool EncryptSeed(const SecureBytes& seed, const SecureBytes& master_key, const std::string& network, Record& record)
{
    return Encrypt(seed, master_key, network, nullptr, record, 1);
}

bool DecryptKey(const SecureBytes& master_key, const Record& record, const std::string& network, mldsa44::Key& key)
{
    return Decrypt(master_key, record, network, nullptr, key, 1);
}

bool EncryptOperatorSeed(const SecureBytes& seed, const SecureBytes& wrapping_key,
                         const std::string& network, const uint256& genesis, Record& record)
{
    return Encrypt(seed, wrapping_key, network, &genesis, record, 2);
}

bool DecryptOperatorKey(const SecureBytes& wrapping_key, const Record& record,
                        const std::string& network, const uint256& genesis, mldsa44::Key& key)
{
    return Decrypt(wrapping_key, record, network, &genesis, key, 2);
}

bool LoadOperatorCredentials(const fs::path& directory, const std::string& network,
                             const uint256& genesis, mldsa44::Key& key, std::string& reason)
{
    key.Clear(); reason = "Could not load private PQ operator credentials";
    try {
        if ((network != "test" && network != "regtest") || genesis.IsNull()) return false;
        SecureBytes wrapping_key, encoded;
        constexpr size_t RECORD_SIZE = 1 + mldsa44::PUBLIC_KEY_SIZE + 24 + 48;
#ifdef WIN32
        static_assert(RECORD_SIZE == 1385, "Windows operator record size mismatch");
        encoded.resize(RECORD_SIZE);
        wrapping_key.resize(32);
        if (!ReadWindowsOperatorCredentials(directory.wstring(), encoded, wrapping_key)) return false;
#else
        const auto& path = directory.native();
        if (path.empty() || !directory.is_absolute() || path.back() == '/' || path.find('\0') != std::string::npos)
            return false;
        for (const auto& component : directory)
            if (component == "." || component == "..") return false;
        Descriptor parent(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        struct stat info;
        if (parent.fd < 0 || fstat(parent.fd, &info) != 0 || !S_ISDIR(info.st_mode) || !PrivateCredential(parent.fd, info, true))
            return false;
        if (!ReadCredential(parent.fd, "olc-pq-operator-record", RECORD_SIZE, encoded) ||
            !ReadCredential(parent.fd, "olc-pq-operator-key", 32, wrapping_key)) return false;
#endif
        Record record;
        const auto* begin = reinterpret_cast<const char*>(encoded.data());
        CDataStream stream(begin, begin + encoded.size(), SER_DISK, 0);
        stream >> record;
        if (!stream.empty() || !DecryptOperatorKey(wrapping_key, record, network, genesis, key)) return false;
        reason.clear();
        return true;
    } catch (const std::exception&) {
        key.Clear();
    }
    return false;
}

std::string EncodeOperatorConfig(const Record& record, const SecureBytes& wrapping_key)
{
    constexpr size_t RECORD_SIZE = 1 + mldsa44::PUBLIC_KEY_SIZE + 24 + 48;
    if (record.version != 2 || wrapping_key.size() != 32) return {};
    CDataStream stream(SER_DISK, 0);
    stream << record;
    if (stream.size() != RECORD_SIZE) return {};
    SecureBytes bytes(stream.begin(), stream.end());
    bytes.insert(bytes.end(), wrapping_key.begin(), wrapping_key.end());
    return HexStr(bytes);
}

bool DecodeOperatorConfig(const std::string& encoded, const std::string& network,
                          const uint256& genesis, mldsa44::Key& key, std::string& reason)
{
    constexpr size_t RECORD_SIZE = 1 + mldsa44::PUBLIC_KEY_SIZE + 24 + 48;
    constexpr size_t WRAPPING_KEY_SIZE = 32;
    constexpr size_t ENCODED_SIZE = 2 * (RECORD_SIZE + WRAPPING_KEY_SIZE);
    key.Clear();
    reason = "Could not load inline private PQ operator credentials";
    if (encoded.size() != ENCODED_SIZE || !IsHex(encoded)) return false;
    for (const char c : encoded) {
        if (c >= 'A' && c <= 'F') return false;
    }
    try {
        std::vector<unsigned char> parsed = ParseHex(encoded);
        SecureBytes bytes(parsed.begin(), parsed.end());
        memory_cleanse(parsed.data(), parsed.size());
        if (bytes.size() != RECORD_SIZE + WRAPPING_KEY_SIZE) return false;
        const char* begin = reinterpret_cast<const char*>(bytes.data());
        CDataStream stream(begin, begin + RECORD_SIZE, SER_DISK, 0);
        Record record;
        stream >> record;
        if (!stream.empty()) return false;
        SecureBytes wrapping_key(WRAPPING_KEY_SIZE);
        std::copy(bytes.begin() + RECORD_SIZE, bytes.end(), wrapping_key.begin());
        if (!DecryptOperatorKey(wrapping_key, record, network, genesis, key)) return false;
        reason.clear();
        return true;
    } catch (const std::exception&) {
        key.Clear();
    }
    return false;
}

bool IsPrivateOperatorConfigFile(const fs::path& path, const std::string& expected_contents)
{
    if (expected_contents.size() > 1024 * 1024) return false;
#ifdef WIN32
    return IsPrivateWindowsConfigFile(path.wstring(), expected_contents);
#else
    const auto native = path.native();
    if (native.empty() || !path.is_absolute() || native.find('\0') != std::string::npos) return false;
    Descriptor file(open(native.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    struct stat info;
    if (file.fd < 0 || fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_nlink != 1 || info.st_size != static_cast<off_t>(expected_contents.size()) ||
        !PrivateCredential(file.fd, info, false)) return false;
    std::string actual(expected_contents.size(), '\0');
    size_t offset = 0;
    while (offset < actual.size()) {
        const auto count = read(file.fd, actual.data() + offset, actual.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += count;
    }
    char extra;
    if (read(file.fd, &extra, 1) != 0) return false;
    return actual == expected_contents && fstat(file.fd, &info) == 0 && PrivateCredential(file.fd, info, false);
#endif
}

bool EncryptOperatorRecovery(const SecureBytes& seed, const SecureBytes& master_key,
                             const std::string& network, const uint256& genesis, Record& record)
{
    return Encrypt(seed, master_key, network, &genesis, record, 3);
}

bool WriteOperatorCredentials(const fs::path& directory, const Record& record, const SecureBytes& wrapping_key,
                              const std::string& network, const uint256& genesis, std::string& reason)
{
    reason = "Could not publish private PQ operator credentials; do not use an incomplete destination";
#ifdef WIN32
    try {
        mldsa44::Key key;
        if (!DecryptOperatorKey(wrapping_key, record, network, genesis, key)) return false;
        CDataStream encoded(SER_DISK, 0);
        encoded << record;
        std::array<unsigned char, 16> random{};
        randombytes_buf(random.data(), random.size());
        if (!WriteWindowsOperatorCredentials(directory.wstring(),
            Span<const unsigned char>(reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()),
            wrapping_key, random)) return false;
        reason.clear();
        return true;
    } catch (const std::exception&) {
        return false;
    }
#elif defined(__linux__) || defined(__APPLE__)
    try {
        const auto& path = directory.native();
        mldsa44::Key key;
        if (path.empty() || !directory.is_absolute() || path.back() == '/' || path.find('\0') != std::string::npos ||
            !DecryptOperatorKey(wrapping_key, record, network, genesis, key)) return false;
        for (const auto& component : directory)
            if (component == "." || component == "..") return false;
        Descriptor parent(open(directory.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat info;
        if (parent.fd < 0 || fstat(parent.fd, &info) != 0 || !PrivateCredential(parent.fd, info, true)) return false;
        const std::string destination = directory.filename().string();
        std::array<unsigned char, 16> random{};
        randombytes_buf(random.data(), random.size());
        const std::string staging = ".olc-pq-" + HexStr(random);
        if (mkdirat(parent.fd, staging.c_str(), 0700) != 0) return false;
        struct PendingDirectory {
            const int parent;
            const std::string& staging;
            const std::string& destination;
            Descriptor directory;
            bool published{false}, keep{false};
            PendingDirectory(int fd, const std::string& temporary, const std::string& final_name)
                : parent(fd), staging(temporary), destination(final_name),
                  directory(openat(fd, temporary.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) {}
            ~PendingDirectory() {
                if (!keep) {
                    unlinkat(directory.fd, "olc-pq-operator-key", 0);
                    unlinkat(directory.fd, "olc-pq-operator-record", 0);
                    unlinkat(parent, (published ? destination : staging).c_str(), AT_REMOVEDIR);
                }
            }
        } pending{parent.fd, staging, destination};
        if (pending.directory.fd < 0 || fstat(pending.directory.fd, &info) != 0 ||
            !PrivateCredential(pending.directory.fd, info, true)) return false;
        const auto write = [&](const char* name, const unsigned char* bytes, size_t size) {
            Descriptor file(openat(pending.directory.fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0400));
            if (file.fd < 0) return false;
            size_t offset = 0;
            while (offset < size) {
                const auto count = ::write(file.fd, bytes + offset, size - offset);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) return false;
                offset += count;
            }
            if (fchmod(file.fd, 0400) != 0 || fsync(file.fd) != 0) return false;
            SecureBytes readback;
            return ReadCredential(pending.directory.fd, name, size, readback) &&
                   sodium_memcmp(readback.data(), bytes, size) == 0;
        };
        CDataStream encoded(SER_DISK, 0);
        encoded << record;
        if (!write("olc-pq-operator-record", reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()) ||
            !write("olc-pq-operator-key", wrapping_key.data(), wrapping_key.size()) || fsync(pending.directory.fd) != 0)
            return false;
#ifdef __linux__
        if (syscall(SYS_renameat2, parent.fd, staging.c_str(), parent.fd, destination.c_str(), RENAME_NOREPLACE) != 0)
            return false;
#else
        if (renameatx_np(parent.fd, staging.c_str(), parent.fd, destination.c_str(), RENAME_EXCL) != 0) return false;
#endif
        pending.published = true;
        if (fsync(parent.fd) != 0) return false;
        pending.keep = true;
        reason.clear();
        return true;
    } catch (const std::exception&) {
        return false;
    }
#endif
    return false;
}

bool DecryptOperatorRecovery(const SecureBytes& master_key, const Record& record,
                             const std::string& network, const uint256& genesis, mldsa44::Key& key)
{
    return Decrypt(master_key, record, network, &genesis, key, 3);
}

bool RewrapOperatorRecovery(const OperatorRecovery& recovery, const pq::KeyID& expected,
                            const SecureBytes& master_key, const SecureBytes& wrapping_key,
                            const std::string& network, const uint256& genesis, Record& record)
{
    if (&record == &recovery.record) return false;
    record = {};
    if (recovery.backed != 1 || master_key.size() != 32 || wrapping_key.size() != 32 ||
        sodium_memcmp(master_key.data(), wrapping_key.data(), 32) == 0) return false;
    const auto id = pq::GetID(recovery.record.public_key, network);
    if (!id || *id != expected) return false;
    mldsa44::Key key;
    SecureBytes seed;
    if (!Decrypt(master_key, recovery.record, network, &genesis, key, 3, &seed)) return false;
    return EncryptOperatorSeed(seed, wrapping_key, network, genesis, record);
}
} // namespace pqwallet
