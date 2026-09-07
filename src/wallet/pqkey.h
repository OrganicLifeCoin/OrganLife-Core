// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#ifndef ORGANICLIFE_WALLET_PQKEY_H
#define ORGANICLIFE_WALLET_PQKEY_H

#include <pqaddress.h>
#include <serialize.h>
#include <uint256.h>
#include <fs.h>
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

// Only controller wallet DB state, never server credentials. A snapshot captures
// backed=0; restoring it requires a fresh snapshot before identity publication.
struct OperatorRecovery {
    Record record;
    uint8_t backed{0};
    SERIALIZE_METHODS(OperatorRecovery, obj) { READWRITE(obj.record, obj.backed); }
};

// Seeds are independent of the existing HD wallet. No plaintext seed export.
bool EncryptSeed(const SecureBytes& seed, const SecureBytes& master_key, const std::string& network, Record& record);
bool DecryptKey(const SecureBytes& master_key, const Record& record, const std::string& network, mldsa44::Key& key);

// Separate operator-only storage domain, bound to a specific chain genesis.
// Caller supplies an independent random 32-byte wrapping key, never a password
// or a controller wallet master key for distribution to operator hosts.
bool EncryptOperatorSeed(const SecureBytes& seed, const SecureBytes& wrapping_key,
                         const std::string& network, const uint256& genesis, Record& record);
bool DecryptOperatorKey(const SecureBytes& wrapping_key, const Record& record,
                        const std::string& network, const uint256& genesis, mldsa44::Key& key);
// Controller-local recovery only. Never distribute this record or wallet master
// as operator credentials; those use the independent version-2 wrapping key.
bool EncryptOperatorRecovery(const SecureBytes& seed, const SecureBytes& master_key,
                             const std::string& network, const uint256& genesis, Record& record);
bool DecryptOperatorRecovery(const SecureBytes& master_key, const Record& record,
                             const std::string& network, const uint256& genesis, mldsa44::Key& key);
// Rewrap only a backed controller operator record for the expected identity.
// The caller supplies a fresh independent random wrapping key and delivers it
// through protected host custody. No plaintext seed or wallet master is output.
// Failure clears output, except aliased input/output is rejected unchanged.
bool RewrapOperatorRecovery(const OperatorRecovery& recovery, const pq::KeyID& expected,
                            const SecureBytes& master_key, const SecureBytes& wrapping_key,
                            const std::string& network, const uint256& genesis, Record& record);
// Read-only Linux/macOS credential delivery, not provisioning or operator authority.
// Caller supplies a trusted absolute credential directory and chain identity.
// Other platforms fail closed; no environment/config fallback or secret logging.
bool LoadOperatorCredentials(const fs::path& directory, const std::string& network,
                             const uint256& genesis, mldsa44::Key& key, std::string& reason);
// Atomically publish a new private credential directory inside a trusted private
// parent. Never overwrite a destination. The pair is operator secret material:
// transfer through a protected channel and seal both files on the operator host.
bool WriteOperatorCredentials(const fs::path& directory, const Record& record, const SecureBytes& wrapping_key,
                              const std::string& network, const uint256& genesis, std::string& reason);
} // namespace pqwallet

#endif // ORGANICLIFE_WALLET_PQKEY_H
