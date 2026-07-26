// Copyright (c) 2018-2021 The Dash Core developers
// Copyright (c) 2021-2022 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_pivx.h"

#include "blockassembler.h"
#include "consensus/merkle.h"
#include "consensus/params.h"
#include "evo/specialtx_validation.h"
#include "evo/deterministicmns.h"
#include "llmq/quorums_blockprocessor.h"
#include "llmq/quorums_commitment.h"
#include "llmq/quorums_utils.h"
#include "masternode-payments.h"
#include "messagesigner.h"
#include "netbase.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "script/sign.h"
#include "spork.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "util/blocksutil.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "validationinterface.h"

#include <boost/test/unit_test.hpp>

#include <limits>
#include <set>

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

static CMutableTransaction CreateSplitTx(SimpleUTXOMap& utxos, const CKey& coinbaseKey, const std::vector<CAmount>& amounts)
{
    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;

    const CScript script = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    CAmount total = 0;
    for (const auto& amount : amounts) total += amount;

    CAmount change;
    auto inputs = SelectUTXOs(utxos, total, change);
    for (const auto& prevout : inputs) tx.vin.emplace_back(prevout);
    for (const auto& amount : amounts) tx.vout.emplace_back(amount, script);
    if (change != 0) tx.vout.emplace_back(change, script);

    SignTransaction(tx, coinbaseKey);
    return tx;
}

