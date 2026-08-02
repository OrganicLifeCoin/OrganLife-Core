// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "consensus/validation.h"
#include "budget/budgetmanager.h"
#include "evo/governancevoteindex.h"
#include "evo/governancevotetx.h"
#include "messagesigner.h"
#include "script/standard.h"

#include <boost/test/unit_test.hpp>

namespace {
CKey GetRandomKey()
{
    CKey key;
    key.MakeNewKey(true);
    return key;
}

CMutableTransaction BuildGovVoteLockTx(const uint256& proposalHash,
                                       const CKeyID& ownerKeyId,
                                       const CAmount amount,
                                       const uint32_t unlockHeight)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType = CTransaction::TxType::GOVVOTELOCK;
    mtx.vin.emplace_back(GetRandHash(), 0);
    mtx.vout.emplace_back(amount, GetScriptForDestination(ownerKeyId));

    CGovVoteLockTx payload;
    payload.proposalHash = proposalHash;
    payload.lockAmount = amount;
    payload.unlockHeight = unlockHeight;
    payload.ownerKeyId = ownerKeyId;
    SetTxPayload(mtx, payload);
    return mtx;
}

CMutableTransaction BuildGovVoteCastTx(const uint256& proposalHash,
                                       const std::vector<COutPoint>& lockRefs,
                                       const CKey& ownerKey)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType = CTransaction::TxType::GOVVOTECAST;
    mtx.vin.emplace_back(GetRandHash(), 0);

    CGovVoteCastTx payload;
    payload.proposalHash = proposalHash;
    payload.voteDirection = CGovVoteCastTx::VOTE_YES;
    payload.lockRefs = lockRefs;
    BOOST_REQUIRE(CHashSigner::SignHash(payload.GetSignatureHash(), ownerKey, payload.sig));
    SetTxPayload(mtx, payload);

    return mtx;
}

