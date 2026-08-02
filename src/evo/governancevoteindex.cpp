// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "evo/governancevoteindex.h"

#include "budget/budgetmanager_bridge.h"
#include "chain.h"
#include "chainparams.h"
#include "consensus/params.h"
#include "consensus/validation.h"
#include "evo/governancevotetx.h"
#include "primitives/block.h"
#include "script/standard.h"

#include <algorithm>
#include <limits>
#include <map>

std::unique_ptr<CGovernanceVoteIndex> governanceVoteIndex;

namespace {
struct CGovVoteTallyRecord
{
    int64_t coinYeas{0};
    int64_t coinNays{0};

    SERIALIZE_METHODS(CGovVoteTallyRecord, obj)
    {
        READWRITE(obj.coinYeas, obj.coinNays);
    }
};

int64_t SaturatingAdd(int64_t current, int64_t delta)
{
    if (delta <= 0) {
        return current;
    }
    if (current > std::numeric_limits<int64_t>::max() - delta) {
        return std::numeric_limits<int64_t>::max();
    }
    return current + delta;
}

int64_t SubFloorZero(int64_t current, int64_t delta)
{
    if (delta <= 0) {
        return current;
    }
    if (delta >= current) {
        return 0;
    }
    return current - delta;
}

bool FindLockOutputIndex(const CTransaction& tx, const CGovVoteLockTx& payload, uint32_t& outputIndexRet)
{
    const CScript ownerScript = GetScriptForDestination(payload.ownerKeyId);
    for (uint32_t i = 0; i < tx.vout.size(); ++i) {
        const CTxOut& out = tx.vout[i];
        if (out.nValue == payload.lockAmount && out.scriptPubKey == ownerScript) {
            outputIndexRet = i;
            return true;
        }
    }
    return false;
}

bool GetIndexedLockOutpoint(const CTransaction& tx, const CGovVoteLockTx& payload, COutPoint& outLockRef)
{
    uint32_t outputIndex = 0;
    if (!FindLockOutputIndex(tx, payload, outputIndex)) {
        return false;
    }
    outLockRef = COutPoint(tx.GetHash(), outputIndex);
    return true;
}

bool IsLockRecordScopeConsistent(const CGovVoteLockRecord& lockRecord)
{
    if (lockRecord.proposalHash.IsNull()) {
        return false;
    }
    return std::all_of(lockRecord.proposalVoteDirections.cbegin(),
                       lockRecord.proposalVoteDirections.cend(),
                       [&lockRecord](const std::pair<const uint256, uint8_t>& entry) {
                           return entry.first == lockRecord.proposalHash;
                       });
}

int64_t ToVoteWeightCoins(CAmount amount)
{
    if (amount <= 0) {
        return 0;
    }
    return amount / COIN;
}

CGovVoteLockRecord BuildLockRecord(const CGovVoteLockTx& payload, uint32_t blockHeight)
{
    CGovVoteLockRecord record;
    record.proposalHash = payload.proposalHash;
    record.lockAmount = payload.lockAmount;
    record.ownerKeyId = payload.ownerKeyId;
    record.unlockHeight = payload.unlockHeight;
    record.createdHeight = blockHeight;
    return record;
}

bool CheckAndStageCast(const CGovVoteCastTx& castPayload,
                        const std::map<COutPoint, CGovVoteLockRecord>& incomingChanges,
                        std::map<COutPoint, CGovVoteLockRecord>& outgoingChanges,
                        uint32_t blockHeight,
                        CEvoDB& evoDb,
                        CValidationState& state)
{
    if (IsBudgetProposalExpired(castPayload.proposalHash, static_cast<int>(blockHeight))) {
        return false;
    }

    CKeyID ownerKeyId;
    bool ownerSet = false;
    for (const auto& lockRef : castPayload.lockRefs) {
        CGovVoteLockRecord lockRecord;

        const auto itOutgoing = outgoingChanges.find(lockRef);
        if (itOutgoing != outgoingChanges.end()) {
            lockRecord = itOutgoing->second;
        } else {
            const auto itIncoming = incomingChanges.find(lockRef);
            if (itIncoming != incomingChanges.end()) {
                lockRecord = itIncoming->second;
            } else if (!evoDb.Read(std::make_pair(EVODB_GOV_LOCK, lockRef), lockRecord)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-ref");
            }
        }

        if (!IsLockRecordScopeConsistent(lockRecord)) {
            return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-scope");
        }
        if (lockRecord.proposalHash != castPayload.proposalHash) {
            return state.DoS(100, false, REJECT_INVALID, "bad-govtx-proposal-mismatch");
        }
        if (lockRecord.unlockHeight <= blockHeight) {
            return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-expired");
        }
        if (!ownerSet) {
            ownerKeyId = lockRecord.ownerKeyId;
            ownerSet = true;
        } else if (ownerKeyId != lockRecord.ownerKeyId) {
            return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-owner-mismatch");
        }
        if (lockRecord.HasVotedForProposal(castPayload.proposalHash)) {
            return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-already-used");
        }

        lockRecord.proposalVoteDirections[castPayload.proposalHash] = castPayload.voteDirection;
        outgoingChanges[lockRef] = std::move(lockRecord);
    }
    return true;
}
} // namespace