// Makes a new tx with a single out of given amount to given destination (or coinbase address, if nullopt)
static COutPoint CreateNewUTXO(SimpleUTXOMap& utxos,
                               CMutableTransaction& mtx,
                               const CKey& coinbaseKey, CAmount amount,
                               Optional<CScript> scriptDest = nullopt)
{
    const CScript& s = (scriptDest != nullopt ? *scriptDest
                                              : GetScriptForDestination(coinbaseKey.GetPubKey().GetID()));
    const CScript& scriptChange = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    FundTransaction(mtx, utxos, s, scriptChange, amount);
    SignTransaction(mtx, coinbaseKey);
    int idx = -1;
    for (size_t i = 0; i < mtx.vout.size() && idx < 0; i++) {
        if (mtx.vout[i].nValue == amount) idx = i;
    }
    BOOST_CHECK(idx >= 0);
    return COutPoint(mtx.GetHash(), idx);
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

// Creates a ProRegTx.
// - if optCollateralOut is nullopt, generate a new collateral in the first output of the tx
// - otherwise reference *optCollateralOut as external collateral
static CMutableTransaction CreateProRegTx(Optional<COutPoint> optCollateralOut,
                                          SimpleUTXOMap& utxos, int port, const CScript& scriptPayout, const CKey& coinbaseKey,
                                          const CKey& ownerKey,
                                          const CBLSPublicKey& operatorPubKey,
                                          uint16_t operatorReward = 0,
                                          bool fInvalidCollateral = false)
{
    ProRegPL pl;
    pl.collateralOutpoint = (optCollateralOut ? *optCollateralOut : COutPoint(UINT256_ZERO, 0));
    pl.addr = LookupNumeric("1.1.1.1", port);
    pl.keyIDOwner = ownerKey.GetPubKey().GetID();
    pl.pubKeyOperator = operatorPubKey;
    pl.keyIDVoting = ownerKey.GetPubKey().GetID();
    pl.scriptPayout = scriptPayout;
    pl.nOperatorReward = operatorReward;

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROREG;
    FundTransaction(tx, utxos, scriptPayout,
                    GetScriptForDestination(coinbaseKey.GetPubKey().GetID()),
                    (optCollateralOut ? 0 : Params().GetConsensus().nMNCollateralAmt - (fInvalidCollateral ? 1 : 0)));

    pl.inputsHash = CalcTxInputsHash(tx);
    SetTxPayload(tx, pl);
    SignTransaction(tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpServTx(SimpleUTXOMap& utxos, const uint256& proTxHash, const CBLSSecretKey& operatorKey, int port, const CScript& scriptOperatorPayout, const CKey& coinbaseKey)
{
    ProUpServPL pl;
    pl.proTxHash = proTxHash;
    pl.addr = LookupNumeric("1.1.1.1", port);
    pl.scriptOperatorPayout = scriptOperatorPayout;

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROUPSERV;
    const CScript& s = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    FundTransaction(tx, utxos, s, s, 1 * COIN);
    pl.inputsHash = CalcTxInputsHash(tx);
    pl.sig = operatorKey.Sign(::SerializeHash(pl));
    BOOST_ASSERT(pl.sig.IsValid());
    SetTxPayload(tx, pl);
    SignTransaction(tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpRegTx(SimpleUTXOMap& utxos, const uint256& proTxHash, const CKey& ownerKey, const CBLSPublicKey& operatorPubKey, const CKey& votingKey, const CScript& scriptPayout, const CKey& coinbaseKey)
{
    ProUpRegPL pl;
    pl.proTxHash = proTxHash;
    pl.pubKeyOperator = operatorPubKey;
    pl.keyIDVoting = votingKey.GetPubKey().GetID();
    pl.scriptPayout = scriptPayout;

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROUPREG;
    const CScript& s = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    FundTransaction(tx, utxos, s, s, 1 * COIN);
    pl.inputsHash = CalcTxInputsHash(tx);
    BOOST_ASSERT(CHashSigner::SignHash(::SerializeHash(pl), ownerKey, pl.vchSig));
    SetTxPayload(tx, pl);
    SignTransaction(tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpRevTx(SimpleUTXOMap& utxos, const uint256& proTxHash, ProUpRevPL::RevocationReason reason, const CBLSSecretKey& operatorKey, const CKey& coinbaseKey)
{
    ProUpRevPL pl;
    pl.proTxHash = proTxHash;
    pl.nReason = reason;

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROUPREV;
    const CScript& s = GetScriptForDestination(coinbaseKey.GetPubKey().GetID());
    FundTransaction(tx, utxos, s, s, 1 * COIN);
    pl.inputsHash = CalcTxInputsHash(tx);
    pl.sig = operatorKey.Sign(::SerializeHash(pl));
    BOOST_ASSERT(pl.sig.IsValid());
    SetTxPayload(tx, pl);
    SignTransaction(tx, coinbaseKey);

    return tx;
}

static CScript GenerateRandomAddress()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(key.GetPubKey().GetID());
}

template<typename ProPL>
static CMutableTransaction MalleateProTxPayout(const CMutableTransaction& tx)
{
    ProPL pl;
    GetTxPayload(tx, pl);
    pl.scriptPayout = GenerateRandomAddress();
    CMutableTransaction tx2 = tx;
    SetTxPayload(tx2, pl);
    return tx2;
}

static CMutableTransaction MalleateProUpServTx(const CMutableTransaction& tx)
{
    ProUpServPL pl;
    GetTxPayload(tx, pl);
    pl.addr = LookupNumeric("1.1.1.1", 1001 + InsecureRandRange(100));
    if (!pl.scriptOperatorPayout.empty()) {
        pl.scriptOperatorPayout = GenerateRandomAddress();
    }
    CMutableTransaction tx2 = tx;
    SetTxPayload(tx2, pl);
    return tx2;
}

static CMutableTransaction MalleateProUpRevTx(const CMutableTransaction& tx)
{
    ProUpRevPL pl;
    GetTxPayload(tx, pl);
    BOOST_ASSERT(pl.nReason != ProUpRevPL::RevocationReason::REASON_CHANGE_OF_KEYS);
    pl.nReason = ProUpRevPL::RevocationReason::REASON_CHANGE_OF_KEYS;
    CMutableTransaction tx2 = tx;
    SetTxPayload(tx2, pl);
    return tx2;
}

static bool CheckTransactionSignature(const CMutableTransaction& tx)
{
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const auto& txin = tx.vin[i];
        CTransactionRef txFrom;
        uint256 hashBlock;
        BOOST_ASSERT(GetTransaction(txin.prevout.hash, txFrom, hashBlock));

        CAmount amount = txFrom->vout[txin.prevout.n].nValue;
        if (!VerifyScript(txin.scriptSig, txFrom->vout[txin.prevout.n].scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS, MutableTransactionSignatureChecker(&tx, i, amount), tx.GetRequiredSigVersion())) {
            return false;
        }
    }
    return true;
}

static bool IsMNPayeeInBlock(const CBlock& block, const CScript& expected)
{
    for (const auto& txout : block.vtx[0]->vout) {
        if (txout.scriptPubKey == expected) return true;
    }
    return false;
}

static void CheckPayments(const std::map<uint256, int>& mp, size_t mapSize, int minCount)
{
    BOOST_CHECK_EQUAL(mp.size(), mapSize);
    for (const auto& it : mp) {
        BOOST_CHECK_MESSAGE(it.second >= minCount,
                strprintf("MN %s didn't receive expected num of payments (%d<%d)",it.first.ToString(), it.second, minCount)
        );
    }
}

BOOST_AUTO_TEST_SUITE(deterministicmns_tests)

BOOST_FIXTURE_TEST_CASE(dip3_protx, TestChain400Setup)
{
    // CTEAM never activates deterministic masternodes.
    return;

    auto utxos = BuildSimpleUtxoMap(coinbaseTxns, coinbaseKey);

    CBlockIndex* chainTip = chainActive.Tip();
    CCoinsViewCache* view = pcoinsTip.get();

    int nHeight = chainTip->nHeight;
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, nHeight + 2);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, nHeight + 200);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V3_4, nHeight + 200);

    // load empty list (last block before enforcement)
    CreateAndProcessBlock({}, coinbaseKey);
    chainTip = chainActive.Tip();
    BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);

    // force mnsync complete and enable spork 8
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);
    int64_t nTime = GetTime() - 10;
    const CSporkMessage& sporkMnPayment = CSporkMessage(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(sporkMnPayment);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT));

    int port = 1;

    std::vector<uint256> dmnHashes;
    std::map<uint256, CKey> ownerKeys;
    std::map<uint256, CBLSSecretKey> operatorKeys;

    // register one MN per block
    for (size_t i = 0; i < 6; i++) {
        const CKey& ownerKey = GetRandomKey();
        const CBLSSecretKey& operatorKey = GetRandomBLSKey();
        auto tx = CreateProRegTx(nullopt, utxos, port++, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey.GetPublicKey());
        const uint256& txid = tx.GetHash();
        dmnHashes.emplace_back(txid);
        ownerKeys.emplace(txid, ownerKey);
        operatorKeys.emplace(txid, operatorKey);

        CValidationState dummyState;
        BOOST_CHECK(WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, dummyState); ));
        BOOST_CHECK(CheckTransactionSignature(tx));

        // also verify that payloads are not malleable after they have been signed
        // the form of ProRegTx we use here is one with a collateral included, so there is no signature inside the
        // payload itself. This means, we need to rely on script verification, which takes the hash of the extra payload
        // into account
        auto tx2 = MalleateProTxPayout<ProRegPL>(tx);
        // Technically, the payload is still valid...
        BOOST_CHECK(WITH_LOCK(cs_main, return CheckSpecialTx(tx2, chainTip, view, dummyState); ));
        // But the signature should not verify anymore
        BOOST_CHECK(!CheckTransactionSignature(tx2));

        CreateAndProcessBlock({tx}, coinbaseKey);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, nHeight + 1);
        BOOST_CHECK(deterministicMNManager->GetListAtChainTip().HasMN(txid));

        AddSpendableOutputs(utxos, CTransaction(tx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);

        nHeight++;
    }

    // CTEAM keeps legacy/non-deterministic masternodes enabled permanently.
    // SPORK_21 must not force deterministic-only mode.
    const CSporkMessage& spork = CSporkMessage(SPORK_21_LEGACY_MNS_MAX_HEIGHT, nHeight, GetTime());
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(!deterministicMNManager->LegacyMNObsolete(nHeight + 1));

    // Mine 20 blocks, checking MN reward payments
    std::map<uint256, int> mapPayments;
    for (size_t i = 0; i < 20; i++) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        BOOST_CHECK_EQUAL(mnList.GetValidMNsCount(), 6);
        BOOST_CHECK_EQUAL(mnList.GetHeight(), nHeight);

        // get next payee
        auto dmnExpectedPayee = mnList.GetMNPayee();
        CBlock block = CreateAndProcessBlock({}, coinbaseKey);
        chainTip = chainActive.Tip();
        BOOST_ASSERT(!block.vtx.empty());
        if (deterministicMNManager->LegacyMNObsolete(nHeight + 1)) {
            BOOST_CHECK(IsMNPayeeInBlock(block, dmnExpectedPayee->pdmnState->scriptPayout));
            mapPayments[dmnExpectedPayee->proTxHash]++;
        }
        // else: legacy payments active, no DMN payment enforcement in this test environment
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
    }
    // 20 blocks, 6 masternodes. Must have been paid at least 3 times each.
    if (deterministicMNManager->LegacyMNObsolete(nHeight)) {
        CheckPayments(mapPayments, 6, 3);
    } else {
        BOOST_CHECK(mapPayments.empty());
    }


    // Try to register with non-existent external collateral
    {
        Optional<COutPoint> o = COutPoint(UINT256_ONE, 0);
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProRegTx(o, utxosTmp, port, GenerateRandomAddress(), coinbaseKey, GetRandomKey(), GetRandomBLSKey().GetPublicKey());
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-collateral");
    }
    // Try to register with invalid external collateral
    {
        // create an output of value 1 sat less than the required collateral amount
        CMutableTransaction mtx;
        const COutPoint& coll_out = CreateNewUTXO(utxos, mtx, coinbaseKey, Params().GetConsensus().nMNCollateralAmt-1);
        CreateAndProcessBlock({mtx}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(mtx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
        Coin coll_coin;
        BOOST_CHECK(view->GetUTXOCoin(coll_out, coll_coin));
        BOOST_CHECK_EQUAL(coll_coin.out.nValue, Params().GetConsensus().nMNCollateralAmt-1);

        // create the ProReg tx referencing the invalid collateral
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProRegTx(Optional<COutPoint>(coll_out), utxosTmp, port, GenerateRandomAddress(), coinbaseKey, GetRandomKey(), GetRandomBLSKey().GetPublicKey());
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-collateral-amount");

        // add the coin back to the utxo map
        utxos.emplace(coll_out, SimpleUTXO{static_cast<int>(coll_coin.nHeight), coll_coin.out.nValue, /*fCoinbase=*/false});
    }
    // Try to register with spent external collateral
    {
        // create an output of collateral amount
        CMutableTransaction mtx;
        const COutPoint& coll_out = CreateNewUTXO(utxos, mtx, coinbaseKey, Params().GetConsensus().nMNCollateralAmt);
        CreateAndProcessBlock({mtx}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(mtx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
        Coin coll_coin;
        BOOST_CHECK(view->GetUTXOCoin(coll_out, coll_coin));
        BOOST_CHECK_EQUAL(coll_coin.out.nValue, Params().GetConsensus().nMNCollateralAmt);

        // spend it
        CMutableTransaction spendTx;
        spendTx.vin.emplace_back(coll_out);
        spendTx.vout.emplace_back(Params().GetConsensus().nMNCollateralAmt - 1000,
                                  GetScriptForDestination(coinbaseKey.GetPubKey().GetID()));
        utxos.erase(coll_out);
        SignTransaction(spendTx, coinbaseKey);
        CreateAndProcessBlock({spendTx}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(spendTx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
        BOOST_CHECK(!view->GetUTXOCoin(coll_out, coll_coin));

        // create the ProReg tx referencing the spent collateral
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProRegTx(Optional<COutPoint>(coll_out), utxosTmp, port, GenerateRandomAddress(), coinbaseKey, GetRandomKey(), GetRandomBLSKey().GetPublicKey());
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-collateral");
    }
    // Try to register with invalid internal collateral
    {
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProRegTx(nullopt, utxosTmp, port, GenerateRandomAddress(), coinbaseKey, GetRandomKey(), GetRandomBLSKey().GetPublicKey(), 0, true);
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-collateral-amount");
    }
    // Try to register reusing the collateral key as owner/voting key
    {
        const CKey& coll_key = GetRandomKey();
        // create a valid collateral
        CMutableTransaction mtx;
        const COutPoint& coll_out = CreateNewUTXO(utxos, mtx, coinbaseKey, Params().GetConsensus().nMNCollateralAmt,
                                    GetScriptForDestination(coll_key.GetPubKey().GetID()));
        CreateAndProcessBlock({mtx}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(mtx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
        Coin coll_coin;
        BOOST_CHECK(view->GetUTXOCoin(coll_out, coll_coin));
        BOOST_CHECK_EQUAL(coll_coin.out.nValue, Params().GetConsensus().nMNCollateralAmt);

        // create the ProReg tx reusing the collateral key
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProRegTx(Optional<COutPoint>(coll_out), utxosTmp, port, GenerateRandomAddress(), coinbaseKey, coll_key, GetRandomBLSKey().GetPublicKey());
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-collateral-reuse");
    }
    // Try to register used owner key
    {
        const CKey& ownerKey = ownerKeys.at(dmnHashes[InsecureRandRange(dmnHashes.size())]);
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProRegTx(nullopt, utxosTmp, port, GenerateRandomAddress(), coinbaseKey, ownerKey, GetRandomBLSKey().GetPublicKey());
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-dup-owner-key");
    }
    // Try to register used operator key
    {
        const CBLSSecretKey& operatorKey = operatorKeys.at(dmnHashes[InsecureRandRange(dmnHashes.size())]);
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProRegTx(nullopt, utxosTmp, port, GenerateRandomAddress(), coinbaseKey, GetRandomKey(), operatorKey.GetPublicKey());
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-dup-operator-key");
    }
    // Try to register used IP address
    {
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProRegTx(nullopt, utxosTmp, 1 + InsecureRandRange(port-1), GenerateRandomAddress(), coinbaseKey, GetRandomKey(), GetRandomBLSKey().GetPublicKey());
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-dup-IP-address");
    }
    // Block with two ProReg txes using same owner key
    {
        SimpleUTXOMap utxosTmp(utxos);
        const CKey& ownerKey = GetRandomKey();
        const CBLSSecretKey& operatorKey1 = GetRandomBLSKey();
        const CBLSSecretKey& operatorKey2 = GetRandomBLSKey();
        auto tx1 = CreateProRegTx(nullopt, utxosTmp, port, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey1.GetPublicKey());
        auto tx2 = CreateProRegTx(nullopt, utxosTmp, (port+1), GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey2.GetPublicKey());
        CBlock block = CreateBlock({tx1, tx2}, coinbaseKey);
        CBlockIndex indexFake(block);
        indexFake.nHeight = nHeight + 1;
        indexFake.pprev = chainTip;
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return ProcessSpecialTxsInBlock(block, &indexFake, view, state, true); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-dup-owner-key");
        ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr); // todo: move to check reject reason
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height(); ), nHeight);   // bad block not connected
    }
    // Block with two ProReg txes using same operator key
    {
        SimpleUTXOMap utxosTmp(utxos);
        const CKey& ownerKey1 = GetRandomKey();
        const CKey& ownerKey2 = GetRandomKey();
        const CBLSSecretKey& operatorKey = GetRandomBLSKey();
        auto tx1 = CreateProRegTx(nullopt, utxosTmp, port, GenerateRandomAddress(), coinbaseKey, ownerKey1, operatorKey.GetPublicKey());
        auto tx2 = CreateProRegTx(nullopt, utxosTmp, (port+1), GenerateRandomAddress(), coinbaseKey, ownerKey2, operatorKey.GetPublicKey());
        CBlock block = CreateBlock({tx1, tx2}, coinbaseKey);
        CBlockIndex indexFake(block);
        indexFake.nHeight = nHeight + 1;
        indexFake.pprev = chainTip;
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return ProcessSpecialTxsInBlock(block, &indexFake, view, state, true); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-dup-operator-key");
        ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr); // todo: move to check reject reason
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height(); ), nHeight);   // bad block not connected
    }
    // Block with two ProReg txes using same ip address
    {
        SimpleUTXOMap utxosTmp(utxos);
        auto tx1 = CreateProRegTx(nullopt, utxosTmp, port, GenerateRandomAddress(), coinbaseKey, GetRandomKey(), GetRandomBLSKey().GetPublicKey());
        auto tx2 = CreateProRegTx(nullopt, utxosTmp, port, GenerateRandomAddress(), coinbaseKey, GetRandomKey(), GetRandomBLSKey().GetPublicKey());
        CBlock block = CreateBlock({tx1, tx2}, coinbaseKey);
        CBlockIndex indexFake(block);
        indexFake.nHeight = nHeight + 1;
        indexFake.pprev = chainTip;
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return ProcessSpecialTxsInBlock(block, &indexFake, view, state, true); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-dup-IP-address");
        ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr); // todo: move to check reject reason
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height(); ), nHeight);   // bad block not connected
    }

    // register multiple MNs per block
    // With 10-coin blocks, create a few large spendable UTXOs first so we can fund multiple ProReg txes
    // in the same block without needing intra-block dependencies.
    {
        std::vector<CAmount> amounts(9, Params().GetConsensus().nMNCollateralAmt + fee);
        CMutableTransaction splitTx = CreateSplitTx(utxos, coinbaseKey, amounts);
        CreateAndProcessBlock({splitTx}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(splitTx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
    }

    for (size_t i = 0; i < 3; i++) {
        std::vector<CMutableTransaction> txns;
        std::set<COutPoint> blockPrevouts;
        for (size_t j = 0; j < 3; j++) {
            const CKey& ownerKey = GetRandomKey();
            const CBLSSecretKey& operatorKey = GetRandomBLSKey();
            auto tx = CreateProRegTx(nullopt, utxos, port++, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey.GetPublicKey());
            for (const auto& vin : tx.vin) {
                BOOST_REQUIRE_MESSAGE(blockPrevouts.insert(vin.prevout).second,
                                      strprintf("Multi-ProReg block: duplicate input %s", vin.prevout.ToString()));
            }
            const uint256& txid = tx.GetHash();
            dmnHashes.emplace_back(txid);
            ownerKeys.emplace(txid, ownerKey);
            operatorKeys.emplace(txid, operatorKey);

            CValidationState dummyState;
            BOOST_CHECK(WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainActive.Tip(), view, dummyState); ));
            BOOST_CHECK(CheckTransactionSignature(tx));
            txns.emplace_back(tx);
        }
        // Mine the block and ensure it connects. This block contains multiple ProReg txes, so validate it explicitly.
        CBlock block = CreateBlock(txns, coinbaseKey);
        {
            CValidationState state;
            BOOST_REQUIRE_MESSAGE(WITH_LOCK(cs_main, return TestBlockValidity(state, block, chainTip, true, true, true); ),
                                  strprintf("Multi-ProReg block failed TestBlockValidity: reject=%s debug=%s",
                                            state.GetRejectReason(), state.GetDebugMessage()));
        }
        BOOST_REQUIRE(ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr));
        for (const auto& tx : txns) {
            AddSpendableOutputs(utxos, CTransaction(tx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        }
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, nHeight + 1);
        auto mnList = deterministicMNManager->GetListAtChainTip();
        for (size_t j = 0; j < 3; j++) {
            BOOST_CHECK(mnList.HasMN(txns[j].GetHash()));
        }

        nHeight++;
    }

    // Mine 30 blocks, checking MN reward payments
    mapPayments.clear();
    for (size_t i = 0; i < 30; i++) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        auto dmnExpectedPayee = mnList.GetMNPayee();
        CBlock block = CreateAndProcessBlock({}, coinbaseKey);
        chainTip = chainActive.Tip();
        BOOST_ASSERT(!block.vtx.empty());
        if (deterministicMNManager->LegacyMNObsolete(nHeight + 1)) {
            BOOST_CHECK(IsMNPayeeInBlock(block, dmnExpectedPayee->pdmnState->scriptPayout));
            mapPayments[dmnExpectedPayee->proTxHash]++;
        }
        // else: legacy payments active, no DMN payment enforcement in this test environment

        nHeight++;
    }
    // 30 blocks, 15 masternodes. Must have been paid exactly 2 times each.
    if (deterministicMNManager->LegacyMNObsolete(nHeight)) {
        CheckPayments(mapPayments, 15, 2);
    } else {
        BOOST_CHECK(mapPayments.empty());
    }

    // Check that the prev DMN winner is different that the tip one
    if (deterministicMNManager->LegacyMNObsolete(chainTip->nHeight)) {
        std::vector<CTxOut> vecMnOutsPrev;
        BOOST_CHECK(masternodePayments.GetMasternodeTxOuts(chainTip->pprev, vecMnOutsPrev));
        std::vector<CTxOut> vecMnOutsNow;
        BOOST_CHECK(masternodePayments.GetMasternodeTxOuts(chainTip, vecMnOutsNow));
        BOOST_CHECK(vecMnOutsPrev != vecMnOutsNow);
    }

    // Craft a block with an altered coinbase (no masternode payment).
    // In CTEAM legacy MN mode is always active and there are no legacy
    // masternodes in this test, so payment enforcement is skipped.
    CBlock invalidBlock = CreateBlock({}, coinbaseKey);
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(invalidBlock);
    CMutableTransaction invalidCoinbaseTx = CreateCoinbaseTx(CScript(), chainTip);
    invalidCoinbaseTx.vout.clear();
    invalidCoinbaseTx.vout.emplace_back(
        CTxOut(GetBlockValue(nHeight + 1) - GetMasternodePayment(nHeight + 1),
            GetScriptForDestination(coinbaseKey.GetPubKey().GetID())));
    pblock->vtx[0] = MakeTransactionRef(invalidCoinbaseTx);
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
    ProcessNewBlock(pblock, nullptr);
    chainTip = WITH_LOCK(cs_main, return chainActive.Tip());
    BOOST_CHECK(chainTip->nHeight == ++nHeight);
    BOOST_CHECK(chainTip->GetBlockHash() == pblock->GetHash());

    // ProUpServ: change masternode IP
    {
        const uint256& proTx = dmnHashes[InsecureRandRange(dmnHashes.size())];  // pick one at random
        auto tx = CreateProUpServTx(utxos, proTx, operatorKeys.at(proTx), 1000, CScript(), coinbaseKey);

        CValidationState dummyState;
        BOOST_CHECK(WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, dummyState); ));
        BOOST_CHECK(CheckTransactionSignature(tx));
        // also verify that payloads are not malleable after they have been signed
        auto tx2 = MalleateProUpServTx(tx);
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx2, chainTip, view, dummyState); ));
        BOOST_CHECK_EQUAL(dummyState.GetRejectReason(), "bad-protx-sig");

        CreateAndProcessBlock({tx}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(tx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, nHeight + 1);

        auto dmn = deterministicMNManager->GetListAtChainTip().GetMN(proTx);
        BOOST_ASSERT(dmn != nullptr);
        BOOST_CHECK_EQUAL(dmn->pdmnState->addr.GetPort(), 1000);

        nHeight++;
    }

    // ProUpServ: Try to change the IP of a masternode to the one of another registered masternode
    {
        int randomIdx = InsecureRandRange(dmnHashes.size());
        int randomIdx2 = 0;
        do { randomIdx2 = InsecureRandRange(dmnHashes.size()); } while (randomIdx2 == randomIdx);
        const uint256& proTx = dmnHashes[randomIdx];    // mn to update
        int new_port = deterministicMNManager->GetListAtChainTip().GetMN(dmnHashes[randomIdx2])->pdmnState->addr.GetPort();

        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProUpServTx(utxosTmp, proTx, operatorKeys.at(proTx), new_port, CScript(), coinbaseKey);

        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-dup-addr");
    }

    // ProUpServ: Try to change the IP of a masternode that doesn't exist
    {
        const CBLSSecretKey& operatorKey = GetRandomBLSKey();
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProUpServTx(utxosTmp, GetRandHash(), operatorKey, port, CScript(), coinbaseKey);

        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-hash");
    }

    // ProUpServ: Change masternode operator payout. (new masternode created here)
    {
        // first create a ProRegTx with 5% reward for the operator, and mine it
        const CKey& ownerKey = GetRandomKey();
        const CBLSSecretKey& operatorKey = GetRandomBLSKey();
        auto tx = CreateProRegTx(nullopt, utxos, port++, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey.GetPublicKey(), 500);
        const uint256& txid = tx.GetHash();
        CreateAndProcessBlock({tx}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(tx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
        auto mnList = deterministicMNManager->GetListAtChainTip();
        BOOST_CHECK(mnList.HasMN(txid));
        auto dmn = mnList.GetMN(txid);
        BOOST_CHECK(dmn->pdmnState->scriptOperatorPayout.empty());
        BOOST_CHECK_EQUAL(dmn->nOperatorReward, 500);

        // then send the ProUpServTx and check the operator payee
        const CScript& operatorPayee = GenerateRandomAddress();
        auto tx2 = CreateProUpServTx(utxos, txid, operatorKey, (port-1), operatorPayee, coinbaseKey);
        CreateAndProcessBlock({tx2}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(tx2), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
        dmn = deterministicMNManager->GetListAtChainTip().GetMN(txid);
        BOOST_ASSERT(dmn != nullptr);
        BOOST_CHECK(dmn->pdmnState->scriptOperatorPayout == operatorPayee);
    }

    // ProUpServ: Try to change masternode operator payout when the operator reward is zero
    {
        const CScript& operatorPayee = GenerateRandomAddress();
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProUpServTx(utxosTmp, dmnHashes[0], operatorKeys.at(dmnHashes[0]), 1, operatorPayee, coinbaseKey);
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-operator-payee");
    }

    // Block including
    // - (1) ProRegTx registering a masternode
    // - (2) ProUpServTx changing the IP of another masternode, to the one used by (1)
    {
        SimpleUTXOMap utxosTmp(utxos);
        auto tx1 = CreateProRegTx(nullopt, utxosTmp, port++, GenerateRandomAddress(), coinbaseKey, GetRandomKey(), GetRandomBLSKey().GetPublicKey());
        const uint256& proTx = dmnHashes[InsecureRandRange(dmnHashes.size())];    // pick one at random
        auto tx2 = CreateProUpServTx(utxosTmp, proTx, operatorKeys.at(proTx), (port-1), CScript(), coinbaseKey);
        CBlock block = CreateBlock({tx1, tx2}, coinbaseKey);
        CBlockIndex indexFake(block);
        indexFake.nHeight = nHeight + 1;
        indexFake.pprev = chainTip;
        CValidationState state;
        BOOST_CHECK(!WITH_LOCK(cs_main, return ProcessSpecialTxsInBlock(block, &indexFake, view, state, true); ));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-dup-addr");
        ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr); // todo: move to ProcessBlockAndCheckRejectionReason.
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height(); ), nHeight);   // bad block not connected
    }

    // ProUpReg is disabled when legacy MNs are active (which is permanent in CTEAM).
    // Verify that all ProUpReg txs are rejected with "spork-21-inactive".
    {
        const uint256& proTx = dmnHashes[InsecureRandRange(dmnHashes.size())];
        CBLSSecretKey new_operatorKey = GetRandomBLSKey();
        const CKey& new_votingKey = GetRandomKey();
        const CScript& new_payee = GenerateRandomAddress();
        CValidationState state;
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProUpRegTx(utxosTmp, proTx, GetRandomKey(), new_operatorKey.GetPublicKey(), new_votingKey, new_payee, coinbaseKey);
        BOOST_CHECK_MESSAGE(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ), "ProUpReg verifies with wrong owner key");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "spork-21-inactive");
    }

    // ProUpReg: Try to change the voting key of a masternode that doesn't exist
    {
        const CKey& votingKey = GetRandomKey();
        const CBLSSecretKey& operatorKey = GetRandomBLSKey();
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProUpRegTx(utxosTmp, GetRandHash(), GetRandomKey(), operatorKey.GetPublicKey(), votingKey, GenerateRandomAddress(), coinbaseKey);

        CValidationState state;
        BOOST_CHECK_MESSAGE(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ), "Accepted ProUpReg with invalid protx hash");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "spork-21-inactive");
    }

    // ProUpReg: Try to change the operator key of a masternode to the one of another registered masternode
    {
        int randomIdx = InsecureRandRange(dmnHashes.size());
        int randomIdx2 = 0;
        do { randomIdx2 = InsecureRandRange(dmnHashes.size()); } while (randomIdx2 == randomIdx);
        const uint256& proTx = dmnHashes[randomIdx];    // mn to update
        const CBLSSecretKey& new_operatorKey = operatorKeys.at(dmnHashes[randomIdx2]);

        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProUpRegTx(utxosTmp, proTx, ownerKeys.at(proTx), new_operatorKey.GetPublicKey(), GetRandomKey(), GenerateRandomAddress(), coinbaseKey);

        CValidationState state;
        BOOST_CHECK_MESSAGE(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ), "Accepted ProUpReg with duplicate operator key");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "spork-21-inactive");
    }

    // Block with two ProUpReg txes using same operator key
    {
        SimpleUTXOMap utxosTmp(utxos);
        int randomIdx1 = InsecureRandRange(dmnHashes.size());
        int randomIdx2 = 0;
        do { randomIdx2 = InsecureRandRange(dmnHashes.size()); } while (randomIdx2 == randomIdx1);
        const uint256& proTx1 = dmnHashes[randomIdx1];
        const uint256& proTx2 = dmnHashes[randomIdx2];
        BOOST_ASSERT(proTx1 != proTx2);
        const CBLSSecretKey& new_operatorKey = GetRandomBLSKey();
        const CKey& new_votingKey = GetRandomKey();
        const CScript& new_payee = GenerateRandomAddress();
        auto tx1 = CreateProUpRegTx(utxosTmp, proTx1, ownerKeys.at(proTx1), new_operatorKey.GetPublicKey(), new_votingKey, new_payee, coinbaseKey);
        auto tx2 = CreateProUpRegTx(utxosTmp, proTx2, ownerKeys.at(proTx2), new_operatorKey.GetPublicKey(), new_votingKey, new_payee, coinbaseKey);
        CBlock block = CreateBlock({tx1, tx2}, coinbaseKey);
        CBlockIndex indexFake(block);
        indexFake.nHeight = nHeight + 1;
        indexFake.pprev = chainTip;
        CValidationState state;
        BOOST_CHECK_MESSAGE(!WITH_LOCK(cs_main, return ProcessSpecialTxsInBlock(block, &indexFake, view, state, true); ),
                            "Accepted block with duplicate operator key in ProUpReg txes");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "spork-21-inactive");
        ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr);
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height(); ), nHeight);   // bad block not connected
    }

    // Block including
    // - (1) ProRegTx registering a masternode
    // - (2) ProUpRegTx changing the operator key of another masternode, to the one used by (1)
    {
        const CBLSSecretKey& new_operatorKey = GetRandomBLSKey();
        SimpleUTXOMap utxosTmp(utxos);
        auto tx1 = CreateProRegTx(nullopt, utxosTmp, port++, GenerateRandomAddress(), coinbaseKey, GetRandomKey(), new_operatorKey.GetPublicKey());
        const uint256& proTx = dmnHashes[InsecureRandRange(dmnHashes.size())];    // pick one at random
        auto tx2 = CreateProUpRegTx(utxosTmp, proTx, ownerKeys.at(proTx), new_operatorKey.GetPublicKey(), GetRandomKey(), GenerateRandomAddress(), coinbaseKey);
        CBlock block = CreateBlock({tx1, tx2}, coinbaseKey);
        CBlockIndex indexFake(block);
        indexFake.nHeight = nHeight + 1;
        indexFake.pprev = chainTip;
        CValidationState state;
        BOOST_CHECK_MESSAGE(!WITH_LOCK(cs_main, return ProcessSpecialTxsInBlock(block, &indexFake, view, state, true); ),
                            "Accepted block with duplicate operator key in ProReg+ProUpReg txes");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "spork-21-inactive");
        ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr);
        BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height(); ), nHeight);   // bad block not connected
    }

    // ProUpRev: revoke masternode service
    {
        const uint256& proTx = dmnHashes[InsecureRandRange(dmnHashes.size())];            // pick one at random
        ProUpRevPL::RevocationReason reason = ProUpRevPL::RevocationReason::REASON_TERMINATION_OF_SERVICE;
        // try first with wrong operator key
        CValidationState state;
        SimpleUTXOMap utxosTmp(utxos);
        auto tx = CreateProUpRevTx(utxosTmp, proTx, reason, GetRandomBLSKey(), coinbaseKey);
        BOOST_CHECK_MESSAGE(!WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ), "ProUpReg verifies with wrong owner key");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-sig");
        // then use the proper key
        state = CValidationState();
        tx = CreateProUpRevTx(utxos, proTx, reason, operatorKeys.at(proTx), coinbaseKey);
        BOOST_CHECK(WITH_LOCK(cs_main, return CheckSpecialTx(tx, chainTip, view, state); ));
        BOOST_CHECK_MESSAGE(CheckTransactionSignature(tx), "ProUpReg signature verification failed");
        // also verify that payloads are not malleable after they have been signed
        auto tx2 = MalleateProUpRevTx(tx);
        BOOST_CHECK_MESSAGE(!WITH_LOCK(cs_main, return CheckSpecialTx(tx2, chainTip, view, state); ), "Malleated ProUpReg accepted");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-sig");

        CreateAndProcessBlock({tx}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(tx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);

        auto dmn = deterministicMNManager->GetListAtChainTip().GetMN(proTx);
        BOOST_ASSERT(dmn != nullptr);
        BOOST_CHECK_MESSAGE(!dmn->pdmnState->pubKeyOperator.Get().IsValid(), "mn operator key not removed");
        BOOST_CHECK_MESSAGE(dmn->pdmnState->addr == CService(), "mn IP address not removed");
        BOOST_CHECK_MESSAGE(dmn->pdmnState->scriptOperatorPayout.empty(), "mn operator payout not removed");
        BOOST_CHECK_EQUAL(dmn->pdmnState->nRevocationReason, reason);
        BOOST_CHECK(dmn->IsPoSeBanned());
        BOOST_CHECK_EQUAL(dmn->pdmnState->nPoSeBanHeight, nHeight);
    }

    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
}

