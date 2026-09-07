// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <wallet/pqkey.h>
#include <bech32.h>
#include <streams.h>
#include <test/data/mldsa44_vectors.h>
#include <boost/algorithm/hex.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <iterator>
#include <sodium.h>
#ifndef WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <membership.h>
#include <sys/acl.h>
#endif
#ifdef __linux__
#include <crypto/common.h>
#include <sys/xattr.h>
#endif

namespace {
pqwallet::SecureBytes TestSeed()
{
    pqwallet::SecureBytes seed;
    boost::algorithm::unhex(std::string(mldsa44_vectors::KEYGEN_SEED), std::back_inserter(seed));
    return seed;
}
// SHA-256 via Node's crypto module and an independent Bech32m reference calculation.
const std::string ADDRESS = "olcpqregtest1ptg5w9rz7lg3wfm0kpkqcucf8k7egr24c68pve32tqklksmw08h8syk3x80";
const std::string LEGACY_ADDRESS = "olcpqregtest1ptg5w9rz7lg3wfm0kpkqcucf8k7egr24c68pve32tqklksmw08h8s32p2zd";
const std::string ID_HEX = "5a28e28c5efa22e4edf60d818e6127b7b281aab8d1c2ccc54b05bf686dcf3dcf";
// Same NIST public key, SHA-256(testnet address domain || key), independent Bech32m calculation.
const std::string TEST_ADDRESS = "olcpqtest1ppf0kwsev98fsffnp9hpe0chp509e7mvpgpzyd5erfslqtgsxkrls25pmyv";
const std::string LEGACY_TEST_ADDRESS = "olcpqtest1ppf0kwsev98fsffnp9hpe0chp509e7mvpgpzyd5erfslqtgsxkrlslg3hpw";
const std::string TEST_ID_HEX = "0a5f67432c29d304a6612dc397e2e1a3cb9f6d81404446d3234c3e05a206b0ff";
const pqwallet::SecureBytes MASTER(32, 42); // Public test material only.
}

BOOST_AUTO_TEST_SUITE(pqkey_tests)

BOOST_AUTO_TEST_CASE(operator_recovery_is_not_spending_or_deployment_storage)
{
    const uint256 genesis = uint256S("1234");
    pqwallet::Record recovery, wallet, deployed;
    BOOST_REQUIRE(pqwallet::EncryptOperatorRecovery(TestSeed(), MASTER, "regtest", genesis, recovery));
    BOOST_CHECK_EQUAL(recovery.version, 3);
    BOOST_REQUIRE(pqwallet::EncryptSeed(TestSeed(), MASTER, "regtest", wallet));
    BOOST_REQUIRE(pqwallet::EncryptOperatorSeed(TestSeed(), MASTER, "regtest", genesis, deployed));
    mldsa44::Key key;
    BOOST_REQUIRE(pqwallet::DecryptOperatorRecovery(MASTER, recovery, "regtest", genesis, key));
    BOOST_CHECK(key.GetPublicKey() == recovery.public_key);
    const std::vector<unsigned char> message{1, 2, 3};
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(message, {}, signature));
    BOOST_CHECK(mldsa44::Verify(recovery.public_key, message, {}, signature));
    BOOST_CHECK(!pqwallet::DecryptKey(MASTER, recovery, "regtest", key));
    BOOST_CHECK(!key.IsValid());
    BOOST_CHECK(!pqwallet::DecryptOperatorKey(MASTER, recovery, "regtest", genesis, key));
    BOOST_CHECK(!key.IsValid());
    auto relabeled = recovery;
    relabeled.version = 1;
    BOOST_CHECK(!pqwallet::DecryptKey(MASTER, relabeled, "regtest", key));
    relabeled.version = 2;
    BOOST_CHECK(!pqwallet::DecryptOperatorKey(MASTER, relabeled, "regtest", genesis, key));
    BOOST_CHECK(!key.IsValid());
    for (auto record : {wallet, deployed, recovery}) {
        const auto original_version = record.version;
        record.version = 3;
        if (original_version == 3) record.encrypted_seed.back() ^= 1;
        BOOST_CHECK(!pqwallet::DecryptOperatorRecovery(MASTER, record, "regtest", genesis, key));
        BOOST_CHECK(!key.IsValid());
    }
    BOOST_CHECK(!pqwallet::DecryptOperatorRecovery(MASTER, recovery, "test", genesis, key));
    BOOST_CHECK(!pqwallet::DecryptOperatorRecovery(MASTER, recovery, "regtest", uint256S("1235"), key));
    BOOST_CHECK(!pqwallet::DecryptOperatorRecovery(pqwallet::SecureBytes(32, 43), recovery, "regtest", genesis, key));
    BOOST_CHECK(!key.IsValid());
    BOOST_CHECK(!pqwallet::EncryptOperatorRecovery(TestSeed(), MASTER, "main", genesis, recovery));
    BOOST_CHECK_EQUAL(recovery.version, 0);
    BOOST_CHECK(!pqwallet::EncryptOperatorRecovery(TestSeed(), MASTER, "regtest", uint256{}, recovery));
    BOOST_CHECK_EQUAL(recovery.version, 0);
}

