// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeman.h"

#include "addrman.h"
#include "evo/deterministicmns.h"
#include "fs.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternode.h"
#include "messagesigner.h"
#include "netbase.h"
#include "netmessagemaker.h"
#include "shutdown.h"
#include "spork.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "validation.h"

#include <boost/thread/thread.hpp>

#define MN_WINNER_MINIMUM_AGE 8000    // Age in seconds. This should be > MASTERNODE_REMOVAL_SECONDS to avoid misconfigured new nodes in the list.

/** Masternode manager */
CMasternodeMan mnodeman;
/** Keep track of the active Masternode */
CActiveMasternode activeMasternode;

struct CompareScoreMN {
    template <typename T>
    bool operator()(const std::pair<int64_t, T>& t1,
        const std::pair<int64_t, T>& t2) const
    {
        return t1.first < t2.first;
    }
};

static void CountNetwork(const CService& addr, int& ipv4, int& ipv6, int& onion)
{
    std::string strHost;
    int port;
    SplitHostPort(addr.ToString(), port, strHost);
    CNetAddr node;
    LookupHost(strHost, node, false);
    switch(node.GetNetwork()) {
        case NET_IPV4:
            ipv4++;
            break;
        case NET_IPV6:
            ipv6++;
            break;
        case NET_ONION:
            onion++;
            break;
        default:
            break;
    }
}

static bool canScheduleMN(bool fFilterSigTime, const MasternodeRef& mn, int minProtocol, int count_enabled, int nBlockHeight)
{
    if (mn->protocolVersion < minProtocol || !mn->IsEnabled()) return false;
    if (fFilterSigTime && GetAdjustedTime() - mn->sigTime < MN_WINNER_MINIMUM_AGE) return false;
    return true;
}

static bool IsSmallQueuePayeeFixActive(int nBlockHeight)
{
    // Mainnet-only rollout guard for the small-network payment queue fix.
    // Regtest/testnet stay enabled immediately so the regression tests exercise the fixed path.
    return Params().NetworkIDString() != CBaseChainParams::MAIN || nBlockHeight >= 11000;
}

//
// CMasternodeDB
//

static const int MASTERNODE_DB_VERSION_BIP155 = 2;

CMasternodeDB::CMasternodeDB()
{
    pathMN = GetDataDir() / "mncache.dat";
    strMagicMessage = "MasternodeCache";
}

bool CMasternodeDB::Write(const CMasternodeMan& mnodemanToSave)
{
    int64_t nStart = GetTimeMillis();
    const auto& params = Params();

    // serialize, checksum data up to that point, then append checksum
    // Always done in the latest format.
    CDataStream ssMasternodes(SER_DISK, CLIENT_VERSION | ADDRV2_FORMAT);
    ssMasternodes << MASTERNODE_DB_VERSION_BIP155;
    ssMasternodes << strMagicMessage;                   // masternode cache file specific magic message
    ssMasternodes << params.MessageStart(); // network specific magic number
    ssMasternodes << mnodemanToSave;
    uint256 hash = Hash(ssMasternodes.begin(), ssMasternodes.end());
    ssMasternodes << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathMN, "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathMN.string());

    // Write and commit header, data
    try {
        fileout << ssMasternodes;
    } catch (const std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    //    FileCommit(fileout);
    fileout.fclose();

    LogPrint(BCLog::MASTERNODE,"Written info to mncache.dat  %dms\n", GetTimeMillis() - nStart);
    {
        LOCK(mnodemanToSave.GetLock());
        LogPrint(BCLog::MASTERNODE,"  %s\n", mnodemanToSave.ToString());
    }

    return true;
}

CMasternodeDB::ReadResult CMasternodeDB::Read(CMasternodeMan& mnodemanToLoad)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathMN, "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathMN.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = fs::file_size(pathMN);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)vchData.data(), dataSize);
        filein >> hashIn;
    } catch (const std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    const auto& params = Params();
    // serialize, checksum data up to that point, then append checksum
    CDataStream ssMasternodes(vchData, SER_DISK,  CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssMasternodes.begin(), ssMasternodes.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    int version;
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header
        ssMasternodes >> version;
        ssMasternodes >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode cache magic message", __func__);
            return IncorrectMagicMessage;
        }

        // de-serialize file header (network specific magic number) and ..
        std::vector<unsigned char> pchMsgTmp(4);
        ssMasternodes >> MakeSpan(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp.data(), params.MessageStart(), pchMsgTmp.size()) != 0) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }
        // de-serialize data into CMasternodeMan object.
        if (version == MASTERNODE_DB_VERSION_BIP155) {
            OverrideStream<CDataStream> s(&ssMasternodes, ssMasternodes.GetType(), ssMasternodes.GetVersion() | ADDRV2_FORMAT);
            s >> mnodemanToLoad;
        } else {
            // Old format
            ssMasternodes >> mnodemanToLoad;
        }
    } catch (const std::exception& e) {
        mnodemanToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint(BCLog::MASTERNODE,"Loaded info from mncache.dat (dbversion=%d) %dms\n", version, GetTimeMillis() - nStart);
    {
        LOCK(mnodemanToLoad.GetLock());
        LogPrint(BCLog::MASTERNODE,"  %s\n", mnodemanToLoad.ToString());
    }

    return Ok;
}

void DumpMasternodes()
{
    int64_t nStart = GetTimeMillis();

    CMasternodeDB mndb;
    LogPrint(BCLog::MASTERNODE,"Writing info to mncache.dat...\n");
    mndb.Write(mnodeman);

    LogPrint(BCLog::MASTERNODE,"Masternode dump finished  %dms\n", GetTimeMillis() - nStart);
}

