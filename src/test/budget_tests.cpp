// Copyright (c) 2018-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test_organiclife.h"

#include "bls/bls_wrapper.h"
#include "budget/budgetmanager.h"
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
    enableMnSyncAndSuperblocksPayment();
    // Pin the deterministic-masternode activation above the heights tested
    // here (with v6 at genesis, block-value checks would require the on-chain
    // DMN list) and PoS below them (so the superblock math uses the fixed
    // cycle budget).
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, 1000);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, 50);

    int nHeight = 100; std::string strError;
    const CAmount nBlockReward = GetBlockValue(nHeight);
    CAmount nExpectedRet = nBlockReward;
    CAmount nBudgetAmtRet = 0;

    // regular block
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, 0, nBudgetAmtRet));
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward-1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);

    // superblock - create the finalized budget with a proposal, and vote on it
    nHeight = 144;
    const CTxIn mnVin(GetRandHash(), 0);
    const CScript payee = GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))));
    const CAmount propAmt = 100 * COIN;
    const uint256& propHash = GetRandHash(), finTxId = GetRandHash();
    const CTxBudgetPayment txBudgetPayment(propHash, payee, propAmt);
    CFinalizedBudget fin("main (test)", 144, {txBudgetPayment}, finTxId);
    const CFinalizedBudgetVote fvote(mnVin, fin.GetHash());
    BOOST_CHECK(fin.AddOrUpdateVote(fvote, strError));
    g_budgetman.ForceAddFinalizedBudget(fin.GetHash(), fin.GetFeeTXHash(), fin);

    // check superblock's block-value
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward + propAmt);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, propAmt);
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt-1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward + propAmt);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, propAmt);
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward + propAmt);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, propAmt);
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt+1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward + propAmt);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, propAmt);

    // disable SPORK_13
    const CSporkMessage& spork2 = CSporkMessage(SPORK_13_ENABLE_SUPERBLOCKS, 4070908800ULL, GetTime());
    sporkManager.AddOrUpdateSporkMessage(spork2);
    BOOST_CHECK(!sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS));

    // check with spork disabled
    nExpectedRet = nBlockReward;
    nBudgetAmtRet = 0;
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nBlockReward, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt-1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
    BOOST_CHECK(!IsBlockValueValid(nHeight, nExpectedRet, nBlockReward+propAmt+1, nBudgetAmtRet));
    BOOST_CHECK_EQUAL(nExpectedRet, nBlockReward);
    BOOST_CHECK_EQUAL(nBudgetAmtRet, 0);
}

BOOST_FIXTURE_TEST_CASE(block_value_undermint, RegTestingSetup)
{
    int nHeight = 100;
    CAmount nExpectedRet = GetBlockValue(nHeight);
    CAmount nBudgetAmtRet = 0;
    // under-minting blocks are invalid after v5.3
    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, -1, nBudgetAmtRet));
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V5_3, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
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

BOOST_FIXTURE_TEST_CASE(block_value_allows_undermint_on_testnet_after_v6_1, TestnetSetup)
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

    BOOST_CHECK(IsBlockValueValid(nHeight, nExpectedRet, nExpectedRet - 1, nBudgetAmtRet));
}

/**
 * 1) Create two proposals and two budget finalizations with a different proposal payment order:
         BudA pays propA and propB, BudB pays propB and propA.
   2) Vote both finalization budgets, adding more votes to budA (so it becomes the most voted one).
 */
