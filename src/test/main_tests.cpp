// Copyright (c) 2014 The Bitcoin Core developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "blocksignature.h"
#include "blockassembler.h"
#include "checkpoints.h"
#include "miner.h"
#include "net.h"
#include "pqtransaction.h"
#include "primitives/transaction.h"
#include "rpc/server.h"
#include "spork.h"
#include "streams.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>

#ifdef ENABLE_MINING_RPC
UniValue getblocktemplate(const JSONRPCRequest& request);
#endif

BOOST_FIXTURE_TEST_SUITE(main_tests, TestingSetup)

CBlock CreateDummyBlockWithSignature(const mldsa44::Key& stakingKey)
{
    const auto id = *pq::GetID(stakingKey.GetPublicKey(), Params().NetworkIDString());
    CMutableTransaction txCoinStake;
    txCoinStake.nVersion = CTransaction::SAPLING;
    txCoinStake.nType = CTransaction::PQ;
    txCoinStake.sapData = nullopt;
    txCoinStake.vin.emplace_back(uint256S("01"), 0);
    txCoinStake.vout.emplace_back(0, CScript());
    txCoinStake.vout.emplace_back(COIN, pq::GetScript(id));
    pq::Payload payload;
    payload.mode = pq::STAKE;
    payload.authorizations.resize(1);
    payload.authorizations[0].public_key = stakingKey.GetPublicKey();
    txCoinStake.extraPayload = pq::EncodePayload(payload);

    CBlock block;
    block.vtx.emplace_back(std::make_shared<const CTransaction>(CTransaction()));
    block.vtx.emplace_back(std::make_shared<const CTransaction>(txCoinStake));
    BOOST_REQUIRE(SignBlockWithPQKey(block, stakingKey));
    return block;
}

bool TestBlockSignature(const CBlock& block)
{
    return CheckBlockSignature(block);
}

namespace {
class UpgradeHeightRestorer
{
public:
    explicit UpgradeHeightRestorer(std::initializer_list<Consensus::UpgradeIndex> upgrades)
    {
        savedHeights.reserve(upgrades.size());
        for (const auto upgrade : upgrades) {
            savedHeights.emplace_back(upgrade, Params().GetConsensus().vUpgrades[upgrade].nActivationHeight);
        }
    }

    ~UpgradeHeightRestorer()
    {
        for (const auto& [upgrade, height] : savedHeights) {
            UpdateNetworkUpgradeParameters(upgrade, height);
        }
    }

private:
    std::vector<std::pair<Consensus::UpgradeIndex, int>> savedHeights;
};
} // namespace

BOOST_AUTO_TEST_CASE(block_signature_test)
{
    SelectParams(CBaseChainParams::REGTEST);
    mldsa44::Key stakingKey;
    BOOST_REQUIRE(stakingKey.Generate());
    CBlock block = CreateDummyBlockWithSignature(stakingKey);
    BOOST_CHECK(TestBlockSignature(block));
    block.vchBlockSig.back() ^= 1;
    BOOST_CHECK(!TestBlockSignature(block));
    SelectParams(CBaseChainParams::MAIN);
    BOOST_CHECK(!TestBlockSignature(block));
}

BOOST_AUTO_TEST_CASE(mainnet_pq_block_signature_test)
{
    mldsa44::Key stakingKey;
    BOOST_REQUIRE(stakingKey.Generate());
    const auto block = CreateDummyBlockWithSignature(stakingKey);
    BOOST_CHECK(CheckBlockSignature(block));
    SelectParams(CBaseChainParams::TESTNET);
    BOOST_CHECK(!CheckBlockSignature(block));
    SelectParams(CBaseChainParams::REGTEST);
    BOOST_CHECK(!CheckBlockSignature(block));
    SelectParams(CBaseChainParams::MAIN);
}