CMasternodeMan::CMasternodeMan():
        cvLastBlockHashes(CACHED_BLOCK_HASHES, UINT256_ZERO),
        nDsqCount(0)
{}

bool CMasternodeMan::Add(CMasternode& mn)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        return false;
    }

    auto mnList = deterministicMNManager->GetListAtChainTip();
    if (mnList.HasMNByCollateral(mn.vin.prevout)) {
        LogPrint(BCLog::MASTERNODE, "ERROR: Not Adding Masternode %s as the collateral is already registered with a DMN\n",
                mn.vin.prevout.ToString());
        return false;
    }

    LOCK(cs);

    if (!mn.IsAvailableState())
        return false;

    const auto& it = mapMasternodes.find(mn.vin.prevout);
    if (it == mapMasternodes.end()) {
        LogPrint(BCLog::MASTERNODE, "Adding new Masternode %s\n", mn.vin.prevout.ToString());
        mapMasternodes.emplace(mn.vin.prevout, std::make_shared<CMasternode>(mn));
        LogPrint(BCLog::MASTERNODE, "Masternode added. New total count: %d\n", (int)mapMasternodes.size());
        return true;
    }

    return false;
}

void CMasternodeMan::AskForMN(CNode* pnode, const CTxIn& vin)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        return;
    }

    {
        LOCK(cs);
        auto i = mWeAskedForMasternodeListEntry.find(vin.prevout);
        if (i != mWeAskedForMasternodeListEntry.end()) {
            int64_t t = i->second;
            if (GetTime() < t) return; // we've asked recently
        }
        mWeAskedForMasternodeListEntry[vin.prevout] = GetTime() + MasternodeMinPingSeconds();
    }

    // ask for the mnb info once from the node that sent mnp
    LogPrint(BCLog::MASTERNODE, "CMasternodeMan::AskForMN - Asking node for missing entry, vin: %s\n", vin.prevout.hash.ToString());
    g_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::GETMNLIST, vin));
}

int CMasternodeMan::CheckAndRemove(bool forceExpiredRemoval)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        LogPrint(BCLog::MASTERNODE, "Removing all legacy mn due to SPORK 21\n");
        Clear();
        return 0;
    }

    {
        extern RecursiveMutex cs_main;
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) return 0;

        int minProtocol = ActiveProtocol();

        LOCK(cs);
        LogPrint(BCLog::MASTERNODE,"  %s\n", ToString());

        //remove inactive and outdated (or replaced by DMN)
        auto it = mapMasternodes.begin();
        while (it != mapMasternodes.end()) {
            MasternodeRef& mn = it->second;
            auto activeState = mn->GetActiveState();
            if (activeState == CMasternode::MASTERNODE_REMOVE ||
                activeState == CMasternode::MASTERNODE_VIN_SPENT ||
                (forceExpiredRemoval && activeState == CMasternode::MASTERNODE_EXPIRED) ||
                mn->protocolVersion < minProtocol) {
                LogPrint(BCLog::MASTERNODE, "Removing inactive (legacy) Masternode %s\n", it->first.ToString());
                //erase all of the broadcasts we've seen from this vin
                // -- if we missed a few pings and the node was removed, this will allow is to get it back without them
                //    sending a brand new mnb
                std::map<uint256, CMasternodeBroadcast>::iterator it3 = mapSeenMasternodeBroadcast.begin();
                while (it3 != mapSeenMasternodeBroadcast.end()) {
                    if (it3->second.vin.prevout == it->first) {
                        g_tiertwo_sync_state.EraseSeenMNB((*it3).first);
                        it3 = mapSeenMasternodeBroadcast.erase(it3);
                    } else {
                        ++it3;
                    }
                }

                // allow us to ask for this masternode again if we see another ping
                std::map<COutPoint, int64_t>::iterator it2 = mWeAskedForMasternodeListEntry.begin();
                while (it2 != mWeAskedForMasternodeListEntry.end()) {
                    if (it2->first == it->first) {
                        it2 = mWeAskedForMasternodeListEntry.erase(it2);
                    } else {
                        ++it2;
                    }
                }

                // clean MN pings right away.
                auto itPing = mapSeenMasternodePing.begin();
                while (itPing != mapSeenMasternodePing.end()) {
                    if (itPing->second.GetVin().prevout == it->first) {
                        itPing = mapSeenMasternodePing.erase(itPing);
                    } else {
                        ++itPing;
                    }
                }

                it = mapMasternodes.erase(it);
                LogPrint(BCLog::MASTERNODE, "Masternode removed.\n");
            } else {
                ++it;
            }
        }
        LogPrint(BCLog::MASTERNODE, "New total masternode count: %d\n", (int)mapMasternodes.size());

        // check who's asked for the Masternode list
        std::map<CNetAddr, int64_t>::iterator itAsked = mAskedUsForMasternodeList.begin();
        while (itAsked != mAskedUsForMasternodeList.end()) {
            if ((*itAsked).second < GetTime()) {
                itAsked = mAskedUsForMasternodeList.erase(itAsked);
            } else {
                ++itAsked;
            }
        }

        // check who we asked for the Masternode list
        std::map<CNetAddr, int64_t>::iterator itWeAsked = mWeAskedForMasternodeList.begin();
        while (itWeAsked != mWeAskedForMasternodeList.end()) {
            if ((*itWeAsked).second < GetTime()) {
                itWeAsked = mWeAskedForMasternodeList.erase(itWeAsked);
            } else {
                ++itWeAsked;
            }
        }

        // check which Masternodes we've asked for
        std::map<COutPoint, int64_t>::iterator itEntry = mWeAskedForMasternodeListEntry.begin();
        while (itEntry != mWeAskedForMasternodeListEntry.end()) {
            if ((*itEntry).second < GetTime()) {
                itEntry = mWeAskedForMasternodeListEntry.erase(itEntry);
            } else {
                ++itEntry;
            }
        }

        // remove expired mapSeenMasternodeBroadcast
        std::map<uint256, CMasternodeBroadcast>::iterator it3 = mapSeenMasternodeBroadcast.begin();
        while (it3 != mapSeenMasternodeBroadcast.end()) {
            if ((*it3).second.lastPing.sigTime < GetTime() - (MasternodeRemovalSeconds() * 2)) {
                g_tiertwo_sync_state.EraseSeenMNB((*it3).first);
                it3 = mapSeenMasternodeBroadcast.erase(it3);
            } else {
                ++it3;
            }
        }

        // remove expired mapSeenMasternodePing
        std::map<uint256, CMasternodePing>::iterator it4 = mapSeenMasternodePing.begin();
        while (it4 != mapSeenMasternodePing.end()) {
            if ((*it4).second.sigTime < GetTime() - (MasternodeRemovalSeconds() * 2)) {
                it4 = mapSeenMasternodePing.erase(it4);
            } else {
                ++it4;
            }
        }
    }

    int totalMasternodes;
    {
        LOCK(cs);
        totalMasternodes = (int)mapMasternodes.size();
    }

    return totalMasternodes;
}

