// Copyright (c) 2011-2014 The Bitcoin Core developers
// Copyright (c) 2016-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/wallet_test_fixture.h"

#include "blockassembler.h"
#include "checkpoints.h"
#include "consensus/upgrades.h"
#include "consensus/merkle.h"
#include "evo/governancevotetx.h"
#include "messagesigner.h"
#include "miner.h"
#include "pow.h"
#include "pubkey.h"
#include "uint256.h"
#include "util/blockstatecatcher.h"
#include "util/system.h"
#include "util/validation.h"
#include "validation.h"
#include "wallet/wallet.h"

#include <algorithm>
#include <boost/test/unit_test.hpp>


BOOST_FIXTURE_TEST_SUITE(miner_tests, WalletRegTestingSetup)

// Test suite for ancestor feerate transaction selection.
// Implemented as an additional function, rather than a separate test case,
// to allow reusing the blockchain created in CreateNewBlock_validity.
void TestPackageSelection(const CChainParams& chainparams, CScript scriptPubKey, std::vector<CTransactionRef>& txFirst)
{
    // Test the ancestor feerate transaction selection.
    TestMemPoolEntryHelper entry;
    const CAmount inputValue = std::min(txFirst[0]->vout.at(0).nValue, txFirst[1]->vout.at(0).nValue);
    auto FindTxIndex = [](const std::vector<CTransactionRef>& vtx, const uint256& txid) -> int {
        for (size_t i = 0; i < vtx.size(); ++i) {
            if (vtx[i]->GetHash() == txid) return (int)i;
        }
        return -1;
    };

    // Test that a medium fee transaction will be selected after a higher fee
    // rate package with a low fee rate parent.
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vin[0].prevout.hash = txFirst[0]->GetHash();
    tx.vin[0].prevout.n = 0;
    tx.vout.resize(1);
    const CAmount minFee = ::minRelayTxFee.GetFee(::GetSerializeSize(tx, PROTOCOL_VERSION));
    const CAmount feeLow = minFee;
    const CAmount feeMed = 2 * minFee;
    const CAmount feeHigh = 5 * minFee;
    tx.vout[0].nValue = inputValue - feeLow;
    // This tx has a low fee: 1000 satoshis
    uint256 hashParentTx = tx.GetHash(); // save this txid for later use
    mempool.addUnchecked(hashParentTx, entry.Fee(feeLow).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(tx));

    // This tx has a medium fee: 10000 satoshis
    tx.vin[0].prevout.hash = txFirst[1]->GetHash();
    tx.vout[0].nValue = inputValue - feeMed;
    uint256 hashMediumFeeTx = tx.GetHash();
    mempool.addUnchecked(hashMediumFeeTx, entry.Fee(feeMed).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(tx));

    // This tx has a high fee, but depends on the first transaction
    tx.vin[0].prevout.hash = hashParentTx;
    tx.vout[0].nValue = inputValue - feeLow - feeHigh;
    uint256 hashHighFeeTx = tx.GetHash();
    mempool.addUnchecked(hashHighFeeTx, entry.Fee(feeHigh).Time(GetTime()).SpendsCoinbaseOrCoinstake(false).FromTx(tx));

    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(chainparams, DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey);

    {
        const int iParent = FindTxIndex(pblocktemplate->block.vtx, hashParentTx);
        const int iHigh = FindTxIndex(pblocktemplate->block.vtx, hashHighFeeTx);
        const int iMed = FindTxIndex(pblocktemplate->block.vtx, hashMediumFeeTx);
        BOOST_CHECK(iParent > 0);
        BOOST_CHECK(iHigh > iParent);
        BOOST_CHECK(iMed > iHigh);
    }

    // Test that a package below the min relay fee doesn't get included
    tx.vin[0].prevout.hash = hashHighFeeTx;
    tx.vout[0].nValue = inputValue - feeLow - feeHigh; // 0 fee
    uint256 hashFreeTx = tx.GetHash();
    mempool.addUnchecked(hashFreeTx, entry.Fee(0).FromTx(tx));
    size_t freeTxSize = ::GetSerializeSize(tx, PROTOCOL_VERSION);

    // Calculate a fee on child transaction that will put the package just
    // below the min relay fee (assuming 1 child tx of the same size).
    CAmount feeToUse = minRelayTxFee.GetFee(2*freeTxSize) - 1;

    tx.vin[0].prevout.hash = hashFreeTx;
    tx.vout[0].nValue = (inputValue - feeLow - feeHigh) - feeToUse;
    uint256 hashLowFeeTx = tx.GetHash();
    mempool.addUnchecked(hashLowFeeTx, entry.Fee(feeToUse).FromTx(tx));
    pblocktemplate = BlockAssembler(chainparams, DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey);
    // Verify that the free tx and the low fee tx didn't get selected
    for (size_t i=0; i<pblocktemplate->block.vtx.size(); ++i) {
        BOOST_CHECK(pblocktemplate->block.vtx[i]->GetHash() != hashFreeTx);
        BOOST_CHECK(pblocktemplate->block.vtx[i]->GetHash() != hashLowFeeTx);
    }

    // Test that packages above the min relay fee do get included, even if one
    // of the transactions is below the min relay fee
    // Remove the low fee transaction and replace with a higher fee transaction
    mempool.removeRecursive(tx);
    tx.vout[0].nValue -= 2; // Now we should be just over the min relay fee
    hashLowFeeTx = tx.GetHash();
    mempool.addUnchecked(hashLowFeeTx, entry.Fee(feeToUse+2).FromTx(tx));
    pblocktemplate = BlockAssembler(chainparams, DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey);
    {
        const int iFree = FindTxIndex(pblocktemplate->block.vtx, hashFreeTx);
        const int iLow = FindTxIndex(pblocktemplate->block.vtx, hashLowFeeTx);
        BOOST_CHECK(iFree > 0);
        BOOST_CHECK(iLow > iFree);
    }

    // Test that transaction selection properly updates ancestor fee
    // calculations as ancestor transactions get included in a block.
    // Add a 0-fee transaction that has 2 outputs.
    tx.vin[0].prevout.hash = txFirst[2]->GetHash();
    const CAmount inputValue2 = txFirst[2]->vout.at(0).nValue;
    const CAmount inputValue2Split = inputValue2 / 10;
    tx.vout.resize(2);
    tx.vout[1].nValue = inputValue2Split;
    tx.vout[0].nValue = inputValue2 - inputValue2Split;
    uint256 hashFreeTx2 = tx.GetHash();
    mempool.addUnchecked(hashFreeTx2, entry.Fee(0).SpendsCoinbaseOrCoinstake(true).FromTx(tx));

    // This tx can't be mined by itself
    tx.vin[0].prevout.hash = hashFreeTx2;
    tx.vout.resize(1);
    feeToUse = minRelayTxFee.GetFee(freeTxSize);
    tx.vout[0].nValue = txFirst[2]->vout.at(0).nValue - inputValue2Split - feeToUse;
    uint256 hashLowFeeTx2 = tx.GetHash();
    mempool.addUnchecked(hashLowFeeTx2, entry.Fee(feeToUse).SpendsCoinbaseOrCoinstake(false).FromTx(tx));
    pblocktemplate = BlockAssembler(chainparams, DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey);

    // Verify that this tx isn't selected.
    for (size_t i=0; i<pblocktemplate->block.vtx.size(); ++i) {
        BOOST_CHECK(pblocktemplate->block.vtx[i]->GetHash() != hashFreeTx2);
        BOOST_CHECK(pblocktemplate->block.vtx[i]->GetHash() != hashLowFeeTx2);
    }

    // This tx will be mineable, and should cause hashLowFeeTx2 to be selected
    // as well.
    tx.vin[0].prevout.n = 1;
    const CAmount mineableFee = std::min<CAmount>(0.1 * COIN, inputValue2Split / 2);
    tx.vout[0].nValue = inputValue2Split - mineableFee;
    mempool.addUnchecked(tx.GetHash(), entry.Fee(mineableFee).FromTx(tx));
    pblocktemplate = BlockAssembler(chainparams, DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey);
    BOOST_CHECK(FindTxIndex(pblocktemplate->block.vtx, hashLowFeeTx2) > 0);
}

