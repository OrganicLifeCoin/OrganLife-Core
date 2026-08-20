// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"

#include "chainparamsseeds.h"
#include "consensus/merkle.h"
#include "tinyformat.h"
#include "utilstrencodings.h"

#include <assert.h>

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.vtx.push_back(std::make_shared<const CTransaction>(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.nVersion = nVersion;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

void CChainParams::UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex idx, int nActivationHeight)
{
    assert(idx > Consensus::BASE_NETWORK && idx < Consensus::MAX_NETWORK_UPGRADES);
    consensus.vUpgrades[idx].nActivationHeight = nActivationHeight;
}

namespace {
static constexpr int64_t GOVERNANCE_CYCLE_SECONDS = 14 * 24 * 60 * 60;
// OrganicLife targets a 55,555 monthly treasury while retaining the existing two 14-day governance cycles.
static constexpr CAmount MONTHLY_GOVERNANCE_BUDGET = 55555 * COIN;
static constexpr CAmount GOVERNANCE_CYCLE_BUDGET = MONTHLY_GOVERNANCE_BUDGET / 2;
static constexpr int GOVERNANCE_MAX_CYCLE_PAYMENTS = 26; // ~1 year at 14-day cycles
static constexpr CAmount ORGANICLIFE_PUBLIC_PREMINE = 264444444 * COIN + 18 * CENT;

static int BudgetCycleBlocksFromTargetSpacing(int64_t targetSpacing)
{
    if (targetSpacing <= 0) {
        targetSpacing = 1;
    }
    return static_cast<int>(GOVERNANCE_CYCLE_SECONDS / targetSpacing);
}
} // namespace

/**
 * Build the genesis block.
 *
 * Note: the genesis coinbase output is not spendable in this codebase.
 */
static CBlock CreateOrganicLifeGenesisBlock(const char* pszTimestamp, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const CScript genesisOutputScript = CScript() << ParseHex("04c10e83b2703ccf322f7dbd62dd5855ac7c10bd055814ce121ba32607d573b8810c02c0582aed05b4deb9c4b77b26d92428c61256cd42774babea0a073b2ed0c9") << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    return CreateOrganicLifeGenesisBlock("OrganicLife Coin Regtest Genesis 2026-08-02", nTime, nNonce, nBits, nVersion, genesisReward);
}

// this one is for testing only
static Consensus::LLMQParams llmq_test = {
        .type = Consensus::LLMQ_TEST,
        .name = "llmq_test",
        .size = 3,
        .minSize = 2,
        .threshold = 2,

        .dkgInterval = 20, // one every 20 minutes
        .dkgPhaseBlocks = 2,
        .dkgMiningWindowStart = 10, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 15,
        .dkgBadVotesThreshold = 2,

        .signingActiveQuorumCount = 2, // just a few ones to allow easier testing

        .keepOldConnections = 3,
        .recoveryMembers = 3,

        .cacheDkgInterval = 60,
};

static Consensus::LLMQParams llmq50_60 = {
        .type = Consensus::LLMQ_50_60,
        .name = "llmq_50_60",
        .size = 50,
        .minSize = 40,
        .threshold = 30,

        .dkgInterval = 60, // one DKG per hour
        .dkgPhaseBlocks = 6,
        .dkgMiningWindowStart = 30, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 40,
        .dkgBadVotesThreshold = 40,

        .signingActiveQuorumCount = 24, // a full day worth of LLMQs

        .keepOldConnections = 25,
        .recoveryMembers = 25,

        .cacheDkgInterval = 600,
};

static Consensus::LLMQParams llmq400_60 = {
        .type = Consensus::LLMQ_400_60,
        .name = "llmq_400_60",
        .size = 400,
        .minSize = 300,
        .threshold = 240,

        .dkgInterval = 60 * 12, // one DKG every 12 hours
        .dkgPhaseBlocks = 10,
        .dkgMiningWindowStart = 50, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 70,
        .dkgBadVotesThreshold = 300,

        .signingActiveQuorumCount = 4, // two days worth of LLMQs

        .keepOldConnections = 5,
        .recoveryMembers = 100,

        .cacheDkgInterval = 60 * 12 * 10, // dkgInterval * 10
};

// Used for deployment and min-proto-version signaling, so it needs a higher threshold
static Consensus::LLMQParams llmq400_85 = {
        .type = Consensus::LLMQ_400_85,
        .name = "llmq_400_85",
        .size = 400,
        .minSize = 350,
        .threshold = 340,

        .dkgInterval = 60 * 24, // one DKG every 24 hours
        .dkgPhaseBlocks = 10,
        .dkgMiningWindowStart = 50, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 70, // give it a larger mining window to make sure it is mined
        .dkgBadVotesThreshold = 300,

        .signingActiveQuorumCount = 4, // four days worth of LLMQs

        .keepOldConnections = 5,
        .recoveryMembers = 100,

        .cacheDkgInterval = 60 * 24 * 10, // dkgInterval * 10
};

/**
 * Main network
 */
/**
 * What makes a good checkpoint block?
 * + Is surrounded by blocks with reasonable timestamps
 *   (no blocks before with a timestamp after, none after with
 *    timestamp before)
 * + Contains no strange transactions
 */
static MapCheckpoints mapCheckpoints = {
    {0, uint256S("0x0000012e114f3ce58cd05631b29091dc543db22061f852dc50b26967d082de6e")},
};

static const CCheckpointData data = {
    &mapCheckpoints,
    1785672000, // * UNIX timestamp of genesis checkpoint block (0)
    0,          // * total number of transactions between genesis and last checkpoint
    1800        // * estimated number of transactions per day after checkpoint
};

static MapCheckpoints mapCheckpointsTestnet = {
    {0, uint256S("0x00000da1c1aee747221262d679d0b0e18ae5b40baccd97fb09be864a2623b484")},
};

static const CCheckpointData dataTestnet = {
    &mapCheckpointsTestnet,
    1785671999,  // timestamp of genesis checkpoint block (0)
    0,           // estimated tx count
    500};        // estimated tx per day

static MapCheckpoints mapCheckpointsRegtest = {{0, uint256S("0x6cbed3ed562f675e62738ee15283cdc2a4631330f665c11c64c3c65a2810f527")}};
static const CCheckpointData dataRegtest = {
    &mapCheckpointsRegtest,
    1785671998,
    1,
    100};

class CMainParams : public CChainParams
{
public:
    CMainParams()
    {
        strNetworkID = "main";

        genesis = CreateOrganicLifeGenesisBlock("OrganicLife Coin Genesis 2026-08-02", 1785672000, 490347, 0x1e0ffff0, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x0000012e114f3ce58cd05631b29091dc543db22061f852dc50b26967d082de6e"));
        assert(genesis.hashMerkleRoot == uint256S("0x33f4424ac84d7e801d2b09fc982a24c9228747a3f01f7402029a4664f1e63a44"));

        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.powLimit   = uint256S("0x00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV1 = uint256S("0x000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV2 = uint256S("0x00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nBudgetFeeConfirmations = 6;      // Number of confirmations for the finalization fee
        consensus.nCoinbaseMaturity = 100;
        consensus.nFutureTimeDriftPoW = 7200;  // 2 hours - generous clock skew tolerance
        consensus.nFutureTimeDriftPoS = 900;   // 15 minutes (7.5 blocks for 2-min blocks) - for network latency
        consensus.nMaxMoneyOut = 777777777 * COIN;
        consensus.nPremineReward = ORGANICLIFE_PUBLIC_PREMINE;
        consensus.nBlockSubsidy = 10 * COIN;
        consensus.nMNCollateralAmt = 4000 * COIN;
        // Masternode rewards are only paid after PoS starts (see GetMasternodePayment()).
        consensus.nMNBlockReward = 0 * COIN;
        consensus.nNewMNBlockReward = 6 * COIN;
        consensus.nMNCollateralMinConf = 15;
        consensus.nProposalEstablishmentTime = 60 * 60 * 24;    // must be at least a day old to make it into a budget
        consensus.nStakeMinAge = 60 * 60;
        consensus.nStakeMinDepth = 600;
        consensus.nTargetTimespan = 80 * 60;
        consensus.nTargetTimespanV2 = 60 * 60;
        consensus.nTargetSpacing = 2 * 60;
        consensus.nBudgetCycleBlocks = BudgetCycleBlocksFromTargetSpacing(consensus.nTargetSpacing); // 14 days
        consensus.nBudgetCycleAmount = GOVERNANCE_CYCLE_BUDGET;
        consensus.nTimeSlotLength = 15;
        consensus.nMaxProposalPayments = GOVERNANCE_MAX_CYCLE_PAYMENTS;

        // spork keys
        consensus.strSporkPubKey = "048a5bba528a0f0f3247292ee1982314ff73748d87eea60db16dab1da3e709b354d9101637cd5512703fa2025d40f510225657bef40c5922f7c4dd44dbf7c5bef2";
        consensus.strSporkPubKeyOld = "";
        consensus.nTime_EnforceNewSporkKey = 0;
        consensus.nTime_RejectOldSporkKey = 0;

        // height-based activations (no legacy chain history on OrganicLife)
        consensus.height_last_invalid_UTXO = -1;

        consensus.nPivxBadBlockTime = 0;
        consensus.nPivxBadBlockBits = 0;

        // Network upgrades
        consensus.vUpgrades[Consensus::BASE_NETWORK].nActivationHeight =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight =
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight           = 10081;
        consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight        = 10081;
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight         = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight          = 10081;
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight          = 10081;
        consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight          = 10081;
        consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight          = 10081;
        consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight          = 10081;
        consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight          = 10081;
        consensus.vUpgrades[Consensus::UPGRADE_V5_6].nActivationHeight          = 10081;
        consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight          = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight      = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 4-byte int at any alignment.
         */
        pchMessageStart[0] = 0xf6;
        pchMessageStart[1] = 0x2f;
        pchMessageStart[2] = 0x01;
        pchMessageStart[3] = 0x8a;
        nDefaultPort = 43721;

        // Seed nodes (bootstrap). TODO(launch): add OrganicLife seed nodes here,
        // e.g. vSeeds.emplace_back("seed.organiclifecoin.example", true);
        // and/or direct IP seeds: vSeeds.emplace_back("1.2.3.4");

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 115);   // addresses start with 'o'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 98);    // addresses start with 'g'
        base58Prefixes[STAKING_ADDRESS] = std::vector<unsigned char>(1, 95);   // addresses start with 'f'
        base58Prefixes[EXCHANGE_ADDRESS] = {0x02, 0x21, 0x0C};
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 45);        // WIF starts with 'K'
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x21, 0x0C, 0x01};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x21, 0x0C, 0x02};
        // BIP44 coin type is from https://github.com/satoshilabs/slips/blob/master/slip-0044.md
        base58Prefixes[EXT_COIN_TYPE] = {0x80, 0x00, 0x14, 0x1E}; // 5150 (unassigned in SLIP-0044 at time of writing)

        // Fixed seeds for OrganicLife mainnet (BIP155 format: networkID, length, IP, port)
        // TODO(launch): add fixed seeds once public listeners exist.
        vFixedSeeds = {};

        // Reject non-standard transactions by default
        fRequireStandard = true;

        // Sapling
        bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "olc";
        bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "olcviews";
        bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "olcivks";
        bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "olc-secret-spending-key-main";
        bech32HRPs[SAPLING_EXTENDED_FVK]         = "olcxviews";

        bech32HRPs[BLS_SECRET_KEY]               = "olc-bls-sk";
        bech32HRPs[BLS_PUBLIC_KEY]               = "olc-bls-pk";

        // long living quorum params
        consensus.llmqs[Consensus::LLMQ_50_60] = llmq50_60;
        consensus.llmqs[Consensus::LLMQ_400_60] = llmq400_60;
        consensus.llmqs[Consensus::LLMQ_400_85] = llmq400_85;
        consensus.llmqs[Consensus::LLMQ_TEST] = llmq_test;

        nLLMQConnectionRetryTimeout = 60;

        // Mainnet V4 is activated at height 10081, use test-sized quorum type for early network rollout.
        consensus.llmqTypeChainLocks = Consensus::LLMQ_TEST;

        // Tier two
        nFulfilledRequestExpireTime = 60 * 60; // fulfilled requests expire in 1 hour
    }

    const CCheckpointData& Checkpoints() const
    {
        return data;
    }

};

