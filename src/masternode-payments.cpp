// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode-payments.h"

#include "budget/budgetmanager.h"
#include "chainparams.h"
#include "evo/deterministicmns.h"
#include "spork.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "util/system.h"
#include "utilmoneystr.h"
#include "validation.h"


/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;

bool IsBlockValueValid(int nHeight, CAmount& nExpectedValue, CAmount nMinted, CAmount& nBudgetAmt)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    const CAmount nBaseExpectedValue = nExpectedValue;
    bool fUsingBudgetWindowFallback = false;
    if (!g_tiertwo_sync_state.IsSynced()) {
        //there is no budget data to use to check anything
        //super blocks will always be on these blocks, max 100 per budgeting
        if (nHeight % consensus.nBudgetCycleBlocks < 100) {
            if (Params().IsTestnet()) {
                return true;
            }
            nExpectedValue += g_budgetman.GetTotalBudget(nHeight);
            fUsingBudgetWindowFallback = true;
        }
    } else {
        // we're synced and have data so check the budget schedule
        // if the superblock spork is enabled
        if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)) {
            // add current payee amount to the expected block value
            if (g_budgetman.GetExpectedPayeeAmount(nHeight, nBudgetAmt)) {
                nExpectedValue += nBudgetAmt;
            }
        }
    }

    if (nMinted < 0 && consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V5_3)) {
        return false;
    }

    const std::string& network = Params().NetworkIDString();
    const bool enforceExactMint = (network == CBaseChainParams::MAIN) &&
                                  consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V6_1_GOV);
    if (enforceExactMint) {
        // During IBD we do not have budget data, so the budget-window fallback is only
        // an upper bound. Exact mint is safe once the actual budget amount is known.
        if (fUsingBudgetWindowFallback) {
            return nMinted >= nBaseExpectedValue && nMinted <= nExpectedValue;
        }
        return nMinted == nExpectedValue;
    }

    return nMinted <= nExpectedValue;
}

bool IsBlockPayeeValid(const CBlock& block, const CBlockIndex* pindexPrev)
{
    int nBlockHeight = pindexPrev->nHeight + 1;
    TrxValidationStatus transactionStatus = TrxValidationStatus::InValid;

    if (!g_tiertwo_sync_state.IsSynced()) { //there is no budget data to use to check anything -- find the longest chain
        LogPrint(BCLog::MASTERNODE, "Client not synced, skipping block payee checks\n");
        return true;
    }

    const bool fPayCoinstake = Params().GetConsensus().NetworkUpgradeActive(nBlockHeight, Consensus::UPGRADE_POS) &&
                               !Params().GetConsensus().NetworkUpgradeActive(nBlockHeight, Consensus::UPGRADE_V6_0);
    const CTransaction& txNew = *(fPayCoinstake ? block.vtx[1] : block.vtx[0]);

    //check if it's a budget block
    if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)) {
        if (g_budgetman.IsBudgetPaymentBlock(nBlockHeight)) {
            transactionStatus = g_budgetman.IsTransactionValid(txNew, block.GetHash(), nBlockHeight);
            if (transactionStatus == TrxValidationStatus::Valid) {
                return true;
            }

            if (transactionStatus == TrxValidationStatus::InValid) {
                LogPrint(BCLog::MASTERNODE,"Invalid budget payment detected %s\n", txNew.ToString().c_str());
                if (sporkManager.IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT))
                    return false;

                LogPrint(BCLog::MASTERNODE,"Budget enforcement is disabled, accepting block\n");
            }
        }
    }

    // If we end here the transaction was either TrxValidationStatus::InValid and Budget enforcement is disabled, or
    // a double budget payment (status = TrxValidationStatus::DoublePayment) was detected, or no/not enough masternode
    // votes (status = TrxValidationStatus::VoteThreshold) for a finalized budget were found
    // In all cases a masternode will get the payment for this block

    //check for masternode payee
    if (masternodePayments.IsTransactionValid(txNew, pindexPrev))
        return true;
    LogPrint(BCLog::MASTERNODE,"Invalid mn payment detected %s\n", txNew.ToString().c_str());

    // OrganicLife post-v6: a block without any determinable payee (empty
    // coinbase) is valid, so the chain can advance before the first
    // masternode registers. Only reject when a payee exists but the payment
    // is missing from the block.
    if (Params().GetConsensus().NetworkUpgradeActive(nBlockHeight, Consensus::UPGRADE_V6_0)) {
        std::vector<CTxOut> vecMnOuts;
        if (!masternodePayments.GetMasternodeTxOuts(pindexPrev, vecMnOuts) || vecMnOuts.empty()) {
            LogPrint(BCLog::MASTERNODE, "No masternode payee determinable at height %d, accepting block\n", nBlockHeight);
            return true;
        }
    }

    if (sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT))
        return false;
    LogPrint(BCLog::MASTERNODE,"Masternode payment enforcement is disabled, accepting block\n");
    return true;
}