void CMasternodeMan::Clear()
{
    LOCK(cs);
    mapMasternodes.clear();
    mAskedUsForMasternodeList.clear();
    mWeAskedForMasternodeList.clear();
    mWeAskedForMasternodeListEntry.clear();
    mapSeenMasternodeBroadcast.clear();
    mapSeenMasternodePing.clear();
    nDsqCount = 0;
}

void CMasternodeMan::CleanupInvalidSeenBroadcasts()
{
    LOCK(cs);
    auto itSeenMNB = mapSeenMasternodeBroadcast.begin();
    while (itSeenMNB != mapSeenMasternodeBroadcast.end()) {
        if (!itSeenMNB->second.addr.IsValid()) {
            itSeenMNB = mapSeenMasternodeBroadcast.erase(itSeenMNB);
        } else {
            ++itSeenMNB;
        }
    }
}

bool CMasternodeMan::HasSeenMasternodeBroadcast(const uint256& hash) const
{
    LOCK(cs);
    return mapSeenMasternodeBroadcast.count(hash) != 0;
}

bool CMasternodeMan::HasSeenMasternodePing(const uint256& hash) const
{
    LOCK(cs);
    return mapSeenMasternodePing.count(hash) != 0;
}

bool CMasternodeMan::GetSeenMasternodeBroadcast(const uint256& hash, CMasternodeBroadcast& mnb) const
{
    LOCK(cs);
    const auto it = mapSeenMasternodeBroadcast.find(hash);
    if (it == mapSeenMasternodeBroadcast.end()) {
        return false;
    }
    mnb = it->second;
    return true;
}

bool CMasternodeMan::GetSeenMasternodePing(const uint256& hash, CMasternodePing& mnp) const
{
    LOCK(cs);
    const auto it = mapSeenMasternodePing.find(hash);
    if (it == mapSeenMasternodePing.end()) {
        return false;
    }
    mnp = it->second;
    return true;
}

void CMasternodeMan::AddSeenMasternodeBroadcast(const uint256& hash, const CMasternodeBroadcast& mnb)
{
    LOCK(cs);
    mapSeenMasternodeBroadcast.emplace(hash, mnb);
}

void CMasternodeMan::AddSeenMasternodePing(const uint256& hash, const CMasternodePing& mnp)
{
    LOCK(cs);
    mapSeenMasternodePing.emplace(hash, mnp);
}

void CMasternodeMan::RemoveSeenMasternodeBroadcast(const uint256& hash)
{
    LOCK(cs);
    mapSeenMasternodeBroadcast.erase(hash);
}

void CMasternodeMan::UpdateSeenMasternodeBroadcastPing(const uint256& hash, const CMasternodePing& ping)
{
    LOCK(cs);
    const auto it = mapSeenMasternodeBroadcast.find(hash);
    if (it != mapSeenMasternodeBroadcast.end()) {
        it->second.SetLastPing(ping);
    }
}

CMasternode* CMasternodeMan::Find(const COutPoint& collateralOut)
{
    LOCK(cs);
    auto it = mapMasternodes.find(collateralOut);
    return it != mapMasternodes.end() ? it->second.get() : nullptr;
}

const CMasternode* CMasternodeMan::Find(const COutPoint& collateralOut) const
{
    LOCK(cs);
    auto it = mapMasternodes.find(collateralOut);
    return it != mapMasternodes.end() ? it->second.get() : nullptr;
}

CMasternode* CMasternodeMan::Find(const CPubKey& pubKeyMasternode)
{
    LOCK(cs);
    for (auto& it : mapMasternodes) {
        if (it.second->pubKeyMasternode == pubKeyMasternode) return it.second.get();
    }
    return nullptr;
}

void CMasternodeMan::CheckSpentCollaterals(const std::vector<CTransactionRef>& vtx)
{
    LOCK(cs);
    for (const auto& ptx : vtx) {
        for (const auto& in : ptx->vin) {
            auto it = mapMasternodes.find(in.prevout);
            if (it != mapMasternodes.end()) {
                it->second->SetSpent();
            }
        }
    }
}

