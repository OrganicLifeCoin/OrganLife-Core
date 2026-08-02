// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "budget/budgetproposal.h"
#include "budget/budgetvoteutil.h"
#include "chainparams.h"
#include "logging.h"
#include "net.h"
#include "script/standard.h"
#include "timedata.h"
#include "util/system.h"
#include "utilstrencodings.h"

#include <algorithm>
#include <limits>

CBudgetProposal::CBudgetProposal():
        nAllotted(0),
        fValid(true),
        strInvalid(""),
        strProposalName("unknown"),
        strURL(""),
        nBlockStart(0),
        nBlockEnd(0),
        address(),
        nAmount(0),
        nFeeTXHash(UINT256_ZERO),
        nCoinYeas(0),
        nCoinNays(0),
        nTime(0)
{}

CBudgetProposal::CBudgetProposal(const std::string& name,
                                 const std::string& url,
                                 int paycount,
                                 const CScript& payee,
                                 const CAmount& amount,
                                 int blockstart,
                                 const uint256& nfeetxhash):
        nAllotted(0),
        fValid(true),
        strInvalid(""),
        strProposalName(name),
        strURL(url),
        nBlockStart(blockstart),
        address(payee),
        nAmount(amount),
        nFeeTXHash(nfeetxhash),
        nCoinYeas(0),
        nCoinNays(0),
        nTime(0)
{
    // calculate the expiration block
    nBlockEnd = nBlockStart + (Params().GetConsensus().nBudgetCycleBlocks + 1)  * paycount;
}

// initialize from network broadcast message
bool CBudgetProposal::ParseBroadcast(CDataStream& broadcast)
{
    *this = CBudgetProposal();
    try {
        broadcast >> LIMITED_STRING(strProposalName, 20);
        broadcast >> LIMITED_STRING(strURL, 64);
        broadcast >> nTime;
        broadcast >> nBlockStart;
        broadcast >> nBlockEnd;
        broadcast >> nAmount;
        broadcast >> address;
        broadcast >> nFeeTXHash;
    } catch (std::exception& e) {
        return error("Unable to deserialize proposal broadcast: %s", e.what());
    }
    return true;
}

void CBudgetProposal::SyncVotes(CNode* pfrom, bool fPartial, int& nInvCount) const
{
    for (const auto& it: mapVotes) {
        const CBudgetVote& vote = it.second;
        if (vote.IsValid() && (!fPartial || !vote.IsSynced())) {
            pfrom->PushInventory(CInv(MSG_BUDGET_VOTE, vote.GetHash()));
            nInvCount++;
        }
    }
}

bool CBudgetProposal::IsHeavilyDownvoted(int mnCount, int64_t coinWeightFixed) const
{
    return IsHeavilyDownvotedHybrid(mnCount, coinWeightFixed);
}

bool CBudgetProposal::IsHeavilyDownvotedHybrid(int mnCount, int64_t coinWeightFixed) const
{
    const int64_t mnNet = GetNetMnVotes();
    const int64_t coinNet = GetNetCoinVotes();

    const __int128 netOpposition = (static_cast<__int128>(-mnNet) * HYBRID_SCORE_SCALE +
                                   static_cast<__int128>(coinWeightFixed) * (-coinNet));

    const __int128 threshold = static_cast<__int128>(mnCount * 3 / 10) * HYBRID_SCORE_SCALE;

    return (netOpposition > threshold &&
            (mnNet < 0 || coinNet < 0));
}

bool CBudgetProposal::CheckStartEnd()
{
    // block start must be a superblock
    if (nBlockStart < 0 ||
            nBlockStart % Params().GetConsensus().nBudgetCycleBlocks != 0) {
        strInvalid = "Invalid nBlockStart";
        return false;
    }

    if (nBlockEnd < nBlockStart) {
        strInvalid = "Invalid nBlockEnd (end before start)";
        return false;
    }

    if (GetTotalPaymentCount() > Params().GetConsensus().nMaxProposalPayments) {
        strInvalid = "Invalid payment count";
        return false;
    }

    return true;
}

