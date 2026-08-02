// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_EVO_GOVERNANCEVOTEINDEX_H
#define PIVX_EVO_GOVERNANCEVOTEINDEX_H

#include "amount.h"
#include "evo/evodb.h"
#include "pubkey.h"
#include "serialize.h"
#include "uint256.h"

#include <map>
#include <memory>
#include <set>

class CBlock;
class CBlockIndex;
struct CGovVoteCastTx;
class COutPoint;
class CTransaction;
class CValidationState;

struct CGovVoteLockRecord
{
    uint256 proposalHash;
    CAmount lockAmount{0};
    CKeyID ownerKeyId;
    uint32_t unlockHeight{0};
    uint32_t createdHeight{0};
    std::map<uint256, uint8_t> proposalVoteDirections;

    bool HasVotedForProposal(const uint256& proposalHash) const
    {
        return proposalVoteDirections.count(proposalHash) != 0;
    }

    uint8_t GetVoteDirection(const uint256& proposalHash) const
    {
        auto it = proposalVoteDirections.find(proposalHash);
        return it != proposalVoteDirections.end() ? it->second : 0;
    }

    SERIALIZE_METHODS(CGovVoteLockRecord, obj)
    {
        READWRITE(obj.proposalHash, obj.lockAmount, obj.ownerKeyId, obj.unlockHeight, obj.createdHeight, obj.proposalVoteDirections);
    }
};

class CGovernanceVoteIndex
{
public:
    explicit CGovernanceVoteIndex(CEvoDB& _evoDb);

    bool ProcessBlock(const CBlock& block, const CBlockIndex* pindex, CValidationState& state, bool fJustCheck);
    bool UndoBlock(const CBlock& block, const CBlockIndex* pindex);

    bool ApplyLockTx(const CTransaction& tx, uint32_t blockHeight, CValidationState& state);
    bool ApplyCastTx(const CTransaction& tx, uint32_t blockHeight, CValidationState& state);
    bool UndoLockTx(const CTransaction& tx);
    bool UndoCastTx(const CTransaction& tx);

    bool GetLockRecord(const COutPoint& lockRef, CGovVoteLockRecord& outRecord) const;
    bool IsLockUsedForProposal(const COutPoint& lockRef, const uint256& proposalHash) const;
    bool GetProposalTally(const uint256& proposalHash, int64_t& coinYeas, int64_t& coinNays) const;

private:
    CEvoDB& evoDb;
};

extern std::unique_ptr<CGovernanceVoteIndex> governanceVoteIndex;

#endif // PIVX_EVO_GOVERNANCEVOTEINDEX_H