BOOST_AUTO_TEST_CASE(operator_recovery_direct_kdf_aead_reference)
{
    const uint256 genesis = uint256S("1234");
    for (const std::string network : {"regtest", "test"}) {
        pqwallet::Record record;
        record.version = 3;
        boost::algorithm::unhex(std::string(mldsa44_vectors::KEYGEN_PUBLIC_KEY), record.public_key.begin());
        for (size_t i = 0; i < record.nonce.size(); ++i) record.nonce[i] = i;
        pqwallet::SecureBytes encryption_key(32);
        BOOST_REQUIRE(sodium_init() >= 0);
        BOOST_REQUIRE_EQUAL(crypto_kdf_derive_from_key(encryption_key.data(), 32, 0, "OLCPQRC1", MASTER.data()), 0);
        const std::string domain = network == "test" ? "OLC/PQ/ML-DSA-44/testnet/operator-recovery/v1" :
                                                       "OLC/PQ/ML-DSA-44/regtest/operator-recovery/v1";
        std::vector<unsigned char> ad(domain.begin(), domain.end());
        ad.push_back(3);
        ad.insert(ad.end(), record.public_key.begin(), record.public_key.end());
        ad.push_back(0x34); ad.push_back(0x12); ad.insert(ad.end(), 30, 0);
        for (bool wrong_seed : {false, true}) {
            auto seed = TestSeed();
            if (wrong_seed) seed[0] ^= 1;
            unsigned long long size = 0;
            BOOST_REQUIRE_EQUAL(crypto_aead_xchacha20poly1305_ietf_encrypt(record.encrypted_seed.data(), &size,
                seed.data(), seed.size(), ad.data(), ad.size(), nullptr, record.nonce.data(), encryption_key.data()), 0);
            BOOST_REQUIRE_EQUAL(size, record.encrypted_seed.size());
            mldsa44::Key key;
            BOOST_REQUIRE(key.Generate());
            BOOST_CHECK(pqwallet::DecryptOperatorRecovery(MASTER, record, network, genesis, key) == !wrong_seed);
            BOOST_CHECK(key.IsValid() == !wrong_seed);
        }
    }
}

