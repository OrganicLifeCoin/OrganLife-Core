// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2016-2021 The PIVX Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_MINER_H
#define PIVX_MINER_H

#include "primitives/block.h"

#include <stdint.h>
#include <vector>

class CBlock;
class CBlockHeader;
class CBlockIndex;
class CChainParams;
class CStakeableOutput;
class CReserveKey;
class CScript;
class CStakerStatus;
class CWallet;

struct CBlockTemplate;

static const bool DEFAULT_PRINTPRIORITY = false;

int GetMinimumStakingPeerEvidence(const CChainParams& params);
bool RequiresNearTipStakingPeerEvidence(const CChainParams& params);

#ifdef ENABLE_WALLET
    /** Run the miner threads */
    void GenerateBitcoins(bool fGenerate, CWallet* pwallet, int nThreads);
    /** Generate a new PoW block, without valid proof-of-work */
    std::unique_ptr<CBlockTemplate> CreateNewBlockWithKey(std::unique_ptr<CReserveKey>& reservekey, CWallet* pwallet);
    std::unique_ptr<CBlockTemplate> CreateNewBlockWithScript(const CScript& coinbaseScript, CWallet* pwallet);

    void BitcoinMiner(CWallet* pwallet, bool fProofOfStake);
    void ThreadStakeMinter(CWallet* pwallet);

    enum class StakerSlotWait {
        Wait,     // last attempt was made on the exact current slot: sleep and retry later
        Proceed,  // a new slot (or tip) is available: run a fresh kernel search
    };

    /**
     * Decide whether the stake minter should wait before re-attempting the
     * current time slot.
     *
     * The minter waits only when the last attempt was already made on the exact
     * current slot. A recorded last-attempt time in the future (clock skew, or
     * a stale artifact of a previously minted too-future slot) is treated as
     * invalid: the staker status is reset and staking proceeds, so the minter
     * can never be wedged waiting for a slot that has already passed.
     */
    StakerSlotWait ShouldStakerWaitForSlot(CStakerStatus* status, const CBlockIndex* pindexPrev, int64_t currentSlot);

    /**
     * Decide whether the stake minter may produce blocks while the local tip is
     * stale ("stale-tip" recovery).
     *
     * Staking is allowed when:
     *  - the best known header is stale (older than maxTipAge), and
     *  - no reachable peer has a better header than ours (localHeight >=
     *    bestHeaderHeight), and
     *  - enough peers are near the local tip (per minPeerEvidence /
     *    requireNearTip / peerHeightTolerance) with none ahead of it.
     *
     * This lets a network whose nodes are all stuck at the same stale tip
     * resume staking (reviving the chain) without waiting for a restart, while
     * still refusing to mint on a fork while any reachable peer is ahead.
     */
    bool CanStakeDuringStaleTip(const int localHeight,
                                const int bestHeaderHeight,
                                const int64_t bestHeaderTime,
                                const int64_t now,
                                const int64_t maxTipAge,
                                const std::vector<int>& peerHeights,
                                const int minPeerEvidence,
                                const bool requireNearTip,
                                const int peerHeightTolerance);
#endif // ENABLE_WALLET

extern double dHashesPerSec;
extern int64_t nHPSTimerStart;

#endif // PIVX_MINER_H
