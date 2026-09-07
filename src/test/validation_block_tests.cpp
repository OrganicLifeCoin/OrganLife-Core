// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <map>

#include "blockassembler.h"
#include "chainparams.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "pow.h"
#include "pqtransaction.h"
#include "random.h"
#include "test/test_organiclife.h"
#include "util/blockstatecatcher.h"
#include "validation.h"
#include "validationinterface.h"


#define ASSERT_WITH_MSG(cond, msg) if (!cond) { BOOST_ERROR(msg); }


BOOST_FIXTURE_TEST_SUITE(validation_block_tests, RegTestingSetup)

struct TestSubscriber : public CValidationInterface {
    uint256 m_expected_tip;
    unsigned m_connected{0};
    unsigned m_disconnected{0};

    explicit TestSubscriber(uint256 tip) : m_expected_tip(std::move(tip)) {}

    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
    {
        BOOST_CHECK_EQUAL(m_expected_tip, pindexNew->GetBlockHash());
    }

    void BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->hashPrevBlock);
        BOOST_CHECK_EQUAL(m_expected_tip, pindex->pprev->GetBlockHash());

        m_expected_tip = block->GetHash();
        ++m_connected;
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock> &block, const uint256& blockHash, int nBlockHeight, int64_t blockTime)
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->GetHash());

        m_expected_tip = block->hashPrevBlock;
        ++m_disconnected;
    }
};

std::shared_ptr<CBlock> Block(CBlockIndex& parent)
{
    static unsigned sequence = 0;
    // Branches are assembled before they enter the real block index. Supply
    // their actual parent context; ProcessNewBlock below performs validation.
    BOOST_REQUIRE(!Params().GetConsensus().NetworkUpgradeActive(parent.nHeight + 1, Consensus::UPGRADE_V5_0));
    auto ptemplate = BlockAssembler(Params(), false).CreateNewBlock(
        pq::GetScript(pq::KeyID{}), nullptr, false, nullptr, true, false, &parent);
    auto pblock = std::make_shared<CBlock>(ptemplate->block);
    pblock->nTime = parent.nTime + 1;
    pblock->nBits = GetNextWorkRequired(&parent, pblock.get());

    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = CScript() << (parent.nHeight + 1) << ++sequence;
    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));

    return pblock;
}

// construct a valid block
const std::shared_ptr<const CBlock> GoodBlock(CBlockIndex& parent)
{
    return FinalizeBlock(Block(parent));
}

// construct an invalid block (but with a valid header)
const std::shared_ptr<const CBlock> BadBlock(CBlockIndex& parent)
{
    auto pblock = Block(parent);

    CMutableTransaction coinbase_spend;
    coinbase_spend.vin.emplace_back(CTxIn(COutPoint(pblock->vtx[0]->GetHash(), 0), CScript(), 0));
    coinbase_spend.vout.emplace_back(pblock->vtx[0]->vout[0]);

    CTransactionRef tx = MakeTransactionRef(coinbase_spend);
    pblock->vtx.emplace_back(tx);

    auto ret = FinalizeBlock(pblock);
    return ret;
}

void BuildChain(CBlockIndex& root, int height, const unsigned int invalid_rate, const unsigned int branch_rate, const unsigned int max_size, std::vector<std::shared_ptr<const CBlock>>& blocks)
{
    if (height <= 0 || blocks.size() >= max_size) return;

    bool gen_invalid = GetRand(100) < invalid_rate;
    bool gen_fork = GetRand(100) < branch_rate;
    const auto extend = [&](const std::shared_ptr<const CBlock>& block) {
        const uint256 hash = block->GetHash();
        CBlockIndex parent(block->GetBlockHeader());
        parent.phashBlock = &hash;
        parent.pprev = &root;
        parent.nHeight = root.nHeight + 1;
        parent.nChainMinted = root.nChainMinted + block->vtx[0]->GetValueOut();
        parent.BuildSkip();
        BuildChain(parent, height - 1, invalid_rate, branch_rate, max_size, blocks);
    };

    const std::shared_ptr<const CBlock> pblock = gen_invalid ? BadBlock(root) : GoodBlock(root);
    blocks.emplace_back(pblock);
    if (!gen_invalid) {
        extend(pblock);
    }

    if (gen_fork) {
        blocks.emplace_back(GoodBlock(root));
        // Keep a stable shared_ptr across recursive vector growth.
        const auto branch = blocks.back();
        extend(branch);
    }
}

