// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include "blockassembler.h"
#include "chainparams.h"
#include "coins.h"
#include "miner.h"
#include "pqaddress.h"
#include "pqtransaction.h"
#include "test/test_organiclife.h"
#include "timedata.h"
#include "validation.h"
#include "validationinterface.h"
#include "pow.h"
#include "rpc/server.h"
#include "streams.h"
#include "utilstrencodings.h"
#include "version.h"

#include <crypto/mldsa44.h>
#include <primitives/block.h>
#include <script/script.h>

#include <boost/test/unit_test.hpp>

#ifdef ENABLE_MINING_RPC
UniValue submitblock(const JSONRPCRequest& request);
#endif

namespace {

CBlock MineMainnetPoWBlock(const CScript& coinbaseScript)
{
    auto blockTemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(
        coinbaseScript, nullptr, false, nullptr, true, false, nullptr, true, false);
    BOOST_REQUIRE(blockTemplate);
    auto candidate = std::make_shared<CBlock>(blockTemplate->block);
    unsigned int extraNonce = 0;
    IncrementExtraNonce(candidate, chainActive.Height() + 1, extraNonce);
    CBlock block = *candidate;
    block.nNonce = 0;
    constexpr uint32_t MAX_NONCE_ATTEMPTS = 100'000'000;
    for (uint32_t attempts = 0; attempts < MAX_NONCE_ATTEMPTS; ++attempts) {
        if (CheckProofOfWork(block.GetHash(), block.nBits)) return block;
        ++block.nNonce;
    }
    BOOST_FAIL("mainnet PoW test block was not solved within the bounded nonce search");
    return block;
}

void ConnectMainnetBlock(const CBlock& block)
{
    BOOST_REQUIRE(ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr));
    SyncWithValidationInterfaceQueue();
    LOCK(cs_main);
    BOOST_REQUIRE_EQUAL(chainActive.Tip()->GetBlockHash(), block.GetHash());
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(pq_mainnet_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(mainnet_height_one_two_pq_premine_and_empty_registry)
{
    struct ResetTime { ~ResetTime() { SetMockTime(0); } } reset;
    SelectParams(CBaseChainParams::MAIN);
    SetMockTime(Params().GetConsensus().nLaunchTime + 120);

    mldsa44::Key key;
    BOOST_REQUIRE(key.Generate());
    const auto id = pq::GetID(key.GetPublicKey(), Params().NetworkIDString());
    BOOST_REQUIRE(id);
    const CScript pqScript = pq::GetScript(*id);

    const CBlock genesis = Params().GenesisBlock();
    BOOST_REQUIRE_EQUAL(chainActive.Height(), 0);
    BOOST_REQUIRE_EQUAL(chainActive.Tip()->GetBlockHash(), genesis.GetHash());

    const CBlock heightOne = MineMainnetPoWBlock(pqScript);
    BOOST_REQUIRE_EQUAL(heightOne.nTime >= Params().GetConsensus().nLaunchTime, true);
    BOOST_REQUIRE_EQUAL(heightOne.vtx.front()->vout.front().nValue,
                        GetBlockValue(1));
#ifdef ENABLE_MINING_RPC
    CDataStream encoded(SER_NETWORK, PROTOCOL_VERSION);
    encoded << heightOne;
    JSONRPCRequest request;
    request.params = UniValue(UniValue::VARR);
    request.params.push_back(HexStr(encoded));
    SetMockTime(Params().GetConsensus().nLaunchTime - 1);
    BOOST_CHECK_EQUAL(submitblock(request).get_str(), "mainnet-not-launched");
    BOOST_CHECK(!WITH_LOCK(cs_main, return LookupBlockIndex(heightOne.GetHash())));
    SetMockTime(Params().GetConsensus().nLaunchTime);
    BOOST_CHECK(submitblock(request).isNull());
#endif
    ConnectMainnetBlock(heightOne);

    {
        LOCK(cs_main);
        const Coin& premine = pcoinsTip->AccessCoin(COutPoint(heightOne.vtx.front()->GetHash(), 0));
        BOOST_REQUIRE(!premine.IsSpent());
        BOOST_CHECK_EQUAL(premine.out.nValue, GetBlockValue(1));
        BOOST_CHECK(pq::HasMarker(premine.out.scriptPubKey));
    }

    SetMockTime(Params().GetConsensus().nLaunchTime + 121);
    const CBlock heightTwo = MineMainnetPoWBlock(pqScript);
    BOOST_REQUIRE_EQUAL(heightTwo.vtx.front()->vout.front().nValue, GetBlockValue(2));
    ConnectMainnetBlock(heightTwo);
    BOOST_CHECK_EQUAL(chainActive.Height(), 2);
}

BOOST_AUTO_TEST_CASE(mainnet_equal_work_forks_converge_independent_of_arrival)
{
    struct ResetTime { ~ResetTime() { SetMockTime(0); } } reset;
    SetMockTime(Params().GetConsensus().nLaunchTime + 120);
    pq::KeyID first{}, second{};
    first[0] = 1;
    second[0] = 2;
    auto preferred = MineMainnetPoWBlock(pq::GetScript(first));
    auto other = MineMainnetPoWBlock(pq::GetScript(second));
    if (UintToArith256(other.GetHash()) < UintToArith256(preferred.GetHash()))
        std::swap(preferred, other);
    BOOST_REQUIRE(preferred.GetHash() != other.GetHash());
    BOOST_REQUIRE_EQUAL(preferred.nBits, other.nBits);
    ConnectMainnetBlock(other);
    BOOST_REQUIRE(ProcessNewBlock(std::make_shared<const CBlock>(preferred), nullptr));
    SyncWithValidationInterfaceQueue();
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), preferred.GetHash());
}

BOOST_AUTO_TEST_CASE(mainnet_height_one_legacy_coinbase_is_rejected_without_poisoning_header)
{
    struct ResetTime { ~ResetTime() { SetMockTime(0); } } reset;
    SelectParams(CBaseChainParams::MAIN);
    SetMockTime(Params().GetConsensus().nLaunchTime + 120);

    const CScript legacyScript = CScript() << OP_TRUE;
    const CBlock candidate = MineMainnetPoWBlock(legacyScript);
    CValidationState state;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(!TestBlockValidity(state, candidate, chainActive.Tip()));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pq-only-output");
        BOOST_CHECK(!LookupBlockIndex(candidate.GetHash()));
    }
}

BOOST_AUTO_TEST_SUITE_END()
