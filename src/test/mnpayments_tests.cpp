// Copyright (c) 2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "bls/bls_wrapper.h"
#include "blockassembler.h"
#include "consensus/merkle.h"
#include "consensus/upgrades.h"
#include "evo/deterministicmns.h"
#include "evo/providertx.h"
#include "evo/specialtx_validation.h"
#include "masternode-payments.h"
#include "netbase.h"
#include "primitives/transaction.h"
#include "script/sign.h"
#include "spork.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "util/blockstatecatcher.h"
#include "utilmoneystr.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <limits>
#include <map>

BOOST_AUTO_TEST_SUITE(mnpayments_tests)

static bool HasPayeeOutput(const CTransactionRef& coinbaseTx, const CScript& payee)
{
    return std::any_of(coinbaseTx->vout.begin(), coinbaseTx->vout.end(),
                       [&](const CTxOut& out) { return out.scriptPubKey == payee; });
}

static void ReplacePayeeOutput(CMutableTransaction& coinbaseTx, const CScript& from, const CScript& to)
{
    auto it = std::find_if(coinbaseTx.vout.begin(), coinbaseTx.vout.end(),
                           [&](const CTxOut& out) { return out.scriptPubKey == from; });
    BOOST_REQUIRE_MESSAGE(it != coinbaseTx.vout.end(), "expected coinbase to pay the original payee");
    it->scriptPubKey = to;
}

void enableMnSyncAndMNPayments()
{
    // force mnsync complete
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);

    // enable SPORK_13
    int64_t nTime = GetTime() - 10;
    CSporkMessage spork(SPORK_13_ENABLE_SUPERBLOCKS, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS));

    spork = CSporkMessage(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT));
}

// -----------------------------------------------------------------------------
// DMN test infrastructure. Mirrors the ProReg-based setup used by
// evo_deterministicmns_tests: register masternodes through on-chain ProReg
// special transactions and resolve the payee through the DMN list.
// -----------------------------------------------------------------------------

// static 0.1 PIV fee used for the special txes in these tests
static const CAmount fee = 10000000;

struct SimpleUTXO
{
    int nHeight;
    CAmount nValue;
    bool fCoinbase;
};

typedef std::map<COutPoint, SimpleUTXO> SimpleUTXOMap;

static bool IsSpendableBy(const CTxOut& out, const CKey& spendKey)
{
    const CScript p2pk = CScript() << ToByteVector(spendKey.GetPubKey()) << OP_CHECKSIG;
    const CScript p2pkh = GetScriptForDestination(spendKey.GetPubKey().GetID());
    return out.scriptPubKey == p2pk || out.scriptPubKey == p2pkh;
}

static void AddSpendableOutputs(SimpleUTXOMap& utxos, const CTransaction& tx, int nHeight, const CKey& spendKey, bool fCoinbase)
{
    for (size_t j = 0; j < tx.vout.size(); j++) {
        if (!IsSpendableBy(tx.vout[j], spendKey)) continue;
        utxos.emplace(std::piecewise_construct,
                      std::forward_as_tuple(tx.GetHash(), j),
                      std::forward_as_tuple(SimpleUTXO{nHeight, tx.vout[j].nValue, fCoinbase}));
    }
}

static SimpleUTXOMap BuildSimpleUtxoMap(const std::vector<CTransaction>& txs, const CKey& spendKey)
{
    SimpleUTXOMap utxos;
    for (size_t i = 0; i < txs.size(); i++) {
        AddSpendableOutputs(utxos, txs[i], /*nHeight=*/(int)i + 1, spendKey, /*fCoinbase=*/true);
    }
    return utxos;
}