// NOTE: These tests rely on CreateNewBlock doing its own self-validation!
BOOST_AUTO_TEST_CASE(CreateNewBlock_validity)
{
    // Note that by default, these tests run with size accounting enabled.
    const CChainParams& chainparams = Params();
    CScript scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ParseHex("8d5b4f83212214d6ef693e02e6d71969fddad976") << OP_EQUALVERIFY << OP_CHECKSIG;

    std::unique_ptr<CBlockTemplate> pblocktemplate;
    CMutableTransaction tx,tx2;
    CScript script;
    uint256 hash;
    TestMemPoolEntryHelper entry;
    entry.nFee = 11;
    entry.nHeight = 11;

    Checkpoints::fEnabled = false;

    // Simple block creation, nothing special yet:
    BOOST_CHECK(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false));
    // Set genesis block
    pblocktemplate->block.hashPrevBlock = chainparams.GetConsensus().hashGenesisBlock;

    // We can't make transactions until we have inputs
    // Therefore, load enough blocks for txFirst[2] (height 4) to mature.
    // With nCoinbaseMaturity=100, we need tip >= 103 so CreateNewBlock (height 104) can spend it.
    std::vector<CTransactionRef>txFirst;
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block); // pointer for convenience
    for (unsigned int i = 0; i < 103; ++i) {
        CBlockIndex* pindexPrev = WITH_LOCK(cs_main, return chainActive.Tip());
        assert(pindexPrev);
        pblock->nTime = pindexPrev->GetMedianTimePast() + 60;
        // Updating nTime can change the work required (e.g. when min-difficulty blocks are allowed).
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock.get());
        pblock->vtx.clear(); // Update coinbase input height manually
        CreateCoinbaseTx(pblock.get(), CScript(), pindexPrev);
        const int nextHeight = pindexPrev->nHeight + 1;
        if (txFirst.size() < 4 && nextHeight > 1)
            txFirst.emplace_back(pblock->vtx[0]);
        pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
        pblock->nNonce = 0;
        while (!CheckProofOfWork(pblock->GetHash(), pblock->nBits)) { ++(pblock->nNonce); }
        BlockStateCatcherWrapper stateCatcher(pblock->GetHash());
        stateCatcher.registerEvent();
        const bool processed = ProcessNewBlock(pblock, nullptr);
        SyncWithValidationInterfaceQueue();
        if (!processed || !stateCatcher.get().found || !stateCatcher.get().state.IsValid()) {
            BOOST_FAIL(strprintf("ProcessNewBlock failed at height %d (%s)",
                                 nextHeight, FormatStateMessage(stateCatcher.get().state)));
        }
        pblock->hashPrevBlock = pblock->GetHash();
    }

    // Just to make sure we can still make simple blocks
    BOOST_CHECK(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false));

    const CAmount coinbaseValue0 = txFirst[0]->vout.at(0).nValue;
    const CAmount coinbaseValue1 = txFirst[1]->vout.at(0).nValue;

    // block sigops > limit: 2000 CHECKMULTISIG + 1
    tx.vin.resize(1);
    // NOTE: OP_NOP is used to force 20 SigOps for the CHECKMULTISIG
    tx.vin[0].scriptSig = CScript() << OP_0 << OP_0 << OP_0 << OP_NOP << OP_CHECKMULTISIG << OP_1;
    tx.vin[0].prevout.hash = txFirst[0]->GetHash();
    tx.vin[0].prevout.n = 0;
    tx.vout.resize(1);
    tx.vout[0].nValue = coinbaseValue0;
    const CAmount sigopsFee = ::minRelayTxFee.GetFee(::GetSerializeSize(tx, PROTOCOL_VERSION));
    for (unsigned int i = 0; i < 2001; ++i) {
        tx.vout[0].nValue -= sigopsFee;
        hash = tx.GetHash();
        bool spendsCoinbase = (i == 0) ? true : false; // only first tx spends coinbase
        // If we don't set the # of sig ops in the CTxMemPoolEntry, template creation fails
        mempool.addUnchecked(hash, entry.Fee(sigopsFee).Time(GetTime()).SpendsCoinbaseOrCoinstake(spendsCoinbase).FromTx(tx));
        tx.vin[0].prevout.hash = hash;
    }
    BOOST_CHECK_EXCEPTION(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false), std::runtime_error, HasReason("bad-blk-sigops"));
    mempool.clear();

    tx.vin[0].prevout.hash = txFirst[0]->GetHash();
    tx.vout[0].nValue = coinbaseValue0;
    for (unsigned int i = 0; i < 1001; ++i) {
        tx.vout[0].nValue -= sigopsFee;
        hash = tx.GetHash();
        bool spendsCoinbase = (i == 0) ? true : false; // only first tx spends coinbase
        // If we do set the # of sig ops in the CTxMemPoolEntry, template creation passes
        mempool.addUnchecked(hash, entry.Fee(sigopsFee).Time(GetTime()).SpendsCoinbaseOrCoinstake(spendsCoinbase).SigOps(20).FromTx(tx));
        tx.vin[0].prevout.hash = hash;
    }
    BOOST_CHECK(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false));
    mempool.clear();

    // block size > limit
    tx.vin[0].scriptSig = CScript();
    // 18 * (520char + DROP) + OP_1 = 9433 bytes
    std::vector<unsigned char> vchData(520);
    for (unsigned int i = 0; i < 18; ++i)
    tx.vin[0].scriptSig << vchData << OP_DROP;
    tx.vin[0].scriptSig << OP_1;
    tx.vin[0].prevout.hash = txFirst[0]->GetHash();
    tx.vin[0].prevout.n = 0;
    tx.vout[0].nValue = coinbaseValue0;
    const CAmount bigTxFee = 1000;
    for (unsigned int i = 0; i < 215; ++i) {
        tx.vout[0].nValue -= bigTxFee;
        hash = tx.GetHash();
        bool spendsCoinbase = i == 0; // only first tx spends coinbase
        mempool.addUnchecked(hash, entry.Fee(bigTxFee).Time(GetTime()).SpendsCoinbaseOrCoinstake(spendsCoinbase).FromTx(tx));
        tx.vin[0].prevout.hash = hash;
    }
    BOOST_CHECK(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false));
    mempool.clear();

    // orphan in mempool, template creation fails
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vout[0].nValue = coinbaseValue0 - sigopsFee;
    hash = tx.GetHash();
    mempool.addUnchecked(hash, entry.Fee(1000000).Time(GetTime()).FromTx(tx));
    BOOST_CHECK_EXCEPTION(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false), std::runtime_error, HasReason("bad-txns-inputs-missingorspent"));
    mempool.clear();

    // child with higher feerate than parent
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vin[0].prevout.hash = txFirst[1]->GetHash();
    const CAmount feeParent = 100000000LL; // matches the Fee() below
    const CAmount parentOut = std::max<CAmount>(coinbaseValue1 - feeParent, 1);
    tx.vout[0].nValue = parentOut;
    hash = tx.GetHash();
    mempool.addUnchecked(hash, entry.Fee(feeParent).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(tx));
    tx.vin[0].prevout.hash = hash;
    tx.vin.resize(2);
    tx.vin[1].scriptSig = CScript() << OP_1;
    tx.vin[1].prevout.hash = txFirst[0]->GetHash();
    tx.vin[1].prevout.n = 0;
    CAmount feeChild = 400000000LL; // matches the Fee() below (as long as it's spendable)
    feeChild = std::min<CAmount>(feeChild, std::max<CAmount>(coinbaseValue0 + parentOut - 1, 0));
    tx.vout[0].nValue = std::max<CAmount>(coinbaseValue0 + parentOut - feeChild, 1);
    hash = tx.GetHash();
    mempool.addUnchecked(hash, entry.Fee(feeChild).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(tx));
    BOOST_CHECK(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false));
    mempool.clear();

    // coinbase in mempool, template creation fails
    tx.vin.resize(1);
    tx.vin[0].prevout.SetNull();
    tx.vin[0].scriptSig = CScript() << OP_0 << OP_1;
    tx.vout[0].nValue = 0;
    hash = tx.GetHash();
    // give it a fee so it'll get mined
    mempool.addUnchecked(hash, entry.Fee(100000).Time(GetTime()).SpendsCoinbaseOrCoinstake(false).FromTx(tx));
    BOOST_CHECK_EXCEPTION(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false), std::runtime_error, HasReason("bad-cb-multiple"));
    mempool.clear();

    // invalid (pre-p2sh) txn in mempool, template creation fails
    tx.vin[0].prevout.hash = txFirst[0]->GetHash();
    tx.vin[0].prevout.n = 0;
    tx.vin[0].scriptSig = CScript() << OP_1;
    const CAmount invalidP2SHFee = 10000000L; // matches the Fee() below
    tx.vout[0].nValue = std::max<CAmount>(coinbaseValue0 - invalidP2SHFee, 1);
    script = CScript() << OP_0;
    tx.vout[0].scriptPubKey = GetScriptForDestination(CScriptID(script));
    hash = tx.GetHash();
    mempool.addUnchecked(hash, entry.Fee(invalidP2SHFee).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(tx));
    tx.vin[0].prevout.hash = hash;
    tx.vin[0].scriptSig = CScript() << std::vector<unsigned char>(script.begin(), script.end());
    tx.vout[0].nValue -= 1000000;
    hash = tx.GetHash();
    mempool.addUnchecked(hash, entry.Fee(1000000).Time(GetTime()).SpendsCoinbaseOrCoinstake(false).FromTx(tx));
     // Should throw block-validation-failed
    BOOST_CHECK_EXCEPTION(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false), std::runtime_error, HasReason("block-validation-failed"));
    mempool.clear();

    // double spend txn pair in mempool, template creation fails
    tx.vin[0].prevout.hash = txFirst[0]->GetHash();
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vout[0].nValue = std::max<CAmount>(coinbaseValue0 - COIN, 1);
    tx.vout[0].scriptPubKey = CScript() << OP_1;
    hash = tx.GetHash();
    mempool.addUnchecked(hash, entry.Fee(100000000L).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(tx));
    tx.vout[0].scriptPubKey = CScript() << OP_2;
    hash = tx.GetHash();
    mempool.addUnchecked(hash, entry.Fee(100000000L).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(tx));
    BOOST_CHECK_EXCEPTION(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false), std::runtime_error, HasReason("bad-txns-inputs-missingorspent"));
    mempool.clear();

    // non-final txs in mempool
    SetMockTime(WITH_LOCK(cs_main, return chainActive.Tip()->GetMedianTimePast()+1));

    // height locked
    tx.vin[0].prevout.hash = txFirst[0]->GetHash();
    tx.vin[0].scriptSig = CScript() << OP_1;
    tx.vin[0].nSequence = 0;
    tx.vout[0].nValue = std::max<CAmount>(coinbaseValue0 - COIN, 1);
    tx.vout[0].scriptPubKey = CScript() << OP_1;
    tx.nLockTime = WITH_LOCK(cs_main, return chainActive.Tip()->nHeight+1);
    hash = tx.GetHash();
    mempool.addUnchecked(hash, entry.Fee(100000000L).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(tx));
    { LOCK(cs_main); BOOST_CHECK(!CheckFinalTx(MakeTransactionRef(tx), LOCKTIME_MEDIAN_TIME_PAST)); }

    // time locked
    tx2.vin.resize(1);
    tx2.vin[0].prevout.hash = txFirst[1]->GetHash();
    tx2.vin[0].prevout.n = 0;
    tx2.vin[0].scriptSig = CScript() << OP_1;
    tx2.vin[0].nSequence = 0;
    tx2.vout.resize(1);
    tx2.vout[0].nValue = std::max<CAmount>(coinbaseValue1 - COIN, 1);
    tx2.vout[0].scriptPubKey = CScript() << OP_1;
    tx2.nLockTime = WITH_LOCK(cs_main, return chainActive.Tip()->GetMedianTimePast()+1);
    hash = tx2.GetHash();
    mempool.addUnchecked(hash, entry.Fee(100000000L).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(tx2));
    { LOCK(cs_main); BOOST_CHECK(!CheckFinalTx(MakeTransactionRef(tx2), LOCKTIME_MEDIAN_TIME_PAST)); }

    BOOST_CHECK(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false));

    // Neither tx should have make it into the template.
    BOOST_CHECK_EQUAL(pblocktemplate->block.vtx.size(), 1);

    {
        LOCK(cs_main);
        // However if we advance height and time, both will.
        // Use a larger offset than +1 to avoid time-slot alignment (15s) rounding
        // the block time back down to the lock-time.
        chainActive.Tip()->nHeight++;
        SetMockTime(chainActive.Tip()->GetMedianTimePast() + 30);
    }

    // FIXME: we should *actually* create a new block so the following test
    //        works; CheckFinalTx() isn't fooled by monkey-patching nHeight.
    //BOOST_CHECK(CheckFinalTx(tx));
    //BOOST_CHECK(CheckFinalTx(tx2));

    BOOST_CHECK(pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false));
    BOOST_CHECK_EQUAL(pblocktemplate->block.vtx.size(), 3);

    WITH_LOCK(cs_main, chainActive.Tip()->nHeight--);
    SetMockTime(0);
    mempool.clear();

    TestPackageSelection(chainparams, scriptPubKey, txFirst);

    Checkpoints::fEnabled = true;
}

