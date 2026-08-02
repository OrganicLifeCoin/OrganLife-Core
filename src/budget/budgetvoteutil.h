// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_BUDGET_BUDGETVOTEUTIL_H
#define PIVX_BUDGET_BUDGETVOTEUTIL_H

#include "budget/budgetvote.h"
#include "logging.h"

#include <map>
#include <string>

template <typename Vote>
bool AddOrUpdateBudgetVote(std::map<COutPoint, Vote>& votes, const Vote& vote, int64_t voteUpdateMin, const char* func, std::string& strError)
{
    const COutPoint& mnId = vote.GetVin().prevout;
    const int64_t voteTime = vote.GetTime();
    const char* strAction = "New vote inserted:";

    const auto it = votes.find(mnId);
    if (it != votes.end()) {
        const int64_t oldTime = it->second.GetTime();
        if (oldTime > voteTime) {
            strError = strprintf("new vote older than existing vote - %s\n", vote.GetHash().ToString());
            LogPrint(BCLog::MNBUDGET, "%s: %s\n", func, strError);
            return false;
        }
        if (voteTime - oldTime < voteUpdateMin) {
            strError = strprintf("time between votes is too soon - %s - %lli sec < %lli sec\n",
                    vote.GetHash().ToString(), voteTime - oldTime, voteUpdateMin);
            LogPrint(BCLog::MNBUDGET, "%s: %s\n", func, strError);
            return false;
        }
        strAction = "Existing vote updated:";
    }

    votes[mnId] = vote;
    LogPrint(BCLog::MNBUDGET, "%s: %s %s\n", func, strAction, vote.GetHash().ToString().c_str());
    return true;
}

#endif // PIVX_BUDGET_BUDGETVOTEUTIL_H