void FillBlockPayee(CMutableTransaction& txCoinbase, CMutableTransaction& txCoinstake, const CBlockIndex* pindexPrev, bool fProofOfStake)
{
    if (!sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) ||           // if superblocks are not enabled
            // ... or this is not a superblock
            !g_budgetman.FillBlockPayee(txCoinbase, txCoinstake, pindexPrev->nHeight + 1, fProofOfStake) ) {
        // ... or there's no budget with enough votes, then pay a masternode
        masternodePayments.FillBlockPayee(txCoinbase, txCoinstake, pindexPrev, fProofOfStake);
    }
}

bool CanBuildRequiredMasternodePayment(const CBlockIndex* pindexPrev)
{
    if (!pindexPrev)
        return false;

    if (Params().IsRegTestNet())
        return true;

    const int nHeight = pindexPrev->nHeight + 1;
    if (!Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_POS))
        return true;

    if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && g_budgetman.IsBudgetPaymentBlock(nHeight)) {
        CAmount budgetPayment{0};
        if (g_budgetman.GetExpectedPayeeAmount(nHeight, budgetPayment) && budgetPayment > 0)
            return true;
    }

    if (GetMasternodePayment(nHeight) <= 0)
        return true;

    std::vector<CTxOut> vecMnOuts;
    return masternodePayments.GetMasternodeTxOuts(pindexPrev, vecMnOuts) && !vecMnOuts.empty();
}

std::string GetRequiredPaymentsString(int nBlockHeight)
{
    if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && g_budgetman.IsBudgetPaymentBlock(nBlockHeight)) {
        return g_budgetman.GetRequiredPaymentsString(nBlockHeight);
    } else {
        return masternodePayments.GetRequiredPaymentsString(nBlockHeight);
    }
}

bool CMasternodePayments::GetMasternodeTxOuts(const CBlockIndex* pindexPrev, std::vector<CTxOut>& voutMasternodePaymentsRet) const
{
    if (deterministicMNManager->LegacyMNObsolete(pindexPrev->nHeight + 1)) {
        CAmount masternodeReward = GetMasternodePayment(pindexPrev->nHeight + 1);
        if (masternodeReward <= 0) {
            LogPrint(BCLog::MASTERNODE, "%s: Masternode reward is zero at height %d, skipping payment\n",
                     __func__, pindexPrev->nHeight + 1);
            return true; // No MN payment required if reward is zero
        }
        auto dmnPayee = deterministicMNManager->GetListForBlock(pindexPrev).GetMNPayee();
        if (!dmnPayee) {
            // No determinable DMN payee: no payment. The coinbase stays empty
            // and the block is valid (post-v6 no-payee blocks are allowed so
            // the chain can advance before the first masternode registers).
            LogPrint(BCLog::MASTERNODE, "%s: no DMN payee at height %d, no payment\n",
                     __func__, pindexPrev->nHeight + 1);
            return true;
        }
        CAmount operatorReward = 0;
        if (dmnPayee->nOperatorReward != 0 && !dmnPayee->pdmnState->scriptOperatorPayout.empty()) {
            operatorReward = (masternodeReward * dmnPayee->nOperatorReward) / 10000;
            masternodeReward -= operatorReward;
        }
        if (masternodeReward > 0) {
            voutMasternodePaymentsRet.emplace_back(masternodeReward, dmnPayee->pdmnState->scriptPayout);
        }
        if (operatorReward > 0) {
            voutMasternodePaymentsRet.emplace_back(operatorReward, dmnPayee->pdmnState->scriptOperatorPayout);
        }
        return true;
    }

    // Pre-v6 (regtest only, since mainnet/testnet activate v6 at genesis):
    // the legacy masternode system is gone, so no masternode payment is
    // required at all.
    return true;
}