bool CBudgetProposal::CheckAmount(const CAmount& nTotalBudget)
{
    (void)nTotalBudget;

    // check minimum amount
    if (nAmount < PROPOSAL_MIN_AMOUNT) {
        strInvalid = "Invalid nAmount (too low)";
        return false;
    }

    // check maximum amount (per-payment proposal cap)
    if (nAmount > PROPOSAL_MAX_AMOUNT) {
        strInvalid = "Invalid nAmount (too high)";
        return false;
    }

    return true;
}

bool CBudgetProposal::CheckAddress()
{
    // !TODO: There might be an issue with multisig in the coinbase on mainnet
    // we will add support for it in a future release.
    if (address.IsPayToScriptHash()) {
        strInvalid = "Multisig is not currently supported.";
        return false;
    }

    // Check address
    CTxDestination dest;
    if (!ExtractDestination(address, dest, false)) {
        strInvalid = "Invalid script";
        return false;
    }
    if (!IsValidDestination(dest)) {
        strInvalid = "Invalid recipient address";
        return false;
    }

    return true;
}

/* TODO: Add this to IsWellFormed() for the next hard-fork
 * This will networkly reject malformed proposal names and URLs
 */
bool CBudgetProposal::CheckStrings()
{
    if (strProposalName != SanitizeString(strProposalName)) {
        strInvalid = "Proposal name contains illegal characters.";
        return false;
    }
    if (strURL != SanitizeString(strURL)) {
        strInvalid = "Proposal URL contains illegal characters.";
        return false;
    }
    return true;
}

bool CBudgetProposal::IsWellFormed(const CAmount& nTotalBudget)
{
    return CheckStartEnd() && CheckAmount(nTotalBudget) && CheckAddress();
}

bool CBudgetProposal::updateExpired(int nCurrentHeight)
{
    if (IsExpired(nCurrentHeight)) {
        strInvalid = "Proposal expired";
        return true;
    }
    return false;
}

bool CBudgetProposal::UpdateValid(int nCurrentHeight, int mnCount, int64_t coinWeightFixed)
{
    fValid = false;

    // Never kill a proposal before the first superblock
    if (nCurrentHeight > nBlockStart && IsHeavilyDownvoted(mnCount, coinWeightFixed)) {
        return false;
    }

    if (updateExpired(nCurrentHeight)) {
        return false;
    }

    fValid = true;
    strInvalid.clear();
    return true;
}

bool CBudgetProposal::IsEstablished() const
{
    return nTime < GetAdjustedTime() - Params().GetConsensus().nProposalEstablishmentTime;
}

bool CBudgetProposal::IsPassing(int nBlockStartBudget, int nBlockEndBudget, int mnCount, int64_t coinWeightFixed) const
{
    if (!fValid)
        return false;

    if (this->nBlockStart > nBlockStartBudget)
        return false;

    if (this->nBlockEnd < nBlockEndBudget)
        return false;

    const int64_t threshold = static_cast<int64_t>(mnCount / 10) * HYBRID_SCORE_SCALE;
    if (GetCombinedScore(coinWeightFixed) <= threshold)
        return false;

    if (!IsEstablished())
        return false;

    return true;
}

bool CBudgetProposal::IsExpired(int nCurrentHeight) const
{
    return nBlockEnd < nCurrentHeight;
}

bool CBudgetProposal::AddOrUpdateVote(const CBudgetVote& vote, std::string& strError)
{
    return AddOrUpdateBudgetVote(mapVotes, vote, BUDGET_VOTE_UPDATE_MIN, __func__, strError);
}

UniValue CBudgetProposal::GetVotesArray() const
{
    UniValue ret(UniValue::VARR);
    for (const auto& it: mapVotes) {
        ret.push_back(it.second.ToJSON());
    }
    return ret;
}

void CBudgetProposal::SetSynced(bool synced)
{
    for (auto& it: mapVotes) {
        CBudgetVote& vote = it.second;
        if (synced) {
            if (vote.IsValid()) vote.SetSynced(true);
        } else {
            vote.SetSynced(false);
        }
    }
}

double CBudgetProposal::GetRatio() const
{
    int yeas = GetYeas();
    int nays = GetNays();

    if (yeas + nays == 0) return 0.0f;

    return ((double)(yeas) / (double)(yeas + nays));
}

int64_t CBudgetProposal::GetNetMnVotes() const
{
    return static_cast<int64_t>(GetYeas()) - static_cast<int64_t>(GetNays());
}

int64_t CBudgetProposal::GetNetCoinVotes() const
{
    return nCoinYeas - nCoinNays;
}

