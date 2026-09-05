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