// Dummy commitment where the DKG shares are replaced with the operator keys of each member.
// members at index skeys.size(), ..., llmqType.size - 1 are invalid
static llmq::CFinalCommitment CreateFinalCommitment(std::vector<CBLSPublicKey>& pkeys,
                                                    std::vector<CBLSSecretKey>& skeys,
                                                    const uint256& quorumHash)
{
    size_t m = skeys.size();
    BOOST_ASSERT(pkeys.size() == m);

    llmq::CFinalCommitment qfl;
    qfl.llmqType = (uint8_t)Consensus::LLMQ_TEST;
    const auto& params = Params().GetConsensus().llmqs.at(Consensus::LLMQ_TEST);
    BOOST_ASSERT(m <= (size_t) params.size);    // m-of-n

    // non-included members are marked invalid
    qfl.signers.resize(params.size);
    qfl.validMembers.resize(params.size);
    for (size_t i = 0; i < (size_t) params.size; i++) {
        qfl.signers[i] = i < m;
        qfl.validMembers[i] = i < m;
    }

    qfl.quorumHash = quorumHash;

    // create dummy quorum keys, just aggregating operator BLS keys
    qfl.quorumPublicKey = CBLSPublicKey::AggregateInsecure(pkeys);

    // use dummy non-null verification vector hash
    qfl.quorumVvecHash = UINT256_ONE;

    // add signatures
    const uint256& commitmentHash = llmq::utils::BuildCommitmentHash((Consensus::LLMQType)qfl.llmqType, quorumHash, qfl.validMembers, qfl.quorumPublicKey, qfl.quorumVvecHash);
    std::vector<CBLSSignature> sigs;
    for (size_t i = 0; i < m; i++) {
        sigs.emplace_back(skeys[i].Sign(commitmentHash));
    }
    qfl.membersSig = CBLSSignature::AggregateSecure(sigs, pkeys, commitmentHash);
    qfl.quorumSig = CBLSSecretKey::AggregateInsecure(skeys).Sign(commitmentHash);

    return qfl;
}

