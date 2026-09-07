// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <wallet/pqkey.h>
#include <sodium.h>
#include <algorithm>
#include <cstring>
#include <streams.h>
#ifndef WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <crypto/common.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <sys/xattr.h>
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
         network == "test" ? "OLC/PQ/ML-DSA-44/testnet/operator-recovery/v1" : nullptr) : genesis ?
        (network == "regtest" ? "OLC/PQ/ML-DSA-44/regtest/operator-seed/v1" :
         network == "test" ? "OLC/PQ/ML-DSA-44/testnet/operator-seed/v1" : nullptr) :
        (network == "regtest" ? "OLC/PQ/ML-DSA-44/regtest/seed/v1" :
         network == "test" ? "OLC/PQ/ML-DSA-44/testnet/seed/v1" : nullptr);
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
#ifndef WIN32
    try {
        const auto& path = directory.native();
        if ((network != "test" && network != "regtest") || genesis.IsNull() ||
            path.empty() || !directory.is_absolute() || path.back() == '/' || path.find('\0') != std::string::npos)
            return false;
        for (const auto& component : directory)
            if (component == "." || component == "..") return false;
        Descriptor parent(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        struct stat info;
        if (parent.fd < 0 || fstat(parent.fd, &info) != 0 || !S_ISDIR(info.st_mode) || !PrivateCredential(parent.fd, info, true))
            return false;
        SecureBytes wrapping_key, encoded;
        constexpr size_t RECORD_SIZE = 1 + mldsa44::PUBLIC_KEY_SIZE + 24 + 48;
        if (!ReadCredential(parent.fd, "olc-pq-operator-record", RECORD_SIZE, encoded) ||
            !ReadCredential(parent.fd, "olc-pq-operator-key", 32, wrapping_key)) return false;
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
#else
    reason = "PQ operator credential loading is not supported on this platform";
#endif
    return false;
}

bool EncryptOperatorRecovery(const SecureBytes& seed, const SecureBytes& master_key,
                             const std::string& network, const uint256& genesis, Record& record)
{
    return Encrypt(seed, master_key, network, &genesis, record, 3);
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