/**
 * Testnet (v5)
 */
class CTestNetParams : public CChainParams
{
public:
    CTestNetParams()
    {
        strNetworkID = "test";

        genesis = CreateOrganicLifeGenesisBlock("OrganicLife Coin Testnet Genesis 2026-08-02", 1785671999, 2269109, 0x1e0ffff0, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x00000da1c1aee747221262d679d0b0e18ae5b40baccd97fb09be864a2623b484"));
        assert(genesis.hashMerkleRoot == uint256S("0x1d359659f716789e16106511749b0555742154f0efa325fac559c9136aaa9272"));

        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = false;
        consensus.powLimit   = uint256S("0x00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV1 = uint256S("0x000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV2 = uint256S("0x00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nBudgetFeeConfirmations = 3;      // (only 8-blocks window for finalization on testnet)
        consensus.nCoinbaseMaturity = 1;
        consensus.nFutureTimeDriftPoW = 600;  // Increased for better tolerance across regions
        consensus.nFutureTimeDriftPoS = 900;  // Increased for better tolerance across regions
        consensus.nMaxMoneyOut = 777777777 * COIN;
        consensus.nPremineReward = ORGANICLIFE_PUBLIC_PREMINE;
        consensus.nBlockSubsidy = 10 * COIN;
        consensus.nMNCollateralAmt = 4000 * COIN;
        // Masternode rewards are only paid after PoS starts (see GetMasternodePayment()).
        consensus.nMNBlockReward = 0 * COIN;
        consensus.nNewMNBlockReward = 6 * COIN;
        consensus.nMNCollateralMinConf = 15;
        consensus.nProposalEstablishmentTime = 60 * 5;  // at least 5 min old to make it into a budget
        // Testnet is tuned for fast iteration and tiny staking networks.
        consensus.nStakeMinAge = 15 * 60;
        consensus.nStakeMinDepth = 1;
        consensus.nTargetTimespan = 80 * 60;
        consensus.nTargetTimespanV2 = 60 * 60;
        consensus.nTargetSpacing = 30;
        consensus.nBudgetCycleBlocks = BudgetCycleBlocksFromTargetSpacing(consensus.nTargetSpacing); // 14 days
        consensus.nBudgetCycleAmount = GOVERNANCE_CYCLE_BUDGET;
        consensus.nTimeSlotLength = 15;
        consensus.nMaxProposalPayments = GOVERNANCE_MAX_CYCLE_PAYMENTS;

        // spork keys
        consensus.strSporkPubKey = "04b3eeab517656ae716e2508977cf5d7c92c43db6c3d496cd5104e351015d3e981fed67d7eaf4f3fd7fc68b388f5051da76f2ec30e3732a1c1c60bcf9578ab2dac";
        consensus.strSporkPubKeyOld = "";
        consensus.nTime_EnforceNewSporkKey = 0;
        consensus.nTime_RejectOldSporkKey = 0;

        // height based activations
        consensus.height_last_invalid_UTXO = -1;

        // Network upgrades
        consensus.vUpgrades[Consensus::BASE_NETWORK].nActivationHeight =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight =
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;

        consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight           = 5041;
        consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight        = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight         = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight          = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight          = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight          = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight          = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight          = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight          = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V5_6].nActivationHeight          = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight          = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight      = Consensus::NetworkUpgrade::ALWAYS_ACTIVE;

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 4-byte int at any alignment.
         */
        pchMessageStart[0] = 0xa9;
        pchMessageStart[1] = 0xf2;
        pchMessageStart[2] = 0x5f;
        pchMessageStart[3] = 0xe6;
        nDefaultPort = 49716;

        // Seed nodes (bootstrap).
        vSeeds.emplace_back("2.29.11.56");
        vSeeds.emplace_back("2.29.14.202");

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 127);   // testnet addresses start with 't'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 196);   // testnet script addresses start with '2'
        base58Prefixes[STAKING_ADDRESS] = std::vector<unsigned char>(1, 138);  // testnet staking addresses start with 'x'
        base58Prefixes[EXCHANGE_ADDRESS] = {0x03, 0x21, 0x0C};
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x21, 0x0C, 0x11};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x21, 0x0C, 0x12};
        // Testnet BIP44 coin type is '1' (all coins' testnet default)
        base58Prefixes[EXT_COIN_TYPE] = {0x80, 0x00, 0x00, 0x01};