BOOST_AUTO_TEST_CASE(subsidy_limit_test)
{
    const CAmount maxMoneyOut = Params().GetConsensus().nMaxMoneyOut;
    const CAmount premine = GetBlockValue(1);
    const CAmount subsidy = GetBlockValue(2);
    const CAmount expectedPremine = 264444444 * COIN + 18 * CENT;

    BOOST_CHECK_EQUAL(maxMoneyOut, 777777777 * COIN);
    BOOST_CHECK_EQUAL(premine, expectedPremine);
    BOOST_CHECK_EQUAL(premine * 100, maxMoneyOut * 34);
    BOOST_CHECK_EQUAL(subsidy, 10 * COIN);
    BOOST_CHECK(Params().GetConsensus().MoneyRange(premine));
    BOOST_CHECK(Params().GetConsensus().MoneyRange(subsidy));

    // PoW phase ends at height (PoS activation height - 1).
    const int posActivationHeight = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight;
    BOOST_CHECK(posActivationHeight > 1);
    const int powEndHeight = posActivationHeight - 1;

    CAmount powSum = 0;
    for (int h = 1; h <= powEndHeight; ++h) {
        const CAmount nSubsidy = GetBlockValue(h);
        BOOST_CHECK(Params().GetConsensus().MoneyRange(nSubsidy));
        powSum += nSubsidy;
    }
    // Height 1 is premine, heights [2..powEndHeight] are constant subsidy.
    const CAmount expectedPowSum = premine + (powEndHeight - 1) * subsidy;
    BOOST_CHECK_EQUAL(powSum, expectedPowSum);
    BOOST_CHECK(powSum <= maxMoneyOut);

    // Verify cap: full subsidy blocks are followed by a final tail reward, if needed.
    const CAmount remainingAfterPremine = maxMoneyOut - premine;
    const int64_t fullRewardBlocks = remainingAfterPremine / subsidy;
    const CAmount tailReward = remainingAfterPremine % subsidy;
    const int64_t lastFullRewardHeight = 1 + fullRewardBlocks;
    BOOST_CHECK_EQUAL(GetBlockValue(lastFullRewardHeight), subsidy);
    if (tailReward > 0) {
        BOOST_CHECK_EQUAL(GetBlockValue(lastFullRewardHeight + 1), tailReward);
        BOOST_CHECK_EQUAL(GetBlockValue(lastFullRewardHeight + 2), 0);
    } else {
        BOOST_CHECK_EQUAL(GetBlockValue(lastFullRewardHeight + 1), 0);
    }
}

BOOST_AUTO_TEST_CASE(subsidy_policy_scope_test)
{
    const CAmount expectedPremine = 264444444 * COIN + 18 * CENT;

    SelectParams(CBaseChainParams::MAIN);
    BOOST_CHECK_EQUAL(GetBlockValue(1), expectedPremine);
    BOOST_CHECK_EQUAL(GetBlockValue(2), 10 * COIN);

    SelectParams(CBaseChainParams::TESTNET);
    BOOST_CHECK_EQUAL(GetBlockValue(1), expectedPremine);
    BOOST_CHECK_EQUAL(GetBlockValue(2), 10 * COIN);

    SelectParams(CBaseChainParams::MAIN);
}

BOOST_AUTO_TEST_CASE(hard_supply_cap_uses_parent_chain_mint_test)
{
    SelectParams(CBaseChainParams::MAIN);
    const CAmount maxMoneyOut = Params().GetConsensus().nMaxMoneyOut;

    BOOST_CHECK_EQUAL(GetBlockValue(2, maxMoneyOut - 7 * COIN), 7 * COIN);
    BOOST_CHECK_EQUAL(GetBlockValue(2, maxMoneyOut), 0);
    BOOST_CHECK_EQUAL(GetBlockValue(2, maxMoneyOut + COIN), 0);
    BOOST_CHECK_EQUAL(GetBlockValue(2, Params().GetConsensus().nPremineReward), 10 * COIN);
}

