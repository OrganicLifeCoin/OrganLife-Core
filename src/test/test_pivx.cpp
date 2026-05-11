// Copyright (c) 2011-2013 The Bitcoin Core developers
// Copyright (c) 2017-2022 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define BOOST_TEST_MODULE Pivx Test Suite

#include "test/test_pivx.h"

#include "blockassembler.h"
#include "consensus/merkle.h"
#include "bls/bls_wrapper.h"
#include "budget/budgetmanager.h"
#include "guiinterface.h"
#include "evo/deterministicmns.h"
#include "evo/evodb.h"
#include "evo/evonotificationinterface.h"
#include "evo/governancevoteindex.h"
#include "llmq/quorums_init.h"
#include "masternode-payments.h"
#include "masternodeman.h"
#include "miner.h"
#include "net_processing.h"
#include "rpc/server.h"
#include "rpc/register.h"
#include "pow.h"
#include "script/sigcache.h"
#include "spork.h"
#include "sporkdb.h"
#include "streams.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "txmempool.h"
#include "utiltime.h"
#include "validation.h"

#include "chainparams.h"
#include "consensus/upgrades.h"

#include <boost/test/unit_test.hpp>

#include <array>
#include <map>

std::unique_ptr<CConnman> g_connman;

CClientUIInterface uiInterface;  // Declared but not defined in guiinterface.h

FastRandomContext g_insecure_rand_ctx;
/** Random context to get unique temp data dirs. Separate from g_insecure_rand_ctx, which can be seeded from a const env var */
static FastRandomContext g_insecure_rand_ctx_temp_path;

/** Return the unsigned from the environment var if available, otherwise 0 */
static uint256 GetUintFromEnv(const std::string& env_name)
{
    const char* num = std::getenv(env_name.c_str());
    if (!num) return {};
    return uint256S(num);
}

void Seed(FastRandomContext& ctx)
{
    // Should be enough to get the seed once for the process
    static uint256 seed{};
    static const std::string RANDOM_CTX_SEED{"RANDOM_CTX_SEED"};
    if (seed.IsNull()) seed = GetUintFromEnv(RANDOM_CTX_SEED);
    if (seed.IsNull()) seed = GetRandHash();
    LogPrintf("%s: Setting random seed for current tests to %s=%s\n", __func__, RANDOM_CTX_SEED, seed.GetHex());
    ctx = FastRandomContext(seed);
}

extern bool fPrintToConsole;
extern void noui_connect();

namespace {
struct UpgradeDefaults {
    bool initialized{false};
    std::array<int, Consensus::MAX_NETWORK_UPGRADES> heights{};
};

static std::map<std::string, UpgradeDefaults> g_upgrade_defaults;

static void ResetNetworkUpgradeParametersToDefaults(const std::string& chainName)
{
    auto& defaults = g_upgrade_defaults[chainName];
    if (!defaults.initialized) {
        for (uint32_t idx = Consensus::BASE_NETWORK + 1; idx < Consensus::MAX_NETWORK_UPGRADES; ++idx) {
            defaults.heights[idx] = Params().GetConsensus().vUpgrades[idx].nActivationHeight;
        }
        defaults.initialized = true;
    }

    for (uint32_t idx = Consensus::BASE_NETWORK + 1; idx < Consensus::MAX_NETWORK_UPGRADES; ++idx) {
        UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex(idx), defaults.heights[idx]);
    }
}

static void ResetTierTwoAndSporks()
{
    SetMockTime(0);
    g_mock_deterministic_tests = false;
    sporkManager.Clear();
    g_budgetman.Clear();
    masternodePayments.Clear();
    mnodeman.Clear();
    g_tiertwo_sync_state.ResetData();
    g_tiertwo_sync_state.SetBlockchainSync(false, 0);
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_INITIAL);
}
} // namespace

std::ostream& operator<<(std::ostream& os, const uint256& num)
{
    os << num.ToString();
    return os;
}

