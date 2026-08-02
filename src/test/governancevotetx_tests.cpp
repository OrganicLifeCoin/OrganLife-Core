// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "consensus/validation.h"
#include "coins.h"
#include "evo/governancevotetx.h"
#include "evo/specialtx_validation.h"
#include "messagesigner.h"
#include "script/standard.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

namespace {
CKeyID GetRandomKeyID()
{
    CKey key;
    key.MakeNewKey(true);
    return key.GetPubKey().GetID();
}

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

    CGovVoteLockTx lockPayload;
    lockPayload.proposalHash = proposalHash;
    lockPayload.lockAmount = amount;
    lockPayload.unlockHeight = unlockHeight;
    lockPayload.ownerKeyId = ownerKeyId;
    SetTxPayload(mtx, lockPayload);
    return mtx;
}

CTransaction BuildGovVoteCastTx(const uint256& proposalHash,
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

    return CTransaction(mtx);
}

int GovActivationPrevHeight()
{
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
    basePayload.ownerKeyId = GetRandomKeyID();

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

    const CKey ownerKey = GetRandomKey();
    const CKey wrongSigner = GetRandomKey();
    const uint256 proposalHashA = GetRandHash();
    const uint256 proposalHashB = GetRandHash();
    const CAmount lockAmount = 25 * COIN;

    const CMutableTransaction lockMtx = BuildGovVoteLockTx(proposalHashA, ownerKey.GetPubKey().GetID(), lockAmount, 50000);
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

    const CKey ownerKeyA = GetRandomKey();
    const CKey ownerKeyB = GetRandomKey();
    const uint256 proposalHash = GetRandHash();
    const CAmount lockAmount = 10 * COIN;

    const CTransactionRef lockTxA = MakeTransactionRef(BuildGovVoteLockTx(proposalHash, ownerKeyA.GetPubKey().GetID(), lockAmount, 50000));
    const CTransactionRef lockTxB = MakeTransactionRef(BuildGovVoteLockTx(proposalHash, ownerKeyB.GetPubKey().GetID(), lockAmount, 50000));
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

    const CKey ownerKey = GetRandomKey();
    const uint256 proposalHash = GetRandHash();
    const CAmount lockAmount = 10 * COIN;

    CMutableTransaction lockMtx = BuildGovVoteLockTx(
            proposalHash,
            ownerKey.GetPubKey().GetID(),
            lockAmount,
            50000);
    // Insert an unrelated output before the actual lock output so the lock output index is not 0.
    lockMtx.vout.insert(lockMtx.vout.begin(), CTxOut(2 * COIN, GetScriptForDestination(GetRandomKeyID())));

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