BOOST_AUTO_TEST_CASE(block_index_chain_mint_roundtrip_test)
{
    CBlockIndex index;
    index.nHeight = 42;
    index.nChainMinted = 123456789 * COIN;

    CDataStream stream(SER_DISK, DBI_SER_VERSION_CHAIN_MINT);
    stream << CDiskBlockIndex(&index);

    CDiskBlockIndex decoded;
    stream >> decoded;
    BOOST_CHECK(decoded.fHasChainMinted);
    BOOST_CHECK_EQUAL(decoded.nChainMinted, index.nChainMinted);

    CDataStream legacyStream(SER_DISK, DBI_SER_VERSION_NO_ZC);
    legacyStream << CDiskBlockIndex(&index);

    CDiskBlockIndex legacyDecoded;
    legacyStream >> legacyDecoded;
    BOOST_CHECK(!legacyDecoded.fHasChainMinted);
    BOOST_CHECK_EQUAL(legacyDecoded.nChainMinted, 0);
}

BOOST_AUTO_TEST_CASE(consensus_upgrade_schedule_safety_test)
{
    const auto assert_mainnet_safe_schedule = [](const Consensus::Params& consensus) {
        const int posHeight = consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight;
        BOOST_REQUIRE(posHeight > 0);

        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight, posHeight);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight, posHeight);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight, posHeight);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight, posHeight);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight, posHeight);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight, posHeight);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight, posHeight);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_6].nActivationHeight, posHeight);
        // OrganicLife activates the deterministic-masternode system at genesis.
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight,
                          Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight,
                          Consensus::NetworkUpgrade::ALWAYS_ACTIVE);

        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight,
                          Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    };

    const auto assert_testnet_schedule = [](const Consensus::Params& consensus) {
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight, 40);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_PQ].nActivationHeight, 1);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_6].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    };

    SelectParams(CBaseChainParams::MAIN);
    assert_mainnet_safe_schedule(Params().GetConsensus());

    SelectParams(CBaseChainParams::TESTNET);
    assert_testnet_schedule(Params().GetConsensus());
}

BOOST_AUTO_TEST_CASE(mainnet_spork_policy_test)
{
    SelectParams(CBaseChainParams::MAIN);
    UpgradeHeightRestorer restoreHeights({
        Consensus::UPGRADE_POS,
        Consensus::UPGRADE_POS_V2,
    });

    sporkManager.Clear();
    BOOST_CHECK(!sporkManager.IsSporkActive(SPORK_21_LEGACY_MNS_MAX_HEIGHT));

    const CSporkMessage spork21(SPORK_21_LEGACY_MNS_MAX_HEIGHT, 1, GetTime());
    sporkManager.AddOrUpdateSporkMessage(spork21);
    BOOST_CHECK(!sporkManager.IsSporkActive(SPORK_21_LEGACY_MNS_MAX_HEIGHT));

    sporkManager.Clear();
    BOOST_CHECK(!sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT));

    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, 0);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS_V2, 0);

    BOOST_CHECK(sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT));
}

BOOST_AUTO_TEST_CASE(mainnet_genesis_retime_lock_test)
{
    SelectParams(CBaseChainParams::MAIN);

    BOOST_CHECK_EQUAL(Params().GenesisBlock().nTime, 1789567200U);
    BOOST_CHECK_EQUAL(Params().GetConsensus().hashGenesisBlock,
                      uint256S("0x0000091cf3aeeed50f65d6e640a35029d7b541b4e42950232385b13e88a12fdb"));
    BOOST_CHECK_EQUAL(Params().GenesisBlock().hashMerkleRoot,
                      uint256S("0xe4f8e329c7db11bdfbb7b2e6716f8d1ca8d1ff8c41e186135c873987e15ce8fc"));
    BOOST_CHECK_EQUAL(Params().Checkpoints().mapCheckpoints->at(0), Params().GetConsensus().hashGenesisBlock);
    BOOST_CHECK_EQUAL(Params().GetConsensus().nTargetSpacing, 2 * 60);
    BOOST_CHECK_EQUAL(Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight, 10081);
}