MasternodeRef CMasternodeMan::GetNextMasternodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount, const CBlockIndex* pChainTip) const
{
    if (deterministicMNManager->LegacyMNObsolete()) {
        LogPrintf("%s: ERROR - called after legacy system disabled\n", __func__);
        return nullptr;
    }

    AssertLockNotHeld(cs_main);
    const CBlockIndex* BlockReading = (pChainTip == nullptr ? GetChainTip() : pChainTip);
    if (!BlockReading) return nullptr;

    MasternodeRef pBestMasternode = nullptr;
    std::vector<std::pair<int64_t, MasternodeRef> > vecMasternodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */
    int minProtocol = ActiveProtocol();
    int count_enabled = CountEnabled();
    {
        LOCK(cs);
        for (const auto& it : mapMasternodes) {
            if (!it.second->IsEnabled()) continue;
            if (canScheduleMN(fFilterSigTime, it.second, minProtocol, count_enabled, nBlockHeight)) {
                vecMasternodeLastPaid.emplace_back(SecondsSincePayment(it.second, count_enabled, BlockReading), it.second);
            }
        }
    }
    // Add deterministic masternodes to the vector
    if (deterministicMNManager->IsDIP3Enforced()) {
        CDeterministicMNList mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
            const MasternodeRef mn = MakeMasternodeRefForDMN(dmn);
            if (canScheduleMN(fFilterSigTime, mn, minProtocol, count_enabled, nBlockHeight)) {
                vecMasternodeLastPaid.emplace_back(SecondsSincePayment(mn, count_enabled, BlockReading), mn);
            }
        });
    }

    nCount = (int)vecMasternodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if (fFilterSigTime && nCount < count_enabled / 3) return GetNextMasternodeInQueueForPayment(nBlockHeight, false, nCount, BlockReading);
    const bool smallQueueFixActive = IsSmallQueuePayeeFixActive(nBlockHeight);
    if (smallQueueFixActive && nCount == 0) return nullptr;

    // Sort them high to low
    sort(vecMasternodeLastPaid.rbegin(), vecMasternodeLastPaid.rend(), CompareScoreMN());

    // Look at 1/10th of the network
    int nThreshold = nCount / 10;
    if (smallQueueFixActive && nThreshold == 0) nThreshold = 1;
    int64_t nHighest = 0;

    for (const auto& s : vecMasternodeLastPaid) {
        // Now candidates are those that are in the threshold
        if (nThreshold-- <= 0) break;

        uint256 hash = UINT256_ZERO;
        {
            LOCK(cs_main);
            if (nBlockHeight - 101 >= 0 && nBlockHeight - 101 <= chainActive.Height())
                hash = chainActive[nBlockHeight - 101]->GetBlockHash();
        }
        if (hash == UINT256_ZERO) continue;
        arith_uint256 n = s.second->CalculateScore(hash);
        int64_t n2 = n.GetCompact(false);

        if (n2 > nHighest) {
            nHighest = n2;
            pBestMasternode = s.second;
        }
    }

    return pBestMasternode;
}

MasternodeRef CMasternodeMan::GetCurrentMasterNode(const uint256& hash, bool onlyEnabled) const
{
    int minProtocol = ActiveProtocol();
    int64_t score = 0;
    MasternodeRef winner = nullptr;

    {
        LOCK(cs);
        try {
            // scan for legacy winner
            for (const auto& it : mapMasternodes) {
                const MasternodeRef& mn = it.second;
                if (mn->protocolVersion < minProtocol) continue;
                if (onlyEnabled && !mn->IsAvailableState()) continue;

                // calculate the score of the masternode
                const int64_t n = mn->CalculateScore(hash).GetCompact(false);
                // determine the winner
                if (!winner || n > score) {
                    score = n;
                    winner = mn;
                }
            }
        } catch (const std::exception& e) {
            LogPrintf("%s: Exception during legacy masternode scan: %s\n", __func__, e.what());
        }
    }

    // scan also dmns
    if (deterministicMNManager->IsDIP3Enforced()) {
        try {
            auto mnList = deterministicMNManager->GetListAtChainTip();
            if (mnList.GetValidMNsCount() == 0) {
                // Early exit if list is malformed or loading
                return winner;
            }
            
            mnList.ForEachMN(onlyEnabled, [&](const CDeterministicMNCPtr& dmn) {
                if (!dmn) return; // Skip null pointers
                try {
                    const MasternodeRef mn = MakeMasternodeRefForDMN(dmn);
                    if (!mn) return;
                    if (mn->protocolVersion < minProtocol) return;
                    // calculate the score of the masternode
                    const int64_t n = mn->CalculateScore(hash).GetCompact(false);
                    // determine the winner
                    if (!winner || n > score) {
                        score = n;
                        winner = mn;
                    }
                } catch (const std::exception& e) {
                    // Log error and continue with other MNs
                    LogPrintf("%s: Error processing DMN %s: %s\n", __func__, dmn->proTxHash.ToString(), e.what());
                }
            });
        } catch (const std::exception& e) {
            LogPrintf("%s: Exception during DMN scan: %s\n", __func__, e.what());
        }
    }

    return winner;
}

std::vector<std::pair<MasternodeRef, int>> CMasternodeMan::GetMnScores(int nLast) const
{
    std::vector<std::pair<MasternodeRef, int>> vecMnLastPaid;
    // TODO: implement this for legacy and DMN
    return vecMnLastPaid;
}