        // Fixed seeds are not needed while the bootstrap peers above are available.
        vFixedSeeds = {};

        fRequireStandard = false;

        // Sapling - use "tolc" prefixes for testnet to distinguish from mainnet
        bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "tolc";
        bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "tolcviews";
        bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "tolcivks";
        bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "olc-secret-spending-key-test";
        bech32HRPs[SAPLING_EXTENDED_FVK]         = "tolcxviews";

        bech32HRPs[BLS_SECRET_KEY]               = "olc-bls-sk-test";
        bech32HRPs[BLS_PUBLIC_KEY]               = "olc-bls-pk-test";

        // long living quorum params
        consensus.llmqs[Consensus::LLMQ_50_60] = llmq50_60;
        consensus.llmqs[Consensus::LLMQ_400_60] = llmq400_60;
        consensus.llmqs[Consensus::LLMQ_400_85] = llmq400_85;
        consensus.llmqs[Consensus::LLMQ_TEST] = llmq_test;

        nLLMQConnectionRetryTimeout = 60;

        consensus.llmqTypeChainLocks = Consensus::LLMQ_TEST;

        // Tier two
        nFulfilledRequestExpireTime = 60 * 60; // fulfilled requests expire in 1 hour
    }

    const CCheckpointData& Checkpoints() const
    {
        return dataTestnet;
    }
};