#if defined(__linux__) || defined(__APPLE__)
BOOST_AUTO_TEST_CASE(operator_credentials_load_from_private_files)
{
    struct TemporaryCredentials {
        const fs::path path = fs::temp_directory_path() / fs::unique_path("olc-credentials-%%%%-%%%%-%%%%");
        ~TemporaryCredentials() { fs::remove_all(path); }
    } temporary;
    const auto& directory = temporary.path;
    BOOST_REQUIRE(fs::create_directory(directory));
    BOOST_REQUIRE_EQUAL(chmod(directory.c_str(), 0700), 0);
    const auto recordFile = directory / "olc-pq-operator-record";
    const auto keyFile = directory / "olc-pq-operator-key";
    const uint256 genesis = uint256S("1234");
    pqwallet::Record record;
    BOOST_REQUIRE(pqwallet::EncryptOperatorSeed(TestSeed(), MASTER, "regtest", genesis, record));
    CDataStream bytes(SER_DISK, 0); bytes << record;
    {
        fsbridge::ofstream file(recordFile, std::ios::binary);
        file.write(bytes.data(), bytes.size());
        BOOST_REQUIRE(file.good());
    }
    {
        fsbridge::ofstream file(keyFile, std::ios::binary);
        file.write(reinterpret_cast<const char*>(MASTER.data()), MASTER.size());
        BOOST_REQUIRE(file.good());
    }
    BOOST_REQUIRE_EQUAL(chmod(recordFile.c_str(), 0400), 0);
    BOOST_REQUIRE_EQUAL(chmod(keyFile.c_str(), 0400), 0);
    mldsa44::Key key; std::string reason = "stale failure";
    BOOST_REQUIRE_MESSAGE(pqwallet::LoadOperatorCredentials(directory, "regtest", genesis, key, reason), reason);
    BOOST_CHECK(key.GetPublicKey() == record.public_key);
    BOOST_CHECK(reason.empty());
    BOOST_CHECK(!pqwallet::LoadOperatorCredentials(directory, "test", genesis, key, reason));
    BOOST_CHECK(!key.IsValid()); BOOST_CHECK(!reason.empty());
    BOOST_CHECK(!pqwallet::LoadOperatorCredentials(directory, "regtest", uint256S("1235"), key, reason));
    BOOST_CHECK(!key.IsValid());

    const auto reject = [&](const fs::path& path) {
        BOOST_REQUIRE(key.SetSeed(TestSeed()));
        reason = "stale failure";
        BOOST_CHECK(!pqwallet::LoadOperatorCredentials(path, "regtest", genesis, key, reason));
        BOOST_CHECK(!key.IsValid());
        BOOST_CHECK_EQUAL(reason, "Could not load private PQ operator credentials");
    };
    for (const auto& path : {fs::path(), fs::path("operator-credentials"),
                            fs::path(directory.string() + "/"), directory / ".",
                            directory / ".." / "operator-credentials",
                            fs::path(directory.string() + std::string("\0ignored", 8))})
        reject(path);
    const auto alias = directory / "credential-alias";
    fs::create_directory_symlink(directory, alias);
    reject(alias);
    BOOST_REQUIRE(fs::remove(alias));
    for (const mode_t mode : {0000, 0100, 0600, 0704, 0720, 01700, 02700, 04700}) {
        BOOST_REQUIRE_EQUAL(chmod(directory.c_str(), mode), 0);
        reject(directory);
    }
    BOOST_REQUIRE_EQUAL(chmod(directory.c_str(), 0700), 0);

    const auto write = [&](const fs::path& path, const std::string& data) {
        fsbridge::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(data.data(), data.size());
        file.close();
        BOOST_REQUIRE(file.good());
        BOOST_REQUIRE_EQUAL(chmod(path.c_str(), 0400), 0);
    };
    for (const auto& path : {recordFile, keyFile}) {
        const std::string good = path == recordFile ? std::string(bytes.begin(), bytes.end()) :
            std::string(reinterpret_cast<const char*>(MASTER.data()), MASTER.size());
        for (const mode_t mode : {0000, 0200, 0404, 0420, 01400, 02400, 04400}) {
            BOOST_REQUIRE_EQUAL(chmod(path.c_str(), mode), 0);
            reject(directory);
        }
        BOOST_REQUIRE_EQUAL(chmod(path.c_str(), 0400), 0);
        fs::create_hard_link(path, alias);
        reject(directory);
        BOOST_REQUIRE(fs::remove(alias));
        fs::rename(path, alias);
        reject(directory); // Missing credential.
        fs::create_symlink(alias, path);
        reject(directory);
        BOOST_REQUIRE(fs::remove(path));
        BOOST_REQUIRE(fs::create_directory(path));
        reject(directory);
        BOOST_REQUIRE(fs::remove(path));
        BOOST_REQUIRE_EQUAL(mkfifo(path.c_str(), 0600), 0);
        reject(directory); // Must not block opening a FIFO with no writer.
        BOOST_REQUIRE(fs::remove(path));
        for (const auto& bad : {std::string(), good.substr(0, good.size() - 1),
                               good + "x", std::string(good.size(), '\0')}) {
            write(path, bad);
            reject(directory);
            BOOST_REQUIRE(fs::remove(path));
        }
        fs::rename(alias, path);
    }
#ifdef __linux__
    // Kernel xattr layout: version 2 and five canonical 8-byte ACL entries.
    std::array<unsigned char, 44> acl{};
    WriteLE32(acl.data(), 2);
    const uint16_t tags[] = {1, 2, 4, 16, 32};
    for (const auto& path : {directory, recordFile, keyFile}) {
        const uint16_t access = path == directory ? 5 : 4;
        for (size_t i = 0; i < 5; ++i) {
            WriteLE16(acl.data() + 4 + i * 8, tags[i]);
            WriteLE16(acl.data() + 6 + i * 8, i == 2 || i == 4 ? 0 : access);
            WriteLE32(acl.data() + 8 + i * 8, i == 1 ? geteuid() + 1 : 0xffffffff);
        }
        if (geteuid() == 0) {
            WriteLE32(acl.data() + 16, 0);
            BOOST_REQUIRE_EQUAL(setxattr(path.c_str(), "system.posix_acl_access", acl.data(), acl.size(), 0), 0);
            BOOST_REQUIRE_MESSAGE(pqwallet::LoadOperatorCredentials(directory, "regtest", genesis, key, reason), reason);
            BOOST_CHECK(key.GetPublicKey() == record.public_key);
            BOOST_CHECK(reason.empty());
            WriteLE32(acl.data() + 16, 1);
        }
        BOOST_REQUIRE_EQUAL(setxattr(path.c_str(), "system.posix_acl_access", acl.data(), acl.size(), 0), 0);
        reject(directory); // An extra named user must never inherit access.
        BOOST_REQUIRE_EQUAL(removexattr(path.c_str(), "system.posix_acl_access"), 0);
        BOOST_REQUIRE_EQUAL(chmod(path.c_str(), path == directory ? 0700 : 0400), 0);
    }
#endif
#ifdef __APPLE__
    // Extended ACLs can grant access not reflected in the POSIX mode bits.
    for (const auto& path : {directory, recordFile, keyFile}) {
        acl_t acl = acl_init(1);
        BOOST_REQUIRE(acl != nullptr);
        acl_entry_t entry;
        uuid_t user;
        BOOST_REQUIRE_EQUAL(mbr_uid_to_uuid(geteuid(), user), 0);
        BOOST_REQUIRE_EQUAL(acl_create_entry(&acl, &entry), 0);
        BOOST_REQUIRE_EQUAL(acl_set_tag_type(entry, ACL_EXTENDED_ALLOW), 0);
        BOOST_REQUIRE_EQUAL(acl_set_qualifier(entry, user), 0);
        acl_permset_t perms;
        BOOST_REQUIRE_EQUAL(acl_get_permset(entry, &perms), 0);
        BOOST_REQUIRE_EQUAL(acl_add_perm(perms, ACL_READ_DATA), 0);
        BOOST_REQUIRE_EQUAL(acl_set_file(path.c_str(), ACL_TYPE_EXTENDED, acl), 0);
        acl_free(acl);
        reject(directory);
        acl = acl_init(0);
        BOOST_REQUIRE(acl != nullptr);
        BOOST_REQUIRE_EQUAL(acl_set_file(path.c_str(), ACL_TYPE_EXTENDED, acl), 0);
        acl_free(acl);
    }
#endif
    for (const std::string network : {"", "main", "unknown"}) {
        BOOST_REQUIRE(key.SetSeed(TestSeed()));
        BOOST_CHECK(!pqwallet::LoadOperatorCredentials(directory, network, genesis, key, reason));
        BOOST_CHECK(!key.IsValid());
    }
    BOOST_REQUIRE(key.SetSeed(TestSeed()));
    BOOST_CHECK(!pqwallet::LoadOperatorCredentials(directory, "regtest", uint256(), key, reason));
    BOOST_CHECK(!key.IsValid());
    BOOST_REQUIRE_MESSAGE(pqwallet::LoadOperatorCredentials(directory, "regtest", genesis, key, reason), reason);
    BOOST_CHECK(key.GetPublicKey() == record.public_key);
    BOOST_CHECK(reason.empty());
}
#endif