std::vector<std::pair<int64_t, MasternodeRef>> CMasternodeMan::GetMasternodeRanks(int nBlockHeight) const
{
    std::vector<std::pair<int64_t, MasternodeRef>> vecMasternodeScores;
    uint256 hash = UINT256_ZERO;
    {
        LOCK(cs_main);
        if (nBlockHeight - 1 >= 0 && nBlockHeight - 1 <= chainActive.Height())
            hash = chainActive[nBlockHeight - 1]->GetBlockHash();
    }
    if (hash == UINT256_ZERO) return vecMasternodeScores;
    const int minProtocol = ActiveProtocol();
    const bool spork_8_active = sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT);
    {
        LOCK(cs);
        // scan for winner
        for (const auto& it : mapMasternodes) {
            const MasternodeRef mn = it.second;
            int64_t score = -1;
            if (mn->IsEnabled() && mn->protocolVersion >= minProtocol) {
                // Eligible for payments: same filters used by GetMasternodeRank/Payment validation.
                if (!spork_8_active || (GetAdjustedTime() - mn->sigTime >= MN_WINNER_MINIMUM_AGE)) {
                    score = mn->CalculateScore(hash).GetCompact(false);
                }
            }

            vecMasternodeScores.emplace_back(score, mn);
        }
    }
    // scan also dmns
    if (deterministicMNManager->IsDIP3Enforced()) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(false, [&](const CDeterministicMNCPtr& dmn) {
            const MasternodeRef mn = MakeMasternodeRefForDMN(dmn);
            const int64_t score = dmn->IsPoSeBanned() ? -1 : mn->CalculateScore(hash).GetCompact(false);

            vecMasternodeScores.emplace_back(score, mn);
        });
    }
    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareScoreMN());
    return vecMasternodeScores;
}

int CMasternodeMan::GetMasternodeRank(const CTxIn& vin, int64_t nBlockHeight) const
{
    uint256 hash = UINT256_ZERO;
    {
        LOCK(cs_main);
        if (nBlockHeight - 1 >= 0 && nBlockHeight - 1 <= chainActive.Height())
            hash = chainActive[nBlockHeight - 1]->GetBlockHash();
    }
    if (hash == UINT256_ZERO) return -1;

    // scan for winner
    const int minProtocol = ActiveProtocol();
    const bool spork_8_active = sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT);
    std::vector<std::pair<int64_t, CTxIn> > vecMasternodeScores;
    {
        LOCK(cs);
        for (const auto& it : mapMasternodes) {
            const MasternodeRef& mn = it.second;
            if (!mn->IsEnabled()) {
                continue; // Skip not enabled
            }
            if (mn->protocolVersion < minProtocol) {
                LogPrint(BCLog::MASTERNODE,"Skipping Masternode with obsolete version %d\n", mn->protocolVersion);
                continue; // Skip obsolete versions
            }
            if (spork_8_active &&
                    GetAdjustedTime() - mn->sigTime < MN_WINNER_MINIMUM_AGE) {
                continue; // Skip masternodes younger than (default) 1 hour
            }
            vecMasternodeScores.emplace_back(mn->CalculateScore(hash).GetCompact(false), mn->vin);
        }
    }

    // scan also dmns
    if (deterministicMNManager->IsDIP3Enforced()) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
            const MasternodeRef mn = MakeMasternodeRefForDMN(dmn);
            vecMasternodeScores.emplace_back(mn->CalculateScore(hash).GetCompact(false), mn->vin);
        });
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareScoreMN());

    int rank = 0;
    for (std::pair<int64_t, CTxIn> & s : vecMasternodeScores) {
        rank++;
        if (s.second.prevout == vin.prevout) {
            return rank;
        }
    }

    return -1;
}

bool CMasternodeMan::CheckInputs(CMasternodeBroadcast& mnb, int nChainHeight, int& nDoS)
{
    // incorrect ping or its sigTime
    if(mnb.lastPing.IsNull() || !mnb.lastPing.CheckAndUpdate(nDoS, false, true)) {
        return false;
    }

    // search existing Masternode list
    CMasternode* pmn = Find(mnb.vin.prevout);
    if (pmn != nullptr) {
        // nothing to do here if we already know about this masternode and it's enabled
        if (pmn->IsEnabled()) return true;
        // if it's not enabled, remove old MN first and continue
        else
            Remove(pmn->vin.prevout);
    }

    const Coin& collateralUtxo = pcoinsTip->AccessCoin(mnb.vin.prevout);
    if (collateralUtxo.IsSpent()) {
        LogPrint(BCLog::MASTERNODE,"mnb - vin %s spent\n", mnb.vin.prevout.ToString());
        return false;
    }

    // Good input
    return true;
}

int CMasternodeMan::ProcessMNBroadcast(CNode* pfrom, CMasternodeBroadcast& mnb)
{
    const uint256& mnbHash = mnb.GetHash();
    if (HasSeenMasternodeBroadcast(mnbHash)) { //seen
        g_tiertwo_sync_state.AddedMasternodeList(mnbHash);
        return 0;
    }

    int chainHeight = GetBestHeight();
    int nDoS = 0;
    if (!mnb.CheckAndUpdate(nDoS)) {
        return nDoS;
    }

    // make sure it's still unspent
    if (!CheckInputs(mnb, chainHeight, nDoS)) {
        return nDoS; // error set internally
    }

    // now that did the mnb checks, can add it.
    AddSeenMasternodeBroadcast(mnbHash, mnb);

    // All checks performed, add it
    LogPrint(BCLog::MASTERNODE,"%s - Got NEW Masternode entry - %s - %lli \n", __func__,
             mnb.vin.prevout.hash.ToString(), mnb.sigTime);
    CMasternode mn(mnb);
    if (!Add(mn)) {
        LogPrint(BCLog::MASTERNODE, "%s - Rejected Masternode entry %s\n", __func__,
                 mnb.vin.prevout.hash.ToString());
        return 0;
    }

    // if it matches our MN pubkey, then we've been remotely activated
    if (mnb.pubKeyMasternode == activeMasternode.pubKeyMasternode && mnb.protocolVersion == PROTOCOL_VERSION) {
        activeMasternode.EnableHotColdMasterNode(mnb.vin, mnb.addr);
    }

    // Relay only if we are synchronized and if the mnb address is not local.
    // Makes no sense to relay MNBs to the peers from where we are syncing them.
    bool isLocal = (mnb.addr.IsRFC1918() || mnb.addr.IsLocal()) && !Params().IsRegTestNet();
    if (!isLocal && g_tiertwo_sync_state.IsSynced()) mnb.Relay();

    // Add it as a peer
    g_connman->AddNewAddress(CAddress(mnb.addr, NODE_NETWORK), pfrom->addr, 2 * 60 * 60);

    // Update sync status
    g_tiertwo_sync_state.AddedMasternodeList(mnbHash);

    // All good
    return 0;
}