BOOST_AUTO_TEST_CASE(processnewblock_signals_ordering)
{
    // build a large-ish chain that's likely to have some forks
    std::vector<std::shared_ptr<const CBlock>> blocks;
    CBlockIndex& genesis = *WITH_LOCK(cs_main, return chainActive.Genesis());
    while (blocks.size() < 50) {
        blocks.clear();
        BuildChain(genesis, 100, 15, 10, 500, blocks);
    }
    std::map<uint256, int> heights{{genesis.GetBlockHash(), 0}};
    int expected_height = 4; // The deterministic reorg below reaches height 4.
    for (const auto& block : blocks) {
        const int height = heights.at(block->hashPrevBlock) + 1;
        if (block->vtx.size() == 1) {
            BOOST_REQUIRE(heights.emplace(block->GetHash(), height).second);
            expected_height = std::max(expected_height, height);
        }
    }

    // Connect the genesis block and drain any outstanding events
    BOOST_CHECK_MESSAGE(ProcessNewBlock(std::make_shared<CBlock>(Params().GenesisBlock()), nullptr), "Error: genesis not connected");
    SyncWithValidationInterfaceQueue();

    // subscribe to events (this subscriber will validate event ordering)
    const CBlockIndex* initial_tip = WITH_LOCK(cs_main, return chainActive.Tip());
    TestSubscriber sub(initial_tip->GetBlockHash());
    RegisterValidationInterface(&sub);

    // Guarantee a real disconnect/connect sequence before random scheduling;
    // rejected branch fixtures must not make the ordering test pass vacuously.
    std::vector<std::shared_ptr<const CBlock>> fork;
    BuildChain(genesis, 3, 0, 0, 3, fork);
    BuildChain(genesis, 4, 0, 0, 7, fork);
    for (const auto& block : fork) BOOST_CHECK(ProcessNewBlock(block, nullptr));
    SyncWithValidationInterfaceQueue();
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height()), 4);
    BOOST_CHECK_GE(sub.m_connected, 7U);
    BOOST_CHECK_GE(sub.m_disconnected, 3U);

    // create a bunch of threads that repeatedly process a block generated above at random
    // this will create parallelism and randomness inside validation - the ValidationInterface
    // will subscribe to events generated during block validation and assert on ordering invariance
    boost::thread_group threads;
    for (int i = 0; i < 10; i++) {
        threads.create_thread([&blocks]() {
            for (int i = 0; i < 1000; i++) {
                auto block = blocks[GetRand(blocks.size() - 1)];
                ProcessNewBlock(block, nullptr);
            }

            BlockStateCatcherWrapper sc(UINT256_ZERO);
            sc.registerEvent();
            // to make sure that eventually we process the full chain - do it here
            for (const auto& block : blocks) {
                if (block->vtx.size() == 1) {
                    sc.get().setBlockHash(block->GetHash());
                    bool processed = ProcessNewBlock(block, nullptr);
                    // Future to do: "prevblk-not-found" here is the only valid reason to not check processed flag.
                    std::string stateReason = sc.get().state.GetRejectReason();
                    if (sc.get().found && (stateReason == "duplicate" || stateReason == "prevblk-not-found" ||
                                              stateReason == "bad-prevblk" || stateReason == "blk-out-of-order")) continue;
                    ASSERT_WITH_MSG(processed, ("Error: " + sc.get().state.GetRejectReason()).c_str());
                }
            }
        });
    }

    threads.join_all();
    while (GetMainSignals().CallbacksPending() > 0) {
        MilliSleep(100);
    }

    SyncWithValidationInterfaceQueue();
    UnregisterValidationInterface(&sub);

    BOOST_CHECK_EQUAL(sub.m_expected_tip, WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()));
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Height()), expected_height);
}

BOOST_AUTO_TEST_SUITE_END()