BasicTestingSetup::BasicTestingSetup(const std::string& chainName)
    : m_path_root{fs::temp_directory_path() / "test_pivx" / std::to_string(g_insecure_rand_ctx_temp_path.rand32())}
{
    ECC_Start();
    BLSInit();
    SetupEnvironment();
    InitSignatureCache();
    fCheckBlockIndex = true;
    SelectParams(chainName);
    ResetNetworkUpgradeParametersToDefaults(chainName);
    ResetTierTwoAndSporks();
    SeedInsecureRand();
    evoDb.reset(new CEvoDB(1 << 20, true, true));
    deterministicMNManager.reset(new CDeterministicMNManager(*evoDb));
    governanceVoteIndex.reset(new CGovernanceVoteIndex(*evoDb));
}

BasicTestingSetup::~BasicTestingSetup()
{
    fs::remove_all(m_path_root);
    ECC_Stop();
    governanceVoteIndex.reset();
    deterministicMNManager.reset();
    evoDb.reset();
}

fs::path BasicTestingSetup::SetDataDir(const std::string& name)
{
    fs::path ret = m_path_root / name;
    fs::create_directories(ret);
    gArgs.ForceSetArg("-datadir", ret.string());
    return ret;
}

TestingSetup::TestingSetup(const std::string& chainName) : BasicTestingSetup(chainName)
{
        SetDataDir("tempdir");
        ClearDatadirCache();

        // Start the lightweight task scheduler thread
        CScheduler::Function serviceLoop = std::bind(&CScheduler::serviceQueue, &scheduler);
        threadGroup.create_thread(std::bind(&TraceThread<CScheduler::Function>, "scheduler", serviceLoop));

        // Note that because we don't bother running a scheduler thread here,
        // callbacks via CValidationInterface are unreliable, but that's OK,
        // our unit tests aren't testing multiple parts of the code at once.
        GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);

        g_connman = std::make_unique<CConnman>(0x1337, 0x1337); // Deterministic randomness for tests.
        connman = g_connman.get();

        // Register EvoNotificationInterface
        pEvoNotificationInterface = new EvoNotificationInterface();
        RegisterValidationInterface(pEvoNotificationInterface);

        // Ideally we'd move all the RPC tests to the functional testing framework
        // instead of unit tests, but for now we need these here.
        RegisterAllCoreRPCCommands(tableRPC);
        zerocoinDB.reset(new CZerocoinDB(0, true));
        pSporkDB.reset(new CSporkDB(0, true));
        pblocktree.reset(new CBlockTreeDB(1 << 20, true));
        pcoinsdbview.reset(new CCoinsViewDB(1 << 23, true));
        pcoinsTip.reset(new CCoinsViewCache(pcoinsdbview.get()));
        llmq::InitLLMQSystem(*evoDb, &scheduler, true);
        if (!LoadGenesisBlock()) {
            throw std::runtime_error("Error initializing block database");
        }
        {
            CValidationState state;
            bool ok = ActivateBestChain(state);
            BOOST_CHECK(ok);
        }
        nScriptCheckThreads = 3;
        for (int i=0; i < nScriptCheckThreads-1; i++)
            threadGroup.create_thread(&ThreadScriptCheck);
        peerLogic.reset(new PeerLogicValidation(connman));
}

TestingSetup::~TestingSetup()
{
        scheduler.stop();
        llmq::InterruptLLMQSystem();
        threadGroup.interrupt_all();
        threadGroup.join_all();
        GetMainSignals().FlushBackgroundCallbacks();
        UnregisterAllValidationInterfaces();
        GetMainSignals().UnregisterBackgroundSignalScheduler();
        g_connman.reset();
        peerLogic.reset();
        UnloadBlockIndex();
        delete pEvoNotificationInterface;
        pcoinsTip.reset();
        pcoinsdbview.reset();
        pblocktree.reset();
        zerocoinDB.reset();
        pSporkDB.reset();
        llmq::DestroyLLMQSystem();
}