BOOST_AUTO_TEST_CASE(CreateNewBlock_orders_gov_vote_lock_before_cast)
{
    const CChainParams& chainparams = Params();
    CScript scriptPubKey = CScript() << OP_DUP << OP_HASH160
                                     << ParseHex("8d5b4f83212214d6ef693e02e6d71969fddad976")
                                     << OP_EQUALVERIFY << OP_CHECKSIG;

    std::unique_ptr<CBlockTemplate> pblocktemplate;
    BOOST_REQUIRE(pblocktemplate = BlockAssembler(chainparams, DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey, &m_wallet, false));
    pblocktemplate->block.hashPrevBlock = chainparams.GetConsensus().hashGenesisBlock;

    std::vector<CTransactionRef> txFirst;
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block);
    for (unsigned int i = 0; i < 130; ++i) {
        CBlockIndex* pindexPrev = WITH_LOCK(cs_main, return chainActive.Tip());
        assert(pindexPrev);
        pblock->nTime = pindexPrev->GetMedianTimePast() + 60;
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock.get());
        pblock->vtx.clear();
        CreateCoinbaseTx(pblock.get(), CScript(), pindexPrev);
        const int nextHeight = pindexPrev->nHeight + 1;
        if (txFirst.size() < 3 && nextHeight > 1) {
            txFirst.emplace_back(pblock->vtx[0]);
        }
        pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
        pblock->nNonce = 0;
        while (!CheckProofOfWork(pblock->GetHash(), pblock->nBits)) {
            ++(pblock->nNonce);
        }
        BlockStateCatcherWrapper stateCatcher(pblock->GetHash());
        stateCatcher.registerEvent();
        const bool processed = ProcessNewBlock(pblock, nullptr);
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE(processed && stateCatcher.get().found && stateCatcher.get().state.IsValid());
        pblock->hashPrevBlock = pblock->GetHash();
    }
    BOOST_REQUIRE_EQUAL(txFirst.size(), 3U);

    const CAmount inputValueLock = txFirst[0]->vout.at(0).nValue;
    const CAmount inputValueCast = txFirst[1]->vout.at(0).nValue;
    const CAmount lockAmount = 25 * COIN;
    const CAmount lockFee = 1 * CENT;
    const CAmount castFee = 5 * CENT;

    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    const CKeyID ownerKeyId = ownerKey.GetPubKey().GetID();
    const CScript ownerScript = GetScriptForDestination(ownerKeyId);
    const uint256 proposalHash = GetRandHash();

    CMutableTransaction lockTx;
    lockTx.nVersion = CTransaction::TxVersion::SAPLING;
    lockTx.nType = CTransaction::TxType::GOVVOTELOCK;
    lockTx.vin.emplace_back(txFirst[0]->GetHash(), 0);
    lockTx.vin[0].scriptSig = CScript() << OP_1;
    lockTx.vout.emplace_back(lockAmount, ownerScript);
    lockTx.vout.emplace_back(inputValueLock - lockAmount - lockFee, ownerScript);
    CGovVoteLockTx lockPayload;
    lockPayload.proposalHash = proposalHash;
    lockPayload.lockAmount = lockAmount;
    lockPayload.unlockHeight = WITH_LOCK(cs_main, return chainActive.Height()) + 100;
    lockPayload.ownerKeyId = ownerKeyId;
    SetTxPayload(lockTx, lockPayload);
    const uint256 lockTxHash = lockTx.GetHash();

    CMutableTransaction castTx;
    castTx.nVersion = CTransaction::TxVersion::SAPLING;
    castTx.nType = CTransaction::TxType::GOVVOTECAST;
    castTx.vin.emplace_back(txFirst[1]->GetHash(), 0);
    castTx.vin[0].scriptSig = CScript() << OP_1;
    castTx.vout.emplace_back(inputValueCast - castFee, ownerScript);
    CGovVoteCastTx castPayload;
    castPayload.proposalHash = proposalHash;
    castPayload.voteDirection = CGovVoteCastTx::VOTE_YES;
    castPayload.lockRefs.emplace_back(lockTxHash, 0);
    BOOST_REQUIRE(CHashSigner::SignHash(castPayload.GetSignatureHash(), ownerKey, castPayload.sig));
    SetTxPayload(castTx, castPayload);
    const uint256 castTxHash = castTx.GetHash();

    TestMemPoolEntryHelper entry;
    mempool.addUnchecked(lockTxHash, entry.Fee(lockFee).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(lockTx));
    mempool.addUnchecked(castTxHash, entry.Fee(castFee).Time(GetTime()).SpendsCoinbaseOrCoinstake(true).FromTx(castTx));

    BOOST_REQUIRE(pblocktemplate = BlockAssembler(chainparams, DEFAULT_PRINTPRIORITY).CreateNewBlock(
            scriptPubKey,
            &m_wallet,
            false,
            nullptr,
            false,
            false));

    auto FindTxIndex = [](const std::vector<CTransactionRef>& txs, const uint256& txid) {
        for (size_t i = 0; i < txs.size(); ++i) {
            if (txs[i]->GetHash() == txid) return static_cast<int>(i);
        }
        return -1;
    };
    const int lockPos = FindTxIndex(pblocktemplate->block.vtx, lockTxHash);
    const int castPos = FindTxIndex(pblocktemplate->block.vtx, castTxHash);
    BOOST_REQUIRE(lockPos > 0);
    BOOST_REQUIRE(castPos > 0);
    BOOST_CHECK(lockPos < castPos);

    mempool.clear();
}

BOOST_AUTO_TEST_SUITE_END()