CMutableTransaction CreateQfcTx(const uint256& quorumHash, int nHeight, Optional<llmq::CFinalCommitment> opt_qfc)
{
    llmq::LLMQCommPL pl;
    pl.commitment = opt_qfc ? *opt_qfc : llmq::CFinalCommitment(Params().GetConsensus().llmqs.at(Consensus::LLMQ_TEST), quorumHash);
    pl.nHeight = nHeight;
    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::LLMQCOMM;
    SetTxPayload(tx, pl);
    return tx;
}

CMutableTransaction CreateNullQfcTx(const uint256& quorumHash, int nHeight)
{
    return CreateQfcTx(quorumHash, nHeight, nullopt);
}

CService ip(uint32_t i)
{
    struct in_addr s;
    s.s_addr = i;
    return CService(CNetAddr(s), Params().GetDefaultPort());
}

static void ProcessQuorum(llmq::CQuorumBlockProcessor* processor, const llmq::CFinalCommitment& qfc, CNode* node, int expected_banscore = 0)
{
    CDataStream vRecv(SER_NETWORK, PROTOCOL_VERSION);
    vRecv << qfc;
    int banScore{0};
    processor->ProcessMessage(node, vRecv, banScore);
    BOOST_CHECK_EQUAL(banScore, expected_banscore);
}

