// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2020-2021 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "consensus/validation.h"
#include "blockassembler.h"
#include "pqtransaction.h"
#include "util/blockstatecatcher.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>
#include <set>

BOOST_AUTO_TEST_SUITE(tx_validationcache_tests)

static bool ToMemPool(CMutableTransaction& tx)
{
    LOCK(cs_main);
    CValidationState state;
    return AcceptToMemoryPool(mempool, state, MakeTransactionRef(tx), false, nullptr, true, false, false);
}

struct PQCacheSetup : TestChainSetup { PQCacheSetup() : TestChainSetup(0) {} };

BOOST_FIXTURE_TEST_CASE(tx_mempool_block_doublespend, PQCacheSetup)
{
    // Make sure skipping validation of transactions that were
    // validated going into the memory pool does not allow
    // double-spends in blocks to pass validation when they should not.

    mldsa44::Key key;
    BOOST_REQUIRE(key.Generate());
    const auto id = pq::GetID(key.GetPublicKey(), Params().NetworkIDString());
    BOOST_REQUIRE(id);
    const CScript scriptPubKey = pq::GetScript(*id);
    for (int i = 0; i < 100; ++i) {
        const auto block = CreateAndProcessBlock({}, scriptPubKey);
        BOOST_REQUIRE_EQUAL(chainActive.Tip()->GetBlockHash(), block.GetHash());
        coinbaseTxns.push_back(*block.vtx[0]);
    }
    BOOST_REQUIRE(pq::PaymentsActive(Params(), chainActive.Height() + 1));
    const std::vector<CTxOut> prevouts{coinbaseTxns[0].vout[0]};
    const auto context = pq::SignatureContext(Params().NetworkIDString());
    BOOST_REQUIRE(context);

    // Create a double-spend of mature coinbase txn:
    std::vector<CMutableTransaction> spends;
    spends.resize(2);
    for (int i = 0; i < 2; i++)
    {
        spends[i].nVersion = 3;
        spends[i].nType = CTransaction::PQ;
        spends[i].sapData = nullopt;
        spends[i].vin.resize(1);
        spends[i].vin[0].prevout.hash = coinbaseTxns[0].GetHash();
        spends[i].vin[0].prevout.n = 0;
        spends[i].vout.resize(1);
        spends[i].vout[0].nValue = prevouts[0].nValue - CENT - i;
        spends[i].vout[0].scriptPubKey = scriptPubKey;

        // Sign:
        pq::Payload payload;
        payload.authorizations.resize(1);
        payload.authorizations[0].public_key = key.GetPublicKey();
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(key.Sign(pq::SignatureMessage(spends[i], prevouts, payload,
            Params().GetConsensus().hashGenesisBlock, 0), *context, signature));
        std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
        spends[i].extraPayload = pq::EncodePayload(payload);
        std::string reason;
        BOOST_REQUIRE_MESSAGE(pq::VerifyInputs(spends[i], prevouts, Params(), reason), reason);
    }
    BOOST_REQUIRE(spends[0].GetHash() != spends[1].GetHash());

    const uint256 tipHash = chainActive.Tip()->GetBlockHash();
    CBlock block;
    unsigned extra_nonce = 1000;
    std::set<uint256> attempted;
    const auto process = [&](const std::vector<CMutableTransaction>& transactions, const std::string& rejection) {
        auto candidate = std::make_shared<CBlock>(CreateBlock(transactions, scriptPubKey, true));
        BOOST_REQUIRE_EQUAL(candidate->vtx.size(), transactions.size() + 1);
        // This helper appends transactions after assembly; include their fees
        // in the coinbase so reward rejection cannot hide the behavior tested.
        CMutableTransaction coinbase(*candidate->vtx[0]);
        for (const auto& tx : transactions) coinbase.vout[0].nValue += prevouts[0].nValue - tx.vout[0].nValue;
        candidate->vtx[0] = MakeTransactionRef(coinbase);
        IncrementExtraNonce(candidate, chainActive.Height() + 1, extra_nonce);
        candidate = FinalizeBlock(candidate);
        BOOST_REQUIRE(attempted.insert(candidate->GetHash()).second);
        BlockStateCatcherWrapper catcher(candidate->GetHash());
        catcher.registerEvent();
        ProcessNewBlock(candidate, nullptr);
        BOOST_REQUIRE(catcher.get().found);
        BOOST_CHECK_EQUAL(catcher.get().state.GetRejectReason(), rejection);
        return *candidate;
    };

    // Test 1: block with both of those transactions should be rejected.
    block = process(spends, "bad-txns-inputs-missingorspent");
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() != block.GetHash());
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() == tipHash);

    // Test 2: ... and should be rejected if spend1 is in the memory pool
    BOOST_REQUIRE(ToMemPool(spends[0]));
    block = process(spends, "bad-txns-inputs-missingorspent");
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() != block.GetHash());
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() == tipHash);
    mempool.clear();

    // Test 3: ... and should be rejected if spend2 is in the memory pool
    BOOST_REQUIRE(ToMemPool(spends[1]));
    block = process(spends, "bad-txns-inputs-missingorspent");
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() != block.GetHash());
    BOOST_CHECK(chainActive.Tip()->GetBlockHash() == tipHash);
    mempool.clear();

    // Final sanity test: first spend in mempool, second in block, that's OK:
    std::vector<CMutableTransaction> oneSpend;
    oneSpend.push_back(spends[0]);
    BOOST_REQUIRE(ToMemPool(spends[1]));
    block = process(oneSpend, "");
    BOOST_REQUIRE(chainActive.Tip()->GetBlockHash() == block.GetHash());
    // spends[1] should have been removed from the mempool when the
    // block with spends[0] is accepted:
    BOOST_CHECK_EQUAL(mempool.size(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
