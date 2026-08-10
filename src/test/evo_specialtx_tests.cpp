// Copyright (c) 2021-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "test/data/specialtx_invalid.json.h"
#include "test/data/specialtx_valid.json.h"

#include "consensus/validation.h"
#include "core_io.h"
#include "evo/providertx.h"
#include "evo/governancevotetx.h"
#include "evo/specialtx_validation.h"
#include "llmq/quorums_commitment.h"
#include "messagesigner.h"
#include "netbase.h"
#include "primitives/transaction.h"

#include <boost/test/unit_test.hpp>

extern UniValue read_json(const std::string& jsondata);

BOOST_FIXTURE_TEST_SUITE(evo_specialtx_tests, TestingSetup)

static CKey GetRandomKey()
{
    CKey key;
    key.MakeNewKey(true);
    return key;
}

static CKeyID GetRandomKeyID()
{
    return GetRandomKey().GetPubKey().GetID();
}

static CBLSPublicKey GetRandomBLSKey()
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    return sk.GetPublicKey();
}

static CScript GetRandomScript()
{
    return GetScriptForDestination(GetRandomKeyID());
}

static ProRegPL GetRandomProRegPayload()
{
    ProRegPL pl;
    pl.collateralOutpoint.hash = GetRandHash();
    pl.collateralOutpoint.n = InsecureRandBits(2);
    BOOST_CHECK(Lookup("57.12.210.11:39616", pl.addr, Params().GetDefaultPort(), false));
    pl.keyIDOwner = GetRandomKeyID();
    pl.pubKeyOperator = GetRandomBLSKey();
    pl.keyIDVoting = GetRandomKeyID();
    pl.scriptPayout = GetRandomScript();
    pl.nOperatorReward = InsecureRandRange(10000);
    pl.scriptOperatorPayout = GetRandomScript();
    pl.inputsHash = GetRandHash();
    pl.vchSig = InsecureRandBytes(63);
    return pl;
}

static ProUpServPL GetRandomProUpServPayload()
{
    ProUpServPL pl;
    pl.proTxHash = GetRandHash();
    BOOST_CHECK(Lookup("127.0.0.1:39616", pl.addr, Params().GetDefaultPort(), false));
    pl.scriptOperatorPayout = GetRandomScript();
    pl.inputsHash = GetRandHash();
    pl.sig.SetByteVector(InsecureRandBytes(BLS_CURVE_SIG_SIZE));
    return pl;
}

static ProUpRegPL GetRandomProUpRegPayload()
{
    ProUpRegPL pl;
    pl.proTxHash = GetRandHash();
    pl.pubKeyOperator = GetRandomBLSKey();
    pl.keyIDVoting = GetRandomKeyID();
    pl.scriptPayout = GetRandomScript();
    pl.inputsHash = GetRandHash();
    pl.vchSig = InsecureRandBytes(63);
    return pl;
}

static ProUpRevPL GetRandomProUpRevPayload()
{
    ProUpRevPL pl;
    pl.proTxHash = GetRandHash();
    pl.nReason = InsecureRand16();
    pl.inputsHash = GetRandHash();
    pl.sig.SetByteVector(InsecureRandBytes(BLS_CURVE_SIG_SIZE));
    return pl;
}

llmq::CFinalCommitment GetRandomLLMQCommitment()
{
    llmq::CFinalCommitment fc;
    fc.nVersion = InsecureRand16();
    fc.llmqType = InsecureRandBits(8);
    fc.quorumHash = GetRandHash();
    int vecsize = InsecureRandRange(500);
    for (int i = 0; i < vecsize; i++) {
        fc.signers.emplace_back((bool)InsecureRandBits(1));
        fc.validMembers.emplace_back((bool)InsecureRandBits(1));
    }
    fc.quorumPublicKey.SetByteVector(InsecureRandBytes(BLS_CURVE_PUBKEY_SIZE));
    fc.quorumVvecHash = GetRandHash();
    fc.quorumSig.SetByteVector(InsecureRandBytes(BLS_CURVE_SIG_SIZE));
    fc.membersSig.SetByteVector(InsecureRandBytes(BLS_CURVE_SIG_SIZE));
    return fc;
}

