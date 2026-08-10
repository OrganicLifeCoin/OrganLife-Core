// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"

#include "addrman.h"
#include "bls/key_io.h"
#include "bls/bls_wrapper.h"
#include "netbase.h"
#include "tiertwo/net_masternodes.h"
#include "util/system.h"

// Keep track of the active Masternode
CActiveDeterministicMasternodeManager* activeMasternodeManager{nullptr};

static bool GetLocalAddress(CService& addrRet)
{
    // First try to find whatever our own local address is known internally.
    // Addresses could be specified via 'externalip' or 'bind' option, discovered via UPnP
    // or added by TorController. Use some random dummy IPv4 peer to prefer the one
    // reachable via IPv4.
    CNetAddr addrDummyPeer;
    bool fFound{false};
    if (LookupHost("8.8.8.8", addrDummyPeer, false)) {
        fFound = GetLocal(addrRet, &addrDummyPeer) && CActiveDeterministicMasternodeManager::IsValidNetAddr(addrRet);
    }
    if (!fFound && Params().IsRegTestNet()) {
        if (Lookup("127.0.0.1", addrRet, GetListenPort(), false)) {
            fFound = true;
        }
    }
    if (!fFound) {
        // If we have some peers, let's try to find our local address from one of them
        g_connman->ForEachNodeContinueIf([&fFound, &addrRet](CNode* pnode) {
            if (pnode->addr.IsIPv4())
                fFound = GetLocal(addrRet, &pnode->addr) && CActiveDeterministicMasternodeManager::IsValidNetAddr(addrRet);
            return !fFound;
        });
    }
    return fFound;
}

std::string CActiveDeterministicMasternodeManager::GetStatus() const
{
    switch (state) {
        case MASTERNODE_WAITING_FOR_PROTX:    return "Waiting for ProTx to appear on-chain";
        case MASTERNODE_POSE_BANNED:          return "Masternode was PoSe banned";
        case MASTERNODE_REMOVED:              return "Masternode removed from list";
        case MASTERNODE_OPERATOR_KEY_CHANGED: return "Operator key changed or revoked";
        case MASTERNODE_PROTX_IP_CHANGED:     return "IP address specified in ProTx changed";
        case MASTERNODE_READY:                return "Ready";
        case MASTERNODE_ERROR:                return "Error. " + strError;
        default:                              return "Unknown";
    }
}

OperationResult CActiveDeterministicMasternodeManager::SetOperatorKey(const std::string& strMNOperatorPrivKey)
{
    LOCK(cs_main); // Lock cs_main so the node doesn't perform any action while we setup the Masternode
    LogPrintf("Initializing deterministic masternode...\n");
    if (strMNOperatorPrivKey.empty()) {
        return errorOut("ERROR: Masternode operator priv key cannot be empty.");
    }

    auto opSk = bls::DecodeSecret(Params(), strMNOperatorPrivKey);
    if (!opSk) {
        return errorOut(_("Invalid mnoperatorprivatekey. Please see the documentation."));
    }
    info.keyOperator = *opSk;
    info.pubKeyOperator = info.keyOperator.GetPublicKey();
    return {true};
}

OperationResult CActiveDeterministicMasternodeManager::GetOperatorKey(CBLSSecretKey& key, CDeterministicMNCPtr& dmn) const
{
    if (!IsReady()) {
        return errorOut("Active masternode not ready");
    }
    dmn = deterministicMNManager->GetListAtChainTip().GetValidMN(info.proTxHash);
    if (!dmn) {
        return errorOut(strprintf("Active masternode %s not registered or PoSe banned", info.proTxHash.ToString()));
    }
    if (info.pubKeyOperator != dmn->pdmnState->pubKeyOperator.Get()) {
        return errorOut("Active masternode operator key changed or revoked");
    }
    // return key
    key = info.keyOperator;
    return {true};
}