void forceAddFakeProposals(const CTxOut& payee1, const CTxOut& payee2)
{
    const CTxIn mnVin(GetRandHash(), 0);
    const uint256& propHash = GetRandHash(), finTxId = GetRandHash();
    const CTxBudgetPayment txBudgetPayment(propHash, payee1.scriptPubKey, payee1.nValue);

    const CTxIn mnVin2(GetRandHash(), 0);
    const uint256& propHash2 = GetRandHash(), finTxId2 = GetRandHash();
    const CTxBudgetPayment txBudgetPayment2(propHash2, payee2.scriptPubKey, payee2.nValue);

    // Create first finalization
    CFinalizedBudget fin("main (test)", 144, {txBudgetPayment, txBudgetPayment2}, finTxId);
    const CFinalizedBudgetVote fvote(mnVin, fin.GetHash());
    const CFinalizedBudgetVote fvote1_a({GetRandHash(), 0}, fin.GetHash());
    const CFinalizedBudgetVote fvote1_b({GetRandHash(), 0}, fin.GetHash());
    std::string strError;
    BOOST_CHECK(fin.AddOrUpdateVote(fvote, strError));
    BOOST_CHECK(fin.AddOrUpdateVote(fvote1_a, strError));
    BOOST_CHECK(fin.AddOrUpdateVote(fvote1_b, strError));
    g_budgetman.ForceAddFinalizedBudget(fin.GetHash(), fin.GetFeeTXHash(), fin);

    // Create second finalization
    CFinalizedBudget fin2("main2 (test)", 144, {txBudgetPayment2, txBudgetPayment}, finTxId2);
    const CFinalizedBudgetVote fvote2(mnVin2, fin2.GetHash());
    const CFinalizedBudgetVote fvote2_a({GetRandHash(), 0}, fin2.GetHash());
    BOOST_CHECK(fin2.AddOrUpdateVote(fvote2, strError));
    BOOST_CHECK(fin2.AddOrUpdateVote(fvote2_a, strError));
    g_budgetman.ForceAddFinalizedBudget(fin2.GetHash(), fin2.GetFeeTXHash(), fin2);
}

BOOST_FIXTURE_TEST_CASE(budget_blocks_payee_test, TestChain100Setup)
{
    // Regtest superblock is every 144 blocks.
    for (int i=0; i<43; i++) CreateAndProcessBlock({}, coinbaseKey);
    enableMnSyncAndSuperblocksPayment();
    g_budgetman.Clear();
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height();), 143);
    BOOST_ASSERT(g_budgetman.GetFinalizedBudgets().size() == 0);

    // Now we are at the superblock height, let's add a proposal to pay.
    const CScript payee1 = GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))));
    const CAmount propAmt1 = 100 * COIN;
    const CScript payee2 = GetScriptForDestination(CKeyID(uint160(ParseHex("8d5b4f83212214d6ef693e02e6d71969fddad976"))));
    const CAmount propAmt2 = propAmt1;
    forceAddFakeProposals({propAmt1, payee1}, {propAmt2, payee2});

    CBlock block = CreateBlock({}, coinbaseKey);
    // Check payee validity:
    CTxOut payeeOut = block.vtx[0]->vout[1];
    BOOST_CHECK_EQUAL(payeeOut.nValue, propAmt1);
    BOOST_CHECK(payeeOut.scriptPubKey == payee1);

    // Good tx
    CMutableTransaction goodMtx(*block.vtx[0]);

    // Modify payee
    CMutableTransaction mtx(*block.vtx[0]);
    mtx.vout[1].scriptPubKey = GetScriptForDestination(CKeyID(uint160(ParseHex("8c988f1a4a4de2161e0f50aac7f17e7f9555caa4"))));
    block.vtx[0] = MakeTransactionRef(mtx);
    std::shared_ptr<CBlock> pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    BOOST_CHECK(block.vtx[0]->vout[1].scriptPubKey != payee1);

    // Verify block rejection reason.
    ProcessBlockAndCheckRejectionReason(pblock, "bad-cb-payee", 143);

    // Try to overmint, valid payee --> bad amount.
    mtx = goodMtx; // reset
    mtx.vout[1].nValue *= 2; // invalid amount
    block.vtx[0] = MakeTransactionRef(mtx);
    pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    BOOST_CHECK(block.vtx[0]->vout[1].scriptPubKey == payee1);
    BOOST_CHECK(block.vtx[0]->vout[1].nValue == payeeOut.nValue * 2);
    ProcessBlockAndCheckRejectionReason(pblock, "bad-blk-amount", 143);

    // Try to send less to a valid payee --> bad amount.
    mtx = goodMtx; // reset
    mtx.vout[1].nValue /= 2;
    block.vtx[0] = MakeTransactionRef(mtx);
    pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    BOOST_CHECK(block.vtx[0]->vout[1].scriptPubKey == payee1);
    BOOST_CHECK(block.vtx[0]->vout[1].nValue == payeeOut.nValue / 2);
    ProcessBlockAndCheckRejectionReason(pblock, "bad-cb-payee", 143);

    // Context, this has:
    // 1) Two proposals and two budget finalizations with a different proposal payment order (read `forceAddFakeProposals()` description):
    //      BudA pays propA and propB, BudB pays propB and propA.
    // 2) Voted both budgets, adding more votes to budA (so it becomes the most voted one).
    // 3) Now: in the superblock, pay to budB order (the less voted finalization) --> which will fail.

    // Try to pay proposals in different order
    mtx = goodMtx; // reset
    std::vector<CFinalizedBudget*> vecFin = g_budgetman.GetFinalizedBudgets();
    CFinalizedBudget* secondFin{nullptr};
    for (auto fin : vecFin) {
        if (!secondFin || fin->GetVoteCount() < secondFin->GetVoteCount()) {
            secondFin = fin;
        }
    }
    secondFin->GetPayeeAndAmount(144, mtx.vout[1].scriptPubKey, mtx.vout[1].nValue);
    BOOST_CHECK(mtx.vout[1].scriptPubKey != goodMtx.vout[1].scriptPubKey);
    BOOST_CHECK(mtx.vout[1].nValue == goodMtx.vout[1].nValue);
    block.vtx[0] = MakeTransactionRef(mtx);
    pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    ProcessBlockAndCheckRejectionReason(pblock, "bad-cb-payee", 143);

    // Now create the good block
    block.vtx[0] = MakeTransactionRef(goodMtx);
    pblock = FinalizeBlock(std::make_shared<CBlock>(block));
    ProcessNewBlock(pblock, nullptr);
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash();), pblock->GetHash());
}

