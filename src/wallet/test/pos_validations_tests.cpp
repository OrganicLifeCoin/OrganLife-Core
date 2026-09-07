// Copyright (c) 2020-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/pos_test_fixture.h"

#include "blockassembler.h"
#include "chainparams.h"
#include "coincontrol.h"
#include "blocksignature.h"
#include "consensus/merkle.h"
#include "masternode-payments.h"
#include "primitives/block.h"
#include "pqtransaction.h"
#include "test/util/blocksutil.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "timedata.h"
#include "util/blockstatecatcher.h"
#include "util/validation.h"
#include "utiltime.h"
#include "validation.h"
#include "wallet/wallet.h"

#include <algorithm>
#include <boost/test/unit_test.hpp>

bool CheckForCoins(CWallet* pwallet, std::vector<CStakeableOutput>* availableCoins, bool lastResult);

BOOST_AUTO_TEST_SUITE(pos_validations_tests)

BOOST_FIXTURE_TEST_CASE(coinstake_tests, TestPoSChainSetup)
{
    // Verify that we are at block 251
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->nHeight), 250);
    SyncWithValidationInterfaceQueue();

    // Let's create the block
    std::vector<CStakeableOutput> availableCoins;
    BOOST_CHECK(pwalletMain->StakeableCoins(&availableCoins));
    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(
            Params(), false).CreateNewBlock(CScript(),
                                            pwalletMain.get(),
                                            true,
                                            &availableCoins,
                                            true);
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block);
    BOOST_CHECK(pblock->IsProofOfStake());

    // Add a second input to a coinstake
    CMutableTransaction mtx(*pblock->vtx[1]);
    const CStakeableOutput& in2 = availableCoins.back();
    availableCoins.pop_back();
    CTxIn vin2(in2.tx->GetHash(), in2.i);
    mtx.vin.emplace_back(vin2);

    pblock->vtx[1] = MakeTransactionRef(mtx);
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
    BOOST_CHECK(SignBlock(*pblock, *pwalletMain));
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_CHECK(!ContextualCheckBlock(*pblock, state, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-cs-multi-inputs");
    }
    // PQ structure rejects this before signature verification. The contextual
    // rule above remains covered independently; invalid stakes are not signed.
    ProcessBlockAndCheckRejectionReason(pblock, "bad-pq-size", 250);

    // Check multi-empty-outputs now
    pblock = std::make_shared<CBlock>(pblocktemplate->block);
    mtx = CMutableTransaction(*pblock->vtx[1]);
    for (int i = 0; i < 999; ++i) {
        mtx.vout.emplace_back();
        mtx.vout.back().SetEmpty();
    }
    pblock->vtx[1] = MakeTransactionRef(mtx);
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
    BOOST_CHECK(!SignBlock(*pblock, *pwalletMain));
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_CHECK(!ContextualCheckBlock(*pblock, state, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-vout-empty");
    }
    ProcessBlockAndCheckRejectionReason(pblock, "bad-pq-size", 250);

    // Now connect the proper block
    pblock = std::make_shared<CBlock>(pblocktemplate->block);
    ProcessNewBlock(pblock, nullptr);
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), pblock->GetHash());
}

BOOST_FIXTURE_TEST_CASE(check_for_coins_rescans_after_same_tip_restart, TestPoSChainSetup)
{
    std::vector<CStakeableOutput> expectedCoins;
    BOOST_REQUIRE(pwalletMain->StakeableCoins(&expectedCoins));
    BOOST_REQUIRE(!expectedCoins.empty());

    const CBlockIndex* tip = WITH_LOCK(cs_main, return chainActive.Tip());
    BOOST_REQUIRE(tip != nullptr);
    {
        WAIT_LOCK(g_best_block_mutex, lock);
        g_best_block = tip->GetBlockHash();
    }

    pwalletMain->pStakerStatus->SetLastTip(tip);
    pwalletMain->pStakerStatus->SetLastTime(GetAdjustedTime());
    pwalletMain->pStakerStatus->SetLastTries(123);

    std::vector<CStakeableOutput> availableCoins;
    BOOST_CHECK_MESSAGE(CheckForCoins(pwalletMain.get(), &availableCoins, false),
                        "staking should rescan stakeable coins after a miner restart on the same tip");
    BOOST_CHECK_EQUAL(availableCoins.size(), expectedCoins.size());
}