BOOST_AUTO_TEST_CASE(mainnet_launch_header_retry_test)
{
    const int64_t launch = 1789567200;
    struct ResetTime { ~ResetTime() { SetMockTime(0); } } reset;
    LOCK(cs_main);
    auto* genesis = chainActive.Genesis();
    BOOST_REQUIRE(genesis);
    CBlock block;
    block.nVersion = 8;
    block.hashPrevBlock = genesis->GetBlockHash();
    block.nBits = Params().GenesisBlock().nBits;
    block.nTime = launch + 1;

    SetMockTime(launch - 1);
    CValidationState genesisState;
    BOOST_CHECK(AcceptBlockHeader(Params().GenesisBlock(), genesisState));
    CValidationState early;
    BOOST_CHECK(!AcceptBlockHeader(block, early));
    BOOST_CHECK_EQUAL(early.GetRejectReason(), "mainnet-not-launched");
    BOOST_CHECK_EQUAL(early.GetDoSScore(), 0);
    BOOST_CHECK(!LookupBlockIndex(block.GetHash()));

    SetMockTime(launch);
    CValidationState onTime;
    BOOST_REQUIRE(AcceptBlockHeader(block, onTime));
    BOOST_CHECK(!(LookupBlockIndex(block.GetHash())->nStatus & BLOCK_FAILED_MASK));
    SetMockTime(launch + 1);
    CValidationState after;
    BOOST_CHECK(AcceptBlockHeader(block, after));

    // Cached headers cannot bypass the wall-clock gate after a clock correction.
    SetMockTime(launch - 1);
    CValidationState cached;
    BOOST_CHECK(!AcceptBlockHeader(block, cached));
    BOOST_CHECK_EQUAL(cached.GetRejectReason(), "mainnet-not-launched");
    BOOST_CHECK(!(LookupBlockIndex(block.GetHash())->nStatus & BLOCK_FAILED_MASK));
    SetMockTime(launch);
    CValidationState retry;
    BOOST_CHECK(AcceptBlockHeader(block, retry));

    block.nTime = launch - 1;
    CValidationState oldTimestamp;
    BOOST_CHECK(!ContextualCheckBlockHeader(block, oldTimestamp, genesis));
    BOOST_CHECK_EQUAL(oldTimestamp.GetRejectReason(), "time-before-launch");
}

#ifdef ENABLE_MINING_RPC
BOOST_AUTO_TEST_CASE(mainnet_prelaunch_template_rpc_explains_wait)
{
    struct ResetTime { ~ResetTime() { SetMockTime(0); } } reset;
    SetMockTime(Params().GetConsensus().nLaunchTime - 1);
    JSONRPCRequest request;
    request.params = UniValue(UniValue::VARR);
    BOOST_CHECK_EXCEPTION(getblocktemplate(request), UniValue, [](const UniValue& error) {
        return error["message"].get_str() == "Mainnet launches on 2026-09-16 at 14:00 UTC";
    });
}
#endif

BOOST_AUTO_TEST_CASE(mainnet_prelaunch_templates_disabled_test)
{
    struct ResetTime { ~ResetTime() { SetMockTime(0); } } reset;
    SetMockTime(1789567199);
    // Skip block-validity checks to prove the assembler itself enforces launch.
    BOOST_CHECK(!BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(
        CScript() << OP_TRUE, nullptr, false, nullptr, true, false));
}

BOOST_AUTO_TEST_CASE(mainnet_message_start_isolated_from_retired_chain_test)
{
    SelectParams(CBaseChainParams::MAIN);

    const auto& messageStart = Params().MessageStart();
    BOOST_CHECK_EQUAL(messageStart[0], 0xf6);
    BOOST_CHECK_EQUAL(messageStart[1], 0x2f);
    BOOST_CHECK_EQUAL(messageStart[2], 0x01);
    BOOST_CHECK_EQUAL(messageStart[3], 0x8a);
}

