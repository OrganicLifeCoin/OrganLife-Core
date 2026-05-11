// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_pivx.h"

#include "budget/budgetmanager.h"
#include "budget/budgetproposal.h"
#include "script/standard.h"

#include <boost/test/unit_test.hpp>
#include <limits>

BOOST_FIXTURE_TEST_SUITE(budget_hybrid_score_tests, TestnetSetup)

BOOST_AUTO_TEST_CASE(hybrid_score_uses_mn_and_coin_votes)
{
    CKey payeeKey;
    payeeKey.MakeNewKey(true);
    const CScript payeeScript = GetScriptForDestination(payeeKey.GetPubKey().GetID());

    CBudgetProposal proposal(
            "hybrid-score",
            "https://pivx.org/hybrid",
            1,
            payeeScript,
            100 * COIN,
            144,
            GetRandHash());

    std::string strError;
    for (int i = 0; i < 5; ++i) {
        const CBudgetVote vote(CTxIn(GetRandHash(), i), proposal.GetHash(), CBudgetVote::VOTE_YES);
        BOOST_REQUIRE_MESSAGE(proposal.AddOrUpdateVote(vote, strError), strError);
    }
    for (int i = 0; i < 2; ++i) {
        const CBudgetVote vote(CTxIn(GetRandHash(), i), proposal.GetHash(), CBudgetVote::VOTE_NO);
        BOOST_REQUIRE_MESSAGE(proposal.AddOrUpdateVote(vote, strError), strError);
    }

    proposal.SetCoinVoteTotals(1000, 250);
    const int64_t combinedScore = proposal.GetCombinedScore(CBudgetManager::DEFAULT_GOVERNANCE_COIN_WEIGHT_FIXED);
    BOOST_CHECK_EQUAL(combinedScore, 1050000000);
}

BOOST_AUTO_TEST_CASE(hybrid_score_clamps_with_shared_math)
{
    const int64_t max = std::numeric_limits<int64_t>::max();
    const int64_t min = std::numeric_limits<int64_t>::min();

    BOOST_CHECK_EQUAL(CBudgetProposal::ComputeCombinedScore(max, max, max), max);
    BOOST_CHECK_EQUAL(CBudgetProposal::ComputeCombinedScore(min, min, max), min);
}

BOOST_AUTO_TEST_SUITE_END()