BOOST_FIXTURE_TEST_CASE(get_next_pos_block_time_waits_when_parent_is_ahead, TestPoSChainSetup)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    const int64_t slotLen = consensus.nTimeSlotLength;
    CBlockIndex* tip = WITH_LOCK(cs_main, return chainActive.Tip());

    // Pin "now" a few slots past the tip: the next PoS block time is the
    // current time slot and never beyond the future-drift limit.
    const int64_t baseSlot = GetTimeSlot(tip->GetBlockTime()) + 10 * slotLen;
    SetMockTime(baseSlot);
    BOOST_CHECK_GT(GetCurrentTimeSlot(), tip->GetBlockTime());
    BOOST_CHECK_EQUAL(GetNextPoSBlockTime(tip, true), GetCurrentTimeSlot());

    // Reproduce the production stall: the tip is accepted with a near-future
    // timestamp, right at the future-drift limit. The next valid timestamp
    // (the slot after the parent) would be beyond the limit and rejected with
    // "time-too-new", so the staker must wait (return 0) instead of minting it.
    tip->nTime = tip->MaxFutureBlockTime();
    BOOST_CHECK_EQUAL(GetNextPoSBlockTime(tip, true), 0);

    // Once the clock advances past the parent, a valid timestamp exists again
    // and it never exceeds the future-drift limit.
    SetMockTime(tip->GetBlockTime() + slotLen);
    BOOST_CHECK_NE(GetNextPoSBlockTime(tip, true), 0);
    BOOST_CHECK_LE(GetNextPoSBlockTime(tip, true), tip->MaxFutureBlockTime());

    SetMockTime(0);
}

BOOST_FIXTURE_TEST_CASE(v6_pos_coinbase_without_masternode_payee_is_valid, TestPoSChainSetup)
{
    const int nextHeight = WITH_LOCK(cs_main, return chainActive.Height() + 1;);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, nextHeight);

    g_tiertwo_sync_state.ResetData();
    g_tiertwo_sync_state.SetBlockchainSync(false, 0);
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_INITIAL);

    std::vector<CStakeableOutput> availableCoins;
    BOOST_REQUIRE(pwalletMain->StakeableCoins(&availableCoins));
    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(
            Params(), false).CreateNewBlock(CScript(),
                                            pwalletMain.get(),
                                            true,
                                            &availableCoins,
                                            true);
    BOOST_REQUIRE(pblocktemplate);

    const CBlock& block = pblocktemplate->block;
    BOOST_REQUIRE(block.IsProofOfStake());
    BOOST_REQUIRE(!block.vtx[0]->vout.empty());
    BOOST_CHECK(block.vtx[0]->vout[0].IsEmpty());
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 2U);
    BOOST_CHECK_EQUAL(block.vtx[1]->nType, CTransaction::PQ);

    CValidationState state;
    LOCK(cs_main);
    BOOST_CHECK_MESSAGE(CheckBlock(block, state, true, true, true), FormatStateMessage(state));
}