BOOST_AUTO_TEST_CASE(testnet_staking_peer_threshold_test)
{
    SelectParams(CBaseChainParams::MAIN);
    BOOST_CHECK_EQUAL(GetMinimumStakingPeerEvidence(Params()), 2);
    BOOST_CHECK(RequiresNearTipStakingPeerEvidence(Params()));

    SelectParams(CBaseChainParams::TESTNET);
    BOOST_CHECK_EQUAL(GetMinimumStakingPeerEvidence(Params()), 1);
    BOOST_CHECK(RequiresNearTipStakingPeerEvidence(Params()));

    SelectParams(CBaseChainParams::REGTEST);
    BOOST_CHECK_EQUAL(GetMinimumStakingPeerEvidence(Params()), 0);
    BOOST_CHECK(!RequiresNearTipStakingPeerEvidence(Params()));

    SelectParams(CBaseChainParams::MAIN);
}

BOOST_AUTO_TEST_CASE(mainnet_bootstrap_seed_policy_test)
{
    SelectParams(CBaseChainParams::MAIN);

    BOOST_CHECK_EQUAL(Params().GetDefaultPort(), 43721);
    BOOST_CHECK(Params().FixedSeeds().empty());
    BOOST_REQUIRE_EQUAL(Params().DNSSeeds().size(), 2);
    BOOST_CHECK_EQUAL(Params().DNSSeeds()[0].host, "2.29.11.56");
    BOOST_CHECK_EQUAL(Params().DNSSeeds()[1].host, "2.29.14.202");
}

BOOST_AUTO_TEST_CASE(verification_progress_handles_future_blocks)
{
    SelectParams(CBaseChainParams::MAIN);
    CBlockIndex genesis(Params().GenesisBlock());
    genesis.nChainTx = 1;
    CBlockIndex future;
    future.nChainTx = Params().Checkpoints().nTransactionsLastCheckpoint + 1;
    future.nTime = GetTime() + 86400;
    for (bool sigchecks : {false, true}) {
        const double progress = Checkpoints::GuessVerificationProgress(&genesis, sigchecks);
        BOOST_CHECK(progress >= 0.0 && progress <= 1.0);
        BOOST_CHECK_EQUAL(Checkpoints::GuessVerificationProgress(&future, sigchecks), 1.0);
    }
}

BOOST_AUTO_TEST_CASE(testnet_bootstrap_seed_policy_test)
{
    SelectParams(CBaseChainParams::TESTNET);

    BOOST_CHECK_EQUAL(Params().GetDefaultPort(), 49716);
    BOOST_CHECK(Params().FixedSeeds().empty());
    BOOST_REQUIRE_EQUAL(Params().DNSSeeds().size(), 2);
    BOOST_CHECK_EQUAL(Params().DNSSeeds()[0].host, "2.29.11.56");
    BOOST_CHECK_EQUAL(Params().DNSSeeds()[1].host, "2.29.14.202");

    SelectParams(CBaseChainParams::MAIN);
}

bool ReturnFalse() { return false; }
bool ReturnTrue() { return true; }

BOOST_AUTO_TEST_CASE(test_combiner_all)
{
    boost::signals2::signal<bool(), CombinerAll> Test;
    BOOST_CHECK(Test());
    Test.connect(&ReturnFalse);
    BOOST_CHECK(!Test());
    Test.connect(&ReturnTrue);
    BOOST_CHECK(!Test());
    Test.disconnect(&ReturnFalse);
    BOOST_CHECK(Test());
    Test.disconnect(&ReturnTrue);
    BOOST_CHECK(Test());
}

BOOST_AUTO_TEST_SUITE_END()