CGovernanceVoteIndex::CGovernanceVoteIndex(CEvoDB& _evoDb) : evoDb(_evoDb)
{
}

bool CGovernanceVoteIndex::GetProposalTally(const uint256& proposalHash, int64_t& coinYeas, int64_t& coinNays) const
{
    coinYeas = 0;
    coinNays = 0;

    CGovVoteTallyRecord tallyRecord;
    if (!evoDb.Read(std::make_pair(EVODB_GOV_TALLY, proposalHash), tallyRecord)) {
        return false;
    }

    coinYeas = std::max<int64_t>(0, tallyRecord.coinYeas);
    coinNays = std::max<int64_t>(0, tallyRecord.coinNays);
    return true;
}

bool CGovernanceVoteIndex::GetLockRecord(const COutPoint& lockRef, CGovVoteLockRecord& outRecord) const
{
    return evoDb.Read(std::make_pair(EVODB_GOV_LOCK, lockRef), outRecord);
}

bool CGovernanceVoteIndex::IsLockUsedForProposal(const COutPoint& lockRef, const uint256& proposalHash) const
{
    CGovVoteLockRecord lockRecord;
    if (!GetLockRecord(lockRef, lockRecord)) {
        return false;
    }
    return lockRecord.HasVotedForProposal(proposalHash);
}

bool CGovernanceVoteIndex::ApplyLockTx(const CTransaction& tx, uint32_t blockHeight, CValidationState& state)
{
    if (tx.nType != CTransaction::TxType::GOVVOTELOCK) {
        return true;
    }

    CGovVoteLockTx payload;
    if (!GetTxPayload(tx, payload) || !payload.IsTriviallyValid(state)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-payload");
    }

    COutPoint lockRef;
    if (!GetIndexedLockOutpoint(tx, payload, lockRef)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-output");
    }
    if (evoDb.Exists(std::make_pair(EVODB_GOV_LOCK, lockRef))) {
        return state.DoS(100, false, REJECT_DUPLICATE, "bad-govtx-duplicate-lock");
    }

    evoDb.Write(std::make_pair(EVODB_GOV_LOCK, lockRef), BuildLockRecord(payload, blockHeight));
    return true;
}

bool CGovernanceVoteIndex::ApplyCastTx(const CTransaction& tx, uint32_t blockHeight, CValidationState& state)
{
    if (tx.nType != CTransaction::TxType::GOVVOTECAST) {
        return true;
    }

    CGovVoteCastTx payload;
    if (!GetTxPayload(tx, payload) || !payload.IsTriviallyValid(state)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-govtx-cast-payload");
    }

    std::map<COutPoint, CGovVoteLockRecord> updates;
    if (!CheckAndStageCast(payload, {}, updates, blockHeight, evoDb, state)) {
        return false;
    }

    int64_t castWeightCoins = 0;
    for (const auto& [lockRef, lockRecord] : updates) {
        castWeightCoins = SaturatingAdd(castWeightCoins, ToVoteWeightCoins(lockRecord.lockAmount));
        evoDb.Write(std::make_pair(EVODB_GOV_LOCK, lockRef), lockRecord);
    }

    CGovVoteTallyRecord tallyRecord;
    evoDb.Read(std::make_pair(EVODB_GOV_TALLY, payload.proposalHash), tallyRecord);
    if (payload.voteDirection == CGovVoteCastTx::VOTE_YES) {
        tallyRecord.coinYeas = SaturatingAdd(tallyRecord.coinYeas, castWeightCoins);
    } else if (payload.voteDirection == CGovVoteCastTx::VOTE_NO) {
        tallyRecord.coinNays = SaturatingAdd(tallyRecord.coinNays, castWeightCoins);
    }
    evoDb.Write(std::make_pair(EVODB_GOV_TALLY, payload.proposalHash), tallyRecord);

    const bool fYesVote = (payload.voteDirection == CGovVoteCastTx::VOTE_YES);
    UpdateBudgetProposalCoinVotes(payload.proposalHash, castWeightCoins, fYesVote);
    return true;
}

bool CGovernanceVoteIndex::UndoLockTx(const CTransaction& tx)
{
    if (tx.nType != CTransaction::TxType::GOVVOTELOCK) {
        return true;
    }

    CGovVoteLockTx payload;
    if (!GetTxPayload(tx, payload)) {
        return false;
    }

    COutPoint lockRef;
    if (!GetIndexedLockOutpoint(tx, payload, lockRef)) {
        return false;
    }

    evoDb.Erase(std::make_pair(EVODB_GOV_LOCK, lockRef));
    return true;
}

