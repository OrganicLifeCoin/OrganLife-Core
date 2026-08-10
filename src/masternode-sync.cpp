// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// clang-format off
#include "addrman.h"
#include "budget/budgetmanager.h"
#include "chainparams.h"
#include "masternode-sync.h"
#include "netmessagemaker.h"
#include "net_processing.h"
#include "random.h"
#include "tiertwo/netfulfilledman.h"
#include "spork.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "util/system.h"
#include "validation.h"
#include <algorithm>
// clang-format on

class CMasternodeSync;
CMasternodeSync masternodeSync;

CMasternodeSync::CMasternodeSync()
{
    Reset();
}

bool CMasternodeSync::NotCompleted()
{
    return (!g_tiertwo_sync_state.IsSynced() && (
            !g_tiertwo_sync_state.IsSporkListSynced() ||
            sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT) ||
            sporkManager.IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT) ||
            sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)));
}

void CMasternodeSync::UpdateBlockchainSynced(bool isRegTestNet)
{
    if (!isRegTestNet && !g_tiertwo_sync_state.CanUpdateChainSync(lastProcess)) return;
    if (fImporting || fReindex) return;

    const bool coreChainSynced = !IsInitialBlockDownload();
    g_tiertwo_sync_state.SetBlockchainSync(coreChainSynced, lastProcess);
}

void CMasternodeSync::Reset()
{
    g_tiertwo_sync_state.SetBlockchainSync(false, 0);
    g_tiertwo_sync_state.ResetData();
    lastProcess = 0;
    lastFailure = 0;
    nCountFailures = 0;
    sumBudgetItemProp = 0;
    sumBudgetItemFin = 0;
    countBudgetItemProp = 0;
    countBudgetItemFin = 0;
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_INITIAL);
    RequestedMasternodeAttempt = 0;
    nAssetSyncStarted = GetTime();
}

bool CMasternodeSync::IsBudgetPropEmpty()
{
    return sumBudgetItemProp == 0 && countBudgetItemProp > 0;
}

bool CMasternodeSync::IsBudgetFinEmpty()
{
    return sumBudgetItemFin == 0 && countBudgetItemFin > 0;
}

int CMasternodeSync::GetNextAsset(int currentAsset)
{
    if (currentAsset > MASTERNODE_SYNC_FINISHED) {
        LogPrintf("%s - invalid asset %d\n", __func__, currentAsset);
        return MASTERNODE_SYNC_FAILED;
    }
    switch (currentAsset) {
    case (MASTERNODE_SYNC_INITIAL):
    case (MASTERNODE_SYNC_FAILED):
        return MASTERNODE_SYNC_SPORKS;
    case (MASTERNODE_SYNC_SPORKS):
        return MASTERNODE_SYNC_BUDGET;
    case (MASTERNODE_SYNC_BUDGET):
    default:
        return MASTERNODE_SYNC_FINISHED;
    }
}

void CMasternodeSync::SwitchToNextAsset()
{
    int RequestedMasternodeAssets = g_tiertwo_sync_state.GetSyncPhase();
    if (RequestedMasternodeAssets == MASTERNODE_SYNC_INITIAL ||
            RequestedMasternodeAssets == MASTERNODE_SYNC_FAILED) {
        ClearFulfilledRequest();
    }
    const int nextAsset = GetNextAsset(RequestedMasternodeAssets);
    if (nextAsset == MASTERNODE_SYNC_FINISHED) {
        LogPrintf("%s - Sync has finished\n", __func__);
    }
    g_tiertwo_sync_state.SetCurrentSyncPhase(nextAsset);
    RequestedMasternodeAttempt = 0;
    nAssetSyncStarted = GetTime();
}

std::string CMasternodeSync::GetSyncStatus()
{
    switch (g_tiertwo_sync_state.GetSyncPhase()) {
    case MASTERNODE_SYNC_INITIAL:
        return _("MNs synchronization pending...");
    case MASTERNODE_SYNC_SPORKS:
        return _("Synchronizing sporks...");
    case MASTERNODE_SYNC_BUDGET:
        return _("Synchronizing budgets...");
    case MASTERNODE_SYNC_FAILED:
        return _("Synchronization failed");
    case MASTERNODE_SYNC_FINISHED:
        return _("Synchronization finished");
    }
    return "";
}