static void SubtractMnPaymentFromCoinstake(CMutableTransaction& txCoinstake, CAmount masternodePayment, int stakerOuts)
{
    assert (stakerOuts >= 2);
    //subtract mn payment from the stake reward
    if (stakerOuts == 2) {
        // Majority of cases; do it quick and move on
        txCoinstake.vout[1].nValue -= masternodePayment;
    } else {
        // special case, stake is split between (stakerOuts-1) outputs
        unsigned int outputs = stakerOuts-1;
        CAmount mnPaymentSplit = masternodePayment / outputs;
        CAmount mnPaymentRemainder = masternodePayment - (mnPaymentSplit * outputs);
        for (unsigned int j=1; j<=outputs; j++) {
            txCoinstake.vout[j].nValue -= mnPaymentSplit;
        }
        // in case it's not an even division, take the last bit of dust from the last one
        txCoinstake.vout[outputs].nValue -= mnPaymentRemainder;
    }
}

static void EnsureEmptyCoinbaseOutput(CMutableTransaction& txCoinbase)
{
    if (!txCoinbase.vout.empty())
        return;

    txCoinbase.vout.emplace_back();
    txCoinbase.vout[0].SetEmpty();
}

void CMasternodePayments::FillBlockPayee(CMutableTransaction& txCoinbase, CMutableTransaction& txCoinstake, const CBlockIndex* pindexPrev, bool fProofOfStake) const
{
    const int nHeight = pindexPrev->nHeight + 1;
    const bool fPayCoinstake = fProofOfStake && !Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V6_0);
    const bool fPayCoinbase = fProofOfStake && !fPayCoinstake;

    // Starting from OrganicLife v6.0 masternode and budgets are paid in the coinbase tx.
    // Keep a valid empty coinbase if there is no payee yet on a small or unsynced network.
    if (fPayCoinbase) txCoinbase.vout.clear();

    std::vector<CTxOut> vecMnOuts;
    if (!GetMasternodeTxOuts(pindexPrev, vecMnOuts)) {
        if (fPayCoinbase) EnsureEmptyCoinbaseOutput(txCoinbase);
        return;
    }
    if (vecMnOuts.empty()) {
        if (fPayCoinbase) EnsureEmptyCoinbaseOutput(txCoinbase);
        return;
    }

    const int initial_cstake_outs = txCoinstake.vout.size();

    CAmount masternodePayment{0};
    for (const CTxOut& mnOut: vecMnOuts) {
        // Add the mn payment to the coinstake/coinbase tx
        if (fPayCoinstake) {
            txCoinstake.vout.emplace_back(mnOut);
        } else {
            txCoinbase.vout.emplace_back(mnOut);
        }
        masternodePayment += mnOut.nValue;
        CTxDestination payeeDest;
        ExtractDestination(mnOut.scriptPubKey, payeeDest);
        LogPrint(BCLog::MASTERNODE,"Masternode payment of %s to %s\n", FormatMoney(mnOut.nValue), EncodeDestination(payeeDest));
    }

    // Subtract mn payment value from the block reward
    if (fProofOfStake) {
        SubtractMnPaymentFromCoinstake(txCoinstake, masternodePayment, initial_cstake_outs);
    } else {
        txCoinbase.vout[0].nValue = GetBlockValue(nHeight) - masternodePayment;
    }
}

