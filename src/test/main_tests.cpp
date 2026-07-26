// Copyright (c) 2014 The Bitcoin Core developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2021 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_pivx.h"

#include "blocksignature.h"
#include "miner.h"
#include "net.h"
#include "primitives/transaction.h"
#include "script/sign.h"
#include "spork.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(main_tests, TestingSetup)

enum BlockSignatureType{
    P2PK,
    P2PKH,
    P2CS
};

CScript GetScriptForType(CPubKey pubKey, BlockSignatureType type)
{
    switch(type){
        case P2PK:
            return CScript() << pubKey << OP_CHECKSIG;
        default:
            return GetScriptForDestination(pubKey.GetID());
    }
}

std::vector<unsigned char> CreateDummyScriptSigWithKey(CPubKey pubKey)
{
    std::vector<unsigned char> vchSig;
    const CScript scriptCode;
    DummySignatureCreator(nullptr).CreateSig(vchSig, pubKey.GetID(), scriptCode, SIGVERSION_BASE);
    return vchSig;
}

CScript GetDummyScriptSigByType(CPubKey pubKey, bool isP2PK)
{
    CScript script = CScript() << CreateDummyScriptSigWithKey(pubKey);
    if (!isP2PK)
        script << ToByteVector(pubKey);
    return script;
}

CBlock CreateDummyBlockWithSignature(CKey stakingKey, BlockSignatureType type, bool useInputP2PK)
{
    CMutableTransaction txCoinStake;
    // Dummy input
    CTxIn input(uint256(), 0);
    // P2PKH input
    input.scriptSig = GetDummyScriptSigByType(stakingKey.GetPubKey(), useInputP2PK);
    // Add dummy input
    txCoinStake.vin.emplace_back(input);
    // Empty first output
    txCoinStake.vout.emplace_back(0, CScript());
    // P2PK staking output
    CScript scriptPubKey = GetScriptForType(stakingKey.GetPubKey(), type);
    txCoinStake.vout.emplace_back(0, scriptPubKey);

    // Now the block.
    CBlock block;
    block.vtx.emplace_back(std::make_shared<const CTransaction>(CTransaction())); // dummy first tx
    block.vtx.emplace_back(std::make_shared<const CTransaction>(txCoinStake));
    SignBlockWithKey(block, stakingKey);

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
    for (int i = 0; i < 20; ++i) {
        CKey stakingKey;
        stakingKey.MakeNewKey(true);
        bool useInputP2PK = i % 2 == 0;

        // Test P2PK block signature
        CBlock block = CreateDummyBlockWithSignature(stakingKey, BlockSignatureType::P2PK, useInputP2PK);
        BOOST_CHECK(TestBlockSignature(block));

        // Test P2PKH block signature
        block = CreateDummyBlockWithSignature(stakingKey, BlockSignatureType::P2PKH, useInputP2PK);
        if (useInputP2PK) {
            // If it's using a P2PK scriptsig as input and a P2PKH output
            // The block doesn't contain the public key to verify the sig anywhere.
            // Must fail.
            BOOST_CHECK(!TestBlockSignature(block));
        } else {
            BOOST_CHECK(TestBlockSignature(block));
        }
    }
}

BOOST_AUTO_TEST_CASE(subsidy_limit_test)
{
    const CAmount maxMoneyOut = Params().GetConsensus().nMaxMoneyOut;
    const CAmount premine = GetBlockValue(1);
    const CAmount subsidy = GetBlockValue(2);

    BOOST_CHECK_EQUAL(premine, 102632000 * COIN);
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
    SelectParams(CBaseChainParams::MAIN);
    BOOST_CHECK_EQUAL(GetBlockValue(1), 102632000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockValue(2), 10 * COIN);

    SelectParams(CBaseChainParams::TESTNET);
    BOOST_CHECK_EQUAL(GetBlockValue(1), 102632000 * COIN);
    BOOST_CHECK_EQUAL(GetBlockValue(2), 10 * COIN);

    SelectParams(CBaseChainParams::MAIN);
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
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight, posHeight);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight, posHeight);

        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_ZC].nActivationHeight,
                          Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight,
                          Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_ZC_PUBLIC].nActivationHeight,
                          Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight,
                          Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    };

    const auto assert_testnet_legacy_schedule = [](const Consensus::Params& consensus) {
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight, 5041);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight, 32000);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_ZC].nActivationHeight, 32200);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight, 32400);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight, 32600);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_ZC_PUBLIC].nActivationHeight, 32800);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight, 33000);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight, 33200);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight, 33400);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight, 33600);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight, 33800);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight, 34000);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V5_6].nActivationHeight, 34200);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight, 34400);
        BOOST_CHECK_EQUAL(consensus.vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight, 41000);
    };

    SelectParams(CBaseChainParams::MAIN);
    assert_mainnet_safe_schedule(Params().GetConsensus());

    SelectParams(CBaseChainParams::TESTNET);
    assert_testnet_legacy_schedule(Params().GetConsensus());
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

    BOOST_CHECK_EQUAL(Params().GenesisBlock().nTime, 1785060000U);
    BOOST_CHECK_EQUAL(Params().Checkpoints().mapCheckpoints->at(0), Params().GetConsensus().hashGenesisBlock);
    BOOST_CHECK_EQUAL(Params().GetConsensus().nTargetSpacing, 2 * 60);
    BOOST_CHECK_EQUAL(Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight, 10081);
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

BOOST_AUTO_TEST_CASE(testnet_fixed_seed_policy_test)
{
    SelectParams(CBaseChainParams::TESTNET);

    const std::vector<uint8_t> masternodeSeed{
        0x01, 0x04, 0x59, 0xA7, 0x10, 0xCA, 0xA2, 0x90
    };
    const std::vector<uint8_t>& fixedSeeds = Params().FixedSeeds();
    BOOST_CHECK(std::search(fixedSeeds.begin(), fixedSeeds.end(), masternodeSeed.begin(), masternodeSeed.end()) != fixedSeeds.end());

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
