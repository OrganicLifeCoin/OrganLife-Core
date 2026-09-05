// Copyright (c) 2018-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test_organiclife.h"

#include "budget/budgetmanager.h"
#include "evo/deterministicmns.h"
#include "masternode-payments.h"
#include "spork.h"
#include "test/util/blocksutil.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "tinyformat.h"
#include "utilmoneystr.h"
#include "validation.h"

#include <array>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(budget_tests)

void CheckBudgetValue(int nHeight, std::string strNetwork, CAmount nExpectedValue)
{
    CBudgetManager budget;
    CAmount nBudget = g_budgetman.GetTotalBudget(nHeight);
    std::string strError = strprintf("Budget is not as expected for %s. Result: %s, Expected: %s", strNetwork, FormatMoney(nBudget), FormatMoney(nExpectedValue));
    BOOST_CHECK_MESSAGE(nBudget == nExpectedValue, strError);
}

void enableMnSyncAndSuperblocksPayment()
{
    // force mnsync complete
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);

    // enable SPORK_13
    int64_t nTime = GetTime() - 10;
    CSporkMessage spork(SPORK_13_ENABLE_SUPERBLOCKS, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS));

    spork = CSporkMessage(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT));
}

BOOST_AUTO_TEST_CASE(masternode_value)
{
    SelectParams(CBaseChainParams::REGTEST);
    const int posActivationHeight = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight;
    const CAmount expectedAtPos = std::min(Params().GetConsensus().nNewMNBlockReward, GetBlockValue(posActivationHeight));
    // Regtest enables masternode payments from height 1 to allow unit-testing
    // the legacy winner system without requiring a full PoS staking setup.
    const CAmount expectedPrePos = std::min(Params().GetConsensus().nNewMNBlockReward, GetBlockValue(1));
    BOOST_CHECK_EQUAL(GetMasternodePayment(1), expectedPrePos);
    BOOST_CHECK_EQUAL(GetMasternodePayment(posActivationHeight), expectedAtPos);
}

BOOST_AUTO_TEST_CASE(budget_value)
{
    // Governance keeps 14-day cycles, so the 55,555 monthly treasury target is split across two cycles.
    static constexpr CAmount expectedBudget = (55555 * COIN) / 2;

    // Treasury is zero during the PoW bootstrap and switches to the fixed cycle budget at PoS activation.
    SelectParams(CBaseChainParams::TESTNET);
    int nHeightTest = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight;
    CheckBudgetValue(nHeightTest - 1, "testnet pre-pos", 0); // no treasury during PoW bootstrap
    CheckBudgetValue(nHeightTest, "testnet pos", expectedBudget);
    CheckBudgetValue(nHeightTest + 1, "testnet post-pos", expectedBudget);

    SelectParams(CBaseChainParams::MAIN);
    nHeightTest = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight;
    CheckBudgetValue(nHeightTest - 1, "mainnet pre-pos", 0); // no treasury during PoW bootstrap
    CheckBudgetValue(nHeightTest, "mainnet pos", expectedBudget);
    CheckBudgetValue(nHeightTest + 1, "mainnet post-pos", expectedBudget);

    SelectParams(CBaseChainParams::REGTEST);
    nHeightTest = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight;
    CheckBudgetValue(nHeightTest - 1, "regtest pre-pos", 0); // no treasury during PoW bootstrap
    CheckBudgetValue(nHeightTest, "regtest pos", expectedBudget);
    CheckBudgetValue(nHeightTest + 1, "regtest post-pos", expectedBudget);
}

BOOST_AUTO_TEST_CASE(governance_cycle_realtime_equivalence)
{
    static constexpr int64_t expectedCycleSeconds = 14 * 24 * 60 * 60;

    SelectParams(CBaseChainParams::MAIN);
    const auto& mainConsensus = Params().GetConsensus();
    BOOST_CHECK_EQUAL(mainConsensus.nBudgetCycleBlocks * mainConsensus.nTargetSpacing, expectedCycleSeconds);
    BOOST_CHECK_EQUAL(mainConsensus.nMaxProposalPayments, 26);

    SelectParams(CBaseChainParams::TESTNET);
    const auto& testConsensus = Params().GetConsensus();
    BOOST_CHECK_EQUAL(testConsensus.nBudgetCycleBlocks * testConsensus.nTargetSpacing, expectedCycleSeconds);
    BOOST_CHECK_EQUAL(testConsensus.nMaxProposalPayments, 26);
}

BOOST_FIXTURE_TEST_CASE(block_value, TestnetSetup)
{
    const int nHeight = 100;
    const CAmount nBlockReward = GetBlockValue(nHeight);
    CAmount nExpectedRet = nBlockReward;
    CAmount nBudgetAmtRet = 0;

    // PQ-only testnet requires exact deterministic issuance.
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward - 1, nBudgetAmtRet));
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+1, nBudgetAmtRet));
}

