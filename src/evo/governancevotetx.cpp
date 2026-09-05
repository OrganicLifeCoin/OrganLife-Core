// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "evo/governancevotetx.h"

#include "budget/budgetproposal.h"
#include "consensus/validation.h"
#include "streams.h"
#include "utilstrencodings.h"

#include <set>

namespace {
template <typename T>
std::vector<unsigned char> EncodeData(const T& payload)
{
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << payload;
    return {stream.begin(), stream.end()};
}

template <typename T>
bool DecodeData(const pq::Payload& payload, uint8_t expectedMode, T& result)
{
    if (payload.mode != expectedMode || payload.data.empty()) return false;
    try {
        CDataStream stream(payload.data, SER_NETWORK, PROTOCOL_VERSION);
        stream >> result;
        return stream.empty() && EncodeData(result) == payload.data;
    } catch (const std::exception&) {
        return false;
    }
}
}

bool CGovProposalTx::IsTriviallyValid(CValidationState& state) const
{
    if (name.empty() || name.size() > 20 || name != SanitizeString(name))
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-proposal-name");
    if (url.empty() || url.size() > 64 || url != SanitizeString(url))
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-proposal-url");
    if (paymentCount == 0)
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-proposal-payment-count");
    if (blockStart == 0)
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-proposal-start");
    if (recipient == pq::KeyID{})
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-proposal-recipient");
    if (amount < PROPOSAL_MIN_AMOUNT || amount > PROPOSAL_MAX_AMOUNT)
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-proposal-amount");
    return true;
}

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
    if (ownerKeyId == pq::KeyID{}) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-owner-key");
    }

    return true;
}

std::vector<unsigned char> CGovVoteCastTx::GetSignatureMessage(const uint256& genesis) const
{
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << genesis << proposalHash << voteDirection << lockRefs << ownerPublicKey;
    return {stream.begin(), stream.end()};
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
    if (ownerPublicKey == mldsa44::PublicKey{} || sig == decltype(sig){}) {
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

std::vector<unsigned char> EncodeGovernanceData(const CGovProposalTx& payload) { return EncodeData(payload); }
std::vector<unsigned char> EncodeGovernanceData(const CGovVoteLockTx& payload) { return EncodeData(payload); }
std::vector<unsigned char> EncodeGovernanceData(const CGovVoteCastTx& payload) { return EncodeData(payload); }
bool DecodeGovernanceData(const pq::Payload& payload, CGovProposalTx& result)
{
    return DecodeData(payload, pq::GOVERNANCE_PROPOSAL, result);
}
bool DecodeGovernanceData(const pq::Payload& payload, CGovVoteLockTx& result)
{
    return DecodeData(payload, pq::GOVERNANCE_LOCK, result);
}
bool DecodeGovernanceData(const pq::Payload& payload, CGovVoteCastTx& result)
{
    return DecodeData(payload, pq::GOVERNANCE_CAST, result);
}
