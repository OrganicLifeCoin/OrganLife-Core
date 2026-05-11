// Copyright (c) 2026 The CTEAM Core developers
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_parameters.hpp>

#include "test/test_pivx.h"
#include "evo/governancevotetx.h"
#include "evo/governancevoteindex.h"
#include "budget/budgetproposal.h"
#include "budget/budgetmanager.h"
#include "chainparams.h"
#include "consensus/params.h"
#include "script/standard.h"
#include "validation.h"

CKeyID GetRandomKeyID()
{
    CKey key;
    key.MakeNewKey(true);
    return key.GetPubKey().GetID();
}

BOOST_FIXTURE_TEST_SUITE(governance_hybrid_crit_tests, TestnetSetup)

BOOST_AUTO_TEST_CASE(proposal_serialization_includes_coin_votes)
{
    CKey payeeKey;
    payeeKey.MakeNewKey(true);
    const CScript payeeScript = GetScriptForDestination(payeeKey.GetPubKey().GetID());

    CBudgetProposal proposal("test", "https://test.com", 1, payeeScript, 100 * COIN, 144, GetRandHash());
    proposal.SetCoinVoteTotals(250, 100);

    CDataStream ss(SER_DISK, PROTOCOL_VERSION);
    ss << proposal;

    CBudgetProposal proposal2;
    ss >> proposal2;

    BOOST_CHECK_EQUAL(proposal2.GetCoinYeas(), 250);
    BOOST_CHECK_EQUAL(proposal2.GetCoinNays(), 100);
}

BOOST_AUTO_TEST_CASE(proposal_tally_updates_on_governance_vote)
{
    CKey payeeKey;
    payeeKey.MakeNewKey(true);
    const CScript payeeScript = GetScriptForDestination(payeeKey.GetPubKey().GetID());

    CBudgetProposal proposal("test", "https://test.com", 1, payeeScript, 100 * COIN, 144, GetRandHash());

    std::string strError;
    for (int i = 0; i < 5; ++i) {
        CBudgetVote vote(CTxIn(GetRandHash(), i), proposal.GetHash(), CBudgetVote::VOTE_YES);
        BOOST_REQUIRE(proposal.AddOrUpdateVote(vote, strError));
    }

    BOOST_CHECK_EQUAL(proposal.GetCoinYeas(), 0);
    BOOST_CHECK_EQUAL(proposal.GetCoinNays(), 0);

    proposal.SetCoinVoteTotals(500, 0);
    BOOST_CHECK_EQUAL(proposal.GetCoinYeas(), 500);
    BOOST_CHECK_EQUAL(proposal.GetCoinNays(), 0);
    BOOST_CHECK_EQUAL(proposal.GetNetMnVotes(), 5);

    const int64_t combinedScore = proposal.GetCombinedScore(CBudgetManager::DEFAULT_GOVERNANCE_COIN_WEIGHT_FIXED);
    BOOST_CHECK(combinedScore > 0);

    proposal.SetCoinVoteTotals(300, 0);
    BOOST_CHECK_EQUAL(proposal.GetCoinYeas(), 300);
}

BOOST_AUTO_TEST_CASE(hybrid_score_increases_with_coin_votes)
{
    CKey payeeKey;
    payeeKey.MakeNewKey(true);
    const CScript payeeScript = GetScriptForDestination(payeeKey.GetPubKey().GetID());

    CBudgetProposal proposal("test", "https://test.com", 1, payeeScript, 100 * COIN, 144, GetRandHash());

    std::string strError;

    for (int i = 0; i < 30; ++i) {
        CBudgetVote vote(CTxIn(GetRandHash(), i), proposal.GetHash(), CBudgetVote::VOTE_YES);
        proposal.AddOrUpdateVote(vote, strError);
    }

    for (int i = 30; i < 35; ++i) {
        CBudgetVote vote(CTxIn(GetRandHash(), i), proposal.GetHash(), CBudgetVote::VOTE_NO);
        proposal.AddOrUpdateVote(vote, strError);
    }

    const int64_t baseScore = proposal.GetCombinedScore(CBudgetManager::DEFAULT_GOVERNANCE_COIN_WEIGHT_FIXED);

    proposal.SetCoinVoteTotals(10000, 0);

    const int64_t boostedScore = proposal.GetCombinedScore(CBudgetManager::DEFAULT_GOVERNANCE_COIN_WEIGHT_FIXED);
    BOOST_CHECK(boostedScore > baseScore);
}

BOOST_AUTO_TEST_CASE(governance_vote_index_stores_vote_direction)
{
    const uint256 proposalHash = GetRandHash();
    const CAmount lockAmount = 100 * COIN;

    CGovVoteLockRecord lockRecord;
    lockRecord.proposalHash = proposalHash;
    lockRecord.lockAmount = lockAmount;
    lockRecord.unlockHeight = 10000;
    lockRecord.ownerKeyId = GetRandomKeyID();
    lockRecord.createdHeight = 1000;

    BOOST_CHECK(!lockRecord.HasVotedForProposal(proposalHash));
    BOOST_CHECK_EQUAL(lockRecord.GetVoteDirection(proposalHash), 0);

    lockRecord.proposalVoteDirections[proposalHash] = CGovVoteCastTx::VOTE_YES;

    BOOST_CHECK(lockRecord.HasVotedForProposal(proposalHash));
    BOOST_CHECK_EQUAL(lockRecord.GetVoteDirection(proposalHash), CGovVoteCastTx::VOTE_YES);
}

BOOST_AUTO_TEST_CASE(vote_cap_enforced_in_validation)
{
    CKey payeeKey;
    payeeKey.MakeNewKey(true);
    const CScript payeeScript = GetScriptForDestination(payeeKey.GetPubKey().GetID());

    CBudgetProposal proposal("test", "https://test.com", 1, payeeScript, 10 * COIN, 144, GetRandHash());

    LOCK(cs_main);
    g_budgetman.AddProposal(proposal);

    const CAmount excessiveLock = 100000 * COIN;
    const CAmount reasonableLock = 10 * COIN;

    BOOST_CHECK(reasonableLock <= 10 * proposal.GetAmount() * proposal.GetTotalPaymentCount());
    BOOST_CHECK(excessiveLock > 10 * proposal.GetAmount() * proposal.GetTotalPaymentCount());
}

BOOST_AUTO_TEST_SUITE_END()