BOOST_FIXTURE_TEST_CASE(block_value_undermint, RegTestingSetup)
{
    int nHeight = 100;
    CAmount nExpectedRet = GetBlockValue(nHeight);
    CAmount nBudgetAmtRet = 0;
    // PQ-only consensus rejects under-minting independently of legacy upgrades.
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, -1, nBudgetAmtRet));
}

BOOST_FIXTURE_TEST_CASE(block_value_exact_mint_after_v6_1_mainnet, TestingSetup)
{
    enableMnSyncAndSuperblocksPayment();
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_1_GOV, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    // Pin the deterministic-masternode activation above the heights tested
    // here (with v6 at genesis, block-value checks would require the on-chain
    // DMN list) and PoS below them (so the superblock math uses the fixed
    // cycle budget).
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, 1000);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, 50);


    const int nHeight = 100;
    CAmount nExpectedRet = GetBlockValue(nHeight);
    CAmount nBudgetAmtRet = 0;

    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nExpectedRet, nBudgetAmtRet));

    nExpectedRet = GetBlockValue(nHeight);
    nBudgetAmtRet = 0;
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nExpectedRet - 1, nBudgetAmtRet));
}

BOOST_FIXTURE_TEST_CASE(block_value_accepts_mainnet_budget_window_during_initial_sync, TestingSetup)
{
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_INITIAL);

    const int nHeight = 10115;
    const CAmount nBlockReward = GetBlockValue(nHeight);
    CAmount nExpectedRet = nBlockReward;
    CAmount nBudgetAmtRet = 0;

    BOOST_REQUIRE(Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V6_1_GOV));
    BOOST_REQUIRE(!g_tiertwo_sync_state.IsSynced());
    BOOST_REQUIRE(nHeight % Params().GetConsensus().nBudgetCycleBlocks < 100);

    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward + Params().GetConsensus().nBudgetCycleAmount);

    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward - 1, nBudgetAmtRet));

    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward + Params().GetConsensus().nBudgetCycleAmount + 1, nBudgetAmtRet));
}

BOOST_FIXTURE_TEST_CASE(block_value_rejects_testnet_overmint_during_initial_sync, TestnetSetup)
{
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_INITIAL);

    const auto& consensus = Params().GetConsensus();
    const std::array<int, 2> heights{{1, consensus.nBudgetCycleBlocks + 1}};
    for (const int nHeight : heights) {
        const CAmount nBlockReward = GetBlockValue(nHeight);
        const CAmount maxMint = nBlockReward + g_budgetman.GetTotalBudget(nHeight);
        CAmount nExpectedRet = nBlockReward;
        CAmount nBudgetAmtRet = 0;

        BOOST_REQUIRE(!g_tiertwo_sync_state.IsSynced());
        BOOST_REQUIRE(nHeight % consensus.nBudgetCycleBlocks < 100);
        BOOST_CHECK_MESSAGE(!IsBlockValueValid(nHeight, nExpectedRet, maxMint + 1, nBudgetAmtRet),
                            "unsynced testnet accepted an overminting block at height " << nHeight);
    }
}

BOOST_FIXTURE_TEST_CASE(block_value_never_crosses_hard_supply_cap, TestnetSetup)
{
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_INITIAL);

    const auto& consensus = Params().GetConsensus();
    const int nHeight = consensus.nBudgetCycleBlocks + 1;
    const CAmount chainMinted = consensus.nMaxMoneyOut - 5 * COIN;
    CAmount nExpectedRet = GetBlockValue(nHeight, chainMinted);
    CAmount nBudgetAmtRet = 0;

    BOOST_REQUIRE_EQUAL(nExpectedRet, 5 * COIN);
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, 5 * COIN, nBudgetAmtRet, chainMinted));

    nExpectedRet = GetBlockValue(nHeight, chainMinted);
    nBudgetAmtRet = 0;
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, 5 * COIN + 1, nBudgetAmtRet, chainMinted));
    BOOST_CHECK_EQUAL(nExpectedRet, 5 * COIN);
}

BOOST_FIXTURE_TEST_CASE(block_value_rejects_undermint_on_pq_testnet, TestnetSetup)
{
    enableMnSyncAndSuperblocksPayment();
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_1_GOV, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    // Pin the deterministic-masternode activation above the heights tested
    // here (with v6 at genesis, block-value checks would require the on-chain
    // DMN list) and PoS below them (so the superblock math uses the fixed
    // cycle budget).
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, 1000);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, 50);


    const int nHeight = 100;
    CAmount nExpectedRet = GetBlockValue(nHeight);
    CAmount nBudgetAmtRet = 0;

    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nExpectedRet - 1, nBudgetAmtRet));
}