static llmq::LLMQCommPL GetRandomLLMQCommPayload()
{
    llmq::LLMQCommPL pl;
    pl.nHeight = InsecureRand32();
    pl.commitment = GetRandomLLMQCommitment();
    return pl;
}

static CMutableTransaction BuildGovVoteLockTx(const uint256& proposalHash,
                                              const CKeyID& ownerKeyId,
                                              const CAmount amount,
                                              const uint32_t unlockHeight)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType = CTransaction::TxType::GOVVOTELOCK;
    mtx.vin.emplace_back(GetRandHash(), 0);
    mtx.vout.emplace_back(amount, GetScriptForDestination(ownerKeyId));

    CGovVoteLockTx lockPayload;
    lockPayload.proposalHash = proposalHash;
    lockPayload.lockAmount = amount;
    lockPayload.unlockHeight = unlockHeight;
    lockPayload.ownerKeyId = ownerKeyId;
    SetTxPayload(mtx, lockPayload);
    return mtx;
}

static CMutableTransaction BuildGovVoteCastTx(const uint256& proposalHash,
                                              const uint8_t voteDirection,
                                              const std::vector<COutPoint>& lockRefs,
                                              const CKey& signer)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType = CTransaction::TxType::GOVVOTECAST;
    mtx.vin.emplace_back(GetRandHash(), 0);

    CGovVoteCastTx castPayload;
    castPayload.proposalHash = proposalHash;
    castPayload.voteDirection = voteDirection;
    castPayload.lockRefs = lockRefs;
    BOOST_CHECK(CHashSigner::SignHash(castPayload.GetSignatureHash(), signer, castPayload.sig));
    SetTxPayload(mtx, castPayload);

    return mtx;
}

static bool EqualCommitments(const llmq::CFinalCommitment& a, const llmq::CFinalCommitment& b)
{
    return a.nVersion == b.nVersion &&
           a.llmqType == b.llmqType &&
           a.quorumHash == b.quorumHash &&
           a.signers == b.signers &&
           a.quorumPublicKey == b.quorumPublicKey &&
           a.quorumVvecHash == b.quorumVvecHash &&
           a.quorumSig == b.quorumSig &&
           a.membersSig == b.membersSig;
}

template <typename T>
static void TrivialCheckSpecialTx(const CMutableTransaction& mtx, const bool shouldFail, const std::string& rejectReason)
{
    T pl;
    CValidationState state;
    GetTxPayload(mtx, pl);
    BOOST_CHECK(pl.IsTriviallyValid(state) == !shouldFail);
    if (shouldFail) {
        BOOST_CHECK(state.GetRejectReason() == rejectReason);
    }
}

static void SpecialTxTrivialValidator(const UniValue& tests)
{
    for (size_t i = 1; i < tests.size(); i++) {
        const auto& test = tests[i];

        uint256 txHash;
        std::string txType;
        CMutableTransaction mtx;
        std::string rejectReason = "";
        try {
            txHash = uint256S(test[0].get_str());

            txType = test[1].get_str();
            CDataStream stream(ParseHex(test[2].get_str()), SER_NETWORK, PROTOCOL_VERSION);
            stream >> mtx;

            bool shouldFail = test.size() > 3;
            if (shouldFail) {
                rejectReason = test[3].get_str();
            }
            BOOST_CHECK(mtx.GetHash() == txHash);

            switch (mtx.nType) {
            case CTransaction::TxType::PROREG:
                BOOST_CHECK(txType == "proreg");
                TrivialCheckSpecialTx<ProRegPL>(mtx, shouldFail, rejectReason);
                break;
            case CTransaction::TxType::PROUPSERV:
                BOOST_CHECK(txType == "proupserv");
                TrivialCheckSpecialTx<ProUpServPL>(mtx, shouldFail, rejectReason);
                break;
            case CTransaction::TxType::PROUPREG:
                BOOST_CHECK(txType == "proupreg");
                TrivialCheckSpecialTx<ProUpRegPL>(mtx, shouldFail, rejectReason);
                break;
            case CTransaction::TxType::PROUPREV:
                BOOST_CHECK(txType == "prouprev");
                TrivialCheckSpecialTx<ProUpRevPL>(mtx, shouldFail, rejectReason);
                break;
            default:
                BOOST_CHECK(false);
            }
        } catch (...) {
            std::string strTest = test.write();
            BOOST_ERROR("Bad test, couldn't deserialize data: " << strTest);
            continue;
        }
    }
}