BOOST_AUTO_TEST_CASE(operator_storage_is_separate_from_wallet_and_chain_bound)
{
    const uint256 genesis = uint256S("1234"); // Public test chain identity, not a live network.
    for (const std::string network : {"regtest", "test"}) {
        pqwallet::Record record, second, wallet;
        BOOST_REQUIRE(pqwallet::EncryptOperatorSeed(TestSeed(), MASTER, network, genesis, record));
        BOOST_REQUIRE(pqwallet::EncryptOperatorSeed(TestSeed(), MASTER, network, genesis, second));
        BOOST_CHECK_EQUAL(record.version, 2);
        BOOST_CHECK(record.nonce != second.nonce);
        BOOST_CHECK(record.encrypted_seed != second.encrypted_seed);
        CDataStream bytes(SER_DISK, 0); bytes << record;
        BOOST_CHECK_EQUAL(bytes.size(), 1385U);
        const auto seed = TestSeed();
        BOOST_CHECK(std::search(bytes.begin(), bytes.end(), seed.begin(), seed.end()) == bytes.end());
        pqwallet::Record restored; bytes >> restored;
        mldsa44::Key key;
        BOOST_REQUIRE(pqwallet::DecryptOperatorKey(MASTER, restored, network, genesis, key));
        BOOST_CHECK(key.GetPublicKey() == record.public_key);
        BOOST_CHECK(!pqwallet::DecryptKey(MASTER, record, network, key));
        BOOST_CHECK(!key.IsValid());
        record.version = 1; // Relabeling cannot turn an operator record into spending material.
        BOOST_CHECK(!pqwallet::DecryptKey(MASTER, record, network, key));
        BOOST_CHECK(!key.IsValid()); record.version = 2;
        BOOST_REQUIRE(pqwallet::EncryptSeed(TestSeed(), MASTER, network, wallet));
        BOOST_CHECK(!pqwallet::DecryptOperatorKey(MASTER, wallet, network, genesis, key));
        wallet.version = 2;
        BOOST_CHECK(!pqwallet::DecryptOperatorKey(MASTER, wallet, network, genesis, key));
        BOOST_CHECK(!key.IsValid());
        for (const auto& other : {uint256(), uint256S("1235")}) {
            BOOST_REQUIRE(key.Generate());
            BOOST_CHECK(!pqwallet::DecryptOperatorKey(MASTER, record, network, other, key));
            BOOST_CHECK(!key.IsValid());
        }
        for (const std::string other : {"test", "regtest", "main", "", "unknown"}) {
            if (other == network) continue;
            BOOST_REQUIRE(key.Generate());
            BOOST_CHECK(!pqwallet::DecryptOperatorKey(MASTER, record, other, genesis, key));
            BOOST_CHECK(!key.IsValid());
        }
    }
}