static CScript GetRandomP2PKH()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(key.GetPubKey().GetID());
}

static CMutableTransaction NewCoinBase(int nHeight, CAmount cbaseAmt, const CScript& cbaseScript)
{
    CMutableTransaction tx;
    tx.vout.emplace_back(cbaseAmt, cbaseScript);
    tx.vin.emplace_back();
    tx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    return tx;
}

BOOST_FIXTURE_TEST_CASE(IsCoinbaseValueValid_test, TestingSetup)
{
    // IsCoinbaseValueValid determines the MN payment from the height of the
    // block built on top of pindexPrev (the chain tip, passed below).
    // In test setup, the chain tip is genesis (height 0), where MN payment is 0.
    // Use the tip height + 1 to match the value IsCoinbaseValueValid will expect.
    const int nBestHeight = WITH_LOCK(cs_main, return chainActive.Height() + 1;);
    const CAmount mnAmt = GetMasternodePayment(nBestHeight);
    const CScript& cbaseScript = GetRandomP2PKH();
    CValidationState state;

    // force mnsync complete
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);

    // -- Regular blocks
    // Note: During PoW phase (height < PoS activation), mnAmt is 0.
    // The validation logic allows any amount <= mnAmt when spork8 is disabled,
    // and requires exact mnAmt when spork8 is enabled (but 0 == 0 always passes).

    // Exact (mnAmt, which is 0 in PoW phase)
    CMutableTransaction cbase = NewCoinBase(1, mnAmt, cbaseScript);
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));

    // If mnAmt is 0, skip underpay/overpay tests that require positive amounts
    if (mnAmt > 0) {
        cbase.vout[0].nValue /= 2;
        cbase.vout.emplace_back(cbase.vout[0]);
        BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));

        // Underpaying with SPORK_8 disabled (good)
        cbase.vout.clear();
        cbase.vout.emplace_back(mnAmt - 1, cbaseScript);
        BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
        cbase.vout[0].nValue = mnAmt/2;
        cbase.vout.emplace_back(cbase.vout[0]);
        cbase.vout[1].nValue = mnAmt/2 - 1;
        BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));

        // Overpaying with SPORK_8 disabled
        cbase.vout.clear();
        cbase.vout.emplace_back(mnAmt + 1, cbaseScript);
        BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt-spork8-disabled");
        state = CValidationState();
        cbase.vout[0].nValue = mnAmt/2;
        cbase.vout.emplace_back(cbase.vout[0]);
        cbase.vout[1].nValue = mnAmt/2 + 1;
        BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt-spork8-disabled");
        state = CValidationState();

        // enable SPORK_8
        int64_t nTime = GetTime() - 10;
        const CSporkMessage& spork = CSporkMessage(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT, nTime + 1, nTime);
        sporkManager.AddOrUpdateSporkMessage(spork);
        BOOST_CHECK(sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT));

        // Underpaying with SPORK_8 enabled
        cbase.vout.clear();
        cbase.vout.emplace_back(mnAmt - 1, cbaseScript);
        BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");
        state = CValidationState();
        cbase.vout[0].nValue = mnAmt/2;
        cbase.vout.emplace_back(cbase.vout[0]);
        cbase.vout[1].nValue = mnAmt/2 - 1;
        BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");
        state = CValidationState();

        // Overpaying with SPORK_8 enabled
        cbase.vout.clear();
        cbase.vout.emplace_back(mnAmt + 1, cbaseScript);
        BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");
        state = CValidationState();
        cbase.vout[0].nValue = mnAmt/2;
        cbase.vout.emplace_back(cbase.vout[0]);
        cbase.vout[1].nValue = mnAmt/2 + 1;
        BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");
        state = CValidationState();
    } else {
        // PoW phase: MN expected payment is 0, but no masternode payee exists.
        // IsCoinbaseValueValid requires coinbase to be exactly 0 when there is no payee.
        cbase.vout.clear();
        cbase.vout.emplace_back(1, cbaseScript);
        BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");
        state = CValidationState();
    }

    const CAmount budgAmt = 200 * COIN;

    // -- Superblocks

    // Exact
    cbase.vout.clear();
    cbase.vout.emplace_back(budgAmt, cbaseScript);
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state, chainActive.Tip()));
    cbase.vout[0].nValue /= 2;
    cbase.vout.emplace_back(cbase.vout[0]);
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state, chainActive.Tip()));

    // Underpaying
    cbase.vout.clear();
    cbase.vout.emplace_back(budgAmt - 1, cbaseScript);
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state, chainActive.Tip()));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-superblock-cb-amt");
    state = CValidationState();
    cbase.vout[0].nValue = budgAmt/2;
    cbase.vout.emplace_back(cbase.vout[0]);
    cbase.vout[1].nValue = budgAmt/2 - 1;
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state, chainActive.Tip()));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-superblock-cb-amt");
    state = CValidationState();

    // Overpaying
    cbase.vout.clear();
    cbase.vout.emplace_back(budgAmt + 1, cbaseScript);
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state, chainActive.Tip()));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-superblock-cb-amt");
    state = CValidationState();
    cbase.vout[0].nValue = budgAmt/2;
    cbase.vout.emplace_back(cbase.vout[0]);
    cbase.vout[1].nValue = budgAmt/2 + 1;
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), budgAmt, state, chainActive.Tip()));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-superblock-cb-amt");
}

