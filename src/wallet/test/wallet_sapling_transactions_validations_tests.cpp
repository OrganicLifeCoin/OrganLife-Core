// Copyright (c) 2020-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"
#include "consensus/tx_verify.h"
#include "pqtransaction.h"
#include "util/blockstatecatcher.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

struct PQShieldedRejectionSetup : TestChainSetup {
    PQShieldedRejectionSetup() : TestChainSetup(0) {}
};

BOOST_FIXTURE_TEST_SUITE(wallet_sapling_transactions_validations_tests, PQShieldedRejectionSetup)

BOOST_AUTO_TEST_CASE(shielded_spends_rejected_from_mempool_and_blocks)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V5_0, 0);
    const CScript script = pq::GetScript(pq::KeyID{});
    const auto funding = CreateAndProcessBlock({}, script);
    BOOST_REQUIRE_EQUAL(chainActive.Tip()->GetBlockHash(), funding.GetHash());
    BOOST_REQUIRE(pq::PaymentsActive(Params(), chainActive.Height() + 1));

    // Structurally valid shielded data, with an intentionally invalid proof.
    // PQ admission must reject the entire transaction class before proof
    // verification or UTXO/maturity checks, even for the first spend.
    CMutableTransaction tx;
    tx.nVersion = CTransaction::SAPLING;
    tx.vin.emplace_back(funding.vtx[0]->GetHash(), 0);
    tx.vout.emplace_back(COIN, script);
    tx.sapData = SaplingTxData();
    tx.sapData->vShieldedSpend.emplace_back();
    tx.sapData->vShieldedSpend[0].nullifier = UINT256_ONE;
    CValidationState structure;
    BOOST_REQUIRE(CheckTransaction(tx, structure, true));

    // Keep the original nullifier parser rule covered without manufacturing
    // shielded funds on a chain that deliberately prohibits them.
    auto duplicate = tx;
    duplicate.sapData->vShieldedSpend.push_back(tx.sapData->vShieldedSpend[0]);
    CValidationState duplicate_state;
    BOOST_CHECK(!CheckTransaction(duplicate, duplicate_state, true));
    BOOST_CHECK_EQUAL(duplicate_state.GetRejectReason(), "bad-spend-description-nullifiers-duplicate");

    for (int variant = 0; variant < 2; ++variant) {
        tx.vout[0].nValue = COIN + variant;
        const auto ref = MakeTransactionRef(tx);
        {
            LOCK(cs_main);
            CValidationState state;
            BOOST_CHECK(!AcceptToMemoryPool(mempool, state, ref, false, nullptr, true, false, false));
            BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pq-only-transaction");
            BOOST_CHECK(!mempool.exists(ref->GetHash()));
        }
        auto block = std::make_shared<CBlock>(CreateBlock({tx}, script));
        BOOST_REQUIRE_EQUAL(block->vtx.size(), 2U);
        BlockStateCatcherWrapper catcher(block->GetHash());
        catcher.registerEvent();
        BOOST_CHECK(!ProcessNewBlock(block, nullptr));
        BOOST_REQUIRE(catcher.get().found);
        BOOST_CHECK_EQUAL(catcher.get().state.GetRejectReason(), "bad-pq-only-transaction");
        BOOST_CHECK_EQUAL(chainActive.Tip()->GetBlockHash(), funding.GetHash());
    }
}

BOOST_AUTO_TEST_SUITE_END()
