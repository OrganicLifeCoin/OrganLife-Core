// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "consensus/validation.h"
#include "coins.h"
#include "evo/governancevotetx.h"
#include "evo/specialtx_validation.h"
#include "pqaddress.h"
#include "pqtransaction.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

namespace {
struct TestPQKey
{
    mldsa44::Key key;
    pq::KeyID id;

    TestPQKey()
    {
        BOOST_REQUIRE(key.Generate());
        const auto keyId = pq::GetID(key.GetPublicKey(), Params().NetworkIDString());
        BOOST_REQUIRE(keyId);
        id = *keyId;
    }
};

CMutableTransaction BuildGovVoteLockTx(const uint256& proposalHash,
                                       const TestPQKey& owner,
                                       const CAmount amount,
                                       const uint32_t unlockHeight)
{
    CMutableTransaction mtx;
    mtx.nVersion = 3;
    mtx.nType = CTransaction::TxType::PQ;
    mtx.sapData = nullopt;
    mtx.vin.emplace_back(GetRandHash(), 0);
    mtx.vout.emplace_back(amount, pq::GetScript(owner.id));

    CGovVoteLockTx lockPayload;
    lockPayload.proposalHash = proposalHash;
    lockPayload.lockAmount = amount;
    lockPayload.unlockHeight = unlockHeight;
    lockPayload.ownerKeyId = owner.id;
    pq::Payload payload;
    payload.mode = pq::GOVERNANCE_LOCK;
    payload.data = EncodeGovernanceData(lockPayload);
    payload.authorizations.resize(1);
    payload.authorizations[0].public_key = owner.key.GetPublicKey();
    payload.authorizations[0].signature.fill(1);
    mtx.extraPayload = pq::EncodePayload(payload);
    std::string reason;
    BOOST_REQUIRE_MESSAGE(pq::CheckStructure(CTransaction(mtx), Params(), reason), reason);
    return mtx;
}

CTransaction BuildGovVoteCastTx(const uint256& proposalHash,
                                const uint8_t voteDirection,
                                const std::vector<COutPoint>& lockRefs,
                                const TestPQKey& signer)
{
    CMutableTransaction mtx;
    mtx.nVersion = 3;
    mtx.nType = CTransaction::TxType::PQ;
    mtx.sapData = nullopt;
    mtx.vin.emplace_back(GetRandHash(), 0);
    mtx.vout.emplace_back(COIN, pq::GetScript(signer.id));

    CGovVoteCastTx castPayload;
    castPayload.proposalHash = proposalHash;
    castPayload.voteDirection = voteDirection;
    castPayload.lockRefs = lockRefs;
    castPayload.ownerPublicKey = signer.key.GetPublicKey();
    const auto context = pq::GovernanceSignatureContext(Params().NetworkIDString());
    BOOST_REQUIRE(context);
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(signer.key.Sign(castPayload.GetSignatureMessage(Params().GetConsensus().hashGenesisBlock),
                                  *context, signature));
    std::copy(signature.begin(), signature.end(), castPayload.sig.begin());
    pq::Payload payload;
    payload.mode = pq::GOVERNANCE_CAST;
    payload.data = EncodeGovernanceData(castPayload);
    payload.authorizations.resize(1);
    payload.authorizations[0].public_key = signer.key.GetPublicKey();
    payload.authorizations[0].signature.fill(1);
    mtx.extraPayload = pq::EncodePayload(payload);
    std::string reason;
    BOOST_REQUIRE_MESSAGE(pq::CheckStructure(CTransaction(mtx), Params(), reason), reason);

    return CTransaction(mtx);
}

int GovActivationPrevHeight()
{
    // V6_1_GOV activates at genesis on the public networks; pin a concrete
    // height here so the activation-boundary behavior stays testable.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_1_GOV, 100);
    const int govActivationHeight =
            Params().GetConsensus().vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight;
    BOOST_REQUIRE(govActivationHeight > 0);
    return govActivationHeight - 1;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(governancevotetx_tests, TestnetSetup)

BOOST_AUTO_TEST_CASE(gov_votelock_trivial_validation)
{
    CGovVoteLockTx basePayload;
    basePayload.proposalHash = GetRandHash();
    basePayload.lockAmount = 10 * COIN;
    basePayload.unlockHeight = 120;
    basePayload.ownerKeyId = TestPQKey().id;

    {
        CGovVoteLockTx payload = basePayload;
        payload.lockAmount = 0;
        CValidationState state;
        BOOST_CHECK(!payload.IsTriviallyValid(state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-lock-amount");
    }

    {
        CGovVoteLockTx payload = basePayload;
        payload.lockAmount = (10 * COIN) + 1;
        CValidationState state;
        BOOST_CHECK(!payload.IsTriviallyValid(state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-lock-amount-unit");
    }

    {
        CGovVoteLockTx payload = basePayload;
        payload.lockAmount = 100001 * COIN;
        CValidationState state;
        BOOST_CHECK(!payload.IsTriviallyValid(state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-lock-amount-excessive");
    }

    {
        CGovVoteLockTx payload = basePayload;
        payload.unlockHeight = 0;
        CValidationState state;
        BOOST_CHECK(!payload.IsTriviallyValid(state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-unlock-height");
    }

    {
        CValidationState state;
        BOOST_CHECK(basePayload.IsTriviallyValid(state));
    }
}

BOOST_AUTO_TEST_CASE(gov_votecast_rejects_invalid_lock_refs)
{
    LOCK(cs_main);

    mempool.clear();

    const TestPQKey ownerKey;
    const TestPQKey wrongSigner;
    const uint256 proposalHashA = GetRandHash();
    const uint256 proposalHashB = GetRandHash();
    const CAmount lockAmount = 25 * COIN;

    const CMutableTransaction lockMtx = BuildGovVoteLockTx(proposalHashA, ownerKey, lockAmount, 50000);
    const CTransactionRef lockTx = MakeTransactionRef(lockMtx);
    const COutPoint lockRef(lockTx->GetHash(), 0);

    TestMemPoolEntryHelper entry;
    BOOST_CHECK(mempool.addUnchecked(lockTx->GetHash(), entry.FromTx(*lockTx)));

    CCoinsViewCache view(pcoinsTip.get());
    view.AddCoin(lockRef, Coin(lockTx->vout[0], 1, false, false), false);

    CBlockIndex prev;
    prev.nHeight = GovActivationPrevHeight();

    {
        CValidationState state;
        const CTransaction castTx = BuildGovVoteCastTx(
                proposalHashB,
                CGovVoteCastTx::VOTE_YES,
                {lockRef},
                ownerKey);
        BOOST_CHECK(!CheckSpecialTx(castTx, &prev, &view, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-proposal-mismatch");
    }

    {
        CValidationState state;
        const CTransaction castTx = BuildGovVoteCastTx(
                proposalHashA,
                CGovVoteCastTx::VOTE_YES,
                {lockRef, lockRef},
                ownerKey);
        BOOST_CHECK(!CheckSpecialTx(castTx, &prev, &view, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-dup-lock-ref");
    }

    {
        CValidationState state;
        const CTransaction castTx = BuildGovVoteCastTx(
                proposalHashA,
                CGovVoteCastTx::VOTE_YES,
                {lockRef},
                wrongSigner);
        BOOST_CHECK(!CheckSpecialTx(castTx, &prev, &view, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-sig");
    }
}

BOOST_AUTO_TEST_CASE(gov_votecast_rejects_mixed_lock_owners)
{
    LOCK(cs_main);

    mempool.clear();

    const TestPQKey ownerKeyA;
    const TestPQKey ownerKeyB;
    const uint256 proposalHash = GetRandHash();
    const CAmount lockAmount = 10 * COIN;

    const CTransactionRef lockTxA = MakeTransactionRef(BuildGovVoteLockTx(proposalHash, ownerKeyA, lockAmount, 50000));
    const CTransactionRef lockTxB = MakeTransactionRef(BuildGovVoteLockTx(proposalHash, ownerKeyB, lockAmount, 50000));
    const COutPoint lockRefA(lockTxA->GetHash(), 0);
    const COutPoint lockRefB(lockTxB->GetHash(), 0);

    TestMemPoolEntryHelper entry;
    BOOST_CHECK(mempool.addUnchecked(lockTxA->GetHash(), entry.FromTx(*lockTxA)));
    BOOST_CHECK(mempool.addUnchecked(lockTxB->GetHash(), entry.FromTx(*lockTxB)));

    CCoinsViewCache view(pcoinsTip.get());
    view.AddCoin(lockRefA, Coin(lockTxA->vout[0], 1, false, false), false);
    view.AddCoin(lockRefB, Coin(lockTxB->vout[0], 1, false, false), false);

    CBlockIndex prev;
    prev.nHeight = GovActivationPrevHeight();

    CValidationState state;
    const CTransaction castTx = BuildGovVoteCastTx(
            proposalHash,
            CGovVoteCastTx::VOTE_YES,
            {lockRefA, lockRefB},
            ownerKeyA);
    BOOST_CHECK(!CheckSpecialTx(castTx, &prev, &view, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-lock-owner-mismatch");
}

BOOST_AUTO_TEST_CASE(gov_votecast_rejects_nonlock_output_refs)
{
    LOCK(cs_main);

    mempool.clear();

    const TestPQKey ownerKey;
    const uint256 proposalHash = GetRandHash();
    const CAmount lockAmount = 10 * COIN;

    CMutableTransaction lockMtx = BuildGovVoteLockTx(
            proposalHash,
            ownerKey,
            lockAmount,
            50000);
    // Insert an unrelated output before the actual lock output so the lock output index is not 0.
    lockMtx.vout.insert(lockMtx.vout.begin(), CTxOut(2 * COIN, pq::GetScript(TestPQKey().id)));

    const CTransactionRef lockTx = MakeTransactionRef(lockMtx);
    const COutPoint wrongRef(lockTx->GetHash(), 0);

    TestMemPoolEntryHelper entry;
    BOOST_CHECK(mempool.addUnchecked(lockTx->GetHash(), entry.FromTx(*lockTx)));

    CCoinsViewCache view(pcoinsTip.get());
    view.AddCoin(wrongRef, Coin(lockTx->vout[0], 1, false, false), false);

    CBlockIndex prev;
    prev.nHeight = GovActivationPrevHeight();

    CValidationState state;
    const CTransaction castTx = BuildGovVoteCastTx(
            proposalHash,
            CGovVoteCastTx::VOTE_YES,
            {wrongRef},
            ownerKey);
    BOOST_CHECK(!CheckSpecialTx(castTx, &prev, &view, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-govtx-lock-ref");
}

BOOST_AUTO_TEST_SUITE_END()