void CMasternodeSync::ProcessSyncStatusMsg(int nItemID, int nCount)
{
    int RequestedMasternodeAssets = g_tiertwo_sync_state.GetSyncPhase();
    if (RequestedMasternodeAssets >= MASTERNODE_SYNC_FINISHED) return;

    //this means we will receive no further communication
    switch (nItemID) {
        case (MASTERNODE_SYNC_BUDGET_PROP):
            if (RequestedMasternodeAssets != MASTERNODE_SYNC_BUDGET) return;
            sumBudgetItemProp += nCount;
            countBudgetItemProp++;
            break;
        case (MASTERNODE_SYNC_BUDGET_FIN):
            if (RequestedMasternodeAssets != MASTERNODE_SYNC_BUDGET) return;
            sumBudgetItemFin += nCount;
            countBudgetItemFin++;
            break;
        default:
            break;
    }

    LogPrint(BCLog::MASTERNODE, "CMasternodeSync:ProcessMessage - ssc - got inventory count %d %d\n", nItemID, nCount);
}

void CMasternodeSync::ClearFulfilledRequest()
{
    g_netfulfilledman.Clear();
}

void CMasternodeSync::Process()
{
    static int tick = 0;
    const bool isRegTestNet = Params().IsRegTestNet();
    const bool isTestnet = Params().IsTestnet();

    // Single-node regtest: there is nobody to sync from (sporks, budgets),
    // so mark the sync as finished right away instead of waiting for ticks.
    if (isRegTestNet) {
        const auto peers = g_connman->CopyNodeVector(CConnman::FullyConnectedOnly);
        const bool noPeers = peers.empty();
        g_connman->ReleaseNodeVector(peers);
        if (noPeers && !g_tiertwo_sync_state.IsSynced()) {
            LogPrintf("CMasternodeSync::Process - no peers on regtest, marking synced (phase=%d)\n",
                      g_tiertwo_sync_state.GetSyncPhase());
            g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);
            return;
        }
    }

    if (tick++ % MASTERNODE_SYNC_TIMEOUT != 0) return;

    // if the last call to this function was more than 60 minutes ago (client was in sleep mode)
    // reset the sync process. Use the real (non-mockable) clock: a mocktime jump
    // (e.g. tests advancing the time by hours/days) must not be mistaken for sleep.
    int64_t now = GetSystemTimeInSeconds();
    if (lastProcess != 0 && now > lastProcess + 60 * 60) {
        Reset();
    }
    lastProcess = now;

    // Update chain sync status using the 'lastProcess' time
    UpdateBlockchainSynced(isRegTestNet);

    // PoW-only phase: tier-two objects (MNs/budgets) are not expected to be available yet.
    // Mark tier-two as synced to avoid confusing UI "syncing masternodes..." loops, and begin
    // the real sync only once PoS is active.
    if (!isRegTestNet) {
        const int chainHeight = WITH_LOCK(cs_main, return chainActive.Height(););
        const bool isPoSActive = Params().GetConsensus().NetworkUpgradeActive(chainHeight, Consensus::UPGRADE_POS);
        if (!isPoSActive) {
            g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);
            m_forcedSyncedUntilPoS = true;
            return;
        }
        if (m_forcedSyncedUntilPoS) {
            // PoS just became active: restart tier-two sync from scratch.
            // Preserve 'lastProcess' so chain sync status can be updated immediately.
            const int64_t savedLastProcess = lastProcess;
            Reset();
            lastProcess = savedLastProcess;
            UpdateBlockchainSynced(isRegTestNet);
            m_forcedSyncedUntilPoS = false;
        }
    }

    if (g_tiertwo_sync_state.IsSynced()) {
        if (isRegTestNet) {
            return;
        }
        if (isTestnet) {
            // Testnet may legitimately have no budgets at all (especially early on).
            // Avoid resetting forever due to an empty list.
            return;
        }
        // Check if we lost all budget proposals from sleep/wake or failure to sync
        // originally. If we did, resync from scratch.
        if (g_budgetman.CountProposals() == 0) {
            Reset();
        } else {
            return;
        }
    }

    // Try syncing again
    int RequestedMasternodeAssets = g_tiertwo_sync_state.GetSyncPhase();
    if (RequestedMasternodeAssets == MASTERNODE_SYNC_FAILED && lastFailure + (1 * 60) < GetTime()) {
        Reset();
    } else if (RequestedMasternodeAssets == MASTERNODE_SYNC_FAILED) {
        return;
    }

    if (RequestedMasternodeAssets == MASTERNODE_SYNC_INITIAL) SwitchToNextAsset();

    // Regtest fallback: no peers may ever reply (e.g. single-node tests or no
    // budgets at all), so a phase that made no progress is advanced. Without
    // this, a node with zero peers would sit in SPORKS forever and never reach
    // the synced state. (Mainnet has equivalent timeouts inside SyncWithNode.)
    if (isRegTestNet && RequestedMasternodeAssets >= MASTERNODE_SYNC_SPORKS &&
        RequestedMasternodeAssets < MASTERNODE_SYNC_FINISHED) {
        const auto peers = g_connman->CopyNodeVector(CConnman::FullyConnectedOnly);
        const bool noPeers = peers.empty();
        g_connman->ReleaseNodeVector(peers);
        if (noPeers || GetTime() - nAssetSyncStarted > MASTERNODE_SYNC_TIMEOUT) {
            LogPrint(BCLog::MASTERNODE, "CMasternodeSync::Process - regtest sync timeout on phase %d%s, advancing\n",
                     RequestedMasternodeAssets, noPeers ? " (no peers)" : "");
            SwitchToNextAsset();
            return;
        }
    }

    // sporks synced but blockchain is not, wait until we're almost at a recent block to continue
    if (!g_tiertwo_sync_state.IsBlockchainSynced() &&
        RequestedMasternodeAssets > MASTERNODE_SYNC_SPORKS) return;

    CMasternodeSync* sync = this;

    const auto peers = g_connman->CopyNodeVector(CConnman::FullyConnectedOnly);

    // New sync architecture, regtest only for now.
    if (isRegTestNet) {
        for (CNode* pnode : peers) {
            sync->SyncRegtest(pnode);
        }
        g_connman->ReleaseNodeVector(peers);
        return;
    }

    // Mainnet sync
    std::vector<CNode*> randomizedPeers = peers;
    FastRandomContext randomizer;
    std::shuffle(randomizedPeers.begin(), randomizedPeers.end(), randomizer);
    for (CNode* pnode : randomizedPeers) {
        if (!sync->SyncWithNode(pnode)) {
            break;
        }
    }
    g_connman->ReleaseNodeVector(peers);
}

