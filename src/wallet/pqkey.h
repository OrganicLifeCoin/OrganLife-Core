// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#ifndef ORGANICLIFE_WALLET_PQKEY_H
#define ORGANICLIFE_WALLET_PQKEY_H

#include <pqaddress.h>
#include <serialize.h>
#include <string>

namespace pqwallet {
using SecureBytes = std::vector<unsigned char, secure_allocator<unsigned char>>;

// Network-bound encrypted seed record. Fixed lengths bound deserialization.
struct Record {
    uint8_t version{0};
    mldsa44::PublicKey public_key{};
    std::array<unsigned char, 24> nonce{};
    std::array<unsigned char, 48> encrypted_seed{};
    SERIALIZE_METHODS(Record, obj) {
        READWRITE(obj.version, obj.public_key, obj.nonce, obj.encrypted_seed);
    }
};

// Seeds are independent of the existing HD wallet. No plaintext seed export.
bool EncryptSeed(const SecureBytes& seed, const SecureBytes& master_key, const std::string& network, Record& record);
bool DecryptKey(const SecureBytes& master_key, const Record& record, const std::string& network, mldsa44::Key& key);
} // namespace pqwallet

#endif // ORGANICLIFE_WALLET_PQKEY_H