BOOST_AUTO_TEST_CASE(operator_storage_failures_clear_outputs)
{
    const uint256 genesis = uint256S("1234");
    for (const std::string network : {"regtest", "test"}) {
        pqwallet::Record record;
        BOOST_REQUIRE(pqwallet::EncryptOperatorSeed(TestSeed(), MASTER, network, genesis, record));
        mldsa44::Key key;
        for (int field = 0; field < 4; ++field) {
            auto bad = record;
            if (field == 0) bad.version = 1;
            if (field == 1) bad.public_key.back() ^= 1;
            if (field == 2) bad.nonce.back() ^= 1;
            if (field == 3) bad.encrypted_seed.back() ^= 1;
            BOOST_REQUIRE(key.Generate());
            BOOST_CHECK(!pqwallet::DecryptOperatorKey(MASTER, bad, network, genesis, key));
            BOOST_CHECK(!key.IsValid());
        }
        auto wrong = MASTER; wrong[0] ^= 1;
        BOOST_REQUIRE(key.Generate());
        BOOST_CHECK(!pqwallet::DecryptOperatorKey(wrong, record, network, genesis, key));
        BOOST_CHECK(!key.IsValid());
        const auto cleared = [](const pqwallet::Record& rejected) {
            BOOST_CHECK_EQUAL(rejected.version, 0);
            BOOST_CHECK(rejected.public_key == mldsa44::PublicKey{});
            BOOST_CHECK(rejected.nonce == decltype(rejected.nonce){});
            BOOST_CHECK(rejected.encrypted_seed == decltype(rejected.encrypted_seed){});
        };
        for (size_t size : {size_t{0}, size_t{31}, size_t{33}}) {
            BOOST_REQUIRE(key.Generate());
            BOOST_CHECK(!pqwallet::DecryptOperatorKey(pqwallet::SecureBytes(size), record, network, genesis, key));
            BOOST_CHECK(!key.IsValid());
            auto rejected = record;
            BOOST_CHECK(!pqwallet::EncryptOperatorSeed(pqwallet::SecureBytes(size), MASTER, network, genesis, rejected));
            cleared(rejected);
            rejected = record;
            BOOST_CHECK(!pqwallet::EncryptOperatorSeed(TestSeed(), pqwallet::SecureBytes(size), network, genesis, rejected));
            cleared(rejected);
        }
        for (const std::string other : {"main", "", "unknown", "testnet", "REGTEST"}) {
            auto rejected = record;
            BOOST_CHECK(!pqwallet::EncryptOperatorSeed(TestSeed(), MASTER, other, genesis, rejected));
            cleared(rejected);
        }
        auto rejected = record;
        BOOST_CHECK(!pqwallet::EncryptOperatorSeed(TestSeed(), MASTER, network, uint256(), rejected));
        cleared(rejected);
    }
}

BOOST_AUTO_TEST_CASE(operator_storage_direct_kdf_aead_reference)
{
    // Independent construction with literal protocol domains, fixed public test
    // nonce and raw little-endian genesis bytes. Production uses random nonces.
    const uint256 genesis = uint256S("1234");
    for (const std::string network : {"regtest", "test"}) {
        BOOST_REQUIRE(sodium_init() >= 0);
        pqwallet::Record record; record.version = 2;
        boost::algorithm::unhex(std::string(mldsa44_vectors::KEYGEN_PUBLIC_KEY), record.public_key.begin());
        for (size_t i = 0; i < record.nonce.size(); ++i) record.nonce[i] = i;
        pqwallet::SecureBytes encryption_key(32);
        BOOST_REQUIRE_EQUAL(crypto_kdf_derive_from_key(encryption_key.data(), 32, 0, "OLCPQOP1", MASTER.data()), 0);
        const std::string domain = network == "test" ? "OLC/PQ/ML-DSA-44/testnet/operator-seed/v1" :
                                                       "OLC/PQ/ML-DSA-44/regtest/operator-seed/v1";
        std::vector<unsigned char> ad(domain.begin(), domain.end());
        ad.push_back(2);
        ad.insert(ad.end(), record.public_key.begin(), record.public_key.end());
        ad.push_back(0x34); ad.push_back(0x12); ad.insert(ad.end(), 30, 0);
        for (bool wrong_seed : {false, true}) {
            auto seed = TestSeed();
            if (wrong_seed) seed[0] ^= 1;
            unsigned long long size = 0;
            BOOST_REQUIRE_EQUAL(crypto_aead_xchacha20poly1305_ietf_encrypt(record.encrypted_seed.data(), &size,
                seed.data(), seed.size(), ad.data(), ad.size(), nullptr, record.nonce.data(), encryption_key.data()), 0);
            BOOST_REQUIRE_EQUAL(size, record.encrypted_seed.size());
            mldsa44::Key key; BOOST_REQUIRE(key.Generate());
            BOOST_CHECK(pqwallet::DecryptOperatorKey(MASTER, record, network, genesis, key) == !wrong_seed);
            BOOST_CHECK(key.IsValid() == !wrong_seed);
            if (!wrong_seed) BOOST_CHECK(key.GetPublicKey() == record.public_key);
        }
    }
}