namespace {
struct CombinedScoreResult
{
    int64_t score{0};
    bool clamped{false};
};

CombinedScoreResult ComputeCombinedScoreInternal(int64_t netMnVotes, int64_t netCoinVotes, int64_t coinWeightFixed)
{
    const __int128 score = static_cast<__int128>(netMnVotes) * CBudgetProposal::HYBRID_SCORE_SCALE
                           + static_cast<__int128>(coinWeightFixed) * netCoinVotes;
    if (score > std::numeric_limits<int64_t>::max()) {
        return {std::numeric_limits<int64_t>::max(), true};
    }
    if (score < std::numeric_limits<int64_t>::min()) {
        return {std::numeric_limits<int64_t>::min(), true};
    }
    return {static_cast<int64_t>(score), false};
}
} // namespace

int64_t CBudgetProposal::ComputeCombinedScore(int64_t netMnVotes, int64_t netCoinVotes, int64_t coinWeightFixed)
{
    return ComputeCombinedScoreInternal(netMnVotes, netCoinVotes, coinWeightFixed).score;
}

int64_t CBudgetProposal::GetCombinedScore(int64_t coinWeightFixed) const
{
    const int64_t netMnVotes = GetNetMnVotes();
    const int64_t netCoinVotes = GetNetCoinVotes();
    const CombinedScoreResult combined = ComputeCombinedScoreInternal(netMnVotes, netCoinVotes, coinWeightFixed);
    if (combined.clamped) {
        LogPrint(BCLog::MNBUDGET,
                 "%s: clamped score for proposal %s (mn_net=%lld, coin_net=%lld, k_fixed=%lld)\n",
                 __func__,
                 GetName(),
                 static_cast<long long>(netMnVotes),
                 static_cast<long long>(netCoinVotes),
                 static_cast<long long>(coinWeightFixed));
    }
    return combined.score;
}

void CBudgetProposal::SetCoinVoteTotals(int64_t coinYeas, int64_t coinNays)
{
    nCoinYeas = std::max<int64_t>(0, coinYeas);
    nCoinNays = std::max<int64_t>(0, coinNays);
}

int CBudgetProposal::GetVoteCount(CBudgetVote::VoteDirection vd) const
{
    int ret = 0;
    for (const auto& it : mapVotes) {
        const CBudgetVote& vote = it.second;
        if (vote.GetDirection() == vd && vote.IsValid())
            ret++;
    }
    return ret;
}

int CBudgetProposal::GetBlockStartCycle() const
{
    //end block is half way through the next cycle (so the proposal will be removed much after the payment is sent)
    return GetBlockCycle(nBlockStart);
}

int CBudgetProposal::GetBlockCycle(int nHeight)
{
    return nHeight - nHeight % Params().GetConsensus().nBudgetCycleBlocks;
}

int CBudgetProposal::GetBlockEndCycle() const
{
    return nBlockEnd;

}

int CBudgetProposal::GetTotalPaymentCount() const
{
    return (GetBlockEndCycle() - GetBlockStartCycle()) / Params().GetConsensus().nBudgetCycleBlocks;
}

int CBudgetProposal::GetRemainingPaymentCount(int nCurrentHeight) const
{
    // If the proposal is already finished (passed the end block cycle), the payments value will be negative
    int nPayments = (GetBlockEndCycle() - GetBlockCycle(nCurrentHeight)) / Params().GetConsensus().nBudgetCycleBlocks - 1;
    // Take the lowest value
    return std::min(nPayments, GetTotalPaymentCount());
}

// return broadcast serialization
CDataStream CBudgetProposal::GetBroadcast() const
{
    CDataStream broadcast(SER_NETWORK, PROTOCOL_VERSION);
    broadcast.reserve(1000);
    broadcast << LIMITED_STRING(strProposalName, 20);
    broadcast << LIMITED_STRING(strURL, 64);
    broadcast << nTime;
    broadcast << nBlockStart;
    broadcast << nBlockEnd;
    broadcast << nAmount;
    broadcast << address;
    broadcast << nFeeTXHash;
    return broadcast;
}

void CBudgetProposal::Relay()
{
    CInv inv(MSG_BUDGET_PROPOSAL, GetHash());
    g_connman->RelayInv(inv);
}
