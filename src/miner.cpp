// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2011-2013 The PPCoin developers
// Copyright (c) 2013-2014 The NovaCoin Developers
// Copyright (c) 2014-2018 The BlackCoin Developers
// Copyright (c) 2015-2021 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "amount.h"
#include "blockassembler.h"
#include "consensus/params.h"
#include "masternode-sync.h"
#include "net.h"
#include "net_processing.h"
#include "policy/feerate.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "timedata.h"
#include "util/blockstatecatcher.h"
#include "util/system.h"
#include "utilmoneystr.h"
#include "validation.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif
#include "invalid.h"
#include "policy/policy.h"

#include <boost/thread.hpp>

#ifdef ENABLE_WALLET
//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//
double dHashesPerSec = 0.0;
int64_t nHPSTimerStart = 0;

std::unique_ptr<CBlockTemplate> CreateNewBlockWithKey(std::unique_ptr<CReserveKey>& reservekey, CWallet* pwallet)
{
    CPubKey pubkey;
    if (!reservekey->GetReservedKey(pubkey)) return nullptr;
    return CreateNewBlockWithScript(GetScriptForDestination(pubkey.GetID()), pwallet);
}

std::unique_ptr<CBlockTemplate> CreateNewBlockWithScript(const CScript& coinbaseScript, CWallet* pwallet)
{
    const int nHeightNext = chainActive.Tip()->nHeight + 1;

    // If we're building a late PoW block, don't continue
    // PoS blocks are built directly with CreateNewBlock
    if (Params().GetConsensus().NetworkUpgradeActive(nHeightNext, Consensus::UPGRADE_POS)) {
        LogPrint(BCLog::STAKING, "%s: Aborting PoW block creation during PoS phase\n", __func__);
        // sleep 1/2 a block time so we don't go into a tight loop.
        MilliSleep((Params().GetConsensus().nTargetSpacing * 1000) >> 1);
        return nullptr;
    }

    return BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(coinbaseScript, pwallet, false);
}

bool ProcessBlockFound(const std::shared_ptr<const CBlock>& pblock, CWallet& wallet, std::unique_ptr<CReserveKey>& reservekey)
{
    LogPrint(BCLog::STAKING, "%s\n", pblock->ToString());
    LogPrint(BCLog::STAKING, "generated %s\n", FormatMoney(pblock->vtx[0]->vout[0].nValue));

    // Found a solution
    {
        WAIT_LOCK(g_best_block_mutex, lock);
        if (pblock->hashPrevBlock != g_best_block)
            return error("%s: generated block is stale", __func__);
    }

    // Remove key from key pool
    if (reservekey)
        reservekey->KeepKey();

    // Process this block the same as if we had received it from another node
    LogPrint(BCLog::NET, "%s: About to process new block %s\n", __func__, pblock->GetHash().ToString());
    const int64_t nStartTime = GetTime();
    
    BlockStateCatcherWrapper sc(pblock->GetHash());
    sc.registerEvent();
    bool res = ProcessNewBlock(pblock, nullptr);
    
    // Check if process took suspiciously long (potential deadlock)
    const int64_t nTimeTaken = GetTime() - nStartTime;
    if (nTimeTaken > 5) {
        LogPrintf("%s: ProcessNewBlock took %ld seconds for block %s\n", __func__, nTimeTaken, pblock->GetHash().ToString());
    }

    if (!res || sc.get().stateErrorFound()) {
        LogPrintf("%s: ProcessNewBlock failed - res=%d, stateErrorFound=%d\n", __func__, res, sc.get().stateErrorFound() ? 1 : 0);
        return error("%s: ProcessNewBlock, block not accepted", __func__);
    }

    g_connman->ForEachNode([&pblock](CNode* node)
    {
        node->PushInventory(CInv(MSG_BLOCK, pblock->GetHash()));
    });

    return true;
}

bool fGenerateBitcoins = false;

bool CheckForCoins(CWallet* pwallet, std::vector<CStakeableOutput>* availableCoins, bool lastResult)
{
    if (!pwallet || !pwallet->pStakerStatus)
        return false;

    // control the amount of times the client will check for mintable coins (every block)
    {
        WAIT_LOCK(g_best_block_mutex, lock);
        if (g_best_block == pwallet->pStakerStatus->GetLastHash())
            return lastResult;
    }
    return pwallet->StakeableCoins(availableCoins);
}