int CMasternodeMan::ProcessMNPing(CNode* pfrom, CMasternodePing& mnp)
{
    const uint256& mnpHash = mnp.GetHash();
    if (HasSeenMasternodePing(mnpHash)) return 0; //seen

    int nDoS = 0;
    if (mnp.CheckAndUpdate(nDoS)) return 0;

    if (nDoS > 0) {
        // if anything significant failed, mark that node
        return nDoS;
    } else {
        // if nothing significant failed, search existing Masternode list
        CMasternode* pmn = Find(mnp.vin.prevout);
        // if it's known, don't ask for the mnb, just return
        if (pmn != nullptr) return 0;
    }

    // something significant is broken or mn is unknown,
    // we might have to ask for the mn entry (while we aren't syncing).
    if (g_tiertwo_sync_state.IsSynced()) {
        AskForMN(pfrom, mnp.vin);
    }

    // All good
    return 0;
}

void CMasternodeMan::BroadcastInvMN(CMasternode* mn, CNode* pfrom)
{
    CMasternodeBroadcast mnb = CMasternodeBroadcast(*mn);
    const uint256& hash = mnb.GetHash();
    pfrom->PushInventory(CInv(MSG_MASTERNODE_ANNOUNCE, hash));

    // Add to mapSeenMasternodeBroadcast in case that isn't there for some reason.
    if (!HasSeenMasternodeBroadcast(hash)) AddSeenMasternodeBroadcast(hash, mnb);
}

int CMasternodeMan::ProcessGetMNList(CNode* pfrom, CTxIn& vin)
{
    // Single MN request
    if (!vin.IsNull()) {
        LOCK(cs);
        CMasternode* mn = Find(vin.prevout);
        if (!mn || !mn->IsEnabled()) return 0; // Nothing to return.

        // Relay the MN.
        BroadcastInvMN(mn, pfrom);
        LogPrint(BCLog::MASTERNODE, "dseg - Sent 1 Masternode entry to peer %i\n", pfrom->GetId());
        return 0;
    }

    // Check if the node asked for mn list sync before.
    bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());
    if (!isLocal) {
        {
            LOCK(cs);
            auto itAskedUsMNList = mAskedUsForMasternodeList.find(pfrom->addr);
            if (itAskedUsMNList != mAskedUsForMasternodeList.end()) {
                int64_t t = (*itAskedUsMNList).second;
                if (GetTime() < t) {
                    LogPrintf("CMasternodeMan::ProcessMessage() : dseg - peer already asked me for the list\n");
                    return 0;
                }
            }
            int64_t askAgain = GetTime() + MASTERNODES_REQUEST_SECONDS;
            mAskedUsForMasternodeList[pfrom->addr] = askAgain;
        }
    }

    int nInvCount = 0;
    {
        LOCK(cs);
        for (auto& it : mapMasternodes) {
            MasternodeRef& mn = it.second;
            if (mn->addr.IsRFC1918()) continue; //local network
            if (mn->IsEnabled()) {
                LogPrint(BCLog::MASTERNODE, "dseg - Sending Masternode entry - %s \n", mn->vin.prevout.hash.ToString());
                BroadcastInvMN(mn.get(), pfrom);
                nInvCount++;
            }
        }
    }

    g_connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_LIST, nInvCount));
    LogPrint(BCLog::MASTERNODE, "dseg - Sent %d Masternode entries to peer %i\n", nInvCount, pfrom->GetId());

    // All good
    return 0;
}

bool CMasternodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv, int& dosScore)
{
    LOCK(cs_process_message);
    dosScore = ProcessMessageInner(pfrom, strCommand, vRecv);
    return dosScore == 0;
}

int CMasternodeMan::ProcessMessageInner(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (!g_tiertwo_sync_state.IsBlockchainSynced()) return 0;

    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        LogPrint(BCLog::MASTERNODE, "%s: skip obsolete message %s\n", __func__, strCommand);
        return 0;
    }

    if (strCommand == NetMsgType::MNBROADCAST) {
        CMasternodeBroadcast mnb;
        vRecv >> mnb;
        {
            // Clear inv request
            extern RecursiveMutex cs_main;
            TRY_LOCK(cs_main, lockMain);
            if (!lockMain) return 0;
            g_connman->RemoveAskFor(mnb.GetHash(), MSG_MASTERNODE_ANNOUNCE);
        }
        return ProcessMNBroadcast(pfrom, mnb);

    } else if (strCommand == NetMsgType::MNBROADCAST2) {
        CMasternodeBroadcast mnb;
        OverrideStream<CDataStream> s(&vRecv, vRecv.GetType(), vRecv.GetVersion() | ADDRV2_FORMAT);
        s >> mnb;
        {
            // Clear inv request
            extern RecursiveMutex cs_main;
            TRY_LOCK(cs_main, lockMain);
            if (!lockMain) return 0;
            g_connman->RemoveAskFor(mnb.GetHash(), MSG_MASTERNODE_ANNOUNCE);
        }

        // For now, let's not process mnb2 with pre-BIP155 node addr format.
        if (mnb.addr.IsAddrV1Compatible()) {
            LogPrint(BCLog::MASTERNODE, "%s: mnb2 with pre-BIP155 node addr format rejected\n", __func__);
            return 30;
        }

        return ProcessMNBroadcast(pfrom, mnb);

    } else if (strCommand == NetMsgType::MNPING) {
        //Masternode Ping
        CMasternodePing mnp;
        vRecv >> mnp;
        LogPrint(BCLog::MNPING, "mnp - Masternode ping, vin: %s\n", mnp.vin.prevout.hash.ToString());
        {
            // Clear inv request
            extern RecursiveMutex cs_main;
            TRY_LOCK(cs_main, lockMain);
            if (!lockMain) return 0;
            g_connman->RemoveAskFor(mnp.GetHash(), MSG_MASTERNODE_PING);
        }
        return ProcessMNPing(pfrom, mnp);

    } else if (strCommand == NetMsgType::GETMNLIST) {
        //Get Masternode list or specific entry
        CTxIn vin;
        vRecv >> vin;
        return ProcessGetMNList(pfrom, vin);
    }
    // Nothing to report
    return 0;
}