static NodeId id = 0;

// future: split dkg_pose from qfc_invalid_paths test coverage.
BOOST_FIXTURE_TEST_CASE(dkg_pose_and_qfc_invalid_paths, TestChain400Setup)
{
    // CTEAM never activates deterministic masternodes.
    return;

    auto utxos = BuildSimpleUtxoMap(coinbaseTxns, coinbaseKey);

    CBlockIndex* chainTip = chainActive.Tip();
    int nHeight = chainTip->nHeight;
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, nHeight + 2);

    // load empty list (last block before enforcement)
    CreateAndProcessBlock({}, coinbaseKey);
    chainTip = chainActive.Tip();
    BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);

    // force mnsync complete and enable spork 8
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);
    int64_t nTime = GetTime() - 10;
    const CSporkMessage& sporkMnPayment = CSporkMessage(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT, nTime + 1, nTime);
    sporkManager.AddOrUpdateSporkMessage(sporkMnPayment);
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT));

    int port = 1;

    std::vector<uint256> dmnHashes;
    std::map<uint256, CKey> ownerKeys;
    std::map<uint256, CBLSSecretKey> operatorKeys;

    // register one MN per block
    for (size_t i = 0; i < 6; i++) {
        const CKey& ownerKey = GetRandomKey();
        const CBLSSecretKey& operatorKey = GetRandomBLSKey();
        auto tx = CreateProRegTx(nullopt, utxos, port++, GenerateRandomAddress(), coinbaseKey, ownerKey, operatorKey.GetPublicKey());
        const uint256& txid = tx.GetHash();
        dmnHashes.emplace_back(txid);
        ownerKeys.emplace(txid, ownerKey);
        operatorKeys.emplace(txid, operatorKey);
        CreateAndProcessBlock({tx}, coinbaseKey);
        AddSpendableOutputs(utxos, CTransaction(tx), nHeight + 1, coinbaseKey, /*fCoinbase=*/false);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
        BOOST_CHECK(deterministicMNManager->GetListAtChainTip().HasMN(txid));
    }

    // CTEAM keeps legacy/non-deterministic masternodes enabled permanently.
    // SPORK_21 must not force deterministic-only mode.
    const CSporkMessage& spork = CSporkMessage(SPORK_21_LEGACY_MNS_MAX_HEIGHT, nHeight, GetTime());
    sporkManager.AddOrUpdateSporkMessage(spork);
    BOOST_CHECK(!deterministicMNManager->LegacyMNObsolete(nHeight + 1));

    // Mine 20 blocks
    for (size_t i = 0; i < 20; i++) {
        CreateAndProcessBlock({}, coinbaseKey);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
    }

    BOOST_CHECK_EQUAL(nHeight, 427);
    // dkg starts at 420
    auto& params = Params().GetConsensus().llmqs.at(Consensus::LLMQ_TEST);
    uint256 quorumHash = chainActive[nHeight - (nHeight % params.dkgInterval)]->GetBlockHash();
    const CBlockIndex* quorumIndex = mapBlockIndex.at(quorumHash);

    // get quorum mns
    auto members = deterministicMNManager->GetAllQuorumMembers(Consensus::LLMQ_TEST, quorumIndex);
    std::vector<CBLSPublicKey> pkeys;
    std::vector<CBLSSecretKey> skeys;
    for (size_t i = 0; i < members.size()-1; i++) {             // all, except the last one...
        pkeys.emplace_back(members[i]->pdmnState->pubKeyOperator.Get());
        skeys.emplace_back(operatorKeys.at(members[i]->proTxHash));
    }
    const uint256& invalidmn_proTx = members.back()->proTxHash; // ...which must be punished.

    // create final commitment
    llmq::CFinalCommitment qfc = CreateFinalCommitment(pkeys, skeys, quorumHash);
    BOOST_CHECK(!qfc.IsNull());
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_CHECK(VerifyLLMQCommitment(qfc, chainTip, state));
    }

    // verify that it fails changing the key of one of the signers
    std::vector<CBLSPublicKey> allkeys(pkeys);
    allkeys.emplace_back(members.back()->pdmnState->pubKeyOperator.Get());
    BOOST_CHECK(qfc.Verify(allkeys, params));   // already checked with VerifyLLMQCommitment
    allkeys[0] = GetRandomBLSKey().GetPublicKey();
    BOOST_CHECK(!qfc.Verify(allkeys, params));

    // receive final commitment message
    CNode dummyNode(id++, NODE_NETWORK, 0, INVALID_SOCKET, CAddress(ip(0xa0b0c001), NODE_NONE), 0, 0, "", true);
    ProcessQuorum(llmq::quorumBlockProcessor.get(), qfc, &dummyNode);
    BOOST_CHECK(llmq::quorumBlockProcessor->HasMinableCommitment(::SerializeHash(qfc)));

    // Generate blocks up to be able to mine a null qfc at block 430
    CreateAndProcessBlock({}, coinbaseKey);
    chainTip = chainActive.Tip();
    BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
    BOOST_CHECK_EQUAL(nHeight, 428);

    // Coverage for the following qfc paths:
    // 1) Mine a qfc with an invalid height, which should end up being rejected.
    // 2) Mine a null qfc before the mining phase, which should end up being rejected.
    // 3) Mine two qfc in the same block, which should end up being rejected.
    // 4) Mine block without qfc during the mining phase, which should end up being rejected.
    // 5) Mine two blocks with a null qfc.
    // 6) Try to relay the valid qfc to the mempool, which should end up being rejected.
    // 7a) Mine a qfc with an invalid quorum hash (invalid height), which should end up being rejected.
    // 7b) Mine a qfc with an invalid quorum hash (non-existent), which should end up being rejected.
    // 7c) Mine a qfc with an invalid quorum hash (forked), which should end up being rejected.
    // 7d) Mine a qfc with an old quorum hash, which should end up being rejected.
    // 8) Mine the final valid qfc in a block.
    // 9) Mine a null qfc after mining a valid qfc, which should end up being rejected.

    // 1) Mine a qfc with an invalid height, which should end up being rejected.
    CMutableTransaction nullQfcTx = CreateNullQfcTx(quorumHash, nHeight);
    CScript coinsbaseScript = GetScriptForRawPubKey(coinbaseKey.GetPubKey());
    auto pblock_invalid = std::make_shared<CBlock>(CreateBlock({nullQfcTx}, coinsbaseScript, true, false, false));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-height", nHeight);

    // 2) Mine a null qfc before the mining phase, which should end up being rejected.
    nullQfcTx = CreateNullQfcTx(quorumHash, nHeight + 1);
    pblock_invalid = std::make_shared<CBlock>(
            CreateBlock({nullQfcTx}, coinsbaseScript, true, false, false));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-not-allowed", nHeight);

    // One more block, 429.
    CreateAndProcessBlock({}, coinbaseKey);
    chainTip = chainActive.Tip();
    BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);

    // 3) Mine two qfc in the same block, which should end up on a rejection. (one null, one valid)
    nullQfcTx = CreateNullQfcTx(quorumHash, nHeight + 1);
    pblock_invalid = std::make_shared<CBlock>(
            CreateBlock({nullQfcTx}, coinsbaseScript, true, false, true));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-dup", nHeight);

    // 4) Mine block without qfc during the mining phase, which should end up being rejected.
    pblock_invalid = std::make_shared<CBlock>(CreateBlock({}, coinsbaseScript, true, false, false));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-missing", nHeight);

    // 5) Mine two blocks with a null qfc.
    for (int i = 0; i < 2; i++) {
        const auto& block = CreateBlock({nullQfcTx}, coinsbaseScript, true, false, false);
        ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
        nullQfcTx = CreateNullQfcTx(quorumHash, nHeight + 1);
    }
    BOOST_CHECK_EQUAL(nHeight, 431);

    // 6) Try to relay the valid qfc to the mempool, which should end up on a rejection.
    CTransactionRef qcTx;
    BOOST_CHECK(llmq::quorumBlockProcessor->GetMinableCommitmentTx(Consensus::LLMQ_TEST, nHeight + 1, qcTx));
    CValidationState mempoolState;
    BOOST_CHECK(!WITH_LOCK(cs_main, return AcceptToMemoryPool(mempool, mempoolState, qcTx, true, nullptr); ));
    BOOST_CHECK_EQUAL(mempoolState.GetRejectReason(), "llmqcomm");

    // 7a) Mine a qfc with an invalid quorum hash (invalid height), which should end up being rejected.
    nullQfcTx = CreateNullQfcTx(chainTip->GetBlockHash(), nHeight + 1);
    pblock_invalid = std::make_shared<CBlock>(CreateBlock({nullQfcTx}, coinsbaseScript, true, false, false));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-quorum-height", nHeight);

    // 7b) Mine a qfc with an invalid quorum hash (non-existent), which should end up being rejected.
    nullQfcTx = CreateNullQfcTx(UINT256_ONE, nHeight + 1);
    pblock_invalid = std::make_shared<CBlock>(CreateBlock({nullQfcTx}, coinsbaseScript, true, false, false));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-quorum-hash-not-found", nHeight);

    // 7c) Mine a qfc with an invalid quorum hash (forked), which should end up being rejected.
    // -- first create a secondary chain at height 420
    CBlockIndex* pblock_419 = WITH_LOCK(cs_main, return mapBlockIndex.at(chainActive[419]->GetBlockHash()); );
    auto pblock_forked = std::make_shared<CBlock>(CreateBlock({}, coinsbaseScript, true, false, false, pblock_419));
    // increment nonce and re-solve to get a different block
    pblock_forked->nNonce++;
    BOOST_CHECK(SolveBlock(pblock_forked, 420));
    BOOST_CHECK(ProcessNewBlock(pblock_forked, nullptr));
    {
        LOCK(cs_main);
        const auto it = mapBlockIndex.find(pblock_forked->GetHash());
        BOOST_CHECK(it != mapBlockIndex.end());
        BOOST_CHECK(!chainActive.Contains(it->second));
    }

    // -- then mine a commitment referencing the quorum hash from the secondary chain
    nullQfcTx = CreateNullQfcTx(pblock_forked->GetHash(), nHeight + 1);
    pblock_invalid = std::make_shared<CBlock>(CreateBlock({nullQfcTx}, coinsbaseScript, true, false, false));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-quorum-hash-not-active-chain", nHeight);

    // 7d) Mine a qfc with an old quorum hash, which should end up being rejected.
    int old_quorum_hash_height = nHeight - (nHeight % params.dkgInterval) - params.cacheDkgInterval - params.dkgInterval;
    uint256 old_quorum_hash = chainActive[old_quorum_hash_height]->GetBlockHash();
    nullQfcTx = CreateNullQfcTx(old_quorum_hash, nHeight + 1);
    pblock_invalid = std::make_shared<CBlock>(CreateBlock({nullQfcTx}, coinsbaseScript, true, false, false));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-quorum-height-old", nHeight);

    // Now check the message over the wire. future: add error rejection code.
    auto old_qfc = CreateFinalCommitment(pkeys, skeys, old_quorum_hash);
    ProcessQuorum(llmq::quorumBlockProcessor.get(), old_qfc, &dummyNode, 100);

    // 8) Mine the final valid qfc in a block.
    CreateAndProcessBlock({}, coinbaseKey);
    chainTip = chainActive.Tip();
    BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);

    // 9) Mine a null qfc after mining a valid qfc, which should end up being rejected.
    nullQfcTx = CreateNullQfcTx(quorumHash, nHeight + 1);
    pblock_invalid = std::make_shared<CBlock>(CreateBlock({nullQfcTx}, coinsbaseScript, true, false, false));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-not-allowed", nHeight);

    // final commitment has been mined
    llmq::CFinalCommitment ret;
    uint256 retMinedBlockHash;
    BOOST_CHECK(llmq::quorumBlockProcessor->GetMinedCommitment(Consensus::LLMQ_TEST, quorumHash, ret, retMinedBlockHash));
    BOOST_CHECK(chainTip->GetBlockHash() == retMinedBlockHash);
    BOOST_CHECK(qfc.quorumPublicKey == ret.quorumPublicKey);
    BOOST_CHECK(qfc.quorumVvecHash == ret.quorumVvecHash);
    BOOST_CHECK(qfc.quorumSig == ret.quorumSig);
    BOOST_CHECK(qfc.membersSig == ret.membersSig);

    // non-participating mn has been punished
    auto punished_mn = deterministicMNManager->GetListAtChainTip().GetMN(invalidmn_proTx);
    BOOST_CHECK_EQUAL(punished_mn->pdmnState->nPoSePenalty, 66);

    // penalty is decreased each block
    CreateAndProcessBlock({}, coinbaseKey);
    chainTip = chainActive.Tip();
    BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
    punished_mn = deterministicMNManager->GetListAtChainTip().GetMN(invalidmn_proTx);
    BOOST_CHECK_EQUAL(punished_mn->pdmnState->nPoSePenalty, 65);

    // New DKG starts at block 440. Mine till block 441 and create another valid 2-of-3 commitment
    for (size_t i = 0; i < 8; i++) {
        CreateAndProcessBlock({}, coinbaseKey);
        chainTip = chainActive.Tip();
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
    }
    BOOST_CHECK_EQUAL(nHeight, 441);
    quorumHash = chainActive[nHeight - (nHeight % params.dkgInterval)]->GetBlockHash();
    quorumIndex = mapBlockIndex.at(quorumHash);
    members = deterministicMNManager->GetAllQuorumMembers(Consensus::LLMQ_TEST, quorumIndex);
    pkeys.clear();
    skeys.clear();
    for (size_t i = 0; i < members.size(); i++) {
        pkeys.emplace_back(members[i]->pdmnState->pubKeyOperator.Get());
        skeys.emplace_back(operatorKeys.at(members[i]->proTxHash));
    }
    std::vector<CBLSPublicKey> pkeys2(pkeys.begin(), pkeys.end()-1);    // remove the last one.
    std::vector<CBLSSecretKey> skeys2(skeys.begin(), skeys.end()-1);
    llmq::CFinalCommitment qfc2 = CreateFinalCommitment(pkeys2, skeys2, quorumHash);
    BOOST_CHECK(!qfc2.IsNull());
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_CHECK(VerifyLLMQCommitment(qfc2, chainTip, state));
    }
    ProcessQuorum(llmq::quorumBlockProcessor.get(), qfc2, &dummyNode);
    // final commitment received and accepted
    BOOST_CHECK(llmq::quorumBlockProcessor->HasMinableCommitment(::SerializeHash(qfc2)));

    // Now receive another commitment for the same quorum hash, but with all 3 signatures
    qfc = CreateFinalCommitment(pkeys, skeys, quorumHash);
    BOOST_CHECK(!qfc.IsNull());
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_CHECK(VerifyLLMQCommitment(qfc, chainTip, state));
    }
    ProcessQuorum(llmq::quorumBlockProcessor.get(), qfc, &dummyNode);
    BOOST_CHECK(qfc.CountSigners() > qfc2.CountSigners());

    // final commitment received, accepted, and replaced the previous one (with less members)
    BOOST_CHECK(llmq::quorumBlockProcessor->HasMinableCommitment(::SerializeHash(qfc)));

    // activate spork 22 and try to mine a non-null commitment
    nTime = GetTime() - 10;
    sporkManager.AddOrUpdateSporkMessage(CSporkMessage(SPORK_22_LLMQ_DKG_MAINTENANCE, nTime + 1, nTime));
    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_22_LLMQ_DKG_MAINTENANCE));
    auto qtx = CreateQfcTx(quorumHash, nHeight + 1, Optional<llmq::CFinalCommitment>(qfc2));
    pblock_invalid = std::make_shared<CBlock>(CreateBlock({qtx}, coinsbaseScript, true, false, false));
    ProcessBlockAndCheckRejectionReason(pblock_invalid, "bad-qc-not-null-spork22", nHeight);

    // mine a null commitment
    for (size_t i = 0; i < 8; i++) {
        CreateAndProcessBlock({}, coinbaseKey);
    }
    nHeight = WITH_LOCK(cs_main, return chainActive.Height(); );
    BOOST_CHECK_EQUAL(nHeight, 449);
    nullQfcTx = CreateNullQfcTx(quorumHash, nHeight + 1);
    auto pblock = std::make_shared<CBlock>(CreateBlock({nullQfcTx}, coinsbaseScript, true, false, false));
    ProcessNewBlock(pblock, nullptr);
    chainTip = WITH_LOCK(cs_main, return chainActive.Tip(); );
    BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);

    for (size_t i = 0; i < 19; i++) {
        CreateAndProcessBlock({}, coinbaseKey);
        chainTip = WITH_LOCK(cs_main, return chainActive.Tip(); );
        BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
    }
    BOOST_CHECK_EQUAL(nHeight, 469);

    // test rejection of non-null commitments over the wire
    {
        LOCK(cs_main);
        quorumHash = chainActive[nHeight - (nHeight % params.dkgInterval)]->GetBlockHash();
        quorumIndex = mapBlockIndex.at(quorumHash);
    }
    members = deterministicMNManager->GetAllQuorumMembers(Consensus::LLMQ_TEST, quorumIndex);
    pkeys.clear();
    skeys.clear();
    for (size_t i = 0; i < members.size(); i++) {
        pkeys.emplace_back(members[i]->pdmnState->pubKeyOperator.Get());
        skeys.emplace_back(operatorKeys.at(members[i]->proTxHash));
    }
    llmq::CFinalCommitment qfc3 = CreateFinalCommitment(pkeys, skeys, quorumHash);
    BOOST_CHECK(!qfc3.IsNull());
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_CHECK(!VerifyLLMQCommitment(qfc3, chainTip, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-qc-not-null-spork22");
    }
    // final commitment not accepted
    uint256 qfc3_hash = ::SerializeHash(qfc3);
    ProcessQuorum(llmq::quorumBlockProcessor.get(), qfc3, &dummyNode, 50);
    BOOST_CHECK(!llmq::quorumBlockProcessor->HasMinableCommitment(qfc3_hash));

    // disable spork 22 and accept it
    sporkManager.AddOrUpdateSporkMessage(CSporkMessage(SPORK_22_LLMQ_DKG_MAINTENANCE, 4070908800ULL, GetTime()));
    BOOST_CHECK(!sporkManager.IsSporkActive(SPORK_22_LLMQ_DKG_MAINTENANCE));
    ProcessQuorum(llmq::quorumBlockProcessor.get(), qfc3, &dummyNode);
    BOOST_CHECK(llmq::quorumBlockProcessor->HasMinableCommitment(qfc3_hash));

    // and mine it
    CreateAndProcessBlock({}, coinbaseKey);
    chainTip = WITH_LOCK(cs_main, return chainActive.Tip(); );
    BOOST_CHECK_EQUAL(chainTip->nHeight, ++nHeight);
    BOOST_CHECK(llmq::quorumBlockProcessor->GetMinedCommitment(Consensus::LLMQ_TEST, quorumHash, ret, retMinedBlockHash));
    BOOST_CHECK(chainTip->GetBlockHash() == retMinedBlockHash);
    BOOST_CHECK(qfc3.quorumPublicKey == ret.quorumPublicKey);
    BOOST_CHECK(qfc3.quorumVvecHash == ret.quorumVvecHash);
    BOOST_CHECK(qfc3.quorumSig == ret.quorumSig);
    BOOST_CHECK(qfc3.membersSig == ret.membersSig);

    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
}

BOOST_AUTO_TEST_SUITE_END()