std::string CMasternodePayments::GetRequiredPaymentsString(int nBlockHeight)
{
    if (!deterministicMNManager->LegacyMNObsolete()) {
        return "Unknown";
    }

    // DMN branch: the payee is derived from the deterministic list.
    const CBlockIndex* pindexTip = GetChainTip();
    if (!pindexTip) return "Unknown";
    auto mnList = deterministicMNManager->GetListAtChainTip();
    const unsigned int nValid = mnList.GetValidMNsCount();
    if (nValid == 0) return "Unknown";

    auto fmtPayees = [](const std::vector<CTxOut>& outs) {
        std::vector<std::string> payees;
        for (const CTxOut& out : outs) {
            CTxDestination dest;
            const std::string& addr = ExtractDestination(out.scriptPubKey, dest)
                                          ? EncodeDestination(dest)
                                          : HexStr(out.scriptPubKey);
            payees.emplace_back(strprintf("%s:0", addr));
        }
        return payees.empty() ? std::string("Unknown") : Join(payees, ",");
    };

    if (nBlockHeight <= pindexTip->nHeight) {
        // Past height: the exact payee of that block.
        const CBlockIndex* pindexPrev = nBlockHeight > 0 ? pindexTip->GetAncestor(nBlockHeight - 1) : nullptr;
        if (!pindexPrev) return "Unknown";
        std::vector<CTxOut> vecMnOuts;
        if (!GetMasternodeTxOuts(pindexPrev, vecMnOuts) || vecMnOuts.empty()) return "Unknown";
        return fmtPayees(vecMnOuts);
    }

    // Future height: the deterministic rotation (least-recently-paid order).
    const auto projected = mnList.GetProjectedMNPayees(nValid);
    const unsigned int pos = (nBlockHeight - pindexTip->nHeight - 1) % nValid;
    const CDeterministicMNCPtr& dmn = projected[pos];
    if (!dmn) return "Unknown";
    std::vector<CTxOut> vecMnOuts;
    CAmount masternodeReward = GetMasternodePayment(nBlockHeight);
    CAmount operatorReward = 0;
    if (dmn->nOperatorReward != 0 && !dmn->pdmnState->scriptOperatorPayout.empty()) {
        operatorReward = (masternodeReward * dmn->nOperatorReward) / 10000;
        masternodeReward -= operatorReward;
    }
    if (masternodeReward > 0) vecMnOuts.emplace_back(masternodeReward, dmn->pdmnState->scriptPayout);
    if (operatorReward > 0) vecMnOuts.emplace_back(operatorReward, dmn->pdmnState->scriptOperatorPayout);
    return fmtPayees(vecMnOuts);
}

bool CMasternodePayments::IsTransactionValid(const CTransaction& txNew, const CBlockIndex* pindexPrev)
{
    const int nBlockHeight = pindexPrev->nHeight + 1;
    if (deterministicMNManager->LegacyMNObsolete(nBlockHeight)) {
        std::vector<CTxOut> vecMnOuts;
        if (!GetMasternodeTxOuts(pindexPrev, vecMnOuts)) {
            // No masternode scheduled to be paid.
            return true;
        }

        for (const CTxOut& o : vecMnOuts) {
            if (std::find(txNew.vout.begin(), txNew.vout.end(), o) == txNew.vout.end()) {
                CTxDestination mnDest;
                const std::string& payee = ExtractDestination(o.scriptPubKey, mnDest) ? EncodeDestination(mnDest)
                                                                                      : HexStr(o.scriptPubKey);
                LogPrint(BCLog::MASTERNODE, "%s: Failed to find expected payee %s in block at height %d (tx %s)\n",
                                            __func__, payee, pindexPrev->nHeight + 1, txNew.GetHash().ToString());
                return false;
            }
        }
        // all the expected payees have been found in txNew outputs
        return true;
    }

    // Pre-v6 (regtest only, since mainnet/testnet activate v6 at genesis):
    // the legacy masternode system is gone, so no payment is required.
    return true;
}