BOOST_AUTO_TEST_CASE(fixed_address_vector)
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.SetSeed(TestSeed()));
    const auto id = pq::GetID(key.GetPublicKey(), "regtest");
    BOOST_REQUIRE(id);
    std::string hex;
    boost::algorithm::hex_lower(id->begin(), id->end(), std::back_inserter(hex));
    BOOST_CHECK_EQUAL(hex, ID_HEX);
    BOOST_CHECK_EQUAL(pq::EncodeAddress(*id, "regtest"), ADDRESS);
    pq::KeyID decoded{};
    BOOST_REQUIRE(pq::DecodeAddress(ADDRESS, "regtest", decoded));
    BOOST_CHECK(*id == decoded);
}

BOOST_AUTO_TEST_CASE(network_specific_identity)
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.SetSeed(TestSeed()));
    const auto regtest = pq::GetID(key.GetPublicKey(), "regtest");
    const auto testnet = pq::GetID(key.GetPublicKey(), "test");
    BOOST_REQUIRE(regtest);
    BOOST_REQUIRE(testnet);
    BOOST_CHECK(*regtest != *testnet);
    std::string hex;
    boost::algorithm::hex_lower(testnet->begin(), testnet->end(), std::back_inserter(hex));
    BOOST_CHECK_EQUAL(hex, TEST_ID_HEX);
    BOOST_CHECK_EQUAL(pq::EncodeAddress(*testnet, "test"), TEST_ADDRESS);
    for (const auto& network : {"main", "", "unknown", "testnet", "REGTEST"})
        BOOST_CHECK(!pq::GetID(key.GetPublicKey(), network));
}

BOOST_AUTO_TEST_CASE(testnet_fixed_address_encoding)
{
    pq::KeyID id{};
    boost::algorithm::unhex(TEST_ID_HEX, id.begin());
    BOOST_CHECK_EQUAL(pq::EncodeAddress(id, "test"), TEST_ADDRESS);
    BOOST_CHECK_EQUAL(TEST_ADDRESS.size(), 69U);
    pq::KeyID decoded{};
    BOOST_CHECK(pq::DecodeAddress(TEST_ADDRESS, "test", decoded));
    BOOST_CHECK(decoded == id);
}

BOOST_AUTO_TEST_CASE(address_rejects_wrong_network_and_encoding)
{
    pq::KeyID id{};
    BOOST_REQUIRE(pq::DecodeAddress(ADDRESS, "regtest", id));
    for (const auto& network : {"main", "", "unknown", "testnet", "REGTEST"}) {
        BOOST_CHECK(pq::EncodeAddress(id, network).empty());
        BOOST_CHECK(!pq::DecodeAddress(ADDRESS, network, id));
    }
    for (const auto& network : {"regtest", "test"}) {
        const auto& address = std::string(network) == "test" ? TEST_ADDRESS : ADDRESS;
        const auto& wrong_network_address = std::string(network) == "test" ? ADDRESS : TEST_ADDRESS;
        auto upper = address;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return c >= 'a' && c <= 'z' ? c - 32 : c; });
        auto changed = address;
        changed.back() = 'q';
        const auto& legacy_address = std::string(network) == "test" ? LEGACY_TEST_ADDRESS : LEGACY_ADDRESS;
        for (const auto& invalid : {std::string{}, address + "q", address.substr(1), upper, changed, legacy_address,
                                   wrong_network_address, std::string(10000, 'q')}) {
            id.fill(42);
            BOOST_CHECK(!pq::DecodeAddress(invalid, network, id));
            BOOST_CHECK(id == pq::KeyID{});
        }
        auto parts = bech32::DecodeM(address);
        id.fill(42);
        BOOST_CHECK(!pq::DecodeAddress(bech32::EncodeM("olcpqmain", parts.second), network, id));
        BOOST_CHECK(id == pq::KeyID{});
        parts.second[0] = 2;
        BOOST_CHECK(!pq::DecodeAddress(bech32::EncodeM(parts.first, parts.second), network, id));
        parts.second[0] = 1;
        parts.second.back() |= 1; // Nonzero padding with a valid checksum.
        BOOST_CHECK(!pq::DecodeAddress(bech32::EncodeM(parts.first, parts.second), network, id));
        BOOST_CHECK(id == pq::KeyID{});
    }
}

