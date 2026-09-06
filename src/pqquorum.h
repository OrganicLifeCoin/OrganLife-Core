// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_PQQUORUM_H
#define ORGANICLIFE_PQQUORUM_H

#include <crypto/mldsa44.h>
#include <uint256.h>
#include <cstdint>
#include <string>

// Certificate primitive only. Not a finality protocol or an activation switch.
namespace pqquorum {
constexpr size_t MAX_MEMBERS = 400;
// Version + purpose + four uint256 fields + height/round + signature count.
constexpr size_t HEADER_SIZE = 1 + 1 + 4 * 32 + 2 * 4 + 2;
constexpr size_t SIGNATURE_RECORD_SIZE = 2 + mldsa44::SIGNATURE_SIZE;
constexpr size_t MAX_CERTIFICATE_SIZE = HEADER_SIZE + MAX_MEMBERS * SIGNATURE_RECORD_SIZE;
enum class Purpose : uint8_t { PREVOTE = 1, PRECOMMIT = 2, SERVICE = 3, HANDOFF = 4 };
struct Member {
    uint256 registration;
    mldsa44::PublicKey operator_key{};
};
struct Statement {
    Purpose purpose{Purpose::PREVOTE};
    // Genesis is the network discriminator; anchor identifies finalized membership.
    uint256 genesis, anchor, committee;
    uint32_t height{0}, round{0};
    // Zero represents a nil vote, never a service or handoff statement.
    uint256 value;
};
struct Signature {
    uint16_t member{0};
    std::array<unsigned char, mldsa44::SIGNATURE_SIZE> bytes{};
};
struct Certificate {
    Statement statement;
    std::vector<Signature> signatures;
};

// Zero means an unsupported committee, never a zero-signature quorum.
size_t Threshold(size_t members);
// Commits to caller-supplied snapshot order (does not sort). IDs/keys must be unique.
// Rejects unsupported sizes, null IDs and all-zero keys. Registration must establish
// proof of operator key possession; hashing a nonzero key does not establish ownership.
uint256 Commitment(const std::vector<Member>& members);
Span<const unsigned char> Context();
std::vector<unsigned char> Message(const Statement& statement);
std::vector<unsigned char> Encode(const Certificate& certificate);
// Clears output on failure; rejects noncanonical and oversized messages before allocation.
bool Decode(Span<const unsigned char> bytes, Certificate& certificate);
// Caller supplies the expected statement and a trusted finalized committee snapshot.
// Success proves endorsements only, not that the statement is safe to finalize.
bool Verify(const Certificate& certificate, const Statement& expected,
            const std::vector<Member>& members, std::string& reason);
} // namespace pqquorum
#endif
