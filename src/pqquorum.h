// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_PQQUORUM_H
#define ORGANICLIFE_PQQUORUM_H

#include <crypto/mldsa44.h>
#include <uint256.h>
#include <cstdint>
#include <string>

// Certificate and single-height decision logic, not an activated finality runtime.
namespace pqquorum {
constexpr size_t MIN_MEMBERS = 4;
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

// Single-height decision logic only. No signing, persistence, network handler,
// block validation or fork-choice authority. Serialize access; a runtime must
// durably journal decisions BEFORE signing/broadcast and validate proposed blocks
// against the trusted anchor. Never recreate this state to resume a used key.
// Identical decisions are idempotent; conflicting votes at one round/step fail.
// A future signer must reuse its journaled signature on a retry, not reset state.
class RoundState {
    Statement current;
    const std::vector<Member> members;
    uint256 locked, prevote, precommit, committed;
    uint32_t lockRound{0};
    bool prevoted{false}, precommitted{false};
public:
    RoundState(const uint256& genesis, const uint256& anchor, uint32_t height,
               const std::vector<Member>& members);
    RoundState(const RoundState&) = delete;
    RoundState& operator=(const RoundState&) = delete;
    bool Advance(uint32_t round);
    bool Prevote(const uint256& validatedProposal, const Certificate* unlockProof,
                 Statement& vote, std::string& reason);
    // validatedBlock must be supplied by local chain validation, not the peer proof.
    // Null means no validated block data; only a nil precommit is then possible.
    bool Precommit(const Certificate* prevotes, const uint256& validatedBlock,
                   Statement& vote, std::string& reason);
    bool Commit(const Certificate& precommits, const uint256& validatedBlock,
                uint256& finalized, std::string& reason);
};
} // namespace pqquorum
#endif