static std::vector<COutPoint> SelectUTXOs(SimpleUTXOMap& utxos, CAmount amount, CAmount& changeRet)
{
    changeRet = 0;
    amount += fee;

    std::vector<COutPoint> selectedUtxos;
    CAmount selectedAmount = 0;
    int chainHeight = WITH_LOCK(cs_main, return chainActive.Height(); );
    while (!utxos.empty()) {
        const int maturity = Params().GetConsensus().nCoinbaseMaturity;

        auto bestNonCoinbaseIt = utxos.end();
        auto bestCoinbaseIt = utxos.end();
        for (auto it = utxos.begin(); it != utxos.end(); ++it) {
            if (!it->second.fCoinbase) {
                if (bestNonCoinbaseIt == utxos.end() || it->second.nValue > bestNonCoinbaseIt->second.nValue) {
                    bestNonCoinbaseIt = it;
                }
                continue;
            }
            if (chainHeight - it->second.nHeight < maturity) continue;
            if (bestCoinbaseIt == utxos.end() || it->second.nValue > bestCoinbaseIt->second.nValue) {
                bestCoinbaseIt = it;
            }
        }

        auto chosenIt = bestNonCoinbaseIt != utxos.end() ? bestNonCoinbaseIt : bestCoinbaseIt;
        if (chosenIt == utxos.end()) {
            int minHeight{std::numeric_limits<int>::max()};
            int maxHeight{std::numeric_limits<int>::min()};
            size_t coinbaseCount{0};
            size_t nonCoinbaseCount{0};
            for (const auto& entry : utxos) {
                minHeight = std::min(minHeight, entry.second.nHeight);
                maxHeight = std::max(maxHeight, entry.second.nHeight);
                if (entry.second.fCoinbase) {
                    coinbaseCount++;
                } else {
                    nonCoinbaseCount++;
                }
            }
            BOOST_REQUIRE_MESSAGE(false,
                                  strprintf("SelectUTXOs: no eligible UTXO found (chainHeight=%d maturity=%d utxos=%u coinbase=%u noncoinbase=%u minHeight=%d maxHeight=%d)",
                                            chainHeight, maturity, utxos.size(), coinbaseCount, nonCoinbaseCount,
                                            minHeight == std::numeric_limits<int>::max() ? -1 : minHeight,
                                            maxHeight == std::numeric_limits<int>::min() ? -1 : maxHeight));
        }

        selectedAmount += chosenIt->second.nValue;
        selectedUtxos.emplace_back(chosenIt->first);
        utxos.erase(chosenIt);
        if (selectedAmount >= amount) {
            changeRet = selectedAmount - amount;
            break;
        }
    }

    BOOST_REQUIRE_MESSAGE(selectedAmount >= amount,
                          strprintf("SelectUTXOs: insufficient funds (selected=%s required=%s utxos_remaining=%u)",
                                    FormatMoney(selectedAmount), FormatMoney(amount), utxos.size()));
    return selectedUtxos;
}

static void FundTransaction(CMutableTransaction& tx, SimpleUTXOMap& utxos, const CScript& scriptPayout, const CScript& scriptChange, CAmount amount)
{
    CAmount change;
    auto inputs = SelectUTXOs(utxos, amount, change);
    for (size_t i = 0; i < inputs.size(); i++) {
        tx.vin.emplace_back(inputs[i]);
    }
    tx.vout.emplace_back(CTxOut(amount, scriptPayout));
    if (change != 0) {
        tx.vout.emplace_back(change, scriptChange);
    }
}

static void SignTransaction(CMutableTransaction& tx, const CKey& coinbaseKey)
{
    CBasicKeyStore tempKeystore;
    tempKeystore.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());

    for (size_t i = 0; i < tx.vin.size(); i++) {
        CTransactionRef txFrom;
        uint256 hashBlock;
        BOOST_ASSERT(GetTransaction(tx.vin[i].prevout.hash, txFrom, hashBlock));
        BOOST_ASSERT(SignSignature(tempKeystore, *txFrom, tx, i, SIGHASH_ALL));
    }
}

static CKey GetRandomKey()
{
    CKey keyRet;
    keyRet.MakeNewKey(true);
    return keyRet;
}

static CBLSSecretKey GetRandomBLSKey()
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    return sk;
}

static CScript GenerateRandomAddress()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(key.GetPubKey().GetID());
}

// Creates a ProRegTx with a new collateral in the first output of the tx.
static CMutableTransaction CreateProRegTx(SimpleUTXOMap& utxos, int port, const CScript& scriptPayout, const CKey& coinbaseKey,
                                          const CKey& ownerKey,
                                          const CBLSPublicKey& operatorPubKey)
{
    ProRegPL pl;
    pl.collateralOutpoint = COutPoint(UINT256_ZERO, 0);
    pl.addr = LookupNumeric("1.1.1.1", port);
    pl.keyIDOwner = ownerKey.GetPubKey().GetID();
    pl.pubKeyOperator = operatorPubKey;
    pl.keyIDVoting = ownerKey.GetPubKey().GetID();
    pl.scriptPayout = scriptPayout;
    pl.nOperatorReward = 0;

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROREG;
    FundTransaction(tx, utxos, scriptPayout,
                    GetScriptForDestination(coinbaseKey.GetPubKey().GetID()),
                    Params().GetConsensus().nMNCollateralAmt);

    pl.inputsHash = CalcTxInputsHash(tx);
    SetTxPayload(tx, pl);
    SignTransaction(tx, coinbaseKey);

    return tx;
}

static bool IsMNPayeeInBlock(const CBlock& block, const CScript& expected)
{
    for (const auto& txout : block.vtx[0]->vout) {
        if (txout.scriptPubKey == expected) return true;
    }
    return false;
}