void BitcoinMiner(CWallet* pwallet, bool fProofOfStake)
{
    LogPrint(BCLog::STAKING, "%s: started\n", __func__);
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    util::ThreadRename("pivx-miner");
    const Consensus::Params& consensus = Params().GetConsensus();
    const int64_t nSpacingMillis = consensus.nTargetSpacing * 1000;

    // Each thread has its own key and counter
    std::unique_ptr<CReserveKey> pReservekey = fProofOfStake ? nullptr : std::make_unique<CReserveKey>(pwallet);

    // Available UTXO set
    std::vector<CStakeableOutput> availableCoins;
    bool fStakeableCoins = false;
    unsigned int nExtraNonce = 0;

    while (fGenerateBitcoins || fProofOfStake) {
        CBlockIndex* pindexPrev = GetChainTip();
        if (!pindexPrev) {
            MilliSleep(nSpacingMillis);       // sleep a block
            continue;
        }

        // Testnet stability: avoid PoW mining on very low connectivity, which otherwise
        // tends to produce frequent competing tips and deep reorgs when multiple nodes
        // are mining. Override with -minpowconnections=N.
        if (!fProofOfStake && Params().MiningRequiresPeers() && g_connman) {
            // Keep this opt-in: by default, allow mining even with few peers.
            const int minPowConnections = gArgs.GetArg("-minpowconnections", 0);
            if (minPowConnections > 0) {
                // Require at least N peers overall and also N peers that are close to our current tip.
                // The "near-tip" check helps avoid mining while partitioned or only connected to
                // stale peers (a common cause of persistent forks on small networks).
                if (g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) < minPowConnections) {
                    MilliSleep(5000);
                    continue;
                }

                int nearTipPeers = 0;
                std::vector<CNodeStats> vstats;
                g_connman->GetNodeStats(vstats);
                for (const auto& stats : vstats) {
                    CNodeStateStats st;
                    if (!GetNodeStateStats(stats.nodeid, st)) continue;
                    // Consider a peer near-tip if we share a common height within 2 blocks of our tip.
                    if (st.nCommonHeight >= pindexPrev->nHeight - 2) nearTipPeers++;
                }
                if (nearTipPeers < minPowConnections) {
                    MilliSleep(5000);
                    continue;
                }
            }
        }

        if (fProofOfStake) {
            if (!consensus.NetworkUpgradeActive(pindexPrev->nHeight + 1, Consensus::UPGRADE_POS)) {
                // The last PoW block hasn't even been mined yet.
                MilliSleep(nSpacingMillis);       // sleep a block
                continue;
            }

            // update fStakeableCoins
            fStakeableCoins = CheckForCoins(pwallet, &availableCoins, fStakeableCoins);

            while ((g_connman && g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0 && Params().MiningRequiresPeers())
                    || IsInitialBlockDownload()
                    || pwallet->IsLocked() || !fStakeableCoins
                    || IsForkRecoveryPauseStaking()
                    || masternodeSync.NotCompleted()) {
                MilliSleep(5000);
                // Do another check here to ensure fStakeableCoins is updated
                if (!fStakeableCoins) {
                    fStakeableCoins = CheckForCoins(pwallet, &availableCoins, fStakeableCoins);
                }
            }

            //search our map of hashed blocks, see if bestblock has been hashed yet
            if (pwallet->pStakerStatus &&
                    pwallet->pStakerStatus->GetLastHash() == pindexPrev->GetBlockHash() &&
                    pwallet->pStakerStatus->GetLastTime() >= GetCurrentTimeSlot()) {
                MilliSleep(2000);
                continue;
            }

        } else if (pindexPrev->nHeight > 6 && consensus.NetworkUpgradeActive(pindexPrev->nHeight - 6, Consensus::UPGRADE_POS)) {
            // Late PoW: run for a little while longer, just in case there is a rewind on the chain.
            LogPrint(BCLog::STAKING, "%s: Exiting PoW mining thread at height %d\n", __func__, pindexPrev->nHeight);
            return;
       }

        //
        // Create new block
        //
        unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();

        std::unique_ptr<CBlockTemplate> pblocktemplate((fProofOfStake ?
                                                        BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(CScript(), pwallet, true, &availableCoins) :
                                                        CreateNewBlockWithKey(pReservekey, pwallet)));
        if (!pblocktemplate) continue;
        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block);

        // POS - block found: process it
        if (fProofOfStake) {
            LogPrint(BCLog::STAKING, "%s: proof-of-stake block was signed %s\n", __func__, pblock->GetHash().ToString().c_str());
            SetThreadPriority(THREAD_PRIORITY_NORMAL);
            if (!ProcessBlockFound(pblock, *pwallet, pReservekey)) {
                LogPrint(BCLog::STAKING, "%s: new block orphaned\n", __func__);
                continue;
            }
            SetThreadPriority(THREAD_PRIORITY_LOWEST);
            continue;
        }

        // POW - miner main
        IncrementExtraNonce(pblock, pindexPrev->nHeight + 1, nExtraNonce);
        LogPrint(BCLog::STAKING, "%s: candidate block with %u transactions (%u bytes)\n",
                 __func__, pblock->vtx.size(), ::GetSerializeSize(*pblock, PROTOCOL_VERSION));

        //
        // Search
        //
        int64_t nStart = GetTime();
        arith_uint256 hashTarget;
        hashTarget.SetCompact(pblock->nBits);
        while (true) {
            unsigned int nHashesDone = 0;

            arith_uint256 hash;
            while (true) {
                hash = UintToArith256(pblock->GetHash());
                if (hash <= hashTarget) {
                    // Found a solution
                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    LogPrint(BCLog::STAKING, "%s:\n", __func__);
                    LogPrint(BCLog::STAKING, "proof-of-work found\n  hash: %s\n  target: %s\n", hash.GetHex(), hashTarget.GetHex());
                    ProcessBlockFound(pblock, *pwallet, pReservekey);
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);

                    // In regression test mode, stop mining after a block is found. This
                    // allows developers to controllably generate a block on demand.
                    if (Params().IsRegTestNet())
                        throw boost::thread_interrupted();

                    // Sleep for a short while to avoid spamming the network on low difficulty
                    MilliSleep(100);

                    break;
                }
                pblock->nNonce += 1;
                nHashesDone += 1;
                if ((pblock->nNonce & 0xFF) == 0)
                    break;
            }

            // Meter hashes/sec
            static int64_t nHashCounter;
            if (nHPSTimerStart == 0) {
                nHPSTimerStart = GetTimeMillis();
                nHashCounter = 0;
            } else
                nHashCounter += nHashesDone;
            if (GetTimeMillis() - nHPSTimerStart > 4000) {
                static RecursiveMutex cs;
                {
                    LOCK(cs);
                    if (GetTimeMillis() - nHPSTimerStart > 4000) {
                        dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                        static int64_t nLogTime;
                        if (GetTime() - nLogTime > 30 * 60) {
                            nLogTime = GetTime();
                            LogPrint(BCLog::STAKING, "hashmeter %6.0f khash/s\n", dHashesPerSec / 1000.0);
                        }
                    }
                }
            }

            // Check for stop or if block needs to be rebuilt
            boost::this_thread::interruption_point();
            if (    (g_connman && g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0 && Params().MiningRequiresPeers()) || // Regtest mode doesn't require peers
                    (pblock->nNonce >= 0xffff0000) ||
                    (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60) ||
                    (pindexPrev != chainActive.Tip())
                ) break;

            // Update nTime every few seconds
            UpdateTime(pblock.get(), consensus, pindexPrev);
            if (Params().GetConsensus().fPowAllowMinDifficultyBlocks) {
                // Changing pblock->nTime can change work required on testnet:
                hashTarget.SetCompact(pblock->nBits);
            }

        }
    }
}

