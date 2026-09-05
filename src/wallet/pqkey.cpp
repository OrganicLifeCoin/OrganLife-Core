// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <wallet/pqkey.h>
#include <sodium.h>
#include <cstring>

namespace pqwallet {
namespace {
constexpr char KDF_CONTEXT[] = "OLCPQ001";
static_assert(sizeof(KDF_CONTEXT) - 1 == crypto_kdf_CONTEXTBYTES, "PQ KDF context size mismatch");
static_assert(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES == 24 &&
              mldsa44::SEED_SIZE + crypto_aead_xchacha20poly1305_ietf_ABYTES == 48,
              "PQ encrypted record size mismatch");

bool StorageKey(const SecureBytes& master, SecureBytes& key)
{
    if (master.size() != crypto_kdf_KEYBYTES || sodium_init() < 0) return false;
    key.resize(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    return crypto_kdf_derive_from_key(key.data(), key.size(), 0, KDF_CONTEXT, master.data()) == 0;
}

std::vector<unsigned char> AssociatedData(const Record& record, const std::string& network)
{
    const char* domain = network == "regtest" ? "OLC/PQ/ML-DSA-44/regtest/seed/v1" :
                         network == "test" ? "OLC/PQ/ML-DSA-44/testnet/seed/v1" : nullptr;
    if (!domain) return {};
    std::vector<unsigned char> data(domain, domain + std::strlen(domain));
    data.push_back(record.version);
    data.insert(data.end(), record.public_key.begin(), record.public_key.end());
    return data;
}
} // namespace

bool EncryptSeed(const SecureBytes& seed, const SecureBytes& master_key, const std::string& network, Record& record)
{
    record = {};
    SecureBytes encryption_key;
    mldsa44::Key key;
    if (seed.size() != mldsa44::SEED_SIZE || !StorageKey(master_key, encryption_key) || !key.SetSeed(seed)) return false;
    Record result;
    result.version = 1;
    result.public_key = key.GetPublicKey();
    randombytes_buf(result.nonce.data(), result.nonce.size());
    const auto ad = AssociatedData(result, network);
    if (ad.empty()) return false;
    unsigned long long size = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(result.encrypted_seed.data(), &size,
            seed.data(), seed.size(), ad.data(), ad.size(), nullptr,
            result.nonce.data(), encryption_key.data()) != 0 || size != result.encrypted_seed.size()) return false;
    record = result;
    return true;
}

bool DecryptKey(const SecureBytes& master_key, const Record& record, const std::string& network, mldsa44::Key& key)
{
    key.Clear();
    SecureBytes encryption_key;
    if (record.version != 1 || !StorageKey(master_key, encryption_key)) return false;
    SecureBytes seed(mldsa44::SEED_SIZE);
    const auto ad = AssociatedData(record, network);
    if (ad.empty()) return false;
    unsigned long long size = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(seed.data(), &size, nullptr,
            record.encrypted_seed.data(), record.encrypted_seed.size(), ad.data(), ad.size(),
            record.nonce.data(), encryption_key.data()) != 0 || size != seed.size() || !key.SetSeed(seed)) return false;
    if (key.GetPublicKey() != record.public_key) {
        key.Clear();
        return false;
    }
    return true;
}
} // namespace pqwallet