BOOST_FIXTURE_TEST_CASE(budget_blocks_reorg_test, TestChain100Setup)
{
    // Regtest superblock is every 144 blocks.
    for (int i=0; i<43; i++) CreateAndProcessBlock({}, coinbaseKey);
    enableMnSyncAndSuperblocksPayment();
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height();), 143);

    // Now we are at the superblock height, let's add a proposal to pay.
    const CScript payee = GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))));
    const CAmount propAmt = 100 * COIN;
    const CScript payee2 = GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))));
    const CAmount propAmt2 = propAmt * 2;
    forceAddFakeProposals({propAmt, payee}, {propAmt2, payee2});

    // This will:
    // 1) Create a proposal to be paid at block 144 (first superblock).
    // 1) create blocksA and blockB at block 144 (paying for the proposal).
    // 2) Process and connect blockA.
    // 3) Create blockC on top of BlockA and blockD on top of blockB. At height 145.
    // 4) Process and connect blockC.
    // 5) Now force the reorg:
    //    a) Process blockB and blockD.
    //    b) Create and process blockE on top of blockD.
    // 6) Verify that tip is at blockE.

    CScript forkCoinbaseScript = GetScriptForDestination(CKeyID(uint160(ParseHex("8c988f1a4a4de2161e0f50aac7f17e7f9555caa4"))));
    CBlock blockA = CreateBlock({}, coinbaseKey, false);
    CBlock blockB = CreateBlock({}, forkCoinbaseScript, false);
    BOOST_CHECK(blockA.GetHash() != blockB.GetHash());
    // Check blocks payee validity:
    CTxOut payeeOut = blockA.vtx[0]->vout[1];
    BOOST_CHECK_EQUAL(payeeOut.nValue, propAmt);
    BOOST_CHECK(payeeOut.scriptPubKey == payee);
    payeeOut = blockB.vtx[0]->vout[1];
    BOOST_CHECK_EQUAL(payeeOut.nValue, propAmt);
    BOOST_CHECK(payeeOut.scriptPubKey == payee);

    // Now let's process BlockA:
    auto pblockA = std::make_shared<const CBlock>(blockA);
    ProcessNewBlock(pblockA, nullptr);
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()) == blockA.GetHash());

    // Now let's create blockC on top of BlockA, blockD on top of blockB
    // and process blockC to expand the chain.
    CBlock blockC = CreateBlock({}, coinbaseKey, false);
    BOOST_CHECK(blockC.hashPrevBlock == blockA.GetHash());
    CBlock blockD = CreateBlock({}, forkCoinbaseScript, false);

    // Process and connect blockC
    ProcessNewBlock(std::make_shared<const CBlock>(blockC), nullptr);
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()) == blockC.GetHash());

    // Now let's process the secondary chain
    blockD.hashPrevBlock = blockB.GetHash();
    std::shared_ptr<CBlock> pblockD = FinalizeBlock(std::make_shared<CBlock>(blockD));

    ProcessNewBlock(std::make_shared<const CBlock>(blockB), nullptr);
    ProcessNewBlock(pblockD, nullptr);
    CBlock blockE = CreateBlock({}, forkCoinbaseScript, false);
    blockE.hashPrevBlock = pblockD->GetHash();
    std::shared_ptr<CBlock> pblockE = FinalizeBlock(std::make_shared<CBlock>(blockE));
    ProcessNewBlock(pblockE, nullptr);
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()) == pblockE->GetHash());
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