void static ThreadBitcoinMiner(void* parg)
{
    boost::this_thread::interruption_point();
    CWallet* pwallet = (CWallet*)parg;
    try {
        BitcoinMiner(pwallet, false);
        boost::this_thread::interruption_point();
    } catch (const boost::thread_interrupted&) {
        LogPrint(BCLog::STAKING, "%s: interrupted\n", __func__);
    } catch (const std::exception& e) {
        LogPrintf("%s: exception: %s\n", __func__, e.what());
    } catch (...) {
        LogPrintf("%s: unknown exception\n", __func__);
    }
    LogPrint(BCLog::STAKING, "%s: exiting\n", __func__);
}

void GenerateBitcoins(bool fGenerate, CWallet* pwallet, int nThreads)
{
    static boost::thread_group* minerThreads = nullptr;
    fGenerateBitcoins = fGenerate;

    if (minerThreads != nullptr) {
        minerThreads->interrupt_all();
        minerThreads->join_all();
        delete minerThreads;
        minerThreads = nullptr;
    }

    // -1 means "all cores" (match -genproclimit semantics).
    if (nThreads < 0) {
        nThreads = std::max(1, GetNumCores());
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        minerThreads->create_thread(std::bind(&ThreadBitcoinMiner, pwallet));
}

void ThreadStakeMinter(CWallet* pwallet)
{
    boost::this_thread::interruption_point();
    if (!pwallet) {
        LogPrintf("ThreadStakeMinter started without a wallet\n");
        return;
    }

    LogPrintf("ThreadStakeMinter started for wallet '%s'\n", pwallet->GetName());
    while (true) {
        try {
            BitcoinMiner(pwallet, true);
            boost::this_thread::interruption_point();
            break;
        } catch (const boost::thread_interrupted&) {
            LogPrint(BCLog::STAKING, "ThreadStakeMinter interrupted for wallet '%s'\n", pwallet->GetName());
            break;
        } catch (const std::exception& e) {
            LogPrintf("ThreadStakeMinter('%s') exception: %s\n", pwallet->GetName(), e.what());
            MilliSleep(2000);
        } catch (...) {
            LogPrintf("ThreadStakeMinter('%s') unknown error\n", pwallet->GetName());
            MilliSleep(2000);
        }
    }
    LogPrintf("ThreadStakeMinter exiting for wallet '%s'\n", pwallet->GetName());
}

#endif // ENABLE_WALLET