BOOST_AUTO_TEST_CASE(encrypted_seed_roundtrip_and_serialization)
{
    const auto seed = TestSeed();
    for (const auto& network : {"regtest", "test"}) {
        pqwallet::Record record, second;
        BOOST_REQUIRE(pqwallet::EncryptSeed(seed, MASTER, network, record));
        BOOST_REQUIRE(pqwallet::EncryptSeed(seed, MASTER, network, second));
        BOOST_CHECK(record.nonce != second.nonce);
        BOOST_CHECK(record.encrypted_seed != second.encrypted_seed);
        const auto id = pq::GetID(record.public_key, network);
        BOOST_REQUIRE(id);
        BOOST_CHECK_EQUAL(pq::EncodeAddress(*id, network), std::string(network) == "test" ? TEST_ADDRESS : ADDRESS);
        CDataStream bytes(SER_DISK, 0);
        bytes << record;
        BOOST_CHECK_EQUAL(bytes.size(), 1385U);
        BOOST_CHECK(std::search(bytes.begin(), bytes.end(), seed.begin(), seed.end()) == bytes.end());
        pqwallet::Record restored;
        bytes >> restored;
        BOOST_CHECK(bytes.empty());
        mldsa44::Key key;
        BOOST_REQUIRE(pqwallet::DecryptKey(MASTER, restored, network, key));
        BOOST_CHECK(key.GetPublicKey() == record.public_key);
        const std::vector<unsigned char> message{1, 2, 3};
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(key.Sign(message, {}, signature));
        BOOST_CHECK(mldsa44::Verify(record.public_key, message, {}, signature));
    }
}

BOOST_AUTO_TEST_CASE(network_bound_encryption_rejects_cross_network_and_clears_key)
{
    for (const auto& network : {"regtest", "test"}) {
        pqwallet::Record record;
        BOOST_REQUIRE(pqwallet::EncryptSeed(TestSeed(), MASTER, network, record));
        const std::string wrong_network = std::string(network) == "test" ? "regtest" : "test";
        for (const auto& other : {wrong_network, std::string("main"), std::string(""), std::string("unknown"), std::string("testnet")}) {
            mldsa44::Key key;
            BOOST_REQUIRE(key.Generate());
            BOOST_CHECK(!pqwallet::DecryptKey(MASTER, record, other, key));
            BOOST_CHECK(!key.IsValid());
        }
        for (const auto& unsupported : {"main", "", "unknown", "testnet", "REGTEST"}) {
            auto rejected = record;
            BOOST_CHECK(!pqwallet::EncryptSeed(TestSeed(), MASTER, unsupported, rejected));
            BOOST_CHECK_EQUAL(rejected.version, 0);
            BOOST_CHECK(rejected.public_key == mldsa44::PublicKey{});
            BOOST_CHECK(rejected.nonce == decltype(rejected.nonce){});
            BOOST_CHECK(rejected.encrypted_seed == decltype(rejected.encrypted_seed){});
        }
    }
}

BOOST_AUTO_TEST_CASE(fixed_network_record_vectors)
{
    // Independently generated with direct libsodium KDF/AEAD calls: master=42*32,
    // subkey 0, OLCPQ001, nonce=0..23, AD=literal network seed domain || 1 || NIST public key.
    // Fixed public test material only; production encryption must keep random nonces.
    for (const auto& network : {"regtest", "test"}) {
        pqwallet::Record record;
        record.version = 1;
        boost::algorithm::unhex(std::string(mldsa44_vectors::KEYGEN_PUBLIC_KEY), record.public_key.begin());
        for (size_t i = 0; i < record.nonce.size(); ++i) record.nonce[i] = i;
        const std::string ciphertext = std::string(network) == "test" ?
            "de71545a03135ac8a8dbeb6f11e89af7d30adc92c64624d594dc76c1d299f1994516295657d1a9601ad3e120ca948e6f" :
            "de71545a03135ac8a8dbeb6f11e89af7d30adc92c64624d594dc76c1d299f199a673fdb900ac707e3a57a28be64272a7";
        boost::algorithm::unhex(ciphertext, record.encrypted_seed.begin());
        mldsa44::Key key;
        BOOST_REQUIRE(pqwallet::DecryptKey(MASTER, record, network, key));
        BOOST_CHECK(key.GetPublicKey() == record.public_key);
        BOOST_CHECK(!pqwallet::DecryptKey(MASTER, record, std::string(network) == "test" ? "regtest" : "test", key));
        BOOST_CHECK(!key.IsValid());
    }
}

