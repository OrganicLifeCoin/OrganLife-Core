// Copyright (c) 2014-2021 The Dash Core developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_ACTIVEMASTERNODE_H
#define PIVX_ACTIVEMASTERNODE_H

#include "evo/deterministicmns.h"
#include "operationresult.h"
#include "sync.h"
#include "validationinterface.h"

class CActiveDeterministicMasternodeManager;
class CBLSPublicKey;
class CBLSSecretKey;

extern CActiveDeterministicMasternodeManager* activeMasternodeManager;

struct CActiveMasternodeInfo
{
    // Keys for the active Masternode
    CBLSPublicKey pubKeyOperator;
    CBLSSecretKey keyOperator;
    // Initialized while registering Masternode
    uint256 proTxHash{UINT256_ZERO};
    CService service;
};

class CActiveDeterministicMasternodeManager : public CValidationInterface
{
public:
    enum masternode_state_t {
        MASTERNODE_WAITING_FOR_PROTX,
        MASTERNODE_POSE_BANNED,
        MASTERNODE_REMOVED,
        MASTERNODE_OPERATOR_KEY_CHANGED,
        MASTERNODE_PROTX_IP_CHANGED,
        MASTERNODE_READY,
        MASTERNODE_ERROR,
    };

private:
    masternode_state_t state{MASTERNODE_WAITING_FOR_PROTX};
    std::string strError;
    CActiveMasternodeInfo info;

public:
    ~CActiveDeterministicMasternodeManager() override = default;
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override;

    void Init(const CBlockIndex* pindexTip);
    void Reset(masternode_state_t _state, const CBlockIndex* pindexTip);
    // Sets the Deterministic Masternode Operator's private/public key
    OperationResult SetOperatorKey(const std::string& strMNOperatorPrivKey);
    // If the active masternode is ready, and the keyID matches with the registered one,
    // return private key, keyID, and pointer to dmn.
    OperationResult GetOperatorKey(CBLSSecretKey& key, CDeterministicMNCPtr& dmn) const;
    // Directly return the operator secret key saved in the manager, without performing any validation
    const CBLSSecretKey* OperatorKey() const { return &info.keyOperator; }
    void SetNullProTx() { info.proTxHash = UINT256_ZERO; }
    const uint256 GetProTx() const { return info.proTxHash; }

    const CActiveMasternodeInfo* GetInfo() const { return &info; }
    masternode_state_t GetState() const { return state; }
    std::string GetStatus() const;
    bool IsReady() const { return state == MASTERNODE_READY; }

    static bool IsValidNetAddr(const CService& addrIn);
};

// Get active masternode BLS operator keys for DMN
bool GetActiveDMNKeys(CBLSSecretKey& key, CTxIn& vin);

#endif // PIVX_ACTIVEMASTERNODE_H