void CMasternodeSync::syncTimeout(const std::string& reason)
{
    LogPrintf("%s - ERROR - Sync has failed on %s, will retry later\n", __func__, reason);
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FAILED);
    RequestedMasternodeAttempt = 0;
    lastFailure = GetTime();
    nCountFailures++;
}

bool CMasternodeSync::SyncWithNode(CNode* pnode)
{
    int RequestedMasternodeAssets = g_tiertwo_sync_state.GetSyncPhase();
    CNetMsgMaker msgMaker(pnode->GetSendVersion());

    //set to synced
    if (RequestedMasternodeAssets == MASTERNODE_SYNC_SPORKS) {

        // Sync sporks from at least 2 peers
        if (RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD) {
            SwitchToNextAsset();
            return false;
        }

        // Request sporks sync if we haven't requested it yet.
        if (g_netfulfilledman.HasFulfilledRequest(pnode->addr, "getspork")) return true;
        g_netfulfilledman.AddFulfilledRequest(pnode->addr, "getspork");

        g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::GETSPORKS));
        RequestedMasternodeAttempt++;
        return false;
    }

    if (pnode->nVersion < ActiveProtocol() || !pnode->CanRelay()) {
        return true; // move to next peer
    }

    // Skip peers with low reliability score (<50%) to save resources
    {
        LOCK(cs_main);
        double reliability = GetPeerReliabilityScore(pnode->GetId());
        if (reliability < 0.5) {
            return true; // move to next reliable peer
        }
    }

    if (RequestedMasternodeAssets == MASTERNODE_SYNC_BUDGET) {
        int lastBudgetItem = g_tiertwo_sync_state.GetlastBudgetItem();
        // We'll start rejecting votes if we accidentally get set as synced too soon
        if (lastBudgetItem > 0 && lastBudgetItem < GetTime() - MASTERNODE_SYNC_TIMEOUT * 10 && RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD) {
            // Hasn't received a new item in the last fifty seconds and more than MASTERNODE_SYNC_THRESHOLD requests were sent,
            // so we'll move to the next asset
            SwitchToNextAsset();
            return false;
        }

        // timeout
        if (lastBudgetItem == 0 &&
            (RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD * 3 || GetTime() - nAssetSyncStarted > MASTERNODE_SYNC_TIMEOUT * 5)) {
            // maybe there is no budgets at all, so just finish syncing
            SwitchToNextAsset();
            return false;
        }

        // Don't request budget initial sync to more than 6 randomly ordered peers in this round.
        if (RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD * 3) return false;

        // Request bud sync if we haven't requested it yet.
        if (g_netfulfilledman.HasFulfilledRequest(pnode->addr, "busync")) return true;

        // Mark sync requested.
        g_netfulfilledman.AddFulfilledRequest(pnode->addr, "busync");

        // Sync proposals, finalizations and votes
        uint256 n;
        g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::BUDGETVOTESYNC, n));
        RequestedMasternodeAttempt++;

        return false; // sleep 1 second before do another request round.
    }

    return true;
}