int GovActivationHeight()
{
    const int height =
            Params().GetConsensus().vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight;
    BOOST_REQUIRE(height > 0);
    return height;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(governancevoteindex_tests, TestnetSetup)

BOOST_AUTO_TEST_CASE(gov_voteindex_connect_disconnect_roundtrip)
{
    BOOST_REQUIRE(governanceVoteIndex);

    const CKey ownerKey = GetRandomKey();
    const CKeyID ownerKeyId = ownerKey.GetPubKey().GetID();
    const uint256 proposalHash = GetRandHash();
    const CAmount amount = 42 * COIN;

    const CTransaction lockTx(BuildGovVoteLockTx(proposalHash, ownerKeyId, amount, 50000));
    const COutPoint lockRef(lockTx.GetHash(), 0);
    const CTransaction castTx(BuildGovVoteCastTx(proposalHash, {lockRef}, ownerKey));

    CBlock lockBlock;
    lockBlock.vtx.emplace_back(MakeTransactionRef(lockTx));

    CBlockIndex lockIndex;
    lockIndex.nHeight = GovActivationHeight();

    CValidationState state;
    BOOST_CHECK(governanceVoteIndex->ProcessBlock(lockBlock, &lockIndex, state, false));

    CGovVoteLockRecord lockRecord;
    BOOST_CHECK(governanceVoteIndex->GetLockRecord(lockRef, lockRecord));
    BOOST_CHECK_EQUAL(lockRecord.proposalHash, proposalHash);
    BOOST_CHECK_EQUAL(lockRecord.lockAmount, amount);
    BOOST_CHECK(!governanceVoteIndex->IsLockUsedForProposal(lockRef, proposalHash));
    int64_t coinYeas = -1;
    int64_t coinNays = -1;
    BOOST_CHECK(!governanceVoteIndex->GetProposalTally(proposalHash, coinYeas, coinNays));
    BOOST_CHECK_EQUAL(coinYeas, 0);
    BOOST_CHECK_EQUAL(coinNays, 0);

    CBlock castBlock;
    castBlock.vtx.emplace_back(MakeTransactionRef(castTx));

    CBlockIndex castIndex;
    castIndex.nHeight = GovActivationHeight() + 1;
    castIndex.pprev = &lockIndex;

    BOOST_CHECK(governanceVoteIndex->ProcessBlock(castBlock, &castIndex, state, false));
    BOOST_CHECK(governanceVoteIndex->IsLockUsedForProposal(lockRef, proposalHash));
    BOOST_CHECK(governanceVoteIndex->GetProposalTally(proposalHash, coinYeas, coinNays));
    BOOST_CHECK_EQUAL(coinYeas, amount / COIN);
    BOOST_CHECK_EQUAL(coinNays, 0);
    int64_t managerCoinYeas = 0;
    int64_t managerCoinNays = 0;
    BOOST_CHECK(g_budgetman.GetCoinVoteTotals(proposalHash, managerCoinYeas, managerCoinNays));
    BOOST_CHECK_EQUAL(managerCoinYeas, amount / COIN);
    BOOST_CHECK_EQUAL(managerCoinNays, 0);

    BOOST_CHECK(governanceVoteIndex->UndoBlock(castBlock, &castIndex));
    BOOST_CHECK(!governanceVoteIndex->IsLockUsedForProposal(lockRef, proposalHash));
    BOOST_CHECK(!governanceVoteIndex->GetProposalTally(proposalHash, coinYeas, coinNays));
    BOOST_CHECK_EQUAL(coinYeas, 0);
    BOOST_CHECK_EQUAL(coinNays, 0);
    BOOST_CHECK(!g_budgetman.GetCoinVoteTotals(proposalHash, managerCoinYeas, managerCoinNays));
    BOOST_CHECK_EQUAL(managerCoinYeas, 0);
    BOOST_CHECK_EQUAL(managerCoinNays, 0);

    BOOST_CHECK(governanceVoteIndex->UndoBlock(lockBlock, &lockIndex));
    BOOST_CHECK(!governanceVoteIndex->GetLockRecord(lockRef, lockRecord));
}

BOOST_AUTO_TEST_CASE(gov_voteindex_rejects_expired_lock_on_cast)
{
    BOOST_REQUIRE(governanceVoteIndex);

    const CKey ownerKey = GetRandomKey();
    const CKeyID ownerKeyId = ownerKey.GetPubKey().GetID();
    const uint256 proposalHash = GetRandHash();

    const int govHeight = GovActivationHeight();
    const CTransaction lockTx(BuildGovVoteLockTx(proposalHash, ownerKeyId, 5 * COIN, govHeight + 1));
    const COutPoint lockRef(lockTx.GetHash(), 0);
    const CTransaction castTx(BuildGovVoteCastTx(proposalHash, {lockRef}, ownerKey));

    CBlock lockBlock;
    lockBlock.vtx.emplace_back(MakeTransactionRef(lockTx));
    CBlockIndex lockIndex;
    lockIndex.nHeight = govHeight;
    CValidationState stateLock;
    BOOST_CHECK(governanceVoteIndex->ProcessBlock(lockBlock, &lockIndex, stateLock, false));

    CBlock castBlock;
    castBlock.vtx.emplace_back(MakeTransactionRef(castTx));
    CBlockIndex castIndex;
    castIndex.nHeight = govHeight + 1;
    castIndex.pprev = &lockIndex;
    CValidationState stateCast;
    BOOST_CHECK(!governanceVoteIndex->ProcessBlock(castBlock, &castIndex, stateCast, false));
    BOOST_CHECK_EQUAL(stateCast.GetRejectReason(), "bad-govtx-lock-expired");
}

BOOST_AUTO_TEST_CASE(gov_voteindex_rejects_inconsistent_lock_scope_record)
{
    BOOST_REQUIRE(governanceVoteIndex);
    BOOST_REQUIRE(evoDb);

    const CKey ownerKey = GetRandomKey();
    const CKeyID ownerKeyId = ownerKey.GetPubKey().GetID();
    const uint256 proposalHash = GetRandHash();
    const uint256 foreignProposalHash = GetRandHash();
    const COutPoint lockRef(GetRandHash(), 7);

    CGovVoteLockRecord lockRecord;
    lockRecord.proposalHash = proposalHash;
    lockRecord.lockAmount = 7 * COIN;
    lockRecord.ownerKeyId = ownerKeyId;
    lockRecord.unlockHeight = 50000;
    lockRecord.createdHeight = GovActivationHeight();
    lockRecord.proposalVoteDirections[foreignProposalHash] = CGovVoteCastTx::VOTE_YES;
    evoDb->Write(std::make_pair(EVODB_GOV_LOCK, lockRef), lockRecord);

    const CTransaction castTx(BuildGovVoteCastTx(proposalHash, {lockRef}, ownerKey));
    CValidationState state;
    BOOST_CHECK(!governanceVoteIndex->ApplyCastTx(castTx, GovActivationHeight() + 1, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-lock-scope");

    evoDb->Erase(std::make_pair(EVODB_GOV_LOCK, lockRef));
}

BOOST_AUTO_TEST_SUITE_END()
