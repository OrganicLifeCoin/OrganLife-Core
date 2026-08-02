// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "evo/governancevotetx.h"

#include "consensus/validation.h"
#include "hash.h"

#include <set>

bool CGovVoteLockTx::IsTriviallyValid(CValidationState& state) const
{
    if (proposalHash.IsNull()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-proposal-hash");
    }
    if (lockAmount < GOV_VOTE_LOCK_MIN_AMOUNT) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-amount");
    }
    if (lockAmount > GOV_VOTE_LOCK_MAX_AMOUNT) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-amount-excessive");
    }
    if (lockAmount % COIN != 0) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-amount-unit");
    }
    if (unlockHeight < GOV_VOTE_LOCK_MIN_UNLOCK_HEIGHT) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-unlock-height");
    }
    if (ownerKeyId.IsNull()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-owner-key");
    }

    return true;
}

uint256 CGovVoteCastTx::GetSignatureHash() const
{
    CHashWriter hw(SER_GETHASH, PROTOCOL_VERSION);
    hw << proposalHash;
    hw << voteDirection;
    hw << lockRefs;
    return hw.GetHash();
}

bool CGovVoteCastTx::IsTriviallyValid(CValidationState& state) const
{
    if (proposalHash.IsNull()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-proposal-hash");
    }
    if (voteDirection != VOTE_YES && voteDirection != VOTE_NO) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-vote-direction");
    }
    if (lockRefs.empty() || lockRefs.size() > GOV_VOTE_CAST_MAX_LOCK_REFS) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-refs");
    }
    if (sig.empty()) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-sig");
    }

    std::set<COutPoint> uniqueRefs;
    for (const auto& ref : lockRefs) {
        if (!uniqueRefs.emplace(ref).second) {
            return state.DoS(100, false, REJECT_INVALID, "bad-govtx-dup-lock-ref");
        }
    }

    return true;
}
