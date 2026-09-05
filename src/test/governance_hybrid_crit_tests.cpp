// Copyright (c) 2026 The OrganicLife Coin developers
#include <boost/test/unit_test.hpp>
#include <boost/test/unit_test_parameters.hpp>

#include "test/test_organiclife.h"
#include "evo/governancevotetx.h"
#include "evo/governancevoteindex.h"
#include "budget/budgetproposal.h"
#include "budget/budgetmanager.h"
#include "chainparams.h"
#include "consensus/params.h"
#include "pqaddress.h"
#include "pqtransaction.h"
#include "validation.h"

pq::KeyID GetRandomPQKeyID()
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.Generate());
    const auto id = pq::GetID(key.GetPublicKey(), Params().NetworkIDString());
    BOOST_REQUIRE(id);
    return *id;
}

BOOST_FIXTURE_TEST_SUITE(governance_crit_tests, TestnetSetup)

BOOST_AUTO_TEST_CASE(proposal_serialization_includes_coin_votes)
{
    const CScript payeeScript = pq::GetScript(GetRandomPQKeyID());

    CBudgetProposal proposal("test", "https://test.com", 1, payeeScript, 100 * COIN, 144, GetRandHash());
    proposal.SetCoinVoteTotals(250, 100);

    CDataStream ss(SER_DISK, PROTOCOL_VERSION);
    ss << proposal;

    CBudgetProposal proposal2;
    ss >> proposal2;

    BOOST_CHECK_EQUAL(proposal2.GetCoinYeas(), 250);
    BOOST_CHECK_EQUAL(proposal2.GetCoinNays(), 100);
}

BOOST_AUTO_TEST_CASE(proposal_tally_uses_only_coin_votes)
{
    const CScript payeeScript = pq::GetScript(GetRandomPQKeyID());

    CBudgetProposal proposal("test", "https://test.com", 1, payeeScript, 100 * COIN, 144, GetRandHash());

    proposal.SetCoinVoteTotals(500, 125);
    BOOST_CHECK_EQUAL(proposal.GetCoinYeas(), 500);
    BOOST_CHECK_EQUAL(proposal.GetCoinNays(), 125);
    BOOST_CHECK_EQUAL(proposal.GetNetCoinVotes(), 375);
}

BOOST_AUTO_TEST_CASE(governance_vote_index_stores_vote_direction)
{
    const uint256 proposalHash = GetRandHash();
    const CAmount lockAmount = 100 * COIN;

    CGovVoteLockRecord lockRecord;
    lockRecord.proposalHash = proposalHash;
    lockRecord.lockAmount = lockAmount;
    lockRecord.unlockHeight = 10000;
    lockRecord.ownerKeyId = GetRandomPQKeyID();
    lockRecord.createdHeight = 1000;

    BOOST_CHECK(!lockRecord.HasVotedForProposal(proposalHash));
    BOOST_CHECK_EQUAL(lockRecord.GetVoteDirection(proposalHash), 0);

    lockRecord.proposalVoteDirections[proposalHash] = CGovVoteCastTx::VOTE_YES;

    BOOST_CHECK(lockRecord.HasVotedForProposal(proposalHash));
    BOOST_CHECK_EQUAL(lockRecord.GetVoteDirection(proposalHash), CGovVoteCastTx::VOTE_YES);
}

BOOST_AUTO_TEST_SUITE_END()