BOOST_AUTO_TEST_CASE(authentication_and_input_failures_clear_output)
{
    for (const auto& network : {"regtest", "test"}) {
        pqwallet::Record record;
        BOOST_REQUIRE(pqwallet::EncryptSeed(TestSeed(), MASTER, network, record));
        mldsa44::Key key;
        for (int field = 0; field < 4; ++field) {
            auto bad = record;
            if (field == 0) bad.version ^= 1;
            if (field == 1) bad.public_key[0] ^= 1;
            if (field == 2) bad.nonce[0] ^= 1;
            if (field == 3) bad.encrypted_seed.back() ^= 1;
            BOOST_REQUIRE(key.Generate());
            BOOST_CHECK(!pqwallet::DecryptKey(MASTER, bad, network, key));
            BOOST_CHECK(!key.IsValid());
        }
        auto wrong = MASTER;
        wrong[0] ^= 1;
        BOOST_REQUIRE(key.Generate());
        BOOST_CHECK(!pqwallet::DecryptKey(wrong, record, network, key));
        BOOST_CHECK(!key.IsValid());
        for (size_t size : {size_t{0}, size_t{31}, size_t{33}}) {
            BOOST_REQUIRE(key.Generate());
            BOOST_CHECK(!pqwallet::DecryptKey(pqwallet::SecureBytes(size), record, network, key));
            BOOST_CHECK(!key.IsValid());
            auto rejected = record;
            BOOST_CHECK(!pqwallet::EncryptSeed(pqwallet::SecureBytes(size), MASTER, network, rejected));
            BOOST_CHECK_EQUAL(rejected.version, 0);
            BOOST_CHECK(!pqwallet::EncryptSeed(TestSeed(), pqwallet::SecureBytes(size), network, rejected));
        }
    }
}

BOOST_AUTO_TEST_CASE(authenticated_seed_must_regenerate_the_stored_public_key)
{
    for (const std::string network : {"regtest", "test"}) {
        pqwallet::Record record;
        BOOST_REQUIRE(pqwallet::EncryptSeed(TestSeed(), MASTER, network, record));
        auto different_seed = TestSeed();
        different_seed[0] ^= 1;
        pqwallet::SecureBytes encryption_key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
        BOOST_REQUIRE_EQUAL(crypto_kdf_derive_from_key(encryption_key.data(), encryption_key.size(),
                                                      0, "OLCPQ001", MASTER.data()), 0);
        const std::string domain = network == "test" ? "OLC/PQ/ML-DSA-44/testnet/seed/v1" :
                                                       "OLC/PQ/ML-DSA-44/regtest/seed/v1";
        std::vector<unsigned char> ad(domain.begin(), domain.end());
        ad.push_back(1);
        ad.insert(ad.end(), record.public_key.begin(), record.public_key.end());
        unsigned long long size = 0;
        BOOST_REQUIRE_EQUAL(crypto_aead_xchacha20poly1305_ietf_encrypt(record.encrypted_seed.data(), &size,
            different_seed.data(), different_seed.size(), ad.data(), ad.size(), nullptr,
            record.nonce.data(), encryption_key.data()), 0);
        BOOST_REQUIRE_EQUAL(size, record.encrypted_seed.size());
        mldsa44::Key key;
        BOOST_REQUIRE(key.Generate());
        BOOST_CHECK(!pqwallet::DecryptKey(MASTER, record, network, key));
        BOOST_CHECK(!key.IsValid());
    }
}

BOOST_AUTO_TEST_CASE(bounded_address_mutations_fail_canonically)
{
    for (const std::string network : {"regtest", "test"}) {
        const auto& address = network == "test" ? TEST_ADDRESS : ADDRESS;
        for (size_t i = 0; i < address.size(); ++i) {
            auto bad = address;
            bad[i] = bad[i] == 'q' ? 'p' : 'q';
            pq::KeyID id{};
            id.fill(42);
            BOOST_CHECK(!pq::DecodeAddress(bad, network, id));
            BOOST_CHECK(id == pq::KeyID{});
            BOOST_CHECK(!pq::DecodeAddress(address.substr(0, i), network, id));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