void CActiveDeterministicMasternodeManager::Init(const CBlockIndex* pindexTip)
{
    // set masternode arg if called from RPC
    if (!fMasterNode) {
        gArgs.ForceSetArg("-masternode", "1");
        fMasterNode = true;
    }

    if (!deterministicMNManager->IsDIP3Enforced(pindexTip->nHeight)) {
        state = MASTERNODE_ERROR;
        strError = "Evo upgrade is not active yet.";
        LogPrintf("%s -- ERROR: %s\n", __func__, strError);
        return;
    }

    LOCK(cs_main);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        state = MASTERNODE_ERROR;
        strError = "Masternode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("%s ERROR: %s\n", __func__, strError);
        return;
    }

    if (!GetLocalAddress(info.service)) {
        state = MASTERNODE_ERROR;
        strError = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("%s ERROR: %s\n", __func__, strError);
        return;
    }

    CDeterministicMNList mnList = deterministicMNManager->GetListForBlock(pindexTip);

    CDeterministicMNCPtr dmn = mnList.GetMNByOperatorKey(info.pubKeyOperator);
    if (!dmn) {
        // MN not appeared on the chain yet
        return;
    }

    if (dmn->IsPoSeBanned()) {
        state = MASTERNODE_POSE_BANNED;
        return;
    }

    LogPrintf("%s: proTxHash=%s, proTx=%s\n", __func__, dmn->proTxHash.ToString(), dmn->ToString());

    if (info.service != dmn->pdmnState->addr) {
        state = MASTERNODE_ERROR;
        strError = strprintf("Local address %s does not match the address from ProTx (%s)",
                             info.service.ToStringIPPort(), dmn->pdmnState->addr.ToStringIPPort());
        LogPrintf("%s ERROR: %s\n", __func__, strError);
        return;
    }

    // Check socket connectivity
    const std::string& strService = info.service.ToString();
    LogPrintf("%s: Checking inbound connection to '%s'\n", __func__, strService);
    SOCKET hSocket = CreateSocket(info.service);
    if (hSocket == INVALID_SOCKET) {
        state = MASTERNODE_ERROR;
        strError = "DMN connectivity check failed, could not create socket to DMN running at " + strService;
        LogPrintf("%s -- ERROR: %s\n", __func__, strError);
        return;
    }
    bool fConnected = ConnectSocketDirectly(info.service, hSocket, nConnectTimeout, true) && IsSelectableSocket(hSocket);
    CloseSocket(hSocket);

    if (!fConnected) {
        state = MASTERNODE_ERROR;
        strError = "DMN connectivity check failed, could not connect to DMN running at " + strService;
        LogPrintf("%s ERROR: %s\n", __func__, strError);
        return;
    }

    info.proTxHash = dmn->proTxHash;
    g_connman->GetTierTwoConnMan()->setLocalDMN(info.proTxHash);
    state = MASTERNODE_READY;
    LogPrintf("Deterministic Masternode initialized\n");
}

void CActiveDeterministicMasternodeManager::Reset(masternode_state_t _state, const CBlockIndex* pindexTip)
{
    state = _state;
    SetNullProTx();
    // MN might have reappeared in same block with a new ProTx
    Init(pindexTip);
}

void CActiveDeterministicMasternodeManager::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload)
        return;

    if (!fMasterNode || !deterministicMNManager->IsDIP3Enforced(pindexNew->nHeight))
        return;

    if (state == MASTERNODE_READY) {
        auto newDmn = deterministicMNManager->GetListForBlock(pindexNew).GetValidMN(info.proTxHash);
        if (newDmn == nullptr) {
            // MN disappeared from MN list
            Reset(MASTERNODE_REMOVED, pindexNew);
            return;
        }

        auto oldDmn = deterministicMNManager->GetListForBlock(pindexNew->pprev).GetMN(info.proTxHash);
        if (oldDmn == nullptr) {
            // should never happen if state is MASTERNODE_READY
            LogPrintf("%s: WARNING: unable to find active mn %s in prev block list %s\n",
                      __func__, info.proTxHash.ToString(), pindexNew->pprev->GetBlockHash().ToString());
            return;
        }

        if (newDmn->pdmnState->pubKeyOperator != oldDmn->pdmnState->pubKeyOperator) {
            // MN operator key changed or revoked
            Reset(MASTERNODE_OPERATOR_KEY_CHANGED, pindexNew);
            return;
        }

        if (newDmn->pdmnState->addr != oldDmn->pdmnState->addr) {
            // MN IP changed
            Reset(MASTERNODE_PROTX_IP_CHANGED, pindexNew);
            return;
        }
    } else {
        // MN might have (re)appeared with a new ProTx or we've found some peers
        // and figured out our local address
        Init(pindexNew);
    }
}

bool CActiveDeterministicMasternodeManager::IsValidNetAddr(const CService& addrIn)
{
    // TODO: check IPv6 and TOR addresses
    return Params().IsRegTestNet() || (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}

bool GetActiveDMNKeys(CBLSSecretKey& key, CTxIn& vin)
{
    if (activeMasternodeManager == nullptr) {
        return error("%s: Active Masternode not initialized", __func__);
    }
    CDeterministicMNCPtr dmn;
    auto res = activeMasternodeManager->GetOperatorKey(key, dmn);
    if (!res) {
        return error("%s: %s", __func__, res.getError());
    }
    vin = CTxIn(dmn->collateralOutpoint);
    return true;
}