bool IsCoinbaseValueValid(const CTransactionRef& tx, CAmount nBudgetAmt, CValidationState& _state, const CBlockIndex* pindexPrev)
{
    assert(tx->IsCoinBase());
    assert(pindexPrev);
    const int nHeight = pindexPrev->nHeight + 1;
    const CAmount nCBaseOutAmt = tx->GetValueOut();
    if (nBudgetAmt > 0) {
        // Superblock
        if (nCBaseOutAmt != nBudgetAmt) {
            const std::string strError = strprintf("%s: invalid coinbase payment for budget (%s vs expected=%s)",
                                                   __func__, FormatMoney(nCBaseOutAmt), FormatMoney(nBudgetAmt));
            return _state.DoS(100, error("%s", strError.c_str()), REJECT_INVALID, "bad-superblock-cb-amt");
        }
        return true;
    }

    // Regular block. The exact payee cannot be reconstructed while tier-two
    // data is still syncing, but the required total payment amount is known.
    const CAmount requiredMasternodePayment = GetMasternodePayment(nHeight);
    if (!g_tiertwo_sync_state.IsSynced()) {
        // Either the expected total payment, or nothing at all when no payee
        // is determinable yet (small or unsynced network: FillBlockPayee keeps
        // a valid empty coinbase).
        if (requiredMasternodePayment > 0 && nCBaseOutAmt != requiredMasternodePayment && nCBaseOutAmt != 0) {
            const std::string strError = strprintf("%s: invalid coinbase payment while masternode payee is syncing (%s vs expected=%s)",
                                                   __func__, FormatMoney(nCBaseOutAmt), FormatMoney(requiredMasternodePayment));
            return _state.DoS(100, error("%s", strError.c_str()), REJECT_INVALID, "bad-cb-amt");
        }
        return true;
    }

    std::vector<CTxOut> vecMnOuts;
    const bool havePayee = masternodePayments.GetMasternodeTxOuts(pindexPrev, vecMnOuts);
    if (!havePayee || vecMnOuts.empty()) {
        // No payee determinable (no masternodes yet): an empty coinbase is
        // valid, so the chain can advance and the first masternodes can
        // register (OrganicLife post-v6 design decision).
        const CAmount expectedCoinbase = 0;
        if (nCBaseOutAmt != expectedCoinbase) {
            const std::string strError = strprintf("%s: invalid coinbase payment without masternode payee (%s vs expected=%s)",
                                                   __func__, FormatMoney(nCBaseOutAmt), FormatMoney(expectedCoinbase));
            return _state.DoS(100, error("%s", strError.c_str()), REJECT_INVALID, "bad-cb-amt");
        }
        return true;
    }

    CAmount nMnAmt = 0;
    for (const CTxOut& out : vecMnOuts) {
        nMnAmt += out.nValue;
    }
    // if enforcement is disabled, there could be no masternode payment
    bool sporkEnforced = sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT);
    const std::string strError = strprintf("%s: invalid coinbase payment for masternode (%s vs expected=%s)",
                                           __func__, FormatMoney(nCBaseOutAmt), FormatMoney(nMnAmt));

    if (sporkEnforced && nCBaseOutAmt != nMnAmt) {
        return _state.DoS(100, error("%s", strError.c_str()), REJECT_INVALID, "bad-cb-amt");
    }
    if (!sporkEnforced && nCBaseOutAmt > nMnAmt) {
        return _state.DoS(100, error("%s", strError.c_str()), REJECT_INVALID, "bad-cb-amt-spork8-disabled");
    }
    return true;
}