void CMasternodeMan::Remove(const COutPoint& collateralOut)
{
    LOCK(cs);
    const auto it = mapMasternodes.find(collateralOut);
    if (it != mapMasternodes.end()) {
        mapMasternodes.erase(it);
    }
}

void CMasternodeMan::UpdateMasternodeList(CMasternodeBroadcast& mnb)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        return;
    }

    AddSeenMasternodePing(mnb.lastPing.GetHash(), mnb.lastPing);
    AddSeenMasternodeBroadcast(mnb.GetHash(), mnb);
    g_tiertwo_sync_state.AddedMasternodeList(mnb.GetHash());

    LogPrint(BCLog::MASTERNODE,"%s -- masternode=%s\n", __func__, mnb.vin.prevout.ToString());

    CMasternode* pmn = Find(mnb.vin.prevout);
    if (pmn == nullptr) {
        CMasternode mn(mnb);
        Add(mn);
    } else {
        pmn->UpdateFromNewBroadcast(mnb);
    }

    // Keep local active legacy MN in sync even when the broadcast is injected
    // through RPC/GUI paths (which bypass ProcessMNBroadcast).
    if (mnb.pubKeyMasternode == activeMasternode.pubKeyMasternode && mnb.protocolVersion == PROTOCOL_VERSION) {
        activeMasternode.EnableHotColdMasterNode(mnb.vin, mnb.addr);
    }
}

int64_t CMasternodeMan::SecondsSincePayment(const MasternodeRef& mn, int count_enabled, const CBlockIndex* BlockReading) const
{
    int64_t sec = (GetAdjustedTime() - GetLastPaid(mn, count_enabled, BlockReading));
    int64_t month = 60 * 60 * 24 * 30;
    if (sec < month) return sec; //if it's less than 30 days, give seconds

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << mn->vin;
    ss << mn->sigTime;
    const arith_uint256& hash = UintToArith256(ss.GetHash());

    // return some deterministic value for unknown/unpaid but force it to be more than 30 days old
    return month + hash.GetCompact(false);
}

int64_t CMasternodeMan::GetLastPaid(const MasternodeRef& mn, int count_enabled, const CBlockIndex* BlockReading) const
{
    if (BlockReading == nullptr) return 0;

    const CScript& mnpayee = mn->GetPayeeScript();

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << mn->vin;
    ss << mn->sigTime;
    const uint256& hash = ss.GetHash();

    // use a deterministic offset to break a tie -- 2.5 minutes
    int64_t nOffset = UintToArith256(hash).GetCompact(false) % 150;

    int max_depth = count_enabled * 1.25;
    LOCK(cs_mapMasternodeBlocks);
    for (int n = 0; n < max_depth; n++) {
        const auto& it = masternodePayments.mapMasternodeBlocks.find(BlockReading->nHeight);
        if (it != masternodePayments.mapMasternodeBlocks.end()) {
            // Search for this payee, with at least 2 votes. This will aid in consensus
            // allowing the network to converge on the same payees quickly, then keep the same schedule.
            if (it->second.HasPayeeWithVotes(mnpayee, 2))
                return BlockReading->nTime + nOffset;
        }
        BlockReading = BlockReading->pprev;

        if (BlockReading == nullptr || BlockReading->nHeight <= 0) {
            break;
        }
    }

    return 0;
}

std::string CMasternodeMan::ToString() const
{
    std::ostringstream info;

    info << "Masternodes: " << (int)mapMasternodes.size()
         << ", peers who asked us for Masternode list: " << (int)mAskedUsForMasternodeList.size()
         << ", peers we asked for Masternode list: " << (int)mWeAskedForMasternodeList.size()
         << ", entries in Masternode list we asked for: " << (int)mWeAskedForMasternodeListEntry.size();

    return info.str();
}

void CMasternodeMan::CacheBlockHash(const CBlockIndex* pindex)
{
    LOCK(cs);
    cvLastBlockHashes.Set(pindex->nHeight, pindex->GetBlockHash());
}

void CMasternodeMan::UncacheBlockHash(const CBlockIndex* pindex)
{
    LOCK(cs);
    cvLastBlockHashes.Set(pindex->nHeight, UINT256_ZERO);
}