// Test chain only available on regtest
TestChainSetup::TestChainSetup(int blockCount) : TestingSetup(CBaseChainParams::REGTEST)
{
    // if blockCount is over PoS start, delay it to 100 blocks after.
    if (blockCount > Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight) {
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, blockCount + 100);
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V3_4, blockCount + 101);
    }

    // Generate a blockCount-block chain:
    coinbaseKey.MakeNewKey(true);
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    for (int i = 0; i < blockCount; i++)
    {
        std::vector<CMutableTransaction> noTxns;
        CBlock b = CreateAndProcessBlock(noTxns, scriptPubKey);
        coinbaseTxns.push_back(*b.vtx[0]);
    }
}

// Create a new block with coinbase paying to scriptPubKey, and try to add it to the current chain.
// Include given transactions, and, if fNoMempoolTx=true, remove transactions coming from the mempool.
CBlock TestChainSetup::CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey, bool fNoMempoolTx)
{
    CBlock block = CreateBlock(txns, scriptPubKey, fNoMempoolTx);
    ProcessNewBlock(std::make_shared<const CBlock>(block), nullptr);
    return block;
}

CBlock TestChainSetup::CreateAndProcessBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey)
{
    CScript scriptPubKey = CScript() <<  ToByteVector(scriptKey.GetPubKey()) << OP_CHECKSIG;
    return CreateAndProcessBlock(txns, scriptPubKey);
}

CBlock TestChainSetup::CreateBlock(const std::vector<CMutableTransaction>& txns,
                                   const CScript& scriptPubKey,
                                   bool fNoMempoolTx,
                                   bool fTestBlockValidity,
                                   bool fIncludeQfc,
                                   CBlockIndex* customPrevBlock)
{
    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(
            Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptPubKey,
                                                            nullptr,       // wallet
                                                            false,   // fProofOfStake
                                                            nullptr, // availableCoins
                                                            fNoMempoolTx,
                                                            fTestBlockValidity,
                                                            customPrevBlock,
                                                            true,
                                                            fIncludeQfc);
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block);

    // Add passed-in txns:
    for (const CMutableTransaction& tx : txns) {
        pblock->vtx.push_back(MakeTransactionRef(tx));
    }

    const int nHeight = (customPrevBlock != nullptr ? customPrevBlock->nHeight + 1
                                                    : WITH_LOCK(cs_main, return chainActive.Height()) + 1);

    // Re-compute sapling root
    pblock->hashFinalSaplingRoot = CalculateSaplingTreeRoot(pblock.get(), nHeight, Params());

    // Find valid PoW
    assert(SolveBlock(pblock, nHeight));
    return *pblock;
}

CBlock TestChainSetup::CreateBlock(const std::vector<CMutableTransaction>& txns, const CKey& scriptKey,
                                   bool fTestBlockValidity)
{
    CScript scriptPubKey = CScript() <<  ToByteVector(scriptKey.GetPubKey()) << OP_CHECKSIG;
    return CreateBlock(txns, scriptPubKey, fTestBlockValidity);
}

std::shared_ptr<CBlock> FinalizeBlock(std::shared_ptr<CBlock> pblock)
{
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
    while (!CheckProofOfWork(pblock->GetHash(), pblock->nBits)) { ++(pblock->nNonce); }
    return pblock;
}

TestChainSetup::~TestChainSetup()
{
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CMutableTransaction& tx)
{
    CTransaction txn(tx);
    return FromTx(txn);
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CTransaction& txn)
{
    return CTxMemPoolEntry(MakeTransactionRef(txn), nFee, nTime, nHeight,
                           spendsCoinbaseOrCoinstake, sigOpCount);
}

[[noreturn]] void Shutdown(void* parg)
{
    std::exit(0);
}

[[noreturn]] void StartShutdown()
{
    std::exit(0);
}

bool ShutdownRequested()
{
  return false;
}