bool CGovernanceVoteIndex::UndoCastTx(const CTransaction& tx)
{
    if (tx.nType != CTransaction::TxType::GOVVOTECAST) {
        return true;
    }

    CGovVoteCastTx payload;
    if (!GetTxPayload(tx, payload)) {
        return false;
    }

    int64_t castWeightCoins = 0;
    for (const auto& lockRef : payload.lockRefs) {
        CGovVoteLockRecord lockRecord;
        if (!evoDb.Read(std::make_pair(EVODB_GOV_LOCK, lockRef), lockRecord)) {
            return false;
        }
        castWeightCoins = SaturatingAdd(castWeightCoins, ToVoteWeightCoins(lockRecord.lockAmount));
        lockRecord.proposalVoteDirections.erase(payload.proposalHash);
        evoDb.Write(std::make_pair(EVODB_GOV_LOCK, lockRef), lockRecord);
    }

    CGovVoteTallyRecord tallyRecord;
    if (evoDb.Read(std::make_pair(EVODB_GOV_TALLY, payload.proposalHash), tallyRecord)) {
        if (payload.voteDirection == CGovVoteCastTx::VOTE_YES) {
            tallyRecord.coinYeas = SubFloorZero(tallyRecord.coinYeas, castWeightCoins);
        } else if (payload.voteDirection == CGovVoteCastTx::VOTE_NO) {
            tallyRecord.coinNays = SubFloorZero(tallyRecord.coinNays, castWeightCoins);
        }

        if (tallyRecord.coinYeas == 0 && tallyRecord.coinNays == 0) {
            evoDb.Erase(std::make_pair(EVODB_GOV_TALLY, payload.proposalHash));
        } else {
            evoDb.Write(std::make_pair(EVODB_GOV_TALLY, payload.proposalHash), tallyRecord);
        }
    }

    const bool fYesVote = (payload.voteDirection == CGovVoteCastTx::VOTE_YES);
    DecrementBudgetProposalCoinVotes(payload.proposalHash, castWeightCoins, fYesVote);

    return true;
}

bool CGovernanceVoteIndex::ProcessBlock(const CBlock& block, const CBlockIndex* pindex, CValidationState& state, bool fJustCheck)
{
    if (!Params().GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_V6_1_GOV)) {
        return true;
    }

    if (!fJustCheck) {
        for (const CTransactionRef& tx : block.vtx) {
            if (!ApplyLockTx(*tx, pindex->nHeight, state)) {
                return false;
            }
            if (!ApplyCastTx(*tx, pindex->nHeight, state)) {
                return false;
            }
        }
        return true;
    }

    std::map<COutPoint, CGovVoteLockRecord> lockAdds;
    std::map<COutPoint, CGovVoteLockRecord> lockUpdates;

    for (const CTransactionRef& tx : block.vtx) {
        if (tx->nType == CTransaction::TxType::GOVVOTELOCK) {
            CGovVoteLockTx lockPayload;
            if (!GetTxPayload(*tx, lockPayload) || !lockPayload.IsTriviallyValid(state)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-payload");
            }

            COutPoint lockRef;
            if (!GetIndexedLockOutpoint(*tx, lockPayload, lockRef)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-govtx-lock-output");
            }
            if (lockAdds.count(lockRef) != 0 || evoDb.Exists(std::make_pair(EVODB_GOV_LOCK, lockRef))) {
                return state.DoS(100, false, REJECT_DUPLICATE, "bad-govtx-duplicate-lock");
            }
            lockAdds.emplace(lockRef, BuildLockRecord(lockPayload, pindex->nHeight));
            continue;
        }

        if (tx->nType == CTransaction::TxType::GOVVOTECAST) {
            CGovVoteCastTx castPayload;
            if (!GetTxPayload(*tx, castPayload) || !castPayload.IsTriviallyValid(state)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-govtx-cast-payload");
            }

            if (!CheckAndStageCast(castPayload, lockAdds, lockUpdates, pindex->nHeight, evoDb, state)) {
                return false;
            }
        }
    }

    return true;
}

bool CGovernanceVoteIndex::UndoBlock(const CBlock& block, const CBlockIndex* pindex)
{
    if (!Params().GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_V6_1_GOV)) {
        return true;
    }

    for (auto it = block.vtx.rbegin(); it != block.vtx.rend(); ++it) {
        const CTransaction& tx = **it;
        if (tx.nType == CTransaction::TxType::GOVVOTECAST) {
            if (!UndoCastTx(tx)) {
                return false;
            }
            continue;
        }
        if (tx.nType == CTransaction::TxType::GOVVOTELOCK) {
            if (!UndoLockTx(tx)) {
                return false;
            }
        }
    }

    return true;
}