// On regtest the masternode payment is nonzero from height 1, so the
// coinbase checks below are meaningful (on mainnet params PoS is not yet
// active at height 1 and GetMasternodePayment returns 0).
BOOST_FIXTURE_TEST_CASE(coinbase_value_deferred_when_tiertwo_unsynced, RegTestingSetup)
{
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_INITIAL);

    const CScript& cbaseScript = GetRandomP2PKH();
    const CAmount mnAmt = GetMasternodePayment(chainActive.Tip()->nHeight + 1);
    BOOST_CHECK(mnAmt > 0);   // sanity: the checks below rely on a nonzero MN payment
    CMutableTransaction cbase = NewCoinBase(1, mnAmt, cbaseScript);
    CValidationState state;

    // Unsynced: the expected total payment is accepted (deferred check).
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));

    // Unsynced with no payee determinable: an empty coinbase is valid too
    // (post-v6 no-payee blocks are allowed so the chain can advance).
    cbase.vout[0].nValue = 0;
    state = CValidationState();
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));

    // Unsynced with a wrong partial payment: rejected.
    cbase.vout[0].nValue = mnAmt / 2;
    state = CValidationState();
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");

    // Synced, no masternodes registered: the coinbase must be empty.
    cbase.vout[0].nValue = mnAmt;
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);
    state = CValidationState();
    BOOST_CHECK(!IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cb-amt");

    cbase.vout[0].nValue = 0;
    state = CValidationState();
    BOOST_CHECK(IsCoinbaseValueValid(MakeTransactionRef(cbase), 0, state, chainActive.Tip()));
}

BOOST_FIXTURE_TEST_CASE(testnet_staking_without_masternodes, TestnetSetup)
{
    BOOST_CHECK(!CanBuildRequiredMasternodePayment(nullptr));
    BOOST_CHECK(CanBuildRequiredMasternodePayment(chainActive.Tip()));
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, 1);
    BOOST_REQUIRE(GetMasternodePayment(1) > 0);
    std::vector<CTxOut> payments;
    BOOST_REQUIRE(masternodePayments.GetMasternodeTxOuts(chainActive.Tip(), payments));
    BOOST_REQUIRE(payments.empty());

    // PQ-only testnet deliberately starts without tier-two. Its background
    // staker must still be able to build the first PoS block.
    auto savedManager = std::move(deterministicMNManager);
    payments.clear();
    const bool canBuildWithoutManager = CanBuildRequiredMasternodePayment(chainActive.Tip());
    const bool gotPaymentsWithoutManager = masternodePayments.GetMasternodeTxOuts(chainActive.Tip(), payments);
    deterministicMNManager = std::move(savedManager);
    BOOST_CHECK(canBuildWithoutManager);
    BOOST_CHECK(gotPaymentsWithoutManager);
    BOOST_CHECK(payments.empty());

    // A no-payee coinbase is already valid; background staking must be able
    // to advance the testnet before its first masternode is registered.
    CMutableTransaction coinbase = NewCoinBase(1, 0, GetRandomP2PKH());
    CValidationState state;
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);
    BOOST_REQUIRE(IsCoinbaseValueValid(MakeTransactionRef(coinbase), 0, state, chainActive.Tip()));
    BOOST_CHECK(CanBuildRequiredMasternodePayment(chainActive.Tip()));
}

BOOST_FIXTURE_TEST_CASE(mainnet_staking_without_masternodes_unchanged, TestingSetup)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, 1);
    BOOST_REQUIRE(GetMasternodePayment(1) > 0);
    std::vector<CTxOut> payments;
    BOOST_REQUIRE(masternodePayments.GetMasternodeTxOuts(chainActive.Tip(), payments));
    BOOST_REQUIRE(payments.empty());
    BOOST_CHECK(!CanBuildRequiredMasternodePayment(chainActive.Tip()));
}

BOOST_AUTO_TEST_SUITE_END()