BOOST_AUTO_TEST_CASE(protx_validation_test)
{
    LOCK(cs_main);

    CMutableTransaction mtx;
    CValidationState state;

    // v1 can only be Type=0
    mtx.nType = CTransaction::TxType::PROREG;
    mtx.nVersion = CTransaction::TxVersion::LEGACY;
    BOOST_CHECK(!CheckSpecialTxNoContext(CTransaction(mtx), state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-type-version");

    // version >= Sapling, type = 0, payload != null.
    mtx.nType = CTransaction::TxType::NORMAL;
    mtx.extraPayload = std::vector<uint8_t>(10, 1);
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    BOOST_CHECK(!CheckSpecialTxNoContext(CTransaction(mtx), state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-type-payload");

    // version >= Sapling, type = 0, payload == null --> pass
    mtx.extraPayload = nullopt;
    BOOST_CHECK(CheckSpecialTxNoContext(CTransaction(mtx), state));

    // nVersion>=2 and nType!=0 without extrapayload
    mtx.nType = CTransaction::TxType::PROREG;
    BOOST_CHECK(!CheckSpecialTxNoContext(CTransaction(mtx), state));
    BOOST_CHECK(state.GetRejectReason().find("without extra payload"));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-payload-empty");

    // Size limits
    mtx.extraPayload = std::vector<uint8_t>(MAX_SPECIALTX_EXTRAPAYLOAD + 1, 1);
    BOOST_CHECK(!CheckSpecialTxNoContext(CTransaction(mtx), state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-payload-oversize");

    // Remove one element, so now it passes the size check
    mtx.extraPayload->pop_back();
    BOOST_CHECK(!CheckSpecialTxNoContext(CTransaction(mtx), state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-payload");

    // valid payload but invalid inputs hash
    mtx.extraPayload->clear();
    ProRegPL pl = GetRandomProRegPayload();
    SetTxPayload(mtx, pl);
    BOOST_CHECK(!CheckSpecialTxNoContext(CTransaction(mtx), state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-inputs-hash");

    // all good.
    mtx.vin.emplace_back(GetRandHash(), 0);
    mtx.extraPayload->clear();
    pl.inputsHash = CalcTxInputsHash(CTransaction(mtx));
    SetTxPayload(mtx, pl);
    BOOST_CHECK(CheckSpecialTxNoContext(CTransaction(mtx), state));
}

BOOST_AUTO_TEST_CASE(proreg_setpayload_test)
{
    const ProRegPL& pl = GetRandomProRegPayload();

    CMutableTransaction mtx;
    SetTxPayload(mtx, pl);
    ProRegPL pl2;
    BOOST_CHECK(GetTxPayload(mtx, pl2));
    BOOST_CHECK(pl.collateralOutpoint == pl2.collateralOutpoint);
    BOOST_CHECK(pl.addr  == pl2.addr);
    BOOST_CHECK(pl.keyIDOwner == pl2.keyIDOwner);
    BOOST_CHECK(pl.pubKeyOperator == pl2.pubKeyOperator);
    BOOST_CHECK(pl.keyIDVoting == pl2.keyIDVoting);
    BOOST_CHECK(pl.scriptPayout == pl2.scriptPayout);
    BOOST_CHECK(pl.nOperatorReward  == pl2.nOperatorReward);
    BOOST_CHECK(pl.scriptOperatorPayout == pl2.scriptOperatorPayout);
    BOOST_CHECK(pl.inputsHash == pl2.inputsHash);
    BOOST_CHECK(pl.vchSig == pl2.vchSig);
}

BOOST_AUTO_TEST_CASE(proupserv_setpayload_test)
{
    const ProUpServPL& pl = GetRandomProUpServPayload();

    CMutableTransaction mtx;
    SetTxPayload(mtx, pl);
    ProUpServPL pl2;
    BOOST_CHECK(GetTxPayload(mtx, pl2));
    BOOST_CHECK(pl.proTxHash == pl2.proTxHash);
    BOOST_CHECK(pl.addr  == pl2.addr);
    BOOST_CHECK(pl.scriptOperatorPayout == pl2.scriptOperatorPayout);
    BOOST_CHECK(pl.inputsHash == pl2.inputsHash);
    BOOST_CHECK(pl.sig == pl2.sig);
}

BOOST_AUTO_TEST_CASE(proupreg_setpayload_test)
{
    const ProUpRegPL& pl = GetRandomProUpRegPayload();

    CMutableTransaction mtx;
    SetTxPayload(mtx, pl);
    ProUpRegPL pl2;
    BOOST_CHECK(GetTxPayload(mtx, pl2));
    BOOST_CHECK(pl.proTxHash == pl2.proTxHash);
    BOOST_CHECK(pl.pubKeyOperator == pl2.pubKeyOperator);
    BOOST_CHECK(pl.keyIDVoting == pl2.keyIDVoting);
    BOOST_CHECK(pl.scriptPayout == pl2.scriptPayout);
    BOOST_CHECK(pl.inputsHash == pl2.inputsHash);
    BOOST_CHECK(pl.vchSig == pl2.vchSig);
}

BOOST_AUTO_TEST_CASE(prouprev_setpayload_test)
{
    const ProUpRevPL& pl = GetRandomProUpRevPayload();

    CMutableTransaction mtx;
    SetTxPayload(mtx, pl);
    ProUpRevPL pl2;
    BOOST_CHECK(GetTxPayload(mtx, pl2));
    BOOST_CHECK(pl.proTxHash == pl2.proTxHash);
    BOOST_CHECK(pl.nReason == pl2.nReason);
    BOOST_CHECK(pl.inputsHash == pl2.inputsHash);
    BOOST_CHECK(pl.sig == pl2.sig);
}

BOOST_AUTO_TEST_CASE(proreg_checkstringsig_test)
{
    ProRegPL pl = GetRandomProRegPayload();
    pl.vchSig.clear();
    const CKey& key = GetRandomKey();
    BOOST_CHECK(CMessageSigner::SignMessage(pl.MakeSignString(), pl.vchSig, key));

    std::string strError;
    const CKeyID& keyID = key.GetPubKey().GetID();
    BOOST_CHECK(CMessageSigner::VerifyMessage(keyID, pl.vchSig, pl.MakeSignString(), strError));
    // Change owner address or script payout
    pl.keyIDOwner = GetRandomKeyID();
    BOOST_CHECK(!CMessageSigner::VerifyMessage(keyID, pl.vchSig, pl.MakeSignString(), strError));
    pl.scriptPayout = GetRandomScript();
    BOOST_CHECK(!CMessageSigner::VerifyMessage(keyID, pl.vchSig, pl.MakeSignString(), strError));
}

BOOST_AUTO_TEST_CASE(llmqcomm_setpayload_test)
{
    const llmq::LLMQCommPL& pl = GetRandomLLMQCommPayload();

    CMutableTransaction mtx;
    SetTxPayload(mtx, pl);
    llmq::LLMQCommPL pl2;
    BOOST_CHECK(GetTxPayload(mtx, pl2));
    BOOST_CHECK(pl.nHeight == pl2.nHeight);
    BOOST_CHECK(EqualCommitments(pl.commitment, pl2.commitment));
}

BOOST_AUTO_TEST_CASE(specialtx_trivial_valid)
{
    UniValue tests = read_json(std::string(json_tests::specialtx_valid, json_tests::specialtx_valid + sizeof(json_tests::specialtx_valid)));
    SpecialTxTrivialValidator(tests);
}

BOOST_AUTO_TEST_CASE(specialtx_trivial_invalid)
{
    UniValue tests = read_json(std::string(json_tests::specialtx_invalid, json_tests::specialtx_invalid + sizeof(json_tests::specialtx_invalid)));
    SpecialTxTrivialValidator(tests);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(evo_specialtx_gov_tests, TestnetSetup)

BOOST_AUTO_TEST_CASE(gov_special_txs_rejected_before_activation)
{
    LOCK(cs_main);

    // V6_1_GOV activates at genesis on the public networks; pin a concrete
    // height here so the activation-boundary behavior stays testable.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_1_GOV, 100);
    const int govActivationHeight =
            Params().GetConsensus().vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight;
    BOOST_REQUIRE(govActivationHeight > 1);

    CBlockIndex prev;
    prev.nHeight = govActivationHeight - 2;

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType = CTransaction::TxType::GOVVOTELOCK;
    mtx.vin.emplace_back(GetRandHash(), 0);
    mtx.extraPayload = std::vector<uint8_t>(1, 0x01);

    CValidationState state;
    BOOST_CHECK(!CheckSpecialTx(CTransaction(mtx), &prev, nullptr, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-gov-not-active");
}

BOOST_AUTO_TEST_CASE(gov_special_txs_activation_boundary)
{
    LOCK(cs_main);

    // V6_1_GOV activates at genesis on the public networks; pin a concrete
    // height here so the activation-boundary behavior stays testable.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_1_GOV, 100);
    const int govActivationHeight =
            Params().GetConsensus().vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight;
    BOOST_REQUIRE(govActivationHeight > 0);

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType = CTransaction::TxType::GOVVOTELOCK;
    mtx.vin.emplace_back(GetRandHash(), 0);
    mtx.extraPayload = std::vector<uint8_t>(1, 0x01);

    // block (activation-1) must still be pre-activation
    CBlockIndex prevBefore;
    prevBefore.nHeight = govActivationHeight - 2;
    CValidationState stateBefore;
    BOOST_CHECK(!CheckSpecialTx(CTransaction(mtx), &prevBefore, nullptr, stateBefore));
    BOOST_CHECK_EQUAL(stateBefore.GetRejectReason(), "bad-txns-gov-not-active");

    // block activation must be post-activation
    CBlockIndex prevAfter;
    prevAfter.nHeight = govActivationHeight - 1;
    CValidationState stateAfter;
    BOOST_CHECK(!CheckSpecialTx(CTransaction(mtx), &prevAfter, nullptr, stateAfter));
    BOOST_CHECK(stateAfter.GetRejectReason() != "bad-txns-gov-not-active");
}

BOOST_AUTO_TEST_CASE(gov_votelock_rejects_excessive_consensus_cap)
{
    LOCK(cs_main);

    // V6_1_GOV activates at genesis on the public networks; pin a concrete
    // height here so the activation-boundary behavior stays testable.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_1_GOV, 100);
    const int govActivationHeight =
            Params().GetConsensus().vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight;
    BOOST_REQUIRE(govActivationHeight > 0);

    CMutableTransaction mtx = evo_specialtx_tests::BuildGovVoteLockTx(
            GetRandHash(),
            evo_specialtx_tests::GetRandomKeyID(),
            100001 * COIN,
            50000);

    CBlockIndex prev;
    prev.nHeight = govActivationHeight - 1;

    CValidationState state;
    BOOST_CHECK(!CheckSpecialTx(CTransaction(mtx), &prev, nullptr, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-lock-amount-excessive");
}

BOOST_AUTO_TEST_CASE(gov_votelock_rejects_missing_matching_lock_output)
{
    LOCK(cs_main);

    // V6_1_GOV activates at genesis on the public networks; pin a concrete
    // height here so the activation-boundary behavior stays testable.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_1_GOV, 100);
    const int govActivationHeight =
            Params().GetConsensus().vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight;
    BOOST_REQUIRE(govActivationHeight > 0);

    CMutableTransaction mtx = evo_specialtx_tests::BuildGovVoteLockTx(
            GetRandHash(),
            evo_specialtx_tests::GetRandomKeyID(),
            10 * COIN,
            50000);
    mtx.vout[0].nValue = 9 * COIN;

    CBlockIndex prev;
    prev.nHeight = govActivationHeight - 1;

    CValidationState state;
    BOOST_CHECK(!CheckSpecialTx(CTransaction(mtx), &prev, nullptr, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-lock-output");
}

BOOST_AUTO_TEST_CASE(gov_votecast_accepts_same_block_lock_without_mempool_lookup)
{
    LOCK(cs_main);

    mempool.clear();

    // V6_1_GOV activates at genesis on the public networks; pin a concrete
    // height here so the activation-boundary behavior stays testable.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_1_GOV, 100);
    const int govActivationHeight =
            Params().GetConsensus().vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight;
    BOOST_REQUIRE(govActivationHeight > 0);

    const CKey ownerKey = evo_specialtx_tests::GetRandomKey();
    const uint256 proposalHash = GetRandHash();
    const CAmount lockAmount = 40 * COIN;
    const uint32_t unlockHeight = 50000;

    const CTransactionRef lockTx = MakeTransactionRef(evo_specialtx_tests::BuildGovVoteLockTx(
            proposalHash,
            ownerKey.GetPubKey().GetID(),
            lockAmount,
            unlockHeight));
    const COutPoint lockRef(lockTx->GetHash(), 0);
    const CTransactionRef castTx = MakeTransactionRef(evo_specialtx_tests::BuildGovVoteCastTx(
            proposalHash,
            CGovVoteCastTx::VOTE_YES,
            {lockRef},
            ownerKey));

    CCoinsViewCache view(pcoinsTip.get());
    view.AddCoin(lockRef, Coin(lockTx->vout[0], govActivationHeight, false, false), false);

    CBlock block;
    block.vtx = {lockTx, castTx};

    CBlockIndex prev;
    prev.nHeight = govActivationHeight - 1;
    const uint256 prevHash = GetRandHash();
    prev.phashBlock = &prevHash;

    CBlockIndex index;
    index.nHeight = govActivationHeight;
    index.pprev = &prev;
    const uint256 indexHash = GetRandHash();
    index.phashBlock = &indexHash;

    // Disable DIP3 enforcement for this test — we are validating governance
    // vote transactions, not DMN list transitions. The fake block indexes
    // used here do not exist in the DMN database, so ProcessBlock would
    // otherwise throw "failed-dmn-block".
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);

    CValidationState state;
    BOOST_CHECK(ProcessSpecialTxsInBlock(block, &index, &view, state, true));
}

BOOST_AUTO_TEST_CASE(gov_votecast_missing_lock_is_non_bannable)
{
    LOCK(cs_main);

    // V6_1_GOV activates at genesis on the public networks; pin a concrete
    // height here so the activation-boundary behavior stays testable.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_1_GOV, 100);
    const int govActivationHeight =
            Params().GetConsensus().vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight;
    BOOST_REQUIRE(govActivationHeight > 0);

    const CKey ownerKey = evo_specialtx_tests::GetRandomKey();
    const uint256 proposalHash = GetRandHash();
    const COutPoint missingLockRef(GetRandHash(), 0);

    CMutableTransaction castMtx = evo_specialtx_tests::BuildGovVoteCastTx(
            proposalHash,
            CGovVoteCastTx::VOTE_YES,
            {missingLockRef},
            ownerKey);

    CBlockIndex prev;
    prev.nHeight = govActivationHeight - 1;

    CCoinsViewCache view(pcoinsTip.get());
    CValidationState state;
    BOOST_CHECK(!CheckSpecialTx(CTransaction(castMtx), &prev, &view, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-lock-missing");

    int nDos = 0;
    BOOST_CHECK(state.IsInvalid(nDos));
    BOOST_CHECK_EQUAL(nDos, 0);
}

BOOST_AUTO_TEST_SUITE_END()
