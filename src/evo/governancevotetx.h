// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_EVO_GOVERNANCEVOTETX_H
#define PIVX_EVO_GOVERNANCEVOTETX_H

#include "amount.h"
#include "crypto/mldsa44.h"
#include "pqtransaction.h"
#include "primitives/transaction.h"
#include "serialize.h"
#include "uint256.h"

class CValidationState;
class COutPoint;

// Consensus constants shared by contextual and non-contextual GOV vote checks.
inline constexpr CAmount GOV_VOTE_LOCK_MIN_AMOUNT = 1 * COIN;
inline constexpr CAmount GOV_VOTE_LOCK_MAX_AMOUNT = 100000 * COIN;
inline constexpr uint32_t GOV_VOTE_LOCK_MIN_UNLOCK_HEIGHT = 1;
inline constexpr size_t GOV_VOTE_CAST_MAX_LOCK_REFS = 64;

struct CGovProposalTx
{
    std::string name;
    std::string url;
    uint8_t paymentCount{0};
    uint32_t blockStart{0};
    pq::KeyID recipient;
    CAmount amount{0};

    SERIALIZE_METHODS(CGovProposalTx, obj)
    {
        READWRITE(obj.name, obj.url, obj.paymentCount, obj.blockStart, obj.recipient, obj.amount);
    }

    bool IsTriviallyValid(CValidationState& state) const;
};

struct CGovVoteLockTx
{
    uint256 proposalHash;
    CAmount lockAmount{0};
    uint32_t unlockHeight{0};
    pq::KeyID ownerKeyId;

    SERIALIZE_METHODS(CGovVoteLockTx, obj)
    {
        READWRITE(obj.proposalHash, obj.lockAmount, obj.unlockHeight, obj.ownerKeyId);
    }

    bool IsTriviallyValid(CValidationState& state) const;
};

struct CGovVoteCastTx
{
    static constexpr uint8_t VOTE_NO = 1;
    static constexpr uint8_t VOTE_YES = 2;

    uint256 proposalHash;
    uint8_t voteDirection{0};
    std::vector<COutPoint> lockRefs;
    mldsa44::PublicKey ownerPublicKey{};
    std::array<unsigned char, mldsa44::SIGNATURE_SIZE> sig{};

    SERIALIZE_METHODS(CGovVoteCastTx, obj)
    {
        READWRITE(obj.proposalHash, obj.voteDirection, obj.lockRefs, obj.ownerPublicKey, obj.sig);
    }

    std::vector<unsigned char> GetSignatureMessage(const uint256& genesis) const;
    bool IsTriviallyValid(CValidationState& state) const;
};

std::vector<unsigned char> EncodeGovernanceData(const CGovProposalTx& payload);
std::vector<unsigned char> EncodeGovernanceData(const CGovVoteLockTx& payload);
std::vector<unsigned char> EncodeGovernanceData(const CGovVoteCastTx& payload);
bool DecodeGovernanceData(const pq::Payload& payload, CGovProposalTx& result);
bool DecodeGovernanceData(const pq::Payload& payload, CGovVoteLockTx& result);
bool DecodeGovernanceData(const pq::Payload& payload, CGovVoteCastTx& result);

#endif // PIVX_EVO_GOVERNANCEVOTETX_H