CTransaction CreateAndCommitTx(CWallet* wallet, const std::string& dest, CAmount destValue, CCoinControl* coinControl = nullptr)
{
    LOCK2(cs_main, wallet->cs_wallet);
    COutPoint input;
    if (coinControl && coinControl->HasSelected()) {
        std::vector<OutPointWrapper> selected;
        coinControl->ListSelected(selected);
        BOOST_REQUIRE_EQUAL(selected.size(), 1U);
        input = COutPoint(selected[0].outPoint.hash, selected[0].outPoint.n);
    } else {
        // Keep coinbase inputs safely mature on the shorter forks, too.
        for (const auto& coin : wallet->GetPQUnspent()) {
            if (coin.nDepth > 120 && coin.Value() >= destValue + CENT) {
                input = COutPoint(coin.tx->GetHash(), coin.i);
                break;
            }
        }
    }
    BOOST_REQUIRE(!input.IsNull());
    const auto* previous = wallet->GetWalletTx(input.hash);
    BOOST_REQUIRE(previous && input.n < previous->tx->vout.size());
    const CTxOut prevout = previous->tx->vout[input.n];
    BOOST_REQUIRE_GE(prevout.nValue, destValue + CENT);
    pq::KeyID recipient, owner;
    BOOST_REQUIRE(pq::DecodeAddress(dest, Params().NetworkIDString(), recipient));
    BOOST_REQUIRE(pq::ExtractID(prevout.scriptPubKey, owner));
    mldsa44::Key key;
    BOOST_REQUIRE(wallet->GetPQKey(owner, key, false));
    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = CTransaction::PQ;
    tx.sapData = nullopt;
    tx.vin.emplace_back(input);
    tx.vout.emplace_back(destValue, pq::GetScript(recipient));
    if (prevout.nValue > destValue + CENT)
        tx.vout.emplace_back(prevout.nValue - destValue - CENT, prevout.scriptPubKey);
    pq::Payload payload;
    payload.authorizations.resize(1);
    payload.authorizations[0].public_key = key.GetPublicKey();
    const auto context = pq::SignatureContext(Params().NetworkIDString());
    BOOST_REQUIRE(context);
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(pq::SignatureMessage(tx, {prevout}, payload,
        Params().GetConsensus().hashGenesisBlock, 0), *context, signature));
    BOOST_REQUIRE_EQUAL(signature.size(), payload.authorizations[0].signature.size());
    std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
    tx.extraPayload = pq::EncodePayload(payload);
    std::string reason;
    BOOST_REQUIRE_MESSAGE(pq::VerifyInputs(tx, {prevout}, Params(), reason), reason);
    // Deliberately construct fork-only and double-spent inputs. The production
    // payment builder correctly refuses those; chain validation is under test.
    const auto result = MakeTransactionRef(tx);
    wallet->CommitTransaction(result, nullptr, nullptr);
    BOOST_REQUIRE(wallet->GetWalletTx(result->GetHash()));
    return *result;
}

COutPoint GetOutpointWithAmount(const CTransaction& tx, CAmount outpointValue)
{
    for (size_t i = 0; i < tx.vout.size(); i++) {
        if (tx.vout[i].nValue == outpointValue) {
            return COutPoint(tx.GetHash(), i);
        }
    }
    BOOST_ASSERT_MSG(false, "error in test, no output in tx for value");
    return {};
}

static bool IsSpentOnFork(const COutput& coin, std::initializer_list<std::shared_ptr<CBlock>> forkchain = {})
{
    const COutPoint outpoint(coin.tx->GetHash(), coin.i);
    for (const auto& block : forkchain) {
        for (const auto& tx : block->vtx)
            for (const auto& input : tx->vin)
                if (input.prevout == outpoint) return true;
    }
    return false;
}

std::shared_ptr<CBlock> CreateBlockInternal(CWallet* pwalletMain, const std::vector<CMutableTransaction>& txns = {},
                                            CBlockIndex* customPrevBlock = nullptr,
                                            std::initializer_list<std::shared_ptr<CBlock>> forkchain = {})
{
    std::vector<CStakeableOutput> availableCoins;
    BOOST_CHECK(pwalletMain->StakeableCoins(&availableCoins));

    // Remove any utxo which is not deeper than 120 blocks (for the same reasoning
    // used when selecting tx inputs in CreateAndCommitTx)
    // Also, as the wallet is not prepared to follow several chains at the same time,
    // need to manually remove from the stakeable utxo set every already used
    // transaction inputs on the previous blocks of the parallel chain so they
    // are not used again.
    for (auto it = availableCoins.begin(); it != availableCoins.end() ;) {
        // These synthetic transactions need not enter the mempool. Reserve their
        // inputs explicitly so the coinstake cannot spend them in the same block.
        const COutPoint outpoint(it->tx->GetHash(), it->i);
        const bool spentInBlock = std::any_of(txns.begin(), txns.end(), [&](const CMutableTransaction& tx) {
            return std::any_of(tx.vin.begin(), tx.vin.end(), [&](const CTxIn& input) {
                return input.prevout == outpoint;
            });
        });
        if (it->nDepth <= 120 || IsSpentOnFork(*it, forkchain) || spentInBlock) {
            it = availableCoins.erase(it);
        } else {
            it++;
        }
    }

    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(
            Params(), false).CreateNewBlock(CScript(),
                                            pwalletMain,
                                            true,
                                            &availableCoins,
                                            true,
                                            false,
                                            customPrevBlock,
                                            false);
    BOOST_ASSERT(pblocktemplate);
    auto pblock = std::make_shared<CBlock>(pblocktemplate->block);
    if (!txns.empty()) {
        for (const auto& tx : txns) {
            pblock->vtx.emplace_back(MakeTransactionRef(tx));
        }
        pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
        assert(SignBlock(*pblock, *pwalletMain));
    }
    return pblock;
}

