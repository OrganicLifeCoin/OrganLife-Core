// Copyright (c) 2019 The Dash Core developers
// Copyright (c) 2023 The PIVX Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_LLMQ_QUORUMS_CHAINLOCKS_H
#define PIVX_LLMQ_QUORUMS_CHAINLOCKS_H

#include "llmq/quorums.h"
#include "llmq/quorums_signing.h"

#include "net.h"
#include "chainparams.h"

#include <atomic>

class CBlockIndex;
class CScheduler;

namespace llmq
{

class CChainLockSig
{
public:
    int32_t nHeight{-1};
    uint256 blockHash;
    CBLSSignature sig;

public:
    SERIALIZE_METHODS(CChainLockSig, obj)
    {
        READWRITE(obj.nHeight);
        READWRITE(obj.blockHash);
        READWRITE(obj.sig);
    }

    bool IsNull() const;
    std::string ToString() const;
};

class CChainLocksHandler : public CRecoveredSigsListener
{
    static const int64_t CLEANUP_INTERVAL = 1000 * 30;
    static const int64_t CLEANUP_SEEN_TIMEOUT = 24 * 60 * 60 * 1000;

private:
    CScheduler* scheduler;
    RecursiveMutex cs;
    bool tryLockChainTipScheduled GUARDED_BY(cs){false};

    uint256 bestChainLockHash GUARDED_BY(cs);
    CChainLockSig bestChainLock GUARDED_BY(cs);

    CChainLockSig bestChainLockWithKnownBlock GUARDED_BY(cs);
    const CBlockIndex* bestChainLockBlockIndex GUARDED_BY(cs){nullptr};

    int32_t lastSignedHeight GUARDED_BY(cs){-1};
    uint256 lastSignedRequestId GUARDED_BY(cs);
    uint256 lastSignedMsgHash GUARDED_BY(cs);

    std::map<uint256, int64_t> seenChainLocks GUARDED_BY(cs);

    int64_t lastCleanupTime GUARDED_BY(cs){0};

public:
    explicit CChainLocksHandler(CScheduler* _scheduler);
    ~CChainLocksHandler();
    void Start();
    void Stop();

public:
    bool AlreadyHave(const CInv& inv);
    bool GetChainLockByHash(const uint256& hash, CChainLockSig& ret);
    CChainLockSig GetBestChainLock();

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman);
    void ProcessNewChainLock(NodeId from, const CChainLockSig& clsig, const uint256& hash);
    void AcceptedBlockHeader(const CBlockIndex* pindexNew);
    void UpdatedBlockTip(const CBlockIndex* pindexNew);
    void TrySignChainTip();
    void EnforceBestChainLock();
    virtual void HandleNewRecoveredSig(const CRecoveredSig& recoveredSig);

    bool HasChainLock(int nHeight, const uint256& blockHash);
    bool HasConflictingChainLock(int nHeight, const uint256& blockHash);

private:
    // these require locks to be held already
    bool InternalHasChainLock(int nHeight, const uint256& blockHash) EXCLUSIVE_LOCKS_REQUIRED(cs);
    bool InternalHasConflictingChainLock(int nHeight, const uint256& blockHash) EXCLUSIVE_LOCKS_REQUIRED(cs);

    void DoInvalidateBlock(const CBlockIndex* pindex, bool activateBestChain) LOCKS_EXCLUDED(cs);

    void Cleanup() LOCKS_EXCLUDED(cs, cs_main);
};

extern std::unique_ptr<CChainLocksHandler> chainLocksHandler;
}

#endif // PIVX_LLMQ_QUORUMS_CHAINLOCKS_H
