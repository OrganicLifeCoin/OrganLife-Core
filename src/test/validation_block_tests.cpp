// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <map>

#include "blockassembler.h"
#include "chainparams.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "net_processing.h"
#include "netbase.h"
#include "pow.h"
#include "pqtransaction.h"
#include "random.h"
#include "test/test_organiclife.h"
#include "util/blockstatecatcher.h"
#include "validation.h"
#include "validationinterface.h"
#include <boost/scope_exit.hpp>


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

BOOST_AUTO_TEST_CASE(test_chain_equal_work_converges_independent_of_arrival)
{
    CBlockIndex& parent = *WITH_LOCK(cs_main, return chainActive.Tip());
    auto preferred = GoodBlock(parent);
    auto other = GoodBlock(parent);
    if (UintToArith256(other->GetHash()) < UintToArith256(preferred->GetHash()))
        std::swap(preferred, other);
    BOOST_REQUIRE(preferred->GetHash() != other->GetHash());
    BOOST_REQUIRE_EQUAL(preferred->nBits, other->nBits);

    // Both are valid and have the same parent/work. Receiving the winner last
    // must not leave this node permanently attached to its first arrival.
    BOOST_REQUIRE(ProcessNewBlock(other, nullptr));
    BOOST_REQUIRE(ProcessNewBlock(preferred, nullptr));
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), preferred->GetHash());
    BOOST_REQUIRE(ProcessNewBlock(other, nullptr));
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), preferred->GetHash());

    // Hash ordering is only a tie-break: more work on the losing branch wins.
    CBlockIndex& losing = *WITH_LOCK(cs_main, return mapBlockIndex.at(other->GetHash()));
    const auto heavier = GoodBlock(losing);
    BOOST_REQUIRE(ProcessNewBlock(heavier, nullptr));
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), heavier->GetHash());
    SyncWithValidationInterfaceQueue();
}

BOOST_AUTO_TEST_CASE(peer_requests_and_accepts_body_for_known_header)
{
    CConnman::Options options;
    options.nSendBufferMaxSize = 1000000;
    options.nReceiveFloodSize = 1000000;
    connman->Init(options);
    const auto block = GoodBlock(*WITH_LOCK(cs_main, return chainActive.Tip()));
    CBlockIndex* index = nullptr;
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_REQUIRE(AcceptBlockHeader(*block, state, &index, chainActive.Tip()));
        BOOST_REQUIRE(index);
        BOOST_REQUIRE(!(index->nStatus & BLOCK_HAVE_DATA));
    }
    CNode peer(9001, NODE_NETWORK, 0, INVALID_SOCKET,
        CAddress(LookupNumeric("198.51.100.1", 7777), NODE_NETWORK), 0, 0, "", true);
    CNode corruptPeer(9002, NODE_NETWORK, 0, INVALID_SOCKET,
        CAddress(LookupNumeric("198.51.100.2", 7777), NODE_NETWORK), 0, 0, "", true);
    for (CNode* node : {&peer, &corruptPeer}) {
        peerLogic->InitializeNode(node);
        node->nVersion = PROTOCOL_VERSION;
        node->SetSendVersion(PROTOCOL_VERSION);
        node->nRecvVersion = PROTOCOL_VERSION;
        node->fSuccessfullyConnected = true;
    }
    RegisterValidationInterface(peerLogic.get());
    BOOST_SCOPE_EXIT_ALL(&) {
        SyncWithValidationInterfaceQueue();
        UnregisterValidationInterface(peerLogic.get());
        bool update = false;
        peerLogic->FinalizeNode(peer.GetId(), update);
        peerLogic->FinalizeNode(corruptPeer.GetId(), update);
    };
    const auto receive = [&](CNode& sender, const char* command, const CDataStream& payload) {
        CMessageHeader header(Params().MessageStart(), command, payload.size());
        const uint256 checksum = Hash(payload.begin(), payload.end());
        std::copy_n(checksum.begin(), CMessageHeader::CHECKSUM_SIZE, header.pchChecksum);
        CDataStream wire(SER_NETWORK, PROTOCOL_VERSION);
        wire << header;
        CNetMessage message(Params().MessageStart(), SER_NETWORK, PROTOCOL_VERSION);
        BOOST_REQUIRE_EQUAL(message.readHeader(wire.data(), wire.size()), int(wire.size()));
        BOOST_REQUIRE_EQUAL(message.readData(payload.data(), payload.size()), int(payload.size()));
        {
            LOCK(sender.cs_vProcessMsg);
            sender.nProcessQueueSize += payload.size() + CMessageHeader::HEADER_SIZE;
            sender.vProcessMsg.push_back(std::move(message));
        }
        std::atomic<bool> interrupted{false};
        BOOST_REQUIRE(!sender.fPauseSend);
        peerLogic->ProcessMessages(&sender, interrupted);
        if (&sender == &peer) BOOST_CHECK(!sender.fDisconnect);
        BOOST_CHECK(sender.vProcessMsg.empty());
    };
    CDataStream inventory(SER_NETWORK, PROTOCOL_VERSION);
    inventory << std::vector<CInv>{CInv(MSG_BLOCK, block->GetHash())};
    receive(peer, NetMsgType::INV, inventory);
    {
        LOCK(peer.cs_vSend);
        BOOST_CHECK_EQUAL(peer.vSendMsg.size(), 2U); // Header and GETDATA payload.
        if (peer.vSendMsg.size() == 2) {
            CMessageHeader header(Params().MessageStart());
            CDataStream(peer.vSendMsg[0], SER_NETWORK, PROTOCOL_VERSION) >> header;
            BOOST_CHECK_EQUAL(header.GetCommand(), NetMsgType::GETDATA);
            std::vector<CInv> requested;
            CDataStream(peer.vSendMsg[1], SER_NETWORK, PROTOCOL_VERSION) >> requested;
            BOOST_REQUIRE_EQUAL(requested.size(), 1U);
            BOOST_CHECK(requested[0].hash == block->GetHash());
        }
    }
    // Header recognition must not bypass normal body validation.
    CBlock corrupt(*block);
    CMutableTransaction coinbase(*corrupt.vtx[0]);
    ++coinbase.vout[0].nValue; // Keep the original header/merkle root.
    corrupt.vtx[0] = MakeTransactionRef(std::move(coinbase));
    CDataStream invalidBody(SER_NETWORK, PROTOCOL_VERSION);
    invalidBody << corrupt;
    receive(corruptPeer, NetMsgType::BLOCK, invalidBody);
    SyncWithValidationInterfaceQueue();
    CNodeStateStats corruptStats;
    BOOST_REQUIRE(GetNodeStateStats(corruptPeer.GetId(), corruptStats));
    BOOST_CHECK(corruptStats.nMisbehavior > 0);
    {
        LOCK(cs_main);
        BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
        BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_MASK));
        BOOST_CHECK(chainActive.Tip() != index);
    }
    CDataStream body(SER_NETWORK, PROTOCOL_VERSION);
    body << *block;
    receive(peer, NetMsgType::BLOCK, body);
    {
        LOCK(cs_main);
        BOOST_CHECK(index->nStatus & BLOCK_HAVE_DATA);
        BOOST_CHECK(chainActive.Tip() == index);
    }
    {
        LOCK(peer.cs_vSend);
        peer.vSendMsg.clear();
        peer.nSendSize = 0;
        peer.nSendOffset = 0;
    }
    receive(peer, NetMsgType::INV, inventory);
    {
        LOCK(peer.cs_vSend);
        BOOST_CHECK(peer.vSendMsg.empty()); // Complete blocks are not re-requested.
    }
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