uint256 CMasternodeMan::GetHashAtHeight(int nHeight) const
{
    // return zero if outside bounds
    if (nHeight < 0) {
        LogPrint(BCLog::MASTERNODE, "%s: Negative height. Returning 0\n",  __func__);
        return UINT256_ZERO;
    }
    int nCurrentHeight = GetBestHeight();
    if (nHeight > nCurrentHeight) {
        LogPrint(BCLog::MASTERNODE, "%s: height %d over current height %d. Returning 0\n",
                __func__, nHeight, nCurrentHeight);
        return UINT256_ZERO;
    }

    if (nHeight > nCurrentHeight - (int) CACHED_BLOCK_HASHES) {
        // Use cached hash
        LOCK(cs);
        return cvLastBlockHashes.Get(nHeight);
    } else {
        // Use chainActive
        LOCK(cs_main);
        if (nHeight > chainActive.Height()) return UINT256_ZERO;
        return chainActive[nHeight]->GetBlockHash();
    }
}

bool CMasternodeMan::IsWithinDepth(const uint256& nHash, int depth) const
{
    // Sanity checks
    if (nHash.IsNull()) {
        return error("%s: Called with null hash\n", __func__);
    }
    if (depth < 0 || (unsigned) depth >= CACHED_BLOCK_HASHES) {
        return error("%s: Invalid depth %d. Cached block hashes: %d\n", __func__, depth, CACHED_BLOCK_HASHES);
    }
    // Check last depth blocks to find one with matching hash
    const int nCurrentHeight = GetBestHeight();
    int nStopHeight = std::max(0, nCurrentHeight - depth);
    for (int i = nCurrentHeight; i >= nStopHeight; i--) {
        if (GetHashAtHeight(i) == nHash)
            return true;
    }
    return false;
}

CMasternodeMan::MNsInfo CMasternodeMan::getMNsInfo() const
{
    MNsInfo info;
    int nMinProtocol = ActiveProtocol();
    bool spork_8_active = sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT);

    CDeterministicMNList mnList;
    if (deterministicMNManager->IsDIP3Enforced()) {
        mnList = deterministicMNManager->GetListAtChainTip();
    }

    // legacy masternodes
    {
        LOCK(cs);
        for (const auto& it : mapMasternodes) {
            const MasternodeRef& mn = it.second;
            info.total++;
            CountNetwork(mn->addr, info.ipv4, info.ipv6, info.onion);
            if (mn->protocolVersion < nMinProtocol || !mn->IsEnabled()) {
                continue;
            }
            info.enabledSize++;
            // Eligible for payments
            if (spork_8_active && (GetAdjustedTime() - mn->sigTime < MN_WINNER_MINIMUM_AGE)) {
                continue; // Skip masternodes younger than (default) 8000 sec (MUST be > MASTERNODE_REMOVAL_SECONDS)
            }
            info.stableSize++;
        }
    }

    // deterministic masternodes
    if (deterministicMNManager->IsDIP3Enforced()) {
        mnList.ForEachMN(false, [&](const CDeterministicMNCPtr& dmn) {
            info.total++;
            CountNetwork(dmn->pdmnState->addr, info.ipv4, info.ipv6, info.onion);
            if (!dmn->IsPoSeBanned()) {
                info.enabledSize++;
                info.stableSize++;
            }
        });
    }

    return info;
}

int CMasternodeMan::CountEnabled(bool only_legacy) const
{
    int count_enabled = 0;
    int protocolVersion = ActiveProtocol();
    CDeterministicMNList mnList;
    if (!only_legacy && deterministicMNManager->IsDIP3Enforced()) {
        mnList = deterministicMNManager->GetListAtChainTip();
    }

    {
        LOCK(cs);
        for (const auto& it : mapMasternodes) {
            const MasternodeRef& mn = it.second;
            if (mn->protocolVersion < protocolVersion || !mn->IsEnabled()) continue;
            count_enabled++;
        }
    }

    if (!only_legacy && deterministicMNManager->IsDIP3Enforced()) {
        count_enabled += mnList.GetValidMNsCount();
    }

    return count_enabled;
}

bool CMasternodeMan::RequestMnList(CNode* pnode)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        return false;
    }

    LOCK(cs);
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForMasternodeList.find(pnode->addr);
            if (it != mWeAskedForMasternodeList.end()) {
                if (GetTime() < (*it).second) {
                    LogPrint(BCLog::MASTERNODE, "dseg - we already asked peer %i for the list; skipping...\n", pnode->GetId());
                    return false;
                }
            }
        }
    }

    g_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::GETMNLIST, CTxIn()));
    int64_t askAgain = GetTime() + MASTERNODES_REQUEST_SECONDS;
    mWeAskedForMasternodeList[pnode->addr] = askAgain;
    return true;
}

void ThreadCheckMasternodes()
{
    // Make this thread recognisable as the wallet flushing thread
    util::ThreadRename("pivx-masternodeman");
    LogPrintf("Masternodes thread started\n");

    unsigned int c = 0;

    try {
        // first clean up stale masternode payments data
        masternodePayments.CleanPaymentList(mnodeman.CheckAndRemove(), mnodeman.GetBestHeight());

        // Startup-only cleanup for stale invalid seen MN broadcasts
        mnodeman.CleanupInvalidSeenBroadcasts();

        while (true) {

            if (ShutdownRequested()) {
                break;
            }

            MilliSleep(1000);
            boost::this_thread::interruption_point();

            // try to sync from all available nodes, one step at a time
            masternodeSync.Process();

            if (g_tiertwo_sync_state.IsBlockchainSynced()) {
                c++;

                // check if we should activate or ping every few minutes,
                // start right after sync is considered to be done
                if (c % (MasternodePingSeconds()/2) == 0)
                    activeMasternode.ManageStatus();

                if (c % (MasternodePingSeconds()/5) == 0) {
                    masternodePayments.CleanPaymentList(mnodeman.CheckAndRemove(), mnodeman.GetBestHeight());
                }
            }
        }
    } catch (boost::thread_interrupted&) {
        // nothing, thread interrupted.
    }
}