BOOST_FIXTURE_TEST_CASE(dmn_payee_test, TestChain100Setup)
{
    enableMnSyncAndMNPayments();

    // Advance the chain and enable v6 (deterministic) masternode payments.
    CreateAndProcessBlock({}, coinbaseKey);
    CBlockIndex* chainTip = chainActive.Tip();
    int nHeight = chainTip->nHeight; // 101
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, nHeight + 2);
    CreateAndProcessBlock({}, coinbaseKey); // last pre-v6 block
    chainTip = chainActive.Tip();
    BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);

    // Build the UTXO set from the 100 pre-mined coinbase outputs (250 PIV each on regtest).
    SimpleUTXOMap utxos = BuildSimpleUtxoMap(coinbaseTxns, coinbaseKey);

    // Register three DMNs, one per block (v6 active from height 103).
    CCoinsViewCache* view = pcoinsTip.get();
    int port = 1;
    for (int i = 0; i < 3; i++) {
        const CKey& ownerKey = GetRandomKey();
        const CBLSSecretKey& operatorKey = GetRandomBLSKey();
        auto tx = CreateProRegTx(utxos, port++, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey.GetPublicKey());
        const uint256& txid = tx.GetHash();

        CValidationState dummyState;
        BOOST_CHECK(WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, dummyState); ));

        CreateAndProcessBlock({tx}, coinbaseKey);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, nHeight + 1);
        BOOST_CHECK(deterministicMNManager->GetListAtChainTip().HasMN(txid));

        AddSpendableOutputs(utxos, CTransaction(tx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        nHeight++;
    }

    // Mine 20 blocks: each coinbase must pay the expected DMN payee, and
    // GetMasternodeTxOuts must resolve to the same payee for that block.
    for (int i = 0; i < 20; i++) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        auto dmnExpectedPayee = mnList.GetMNPayee();
        BOOST_REQUIRE(dmnExpectedPayee);

        std::vector<CTxOut> vecMnOuts;
        BOOST_CHECK(masternodePayments.GetMasternodeTxOuts(chainActive.Tip(), vecMnOuts));
        BOOST_REQUIRE(!vecMnOuts.empty());
        BOOST_CHECK_EQUAL(vecMnOuts.size(), 1); // no operator reward configured
        BOOST_CHECK(vecMnOuts[0].scriptPubKey == dmnExpectedPayee->pdmnState->scriptPayout);
        BOOST_CHECK_EQUAL(vecMnOuts[0].nValue, GetMasternodePayment(chainActive.Tip()->nHeight + 1));

        CBlock block = CreateAndProcessBlock({}, coinbaseKey);
        BOOST_CHECK_MESSAGE(IsMNPayeeInBlock(block, dmnExpectedPayee->pdmnState->scriptPayout),
                            "error: block not paying to the deterministic masternode payee");
        nHeight++;
    }
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height();), nHeight);

    // A block paying to a different script instead of the DMN payee must be rejected.
    auto mnList = deterministicMNManager->GetListAtChainTip();
    auto dmnExpectedPayee = mnList.GetMNPayee();
    BOOST_REQUIRE(dmnExpectedPayee);
    const CScript& payeeScript = dmnExpectedPayee->pdmnState->scriptPayout;

    CBlock badBlock = CreateBlock({}, coinbaseKey);
    CMutableTransaction coinbase(*badBlock.vtx[0]);
    ReplacePayeeOutput(coinbase, payeeScript, GenerateRandomAddress());
    badBlock.vtx[0] = MakeTransactionRef(coinbase);
    badBlock.hashMerkleRoot = BlockMerkleRoot(badBlock);

    // Outside a governance payment window, the deterministic payee is fully
    // derivable from the chain and must remain enforced while tier-two data is
    // syncing (the same state used during initial block download).
    BOOST_REQUIRE_GE((nHeight + 1) % Params().GetConsensus().nBudgetCycleBlocks, 100);
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_INITIAL);
    BOOST_CHECK(!IsBlockPayeeValid(badBlock, chainActive.Tip()));
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);

    {
        auto pBadBlock = std::make_shared<CBlock>(badBlock);
        SolveBlock(pBadBlock, nHeight + 1);
        BlockStateCatcherWrapper sc(pBadBlock->GetHash());
        sc.registerEvent();
        ProcessNewBlock(pBadBlock, nullptr);
        BOOST_CHECK(sc.get().found && !sc.get().state.IsValid());
        BOOST_CHECK_EQUAL(sc.get().state.GetRejectReason(), "bad-cb-payee");
    }
    BOOST_CHECK(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash();) != badBlock.GetHash());
}

BOOST_AUTO_TEST_SUITE_END()
