// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_BUDGET_BUDGETPROPOSAL_H
#define PIVX_BUDGET_BUDGETPROPOSAL_H

#include "budget/budgetvote.h"
#include "protocol.h"
#include "streams.h"

#include <cstdint>

static const CAmount PROPOSAL_FEE_TX = (100 * COIN);
static const CAmount BUDGET_FEE_TX_OLD = (100 * COIN);
static const CAmount BUDGET_FEE_TX = (5 * COIN);
static const int64_t BUDGET_VOTE_UPDATE_MIN = 60 * 60;

// Minimum value for a proposal to be considered valid
static const CAmount PROPOSAL_MIN_AMOUNT = 10 * COIN;
// Maximum value for a proposal payment amount.
// Total funded budget per cycle is still constrained by CBudgetManager::GetTotalBudget().
static const CAmount PROPOSAL_MAX_AMOUNT = 1000000 * COIN;

// Net ser values
static const size_t PROP_URL_MAX_SIZE = 64;
static const size_t PROP_NAME_MAX_SIZE = 20;

class CBudgetManager;
class CNode;

//
// Budget Proposal : Contains the masternode votes for each budget
//

class CBudgetProposal
{
private:
    friend class CBudgetManager;
    CAmount nAllotted;
    bool fValid;
    std::string strInvalid;

    // Functions used inside UpdateValid()/IsWellFormed - setting strInvalid
    bool IsHeavilyDownvoted(int mnCount, int64_t coinWeightFixed) const;
    bool IsHeavilyDownvotedHybrid(int mnCount, int64_t coinWeightFixed) const;
    bool updateExpired(int nCurrentHeight);
    bool CheckStartEnd();
    bool CheckAmount(const CAmount& nTotalBudget);
    bool CheckAddress();
    bool CheckStrings();

protected:
    std::map<COutPoint, CBudgetVote> mapVotes;
    std::string strProposalName;
    std::string strURL;
    int nBlockStart;
    int nBlockEnd;
    CScript address;
    CAmount nAmount;
    uint256 nFeeTXHash;
    int64_t nCoinYeas{0};
    int64_t nCoinNays{0};

public:
    static constexpr int64_t HYBRID_SCORE_SCALE = 100000000;

    // Set in CBudgetManager::AddProposal via CheckCollateral
    int64_t nTime;

    CBudgetProposal();
    CBudgetProposal(const std::string& name, const std::string& url, int paycount, const CScript& payee, const CAmount& amount, int blockstart, const uint256& nfeetxhash);

    bool AddOrUpdateVote(const CBudgetVote& vote, std::string& strError);
    UniValue GetVotesArray() const;
    void SetSynced(bool synced);    // sets fSynced on votes (true only if valid)

    // sync proposal votes with a node
    void SyncVotes(CNode* pfrom, bool fPartial, int& nInvCount) const;

    // sets fValid and strInvalid, returns fValid
    bool UpdateValid(int nHeight, int mnCount, int64_t coinWeightFixed);
    // Static checks that should be done only once - sets strInvalid
    bool IsWellFormed(const CAmount& nTotalBudget);
    bool IsValid() const  { return fValid; }
    void SetStrInvalid(const std::string& _strInvalid) { strInvalid = _strInvalid; }
    std::string IsInvalidReason() const { return strInvalid; }
    std::string IsInvalidLogStr() const { return strprintf("[%s]: %s", GetName(), IsInvalidReason()); }

    bool IsEstablished() const;
    bool IsPassing(int nBlockStartBudget, int nBlockEndBudget, int mnCount, int64_t coinWeightFixed) const;
    bool IsExpired(int nCurrentHeight) const;

    std::string GetName() const { return strProposalName; }
    std::string GetURL() const { return strURL; }
    int GetBlockStart() const { return nBlockStart; }
    int GetBlockEnd() const { return nBlockEnd; }
    CScript GetPayee() const { return address; }
    int GetTotalPaymentCount() const;
    int GetRemainingPaymentCount(int nCurrentHeight) const;
    int GetBlockStartCycle() const;
    static int GetBlockCycle(int nCurrentHeight);
    int GetBlockEndCycle() const;
    const uint256& GetFeeTXHash() const { return nFeeTXHash;  }
    double GetRatio() const;
    int GetVoteCount(CBudgetVote::VoteDirection vd) const;
    std::map<COutPoint, CBudgetVote> GetVotes() const { return mapVotes; }
    int GetYeas() const { return GetVoteCount(CBudgetVote::VOTE_YES); }
    int GetNays() const { return GetVoteCount(CBudgetVote::VOTE_NO); }
    int GetAbstains() const { return GetVoteCount(CBudgetVote::VOTE_ABSTAIN); }
    int64_t GetCoinYeas() const { return nCoinYeas; }
    int64_t GetCoinNays() const { return nCoinNays; }
    int64_t GetNetMnVotes() const;
    int64_t GetNetCoinVotes() const;
    static int64_t ComputeCombinedScore(int64_t netMnVotes, int64_t netCoinVotes, int64_t coinWeightFixed);
    int64_t GetCombinedScore(int64_t coinWeightFixed) const;
    CAmount GetAmount() const { return nAmount; }
    void SetAllotted(CAmount nAllottedIn) { nAllotted = nAllottedIn; }
    CAmount GetAllotted() const { return nAllotted; }
    void SetFeeTxHash(const uint256& txid) { nFeeTXHash = txid; }
    void SetCoinVoteTotals(int64_t coinYeas, int64_t coinNays);

    uint256 GetHash() const
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << strProposalName;
        ss << strURL;
        ss << nBlockStart;
        ss << nBlockEnd;
        ss << nAmount;
        ss << std::vector<unsigned char>(address.begin(), address.end());
        return ss.GetHash();
    }

    // Serialization for local DB
    SERIALIZE_METHODS(CBudgetProposal, obj)
    {
        READWRITE(LIMITED_STRING(obj.strProposalName, PROP_NAME_MAX_SIZE));
        READWRITE(LIMITED_STRING(obj.strURL, PROP_URL_MAX_SIZE));
        READWRITE(obj.nBlockStart);
        READWRITE(obj.nBlockEnd);
        READWRITE(obj.nAmount);
        READWRITE(obj.address);
        READWRITE(obj.nFeeTXHash);
        READWRITE(obj.nTime);
        READWRITE(obj.mapVotes);
        READWRITE(obj.nCoinYeas);
        READWRITE(obj.nCoinNays);
    }

    // Serialization for network messages.
    bool ParseBroadcast(CDataStream& broadcast);
    CDataStream GetBroadcast() const;
    void Relay();

    inline bool operator==(const CBudgetProposal& other) const
    {
        return GetHash() == other.GetHash();
    }

    // compare proposals by proposal hash
    inline bool operator>(const CBudgetProposal& other) const
    {
        return UintToArith256(GetHash()) > UintToArith256(other.GetHash());
    }
    //
    // compare proposals pointers by net yes count (solve tie with feeHash)
    static inline bool PtrHigherYes(CBudgetProposal* a, CBudgetProposal* b)
    {
        const int netYes_a = a->GetYeas() - a->GetNays();
        const int netYes_b = b->GetYeas() - b->GetNays();
        if (netYes_a == netYes_b) return UintToArith256(a->GetFeeTXHash()) > UintToArith256(b->GetFeeTXHash());
        return netYes_a > netYes_b;
    }

};

#endif // PIVX_BUDGET_BUDGETPROPOSAL_H