static COutput GetUnspentCoin(CWallet* pwallet, std::initializer_list<std::shared_ptr<CBlock>> forkchain = {})
{
    const auto availableCoins = pwallet->GetPQUnspent();
    for (const auto& coin : availableCoins) {
        if (coin.nDepth > 120 && !IsSpentOnFork(coin, forkchain)) {
            return coin;
        }
    }
    throw std::runtime_error("Unspent coin not found");
}

BOOST_FIXTURE_TEST_CASE(stake_fixture_excludes_ordinary_fork_spends, TestPoSChainSetup)
{
    const auto coins = pwalletMain->GetPQUnspent();
    BOOST_REQUIRE(!coins.empty());
    const auto& coin = coins.front();
    CMutableTransaction other, spend;
    other.vin.emplace_back(COutPoint());
    spend.vin.emplace_back(coin.tx->GetHash(), coin.i);
    auto block = std::make_shared<CBlock>();
    block->vtx = {MakeTransactionRef(other), MakeTransactionRef(other), MakeTransactionRef(spend)};
    BOOST_CHECK(IsSpentOnFork(coin, {block}));
}

BOOST_FIXTURE_TEST_CASE(created_on_fork_tests, TestPoSChainSetup)
{
    // Keep test transaction amounts small enough to be satisfied by typical regtest coinbase outputs
    // under the current monetary policy (10 coins/block on regtest) and to avoid large-input selection
    // skew (e.g. premine output).
    const CAmount kTxLarge = 9 * COIN;
    const CAmount kTxMid = 8 * COIN;
    const CAmount kTxSmall = 7 * COIN;

    // Let's create few more PoS blocks
    for (int i=0; i<30; i++) {
        std::shared_ptr<CBlock> pblock = CreateBlockInternal(pwalletMain.get());
        BOOST_CHECK(ProcessNewBlock(pblock, nullptr));
    }

    /*
    Chains diagram:
    A -- B -- C -- D -- E -- F -- G -- H -- I
              \
                -- D1 -- E1 -- F1
                     \
                      -- E2 -- F2
              \
                -- D3 -- E3 -- F3
                           \
                            -- F3_1 -- G3 -- H3 -- I3
    */

    // Tests:
    // 1) coins created in D1 and spent in E1. --> should pass
    // 2) coins created in E, being spent in D4 --> should fail.
    // 3) coins created and spent in E2, being double spent in F2. --> should fail
    // 4) coins created in D and spent in E3. --> should fail
    // 5) coins create in D, spent in F and then double spent in F3. --> should fail
    // 6) coins created in G and G3, being spent in H and H3 --> should pass.
    // 7) use coinstake on different chains --> should pass.

    // Let's create block C with a valid cTx
    std::string dest;
    BOOST_REQUIRE(pwalletMain->GeneratePQAddress(dest));
    auto cTx = CreateAndCommitTx(pwalletMain.get(), dest, kTxLarge);
    const auto& cTx_out = GetOutpointWithAmount(cTx, kTxLarge);
    WITH_LOCK(pwalletMain->cs_wallet, pwalletMain->LockCoin(cTx_out));
    std::shared_ptr<CBlock> pblockC = CreateBlockInternal(pwalletMain.get(), {cTx});
    BOOST_CHECK(ProcessNewBlock(pblockC, nullptr));

    // Create block D with a valid dTx
    BOOST_REQUIRE(pwalletMain->GeneratePQAddress(dest));
    auto dTx = CreateAndCommitTx(pwalletMain.get(), dest, kTxLarge);
    auto dTxOutPoint = GetOutpointWithAmount(dTx, kTxLarge);
    WITH_LOCK(pwalletMain->cs_wallet, pwalletMain->LockCoin(dTxOutPoint));
    std::shared_ptr<CBlock> pblockD = CreateBlockInternal(pwalletMain.get(), {dTx});

    // Create D1 forked block that connects a new tx
    BOOST_REQUIRE(pwalletMain->GeneratePQAddress(dest));
    auto d1Tx = CreateAndCommitTx(pwalletMain.get(), dest, kTxMid);
    std::shared_ptr<CBlock> pblockD1 = CreateBlockInternal(pwalletMain.get(), {d1Tx});

    // Process blocks
    ProcessNewBlock(pblockD, nullptr);
    ProcessNewBlock(pblockD1, nullptr);
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash() ==  pblockD->GetHash()));

    // Ensure that the coin does not exist in the main chain
    const Coin& utxo = pcoinsTip->AccessCoin(COutPoint(d1Tx.GetHash(), 0));
    BOOST_CHECK(utxo.out.IsNull());

    // Create valid block E
    auto eTx = CreateAndCommitTx(pwalletMain.get(), dest, kTxMid);
    std::shared_ptr<CBlock> pblockE = CreateBlockInternal(pwalletMain.get(), {eTx});
    BOOST_CHECK(ProcessNewBlock(pblockE, nullptr));

    // #################################################
    // ### 1) -> coins created in D' and spent in E' ###
    // #################################################

    // Create tx spending the previously created tx on the forked chain
    CCoinControl coinControl;
    coinControl.fAllowOtherInputs = false;
    coinControl.Select(COutPoint(d1Tx.GetHash(), 0), d1Tx.vout[0].nValue);
    auto e1Tx = CreateAndCommitTx(pwalletMain.get(), dest, d1Tx.vout[0].nValue - 0.1 * COIN, &coinControl);

    CBlockIndex* pindexPrev = mapBlockIndex.at(pblockD1->GetHash());
    std::shared_ptr<CBlock> pblockE1 = CreateBlockInternal(pwalletMain.get(), {e1Tx}, pindexPrev, {pblockD1});
    {
        BlockStateCatcherWrapper stateCatcherE1(pblockE1->GetHash());
        stateCatcherE1.registerEvent();
        const bool ok = ProcessNewBlock(pblockE1, nullptr);
        BOOST_CHECK_MESSAGE(ok, stateCatcherE1.get().state.GetRejectReason());
    }

    // #################################################################
    // ### 2) coins created in E, being spent in D4 --> should fail. ###
    // #################################################################

    coinControl.UnSelectAll();
    coinControl.Select(GetOutpointWithAmount(eTx, kTxMid), kTxMid);
    coinControl.fAllowOtherInputs = false;
    auto D4_tx1 = CreateAndCommitTx(pwalletMain.get(), dest, kTxSmall, &coinControl);
    std::shared_ptr<CBlock> pblockD4 = CreateBlockInternal(pwalletMain.get(), {D4_tx1}, mapBlockIndex.at(pblockC->GetHash()));
    BOOST_CHECK(!ProcessNewBlock(pblockD4, nullptr));

    // #####################################################################
    // ### 3) -> coins created and spent in E2, being double spent in F2 ###
    // #####################################################################

    // Create block E2 with E2_tx1 and E2_tx2. Where E2_tx2 is spending the outputs of E2_tx1
    CCoinControl coinControlE2;
    coinControlE2.Select(cTx_out, kTxLarge);
    auto E2_tx1 = CreateAndCommitTx(pwalletMain.get(), dest, kTxMid, &coinControlE2);

    coinControl.UnSelectAll();
    coinControl.Select(GetOutpointWithAmount(E2_tx1, kTxMid), kTxMid);
    coinControl.fAllowOtherInputs = false;
    auto E2_tx2 = CreateAndCommitTx(pwalletMain.get(), dest, kTxSmall, &coinControl);

    std::shared_ptr<CBlock> pblockE2 = CreateBlockInternal(pwalletMain.get(), {E2_tx1, E2_tx2},
                                                           pindexPrev, {pblockD1});
    BOOST_CHECK(ProcessNewBlock(pblockE2, nullptr));

    // Create block with F2_tx1 spending E2_tx1 again.
    auto F2_tx1 = CreateAndCommitTx(pwalletMain.get(), dest, kTxSmall, &coinControl);

    pindexPrev = mapBlockIndex.at(pblockE2->GetHash());
    std::shared_ptr<CBlock> pblock5Forked = CreateBlockInternal(pwalletMain.get(), {F2_tx1},
                                                                pindexPrev, {pblockD1, pblockE2});
    BlockStateCatcherWrapper stateCatcher(pblock5Forked->GetHash());
    stateCatcher.registerEvent();
    BOOST_CHECK(!ProcessNewBlock(pblock5Forked, nullptr));
    BOOST_CHECK(stateCatcher.get().found);
    BOOST_CHECK(!stateCatcher.get().state.IsValid());
    BOOST_CHECK_EQUAL(stateCatcher.get().state.GetRejectReason(), "bad-txns-inputs-spent-fork-post-split");

    // #############################################
    // ### 4) coins created in D and spent in E3 ###
    // #############################################

    // First create D3
    pindexPrev = mapBlockIndex.at(pblockC->GetHash());
    std::shared_ptr<CBlock> pblockD3 = CreateBlockInternal(pwalletMain.get(), {}, pindexPrev);
    BOOST_CHECK(ProcessNewBlock(pblockD3, nullptr));

    // Now let's try to spend the coins created in D in E3
    coinControl.UnSelectAll();
    coinControl.Select(dTxOutPoint, kTxLarge);
    coinControl.fAllowOtherInputs = false;
    auto E3_tx1 = CreateAndCommitTx(pwalletMain.get(), dest, kTxMid, &coinControl);

    pindexPrev = mapBlockIndex.at(pblockD3->GetHash());
    std::shared_ptr<CBlock> pblockE3 = CreateBlockInternal(pwalletMain.get(), {E3_tx1}, pindexPrev, {pblockD3});
    stateCatcher.get().clear();
    stateCatcher.get().setBlockHash(pblockE3->GetHash());
    BOOST_CHECK(!ProcessNewBlock(pblockE3, nullptr));
    BOOST_CHECK(stateCatcher.get().found);
    BOOST_CHECK(!stateCatcher.get().state.IsValid());
    BOOST_CHECK_EQUAL(stateCatcher.get().state.GetRejectReason(), "bad-txns-inputs-created-post-split");

    // ####################################################################
    // ### 5) coins create in D, spent in F and then double spent in F3 ###
    // ####################################################################

    // Create valid block F spending the coins created in D
    const auto& F_tx1 = E3_tx1;
    std::shared_ptr<CBlock> pblockF = CreateBlockInternal(pwalletMain.get(), {F_tx1});
    BOOST_CHECK(ProcessNewBlock(pblockF, nullptr));
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash() ==  pblockF->GetHash()));

    // Create valid block E3
    pindexPrev = mapBlockIndex.at(pblockD3->GetHash());
    pblockE3 = CreateBlockInternal(pwalletMain.get(), {}, pindexPrev, {pblockD3});
    BOOST_CHECK(ProcessNewBlock(pblockE3, nullptr));

    // Now double spend F_tx1 in F3
    pindexPrev = mapBlockIndex.at(pblockE3->GetHash());
    std::shared_ptr<CBlock> pblockF3 = CreateBlockInternal(pwalletMain.get(), {F_tx1}, pindexPrev, {pblockD3, pblockE3});
    // Accepted on disk but not connected.
    BOOST_CHECK(ProcessNewBlock(pblockF3, nullptr));
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash();) != pblockF3->GetHash());

    {
        // Trigger a rescan so the wallet cleans up its internal state.
        WalletRescanReserver reserver(pwalletMain.get());
        BOOST_CHECK(reserver.reserve());
        pwalletMain->RescanFromTime(0, reserver, true /* update */);
    }

    // ##############################################################################
    // ### 6) coins created in G and G3, being spent in H and H3 --> should pass. ###
    // ##############################################################################

    // First create new coins in G
    // select an input that is not already spent in D3 or E3 (since we want to spend it also in G3)
    const COutput& input = GetUnspentCoin(pwalletMain.get(), {pblockD3, pblockE3});

    coinControl.UnSelectAll();
    coinControl.Select(COutPoint(input.tx->GetHash(), input.i), input.Value());
    coinControl.fAllowOtherInputs = false;

    BOOST_REQUIRE(pwalletMain->GeneratePQAddress(dest));
    auto gTx = CreateAndCommitTx(pwalletMain.get(), dest, kTxMid, &coinControl);
    auto gOut = GetOutpointWithAmount(gTx, kTxMid);
    std::shared_ptr<CBlock> pblockG = CreateBlockInternal(pwalletMain.get(), {gTx});
    BOOST_CHECK(ProcessNewBlock(pblockG, nullptr));

    // Now create the same coin in G3
    pblockF3 = CreateBlockInternal(pwalletMain.get(), {}, mapBlockIndex.at(pblockE3->GetHash()), {pblockD3, pblockE3});
    BOOST_CHECK(ProcessNewBlock(pblockF3, nullptr));
    auto pblockG3 = CreateBlockInternal(pwalletMain.get(), {gTx}, mapBlockIndex.at(pblockF3->GetHash()), {pblockD3, pblockE3, pblockF3});
    BOOST_CHECK(ProcessNewBlock(pblockG3, nullptr));
    FlushStateToDisk();

    // Now spend the coin in both, H and H3
    coinControl.UnSelectAll();
    coinControl.Select(gOut, kTxMid);
    coinControl.fAllowOtherInputs = false;
    auto hTx = CreateAndCommitTx(pwalletMain.get(), dest, kTxSmall, &coinControl);
    std::shared_ptr<CBlock> pblockH = CreateBlockInternal(pwalletMain.get(), {hTx});
    BOOST_CHECK(ProcessNewBlock(pblockH, nullptr));
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash() ==  pblockH->GetHash()));
    FlushStateToDisk();

    // H3 now..
    std::shared_ptr<CBlock> pblockH3 = CreateBlockInternal(pwalletMain.get(),
                                                           {hTx},
                                                           mapBlockIndex.at(pblockG3->GetHash()),
                                                           {pblockD3, pblockE3, pblockF3, pblockG3});
    BOOST_CHECK(ProcessNewBlock(pblockH3, nullptr));

    // Try to read the forking point manually
    CBlock bl;
    BOOST_CHECK(ReadBlockFromDisk(bl, mapBlockIndex.at(pblockC->GetHash())));

    // Make I3 the tip now.
    std::shared_ptr<CBlock> pblockI3 = CreateBlockInternal(pwalletMain.get(),
                                                           {},
                                                           mapBlockIndex.at(pblockH3->GetHash()),
                                                           {pblockD3, pblockE3, pblockF3, pblockG3, pblockH3});
    BOOST_CHECK(ProcessNewBlock(pblockI3, nullptr));
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash() ==  pblockI3->GetHash()));

    // And rescan the wallet on top of the new chain
    WalletRescanReserver reserver(pwalletMain.get());
    BOOST_CHECK(reserver.reserve());
    pwalletMain->RescanFromTime(0, reserver, true /* update */);

    // #################################################################################
    // ### 7) Now try to use the same coinstake on different chains --> should pass. ###
    // #################################################################################

    // Take I3 coinstake and use it for block I, changing its hash adding a new tx
    std::shared_ptr<CBlock> pblockI = std::make_shared<CBlock>(*pblockI3);
    auto iTx = CreateAndCommitTx(pwalletMain.get(), dest, 1 * COIN);
    pblockI->vtx.emplace_back(MakeTransactionRef(iTx));
    pblockI->hashMerkleRoot = BlockMerkleRoot(*pblockI);
    assert(SignBlock(*pblockI, *pwalletMain));
    BOOST_CHECK(pblockI3->GetHash() != pblockI->GetHash());
    BOOST_CHECK(ProcessNewBlock(pblockI, nullptr));
}

BOOST_AUTO_TEST_SUITE_END()
