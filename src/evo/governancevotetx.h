// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_EVO_GOVERNANCEVOTETX_H
#define PIVX_EVO_GOVERNANCEVOTETX_H

#include "amount.h"
#include "primitives/transaction.h"
#include "pubkey.h"
#include "serialize.h"
#include "uint256.h"

class CValidationState;
class COutPoint;

// Consensus constants shared by contextual and non-contextual GOV vote checks.
inline constexpr CAmount GOV_VOTE_LOCK_MIN_AMOUNT = 1 * COIN;
inline constexpr CAmount GOV_VOTE_LOCK_MAX_AMOUNT = 100000 * COIN;
inline constexpr uint32_t GOV_VOTE_LOCK_MIN_UNLOCK_HEIGHT = 1;
inline constexpr size_t GOV_VOTE_CAST_MAX_LOCK_REFS = 64;

struct CGovVoteLockTx
{
    static constexpr int16_t SPECIALTX_TYPE = CTransaction::TxType::GOVVOTELOCK;

    uint256 proposalHash;
    CAmount lockAmount{0};
    uint32_t unlockHeight{0};
    CKeyID ownerKeyId;

    SERIALIZE_METHODS(CGovVoteLockTx, obj)
    {
        READWRITE(obj.proposalHash, obj.lockAmount, obj.unlockHeight, obj.ownerKeyId);
    }

    bool IsTriviallyValid(CValidationState& state) const;
};

struct CGovVoteCastTx
{
    static constexpr int16_t SPECIALTX_TYPE = CTransaction::TxType::GOVVOTECAST;

    static constexpr uint8_t VOTE_NO = 1;
    static constexpr uint8_t VOTE_YES = 2;

    uint256 proposalHash;
    uint8_t voteDirection{0};
    std::vector<COutPoint> lockRefs;
    std::vector<uint8_t> sig;

    SERIALIZE_METHODS(CGovVoteCastTx, obj)
    {
        READWRITE(obj.proposalHash, obj.voteDirection, obj.lockRefs, obj.sig);
    }

    uint256 GetSignatureHash() const;
    bool IsTriviallyValid(CValidationState& state) const;
};

#endif // PIVX_EVO_GOVERNANCEVOTETX_H