/**
 * Regression test
 */
class CRegTestParams : public CChainParams
{
public:
    CRegTestParams()
    {
        strNetworkID = "regtest";

        genesis = CreateGenesisBlock(1785671998, 0, 0x207fffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x6cbed3ed562f675e62738ee15283cdc2a4631330f665c11c64c3c65a2810f527"));
        assert(genesis.hashMerkleRoot == uint256S("0xfdd9758d1b3adffe58ecb6abe2c50a9458ada72ce9617665772c70c537d1e1d5"));

        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        consensus.powLimit   = uint256S("0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV1 = uint256S("0x000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV2 = uint256S("0x00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nBudgetCycleBlocks = 144;         // approx 10 cycles per day
        consensus.nBudgetFeeConfirmations = 3;      // (only 8-blocks window for finalization on regtest)
        consensus.nCoinbaseMaturity = 100;
        consensus.nFutureTimeDriftPoW = 7200;
        consensus.nFutureTimeDriftPoS = 180;
        consensus.nMaxMoneyOut = 777777777 * COIN;
        consensus.nMNCollateralAmt = 100 * COIN;
        consensus.nMNBlockReward = 0 * COIN;
        consensus.nNewMNBlockReward = 6 * COIN;
        consensus.nMNCollateralMinConf = 1;
        consensus.nProposalEstablishmentTime = 60 * 5;  // at least 5 min old to make it into a budget
        consensus.nStakeMinAge = 0;
        consensus.nStakeMinDepth = 20;
        consensus.nTargetTimespan = 80 * 60;
        consensus.nTargetTimespanV2 = 60 * 60;
        consensus.nTargetSpacing = 2 * 60;
        consensus.nBudgetCycleAmount = GOVERNANCE_CYCLE_BUDGET;
        consensus.nTimeSlotLength = 15;
        consensus.nMaxProposalPayments = 20;

        /* Spork Key for RegTest:
        WIF private key: 932HEevBSujW2ud7RfB1YF91AFygbBRQj3de3LyaCRqNzKKgWXi
        private key hex: bd4960dcbd9e7f2223f24e7164ecb6f1fe96fc3a416f5d3a830ba5720c84b8ca
        Address: yCvUVd72w7xpimf981m114FSFbmAmne7j9
        */
        consensus.strSporkPubKey = "043969b1b0e6f327de37f297a015d37e2235eaaeeb3933deecd8162c075cee0207b13537618bde640879606001a8136091c62ec272dd0133424a178704e6e75bb7";
        consensus.strSporkPubKeyOld = "";
        consensus.nTime_EnforceNewSporkKey = 0;
        consensus.nTime_RejectOldSporkKey = 0;

        // height based activations
        consensus.height_last_invalid_UTXO = -1;

        // Network upgrades
        consensus.vUpgrades[Consensus::BASE_NETWORK].nActivationHeight =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight =
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight           = 251;
        consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight        = 251;
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight         =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight          = 251;
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight          =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight          = 300;
        consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight          = 300;
        consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight          = 251;
        consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight          = 576;
        consensus.vUpgrades[Consensus::UPGRADE_V5_6].nActivationHeight          = 1000;
        consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight =
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        consensus.vUpgrades[Consensus::UPGRADE_V6_1_GOV].nActivationHeight =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 4-byte int at any alignment.
         */
        pchMessageStart[0] = 0x9b;
        pchMessageStart[1] = 0xcf;
        pchMessageStart[2] = 0x21;
        pchMessageStart[3] = 0x1d;
        nDefaultPort = 59616;

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 127);   // regtest mirrors testnet prefixes
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 196);
        base58Prefixes[STAKING_ADDRESS] = std::vector<unsigned char>(1, 138);
        base58Prefixes[EXCHANGE_ADDRESS] = {0x03, 0x21, 0x0C};
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x21, 0x0C, 0x11};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x21, 0x0C, 0x12};
        // Testnet BIP44 coin type is '1' (all coins' testnet default)
        base58Prefixes[EXT_COIN_TYPE] = {0x80, 0x00, 0x00, 0x01};

