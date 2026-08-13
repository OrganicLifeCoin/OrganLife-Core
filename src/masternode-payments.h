// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_MASTERNODE_PAYMENTS_H
#define PIVX_MASTERNODE_PAYMENTS_H

#include "key.h"
#include "validationinterface.h"

class CMasternodePayments;
class CValidationState;

extern CMasternodePayments masternodePayments;

bool IsBlockPayeeValid(const CBlock& block, const CBlockIndex* pindexPrev);
std::string GetRequiredPaymentsString(int nBlockHeight);
bool IsBlockValueValid(int nHeight, CAmount& nExpectedValue, CAmount nMinted, CAmount& nBudgetAmt,
                       CAmount nChainMinted = 0, CAmount nRecycledFees = 0);
void FillBlockPayee(CMutableTransaction& txCoinbase, CMutableTransaction& txCoinstake, const CBlockIndex* pindexPrev, bool fProofOfStake);
bool CanBuildRequiredMasternodePayment(const CBlockIndex* pindexPrev);

/**
 * Check coinbase output value for blocks after v6.0 enforcement.
 * It must pay the masternode for regular blocks and a proposal during superblocks.
 */
bool IsCoinbaseValueValid(const CTransactionRef& tx, CAmount nBudgetAmt, CValidationState& _state, const CBlockIndex* pindexPrev);

//
// Masternode Payments Class
// Resolves the deterministic masternode payee for each block (post-v6).
//

class CMasternodePayments
{
public:
    CMasternodePayments() {}

    // get the masternode payment outs for block built on top of pindexPrev
    bool GetMasternodeTxOuts(const CBlockIndex* pindexPrev, std::vector<CTxOut>& voutMasternodePaymentsRet) const;

    bool IsTransactionValid(const CTransaction& txNew, const CBlockIndex* pindexPrev);
    std::string GetRequiredPaymentsString(int nBlockHeight);
    void FillBlockPayee(CMutableTransaction& txCoinbase, CMutableTransaction& txCoinstake, const CBlockIndex* pindexPrev, bool fProofOfStake) const;
};

#endif // PIVX_MASTERNODE_PAYMENTS_H