BOOST_AUTO_TEST_CASE(fbv_signverify_bls)
{
    CBLSSecretKey sk1, sk2;
    sk1.MakeNewKey();
    sk2.MakeNewKey();
    BOOST_ASSERT(sk1 != sk2);

    CTxIn vin(COutPoint(uint256S("0000000000000000000000000000000000000000000000000000000000000002"), 0));
    CTxIn vin2(COutPoint(uint256S("000000000000000000000000000000000000000000000000000000000000003"), 0));
    CTxIn vin3(COutPoint(uint256S("0000000000000000000000000000000000000000000000000000000000000002"), 1));

    uint256 budgetHash1 = uint256S("0000000000000000000000000000000000000000000000000000000000000001");
    uint256 budgetHash2 = uint256S("0000000000000000000000000000000000010000000000000000000000000001");

    // Create serialized finalbudgetvote for budgetHash1, signed with sk1
    CFinalizedBudgetVote vote(vin, budgetHash1);
    BOOST_CHECK(vote.Sign(sk1));
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << vote;

    // Verify received message on pk1
    CFinalizedBudgetVote _vote;
    ss >> _vote;
    BOOST_CHECK(_vote.CheckSignature(sk1.GetPublicKey()));

    // Failing verification on pk2
    BOOST_CHECK(!_vote.CheckSignature(sk2.GetPublicKey()));

    std::vector<unsigned char> sig = _vote.GetVchSig();

    // Failing with different time
    CFinalizedBudgetVote vote1(_vote);
    vote1.SetTime(vote1.GetTime()+1);
    BOOST_CHECK(!vote1.CheckSignature(sk1.GetPublicKey()));

    // Failing with different budget hash
    CFinalizedBudgetVote vote2(vin, budgetHash2);
    vote2.SetTime(_vote.GetTime());
    vote2.SetVchSig(sig);
    BOOST_CHECK(!vote2.CheckSignature(sk1.GetPublicKey()));

    // Failing with different vins: different txid (vin2) or voutn (vin3)
    CFinalizedBudgetVote vote3_1(vin, budgetHash1);
    CFinalizedBudgetVote vote3_2(vin2, budgetHash1);
    CFinalizedBudgetVote vote3_3(vin3, budgetHash1);
    vote3_1.SetTime(_vote.GetTime());
    vote3_2.SetTime(_vote.GetTime());
    vote3_3.SetTime(_vote.GetTime());
    vote3_1.SetVchSig(sig);
    vote3_2.SetVchSig(sig);
    vote3_3.SetVchSig(sig);
    BOOST_CHECK(vote3_1.CheckSignature(sk1.GetPublicKey()));    // vote3_1 == _vote
    BOOST_CHECK(!vote3_2.CheckSignature(sk1.GetPublicKey()));
    BOOST_CHECK(!vote3_3.CheckSignature(sk1.GetPublicKey()));
}

BOOST_AUTO_TEST_SUITE_END()
