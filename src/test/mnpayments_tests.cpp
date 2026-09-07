// Copyright (c) 2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "bls/bls_wrapper.h"
#include "blockassembler.h"
#include "consensus/upgrades.h"
#include "evo/deterministicmns.h"
#include "evo/evodb.h"
#include "masternode-payments.h"
#include "netbase.h"
#include "pqtransaction.h"
#include "spork.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "util/blockstatecatcher.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>

BOOST_AUTO_TEST_SUITE(mnpayments_tests)

static bool HasPayeeOutput(const CTransactionRef& coinbaseTx, const CScript& payee)
{
    return std::any_of(coinbaseTx->vout.begin(), coinbaseTx->vout.end(),
                       [&](const CTxOut& out) { return out.scriptPubKey == payee; });
}

static CKey GetRandomKey()
{
    CKey key;
    key.MakeNewKey(true);
    return key;
}

static CBLSSecretKey GetRandomBLSKey()
{
    CBLSSecretKey key;
    key.MakeNewKey();
    return key;
}

static CScript GenerateRandomAddress()
{
    return GetScriptForDestination(GetRandomKey().GetPubKey().GetID());
}

static void SetPaymentSettings(bool synced, bool enforce)
{
    g_tiertwo_sync_state.SetCurrentSyncPhase(synced ? MASTERNODE_SYNC_FINISHED : MASTERNODE_SYNC_INITIAL);
    const int64_t now = GetTime() - 10;
    sporkManager.AddOrUpdateSporkMessage(CSporkMessage(
        SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT, enforce ? now + 1 : 4070908800LL, now));
    BOOST_CHECK_EQUAL(sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT), enforce);
}

struct PQPaymentSetup : TestChainSetup { PQPaymentSetup() : TestChainSetup(0) {} };

BOOST_FIXTURE_TEST_CASE(pq_ignores_stale_legacy_dmn_payments, PQPaymentSetup)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 0);

    const CScript minerScript = pq::GetScript(pq::KeyID{});
    CreateAndProcessBlock({}, minerScript);
    BOOST_REQUIRE_EQUAL(chainActive.Height(), 1);

    CBlockIndex* tip = WITH_LOCK(cs_main, return chainActive.Tip(););
    const uint256 tipHash = tip->GetBlockHash();
    const CScript stalePayee = GenerateRandomAddress();

    auto state = std::make_shared<CDeterministicMNState>();
    state->nRegisteredHeight = 1;
    state->keyIDOwner = GetRandomKey().GetPubKey().GetID();
    state->pubKeyOperator.Set(GetRandomBLSKey().GetPublicKey());
    state->keyIDVoting = state->keyIDOwner;
    state->addr = LookupNumeric("1.1.1.1", 1001);
    state->scriptPayout = stalePayee;

    auto dmn = std::make_shared<CDeterministicMN>(1);
    dmn->proTxHash = uint256S("01");
    dmn->collateralOutpoint = COutPoint(uint256S("02"), 0);
    dmn->nOperatorReward = 0;
    dmn->pdmnState = state;

    CDeterministicMNList staleList(tipHash, tip->nHeight, 1);
    staleList.AddMN(dmn);
    SyncWithValidationInterfaceQueue();
    {
        LOCK(cs_main);
        auto transaction = evoDb->BeginTransaction();
        evoDb->Write(std::make_pair(std::string("dmn_S"), tipHash), staleList);
        transaction->Commit();
        BOOST_REQUIRE(evoDb->CommitRootTransaction());
        deterministicMNManager = std::make_unique<CDeterministicMNManager>(*evoDb);
        deterministicMNManager->SetTipIndex(tip);
    }

    const auto loaded = deterministicMNManager->GetListAtChainTip();
    BOOST_REQUIRE_EQUAL(loaded.GetAllMNsCount(), 1U);
    const auto loadedMN = loaded.GetMNPayee();
    BOOST_REQUIRE(loadedMN);
    BOOST_CHECK(loadedMN->pdmnState->scriptPayout == stalePayee);

    std::vector<CTxOut> legacyPayments;
    BOOST_REQUIRE(masternodePayments.GetMasternodeTxOuts(tip, legacyPayments));
    BOOST_REQUIRE_EQUAL(legacyPayments.size(), 1U);
    BOOST_CHECK(legacyPayments[0].scriptPubKey == stalePayee);
    BOOST_CHECK_GT(legacyPayments[0].nValue, 0);

    unsigned variant = 0;
    for (const bool synced : {false, true}) {
        for (const bool enforce : {false, true}) {
            SetPaymentSettings(synced, enforce);
            const CBlock block = CreateBlock({}, minerScript);
            BOOST_REQUIRE_EQUAL(block.vtx.size(), 1U);
            BOOST_CHECK(!HasPayeeOutput(block.vtx[0], stalePayee));
            BOOST_REQUIRE_EQUAL(block.vtx[0]->vout.size(), 1U);
            BOOST_CHECK(block.vtx[0]->vout[0].scriptPubKey == minerScript);
            BOOST_CHECK_EQUAL(block.vtx[0]->vout[0].nValue,
                              GetBlockValue(chainActive.Height() + 1, tip->nChainMinted));

            auto badBlock = std::make_shared<CBlock>(block);
            CMutableTransaction badCoinbase(*badBlock->vtx[0]);
            BOOST_REQUIRE_GT(badCoinbase.vout[0].nValue, COIN);
            badCoinbase.vout[0].nValue -= COIN;
            badCoinbase.vout.emplace_back(COIN, stalePayee);
            badCoinbase.nLockTime = ++variant; // distinct invalid-cache entries
            badBlock->vtx[0] = MakeTransactionRef(badCoinbase);
            BOOST_REQUIRE(SolveBlock(badBlock, tip->nHeight + 1));
            BlockStateCatcherWrapper catcher(badBlock->GetHash());
            catcher.registerEvent();
            BOOST_CHECK(!ProcessNewBlock(badBlock, nullptr));
            BOOST_REQUIRE(catcher.get().found);
            BOOST_CHECK_EQUAL(catcher.get().state.GetRejectReason(), "bad-pq-only-output");
            BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), tipHash);
        }
    }

    SetPaymentSettings(true, true);
    CBlock goodBlock = CreateBlock({}, minerScript);
    BOOST_REQUIRE(ProcessNewBlock(std::make_shared<CBlock>(goodBlock), nullptr));
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), goodBlock.GetHash());
}

BOOST_AUTO_TEST_SUITE_END()
