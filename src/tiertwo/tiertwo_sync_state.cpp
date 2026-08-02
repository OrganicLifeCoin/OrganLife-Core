// Copyright (c) 2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "tiertwo/tiertwo_sync_state.h"
#include "sync.h"
#include "uint256.h"
#include "utiltime.h"

TierTwoSyncState g_tiertwo_sync_state;

namespace {
RecursiveMutex& GetSeenSyncMutex()
{
    static RecursiveMutex seen_sync_mutex;
    return seen_sync_mutex;
}
} // namespace

static void UpdateLastTime(const uint256& hash, int64_t& last, std::map<uint256, int>& mapSeen)
{
    auto it = mapSeen.find(hash);
    if (it != mapSeen.end()) {
        if (it->second < MASTERNODE_SYNC_THRESHOLD) {
            last = GetTime();
            it->second++;
        }
    } else {
        last = GetTime();
        mapSeen.emplace(hash, 1);
    }
}

void TierTwoSyncState::AddedMasternodeList(const uint256& hash)
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    UpdateLastTime(hash, lastMasternodeList, mapSeenSyncMNB);
}

void TierTwoSyncState::AddedMasternodeWinner(const uint256& hash)
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    UpdateLastTime(hash, lastMasternodeWinner, mapSeenSyncMNW);
}

void TierTwoSyncState::AddedBudgetItem(const uint256& hash)
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    UpdateLastTime(hash, lastBudgetItem, mapSeenSyncBudget);
}

int64_t TierTwoSyncState::GetlastMasternodeList() const
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    return lastMasternodeList;
}

int64_t TierTwoSyncState::GetlastMasternodeWinner() const
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    return lastMasternodeWinner;
}

int64_t TierTwoSyncState::GetlastBudgetItem() const
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    return lastBudgetItem;
}

void TierTwoSyncState::ResetLastBudgetItem()
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    lastBudgetItem = 0;
}

void TierTwoSyncState::EraseSeenMNB(const uint256& hash)
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    mapSeenSyncMNB.erase(hash);
}

void TierTwoSyncState::EraseSeenMNW(const uint256& hash)
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    mapSeenSyncMNW.erase(hash);
}

void TierTwoSyncState::EraseSeenSyncBudget(const uint256& hash)
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    mapSeenSyncBudget.erase(hash);
}

void TierTwoSyncState::ResetData()
{
    auto& seenSyncMutex = GetSeenSyncMutex();
    LOCK(seenSyncMutex);
    lastMasternodeList = 0;
    lastMasternodeWinner = 0;
    lastBudgetItem = 0;
    mapSeenSyncMNB.clear();
    mapSeenSyncMNW.clear();
    mapSeenSyncBudget.clear();
}