        // Reject non-standard transactions by default
        fRequireStandard = true;

        // Sapling - use "rolc" prefixes for regtest to distinguish from mainnet/testnet
        bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "rolc";
        bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "rolcviews";
        bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "rolcivks";
        bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "olc-secret-spending-key-regtest";
        bech32HRPs[SAPLING_EXTENDED_FVK]         = "rolcxviews";

        bech32HRPs[BLS_SECRET_KEY]               = "olc-bls-sk-regtest";
        bech32HRPs[BLS_PUBLIC_KEY]               = "olc-bls-pk-regtest";

        // long living quorum params
        consensus.llmqs[Consensus::LLMQ_TEST] = llmq_test;
        nLLMQConnectionRetryTimeout = 10;

        consensus.llmqTypeChainLocks = Consensus::LLMQ_TEST;

        // Tier two
        nFulfilledRequestExpireTime = 60 * 60; // fulfilled requests expire in 1 hour
    }

    const CCheckpointData& Checkpoints() const
    {
        return dataRegtest;
    }
};

static std::unique_ptr<CChainParams> globalChainParams;

const CChainParams &Params()
{
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<CChainParams> CreateChainParams(const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN)
        return std::unique_ptr<CChainParams>(new CMainParams());
    else if (chain == CBaseChainParams::TESTNET)
        return std::unique_ptr<CChainParams>(new CTestNetParams());
    else if (chain == CBaseChainParams::REGTEST)
        return std::unique_ptr<CChainParams>(new CRegTestParams());
    throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
    SelectBaseParams(network);
    globalChainParams = CreateChainParams(network);
}

void UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex idx, int nActivationHeight)
{
    globalChainParams->UpdateNetworkUpgradeParameters(idx, nActivationHeight);
}
