// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqanchors.h>
#include <test/test_organiclife.h>
#include <boost/test/unit_test.hpp>
#include <chain.h>
#include <coins.h>
#include <consensus/merkle.h>
#include <netbase.h>
#include <util/system.h>
#include <validation.h>
#include <algorithm>


namespace {
struct AnchorSetup : BasicTestingSetup {
    std::array<mldsa44::Key, 15> keys;
    CCoinsView base;
    CCoinsViewCache view{&base};
    uint32_t nextInput{1};
    const COutPoint collateral{uint256S("aa"), 0};
    AnchorSetup() : BasicTestingSetup(CBaseChainParams::REGTEST)
    {
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 1);
        for (size_t i = 0; i < keys.size(); ++i) {
            std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
            seed[0] = static_cast<unsigned char>(i + 30); // Public deterministic test material only.
            BOOST_REQUIRE(keys[i].SetSeed(seed));
        }
        view.AddCoin(collateral, Coin(CTxOut(Params().GetConsensus().nMNCollateralAmt,
                                             pq::GetScript(ID(2))), 1, false, false), false);
    }
    pq::KeyID ID(size_t key) { return *pq::GetID(keys[key].GetPublicKey(), Params().NetworkIDString()); }

    ~AnchorSetup()
    {
        std::string reason;
        gArgs.ForceSetArg("-pqbootstrap", "");
        pqanchor::InitBootstrap(Params(), reason);
        gArgs.ForceSetArg("-pqemergencycheckpoint", "");
        pqanchor::InitRecovery(Params(), reason);
    }

    std::string BootstrapSpec(uint32_t height, const uint256& hash, const std::vector<pqquorum::Member>& members)
    {
        std::string spec = strprintf("%u:%s", height, hash.ToString());
        for (const auto& member : members)
            spec += ":" + member.registration.ToString() + ":" + HexStr(member.operator_key);
        return spec;
    }

    // Registration node uses keys[3n] owner, keys[3n+1] operator, keys[3n+2] collateral.
    pqmn::Payload Registration(size_t node, size_t port)
    {
        pqmn::Payload op;
        op.collateral = COutPoint{uint256S("cc"), nextInput++};
        view.AddCoin(op.collateral, Coin(CTxOut(Params().GetConsensus().nMNCollateralAmt,
                                                pq::GetScript(ID(3 * node + 2))), 1, false, false), false);
        op.owner = keys[3 * node].GetPublicKey();
        op.operatorKey = keys[3 * node + 1].GetPublicKey();
        op.collateralKey = keys[3 * node + 2].GetPublicKey();
        op.payout = ID(3 * node);
        op.operatorReward = 100;
        op.operatorPayout = ID(3 * node + 1);
        BOOST_REQUIRE(Lookup(strprintf("127.0.0.1:%u", port), op.service, 0, false));
        return op;
    }

    CTransactionRef FeeOnly(pqmn::Payload op, size_t node)
    {
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = CTransaction::PQ;
        tx.sapData = nullopt;
        tx.vin.emplace_back(COutPoint{uint256S("dd"), nextInput++});
        view.AddCoin(tx.vin[0].prevout, Coin(CTxOut(10 * COIN, pq::GetScript(ID(14))), 1, false, false), false);
        const auto available = view.AccessCoin(tx.vin[0].prevout).out.nValue;
        tx.vout.emplace_back(available - COIN, pq::GetScript(ID(14)));
        pq::Payload payload;
        payload.mode = pq::MASTERNODE;
        payload.authorizations.resize(1);
        payload.authorizations[0].public_key = keys[14].GetPublicKey();
        payload.data = pqmn::Encode(op);
        BOOST_REQUIRE(!payload.data.empty());
        tx.extraPayload = pq::EncodePayload(payload);
        const auto message = pqmn::SigningMessage(tx, {view.AccessCoin(tx.vin[0].prevout).out},
                                                  Params().GetConsensus().hashGenesisBlock);
        BOOST_REQUIRE(!message.empty());
        const auto sign = [&](size_t key, pqmn::Role role, pqmn::Signature& out) {
            std::vector<unsigned char> sig;
            BOOST_REQUIRE(keys[key].Sign(message, pqmn::Context(role), sig));
            std::copy(sig.begin(), sig.end(), out.begin());
        };
        if (op.action == pqmn::Action::REGISTER || op.action == pqmn::Action::UPDATE)
            sign(3 * node, pqmn::Role::OWNER, op.ownerSignature);
        sign(3 * node + 1, pqmn::Role::OPERATOR, op.operatorSignature);
        if (op.action == pqmn::Action::REGISTER) sign(3 * node + 2, pqmn::Role::COLLATERAL, op.collateralSignature);
        payload.data = pqmn::Encode(op);
        tx.extraPayload = pq::EncodePayload(payload);
        const std::vector<CTxOut> prev{view.AccessCoin(tx.vin[0].prevout).out};
        const auto feeMessage = pq::SignatureMessage(tx, prev, payload,
                                                     Params().GetConsensus().hashGenesisBlock, 0);
        BOOST_REQUIRE(!feeMessage.empty());
        std::vector<unsigned char> feeSig;
        BOOST_REQUIRE(keys[14].Sign(feeMessage, *pq::SignatureContext(Params().NetworkIDString()), feeSig));
        std::copy(feeSig.begin(), feeSig.end(), payload.authorizations[0].signature.begin());
        tx.extraPayload = pq::EncodePayload(payload);
        return MakeTransactionRef(std::move(tx));
    }

    // Builds a synthetic chain of block indexes (no disk blocks needed). The
    // hashes vector must outlive the indexes (phashBlock points into it).
    void MakeChain(std::vector<CBlockIndex>& storage, std::vector<uint256>& hashes, uint32_t height)
    {
        storage.clear();
        hashes.clear();
        hashes.resize(height + 1);
        storage.resize(height + 1);
        for (uint32_t h = 0; h <= height; ++h) {
            CBlock block;
            block.hashPrevBlock = h ? hashes[h - 1] : uint256();
            block.nTime = 100 + h;
            CMutableTransaction coinbase;
            coinbase.vin.resize(1);
            coinbase.vout.emplace_back(0, pq::GetScript(pq::KeyID{}));
            coinbase.vin[0].scriptSig = CScript() << h;
            block.vtx.push_back(MakeTransactionRef(coinbase));
            block.hashMerkleRoot = BlockMerkleRoot(block);
            hashes[h] = block.GetHash();
            storage[h] = CBlockIndex(block);
            storage[h].nHeight = h;
        }
        // Second pass: pointers are stable now that all storage is allocated.
        // pskip mirrors the real construction (skip ancestor) so GetAncestor
        // shortcuts match a normal in-memory chain.
        const auto invertLowestOne = [](int n) { return n & (n - 1); };
        const auto skipHeight = [&](int h) {
            if (h < 2) return 0;
            return (h & 1) ? invertLowestOne(invertLowestOne(h - 1)) + 1 : invertLowestOne(h);
        };
        for (uint32_t h = 0; h <= height; ++h) {
            storage[h].phashBlock = &hashes[h];
            storage[h].pprev = h ? &storage[h - 1] : nullptr;
            if (h) storage[h].pskip = skipHeight(h) ? &storage[skipHeight(h)] : nullptr;
        }
    }

    // Four mature configured registrations applied at height 20.
    void RegisterFour(pqmn::Index& index, std::string& reason)
    {
        for (size_t i = 0; i < 4; ++i) {
            const auto tx = FeeOnly(Registration(i, uint32_t(51000 + i)), i);
            BOOST_REQUIRE_MESSAGE(index.Apply(*tx, view, 20, reason), "apply " << i << ": " << reason);
        }
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(pqanchors_tests, AnchorSetup)

BOOST_AUTO_TEST_CASE(committee_selection_requires_four_mature_configured)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    std::string reason;
    // Fewer than four registrations: no committee at any height.
    BOOST_CHECK(pqanchor::SelectCommittee(index, 40).empty());
    RegisterFour(index, reason);
    auto members = pqanchor::SelectCommittee(index, 40);
    BOOST_REQUIRE_EQUAL(members.size(), 4U);
    // Sorted ascending by registration ID.
    BOOST_CHECK(std::is_sorted(members.begin(), members.end(),
                               [](const pqquorum::Member& a, const pqquorum::Member& b) {
                                   return a.registration < b.registration;
                               }));
    for (const auto& member : members)
        BOOST_CHECK(!std::all_of(member.operator_key.begin(), member.operator_key.end(),
                                 [](unsigned char c) { return c == 0; }));
    BOOST_CHECK(!pqquorum::Commitment(members).IsNull());
    // Before maturity there is no committee at all.
    BOOST_CHECK(pqanchor::SelectCommittee(index, 19).empty());
    // A fifth registration joins deterministically once mature.
    BOOST_REQUIRE_MESSAGE(index.Apply(*FeeOnly(Registration(4, uint32_t(51004)), 4), view, 20, reason),
                         "apply fifth: " << reason);
    const auto five = pqanchor::SelectCommittee(index, 40);
    BOOST_CHECK_EQUAL(five.size(), 5U);
    transaction->Commit();
}

BOOST_AUTO_TEST_CASE(snapshots_capture_changes_and_undo_restores)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    std::vector<CBlockIndex> chain;
    std::vector<uint256> hashes;
    MakeChain(chain, hashes, 30);

    // No committee before registrations: nothing captured, lookup fails.
    BOOST_REQUIRE(state.CaptureCommittee(index, 10, reason));
    std::vector<pqquorum::Member> committee;
    BOOST_CHECK(!state.CommitteeAt(10, committee));

    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 30, reason));
    BOOST_REQUIRE(state.CommitteeAt(30, committee));
    BOOST_CHECK_EQUAL(committee.size(), 4U);
    const uint256 commitment = pqquorum::Commitment(committee);
    // Unchanged state captures nothing new.
    BOOST_REQUIRE(state.CaptureCommittee(index, 31, reason));
    BOOST_REQUIRE(state.CommitteeAt(31, committee));
    BOOST_CHECK_EQUAL(committee.size(), 4U);
    BOOST_CHECK(pqquorum::Commitment(committee) == commitment);

    // Undo: reverting height 30 removes the first snapshot entirely.
    BOOST_REQUIRE(state.UndoBlock(30, std::nullopt, reason));
    BOOST_CHECK(!state.CommitteeAt(30, committee));
    BOOST_CHECK_EQUAL(state.TipHeight(), 0U);
    transaction->Commit();
}

BOOST_AUTO_TEST_CASE(anchor_records_require_snapshot_and_monotonic_height)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));

    // Without a tip the first anchor may be at any height with a snapshot below.
    const auto blockHash = uint256S("55");
    BOOST_CHECK(!state.RecordAnchor(19, blockHash, {}, reason, 22)); // below the snapshot
    BOOST_REQUIRE(state.RecordAnchor(21, blockHash, {uint256S("1")}, reason, 22));
    pqanchor::Record anchor;
    BOOST_REQUIRE(state.TipAnchor(anchor));
    BOOST_CHECK_EQUAL(anchor.height, 21U);
    BOOST_CHECK(anchor.blockHash == blockHash);
    BOOST_CHECK_EQUAL(anchor.signers.size(), 1U);
    // Strictly increasing: no skipping and no rewrite.
    BOOST_CHECK(!state.RecordAnchor(23, uint256S("66"), {}, reason, 24));
    BOOST_CHECK(!state.RecordAnchor(21, uint256S("66"), {}, reason, 22));
    // Undo restores the empty tip.
    BOOST_REQUIRE(state.UndoBlock(22, 21, reason));
    BOOST_CHECK_EQUAL(state.TipHeight(), 0U);
    transaction->Commit();
}

BOOST_AUTO_TEST_CASE(service_window_uses_certificate_carrier_not_old_finalized_height)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    // A recovered certificate for old block21 is published in block40.
    BOOST_REQUIRE(state.RecordAnchor(21, uint256S("21"), {uint256S("1"), uint256S("2")}, reason, 40));
    std::map<uint256, uint32_t> signers;
    bool any = false;
    BOOST_REQUIRE(pqanchor::SignersInWindow(*evoDb, Params(), 39, 3, signers, any));
    BOOST_CHECK(!any && signers.empty());
    BOOST_REQUIRE(pqanchor::SignersInWindow(*evoDb, Params(), 42, 3, signers, any));
    BOOST_REQUIRE(any);
    BOOST_CHECK_EQUAL(signers.size(), 2U);
    BOOST_CHECK_EQUAL(signers.at(uint256S("1")), 40U);
    BOOST_REQUIRE(pqanchor::SignersInWindow(*evoDb, Params(), 43, 3, signers, any));
    BOOST_CHECK(!any && signers.empty());
    BOOST_REQUIRE(state.RecordAnchor(22, uint256S("22"), {uint256S("2"), uint256S("3")}, reason, 41));
    BOOST_REQUIRE(pqanchor::SignersInWindow(*evoDb, Params(), 42, 3, signers, any));
    BOOST_CHECK_EQUAL(signers.at(uint256S("2")), 41U);
    BOOST_CHECK(!state.UndoBlock(42, 22, reason)); // Wrong carrier must not remove evidence.
    BOOST_REQUIRE(state.UndoBlock(41, 22, reason));
    BOOST_REQUIRE(pqanchor::SignersInWindow(*evoDb, Params(), 42, 3, signers, any));
    BOOST_CHECK_EQUAL(signers.at(uint256S("2")), 40U);
    BOOST_CHECK(!signers.count(uint256S("3")));
    BOOST_REQUIRE(state.UndoBlock(40, 21, reason));
    BOOST_REQUIRE(pqanchor::SignersInWindow(*evoDb, Params(), 42, 3, signers, any));
    BOOST_CHECK(!any && signers.empty());
}

BOOST_AUTO_TEST_CASE(service_evidence_exclusion_expiry_and_revival_feed_reward_queue)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    std::vector<pqquorum::Member> members;
    BOOST_REQUIRE(state.CommitteeAt(20, members));
    gArgs.ForceSetArg("-pqbootstrap", BootstrapSpec(20, uint256S("20"), members));
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    uint256 selected;
    pqmn::Record record;
    BOOST_REQUIRE(index.GetPayee(100, selected, record));
    const auto oldest = selected;
    const auto other = members[0].registration == oldest ? members[1].registration : members[0].registration;
    BOOST_REQUIRE(state.RecordAnchor(21, uint256S("21"), {other}, reason, 100));
    // No retroactive/current-block credit; selection uses parent state.
    BOOST_REQUIRE(index.GetPayee(100, selected, record));
    BOOST_CHECK(selected == oldest);
    BOOST_REQUIRE(index.GetPayee(101, selected, record));
    BOOST_CHECK(selected == other);
    const uint32_t expires = 100 + Params().GetConsensus().nPQServiceWindow;
    BOOST_REQUIRE(index.GetPayee(expires, selected, record));
    BOOST_CHECK(selected == other); // Last committed W blocks still include100.
    BOOST_REQUIRE(index.GetPayee(expires + 1, selected, record));
    BOOST_CHECK(selected == oldest); // No certs in the window: do not punish a network stall.
    BOOST_REQUIRE(state.RecordAnchor(22, uint256S("22"), {oldest}, reason, expires));
    BOOST_REQUIRE(index.GetPayee(expires + 1, selected, record));
    BOOST_CHECK(selected == oldest); // Fresh certificate revives the omitted signer.
    BOOST_REQUIRE(state.UndoBlock(expires, 22, reason));
    std::map<uint256, uint32_t> last;
    bool any = true;
    BOOST_REQUIRE(pqanchor::SignersInWindow(*evoDb, Params(), expires, Params().GetConsensus().nPQServiceWindow, last, any));
    BOOST_CHECK(!any && last.empty());
}

BOOST_AUTO_TEST_CASE(anchor_uses_finalized_height_committee_not_latest_carrier_snapshot)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    std::vector<pqquorum::Member> original;
    BOOST_REQUIRE(state.CommitteeAt(20, original));
    // A membership change at the very height being finalized is permitted.
    BOOST_REQUIRE(state.RecordAnchor(20, uint256S("20"), {original[0].registration}, reason, 21));
    pqanchor::Record anchor;
    BOOST_REQUIRE(state.TipAnchor(anchor));
    BOOST_CHECK(anchor.committee == pqquorum::Commitment(original));

    // A later carrier block can already have a different committee snapshot.
    auto changed = original;
    changed[0].operator_key = keys[14].GetPublicKey();
    BOOST_REQUIRE(state.SetInitialCommittee(30, changed, reason));
    BOOST_REQUIRE(state.RecordAnchor(21, uint256S("21"), {original[0].registration}, reason, 30));
    BOOST_REQUIRE(state.TipAnchor(anchor));
    BOOST_CHECK(anchor.committee == pqquorum::Commitment(original));
}

BOOST_AUTO_TEST_CASE(delayed_carrier_undo_removes_target_anchor)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    BOOST_REQUIRE(state.RecordAnchor(21, uint256S("21"), {}, reason, 40));

    BOOST_REQUIRE(state.UndoBlock(40, 21, reason));
    BOOST_CHECK_EQUAL(state.TipHeight(), 0U);
    pqanchor::Record anchor;
    BOOST_CHECK(!state.GetAnchor(21, anchor));
    transaction->Commit();
}

BOOST_AUTO_TEST_CASE(sequential_delayed_carriers_undo_in_lifo_order)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    BOOST_REQUIRE(state.RecordAnchor(21, uint256S("21"), {}, reason, 40));
    BOOST_REQUIRE(state.RecordAnchor(22, uint256S("22"), {}, reason, 50));

    BOOST_REQUIRE(state.UndoBlock(50, 22, reason));
    BOOST_CHECK_EQUAL(state.TipHeight(), 21U);
    BOOST_REQUIRE(state.UndoBlock(40, 21, reason));
    BOOST_CHECK_EQUAL(state.TipHeight(), 0U);
    transaction->Commit();
}

BOOST_AUTO_TEST_CASE(non_certificate_carrier_undo_does_not_erase_anchor)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    BOOST_REQUIRE(state.RecordAnchor(21, uint256S("21"), {}, reason, 39));

    BOOST_REQUIRE(state.UndoBlock(40, std::nullopt, reason));
    BOOST_CHECK_EQUAL(state.TipHeight(), 21U);
    pqanchor::Record anchor;
    BOOST_REQUIRE(state.GetAnchor(21, anchor));
    BOOST_CHECK(anchor.blockHash == uint256S("21"));
    transaction->Commit();
}

BOOST_AUTO_TEST_CASE(delayed_carrier_undo_fails_closed_for_missing_or_corrupt_target)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    BOOST_REQUIRE(state.RecordAnchor(21, uint256S("21"), {}, reason, 40));

    BOOST_CHECK(!state.UndoBlock(40, 22, reason));
    BOOST_CHECK_EQUAL(state.TipHeight(), 21U);

    pqanchor::Record corrupt;
    BOOST_REQUIRE(state.GetAnchor(21, corrupt));
    corrupt.height = 22;
    const auto key = std::make_pair(std::string("pqmn1f00000015"), Params().GetConsensus().hashGenesisBlock);
    evoDb->Write(key, corrupt);
    BOOST_CHECK(!state.UndoBlock(40, 21, reason));
    BOOST_CHECK_EQUAL(state.TipHeight(), 21U);
    pqanchor::Record stillCorrupt;
    BOOST_REQUIRE(state.GetAnchor(21, stillCorrupt));
    BOOST_CHECK_EQUAL(stillCorrupt.height, 22U);
    transaction->Commit();
}

BOOST_AUTO_TEST_CASE(bootstrap_configuration_is_atomic_and_exactly_four)
{
    LOCK(cs_main);
    pqmn::Index index(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    const auto members = pqanchor::SelectCommittee(index, 20);
    const auto valid = BootstrapSpec(20, uint256S("20"), members);
    gArgs.ForceSetArg("-pqbootstrap", valid);
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    BOOST_REQUIRE(pqanchor::GetBootstrap());

    BOOST_CHECK(pqanchor::InitBootstrap(*CreateChainParams("test"), reason));
    BOOST_CHECK(pqanchor::GetBootstrap() != nullptr);
    BOOST_CHECK(!pqanchor::InitBootstrap(*CreateChainParams("main"), reason));
    BOOST_CHECK(pqanchor::GetBootstrap() == nullptr);
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));

    for (const auto& invalid : {valid + ":bad:key", BootstrapSpec(UINT32_MAX, uint256S("20"), members),
                               BootstrapSpec(20, {}, members)}) {
        gArgs.ForceSetArg("-pqbootstrap", invalid);
        BOOST_CHECK(!pqanchor::InitBootstrap(Params(), reason));
        BOOST_CHECK(pqanchor::GetBootstrap() == nullptr);
    }
    auto five = members;
    five.push_back({uint256S("123"), keys[14].GetPublicKey()});
    gArgs.ForceSetArg("-pqbootstrap", BootstrapSpec(20, uint256S("20"), five));
    BOOST_CHECK(!pqanchor::InitBootstrap(Params(), reason));
    BOOST_CHECK(pqanchor::GetBootstrap() == nullptr);
    gArgs.ForceSetArg("-pqbootstrap", valid);
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    gArgs.ForceSetArg("-pqbootstrap", "");
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    BOOST_CHECK(pqanchor::GetBootstrap() == nullptr);
}

BOOST_AUTO_TEST_CASE(bootstrap_requires_historical_snapshot_not_current_registry)
{
    LOCK(cs_main);
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    const auto members = pqanchor::SelectCommittee(index, 20);
    std::vector<CBlockIndex> chain;
    std::vector<uint256> hashes;
    MakeChain(chain, hashes, 60);
    gArgs.ForceSetArg("-pqbootstrap", BootstrapSpec(20, hashes[20], members));
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    BOOST_CHECK(!pqanchor::ValidateBootstrap(state, &chain.back(), reason));
    std::vector<pqquorum::Member> snapshot;
    BOOST_CHECK(!state.CommitteeAt(20, snapshot)); // Validation cannot manufacture evidence.
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    BOOST_REQUIRE(pqanchor::ValidateBootstrap(state, &chain.back(), reason));

    pqmn::Payload revoke;
    revoke.action = pqmn::Action::REVOKE;
    revoke.sequence = 1;
    for (const auto& member : members)
        if (member.operator_key == keys[1].GetPublicKey()) revoke.registration = member.registration;
    BOOST_REQUIRE(index.Apply(*FeeOnly(revoke, 0), view, 30, reason));
    BOOST_REQUIRE(state.CaptureCommittee(index, 30, reason));
    BOOST_CHECK(pqanchor::SelectCommittee(index, 60).empty());
    BOOST_CHECK_MESSAGE(pqanchor::ValidateBootstrap(state, &chain.back(), reason), reason);
    BOOST_REQUIRE(state.CommitteeAt(20, snapshot));
    BOOST_CHECK(pqquorum::Commitment(snapshot) == pqquorum::Commitment(members));

    // A checkpoint before registration cannot become valid by waiting longer.
    gArgs.ForceSetArg("-pqbootstrap", BootstrapSpec(19, hashes[19], members));
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    BOOST_CHECK(!pqanchor::ValidateBootstrap(state, &chain.back(), reason));
}

BOOST_AUTO_TEST_CASE(bootstrap_rejects_conflicting_historical_snapshot)
{
    LOCK(cs_main);
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    const auto members = pqanchor::SelectCommittee(index, 20);
    auto wrong = members;
    wrong[0].operator_key = keys[14].GetPublicKey();
    BOOST_REQUIRE(state.SetInitialCommittee(20, wrong, reason));
    std::vector<CBlockIndex> chain;
    std::vector<uint256> hashes;
    MakeChain(chain, hashes, 60);
    gArgs.ForceSetArg("-pqbootstrap", BootstrapSpec(20, hashes[20], members));
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    BOOST_CHECK(!pqanchor::ValidateBootstrap(state, &chain.back(), reason));
    std::vector<pqquorum::Member> snapshot;
    BOOST_REQUIRE(state.CommitteeAt(20, snapshot));
    BOOST_CHECK(pqquorum::Commitment(snapshot) == pqquorum::Commitment(wrong));
}

BOOST_AUTO_TEST_CASE(bootstrap_parses_and_validates_lazily)
{
    BOOST_TEST_MESSAGE("step 0");
    std::string reason;
    gArgs.ForceSetArg("-pqbootstrap", "");
    BOOST_CHECK(pqanchor::InitBootstrap(Params(), reason));
    BOOST_CHECK(pqanchor::GetBootstrap() == nullptr);

    const std::vector<unsigned char> keyBytes(keys[1].GetPublicKey().begin(), keys[1].GetPublicKey().end());
    const std::string keyHex = HexStr(keyBytes);
    const std::string spec = "50:" + uint256S("55").ToString() + ":" + uint256S("aaa1").ToString() + ":" + keyHex;
    gArgs.ForceSetArg("-pqbootstrap", spec);
    BOOST_CHECK(!pqanchor::InitBootstrap(Params(), reason));
    BOOST_CHECK_EQUAL(reason, "pq-bootstrap-members");

    // Pinned member i binds node i's operator key (keys[3i+1]) to a test ID.
    const auto member = [&](size_t i) {
        const std::vector<unsigned char> pub(keys[3 * i + 1].GetPublicKey().begin(),
                                             keys[3 * i + 1].GetPublicKey().end());
        return uint256S(strprintf("aaa%u", i)).ToString() + ":" + HexStr(pub);
    };
    const std::string four = "50:" + uint256S("55").ToString() + ":" + member(0) + ":" + member(1) + ":" +
                             member(2) + ":" + member(3);
    gArgs.ForceSetArg("-pqbootstrap", four);
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    const auto* bootstrap = pqanchor::GetBootstrap();
    BOOST_TEST_MESSAGE("step 1");
    BOOST_REQUIRE(bootstrap);
    BOOST_CHECK_EQUAL(bootstrap->height, 50U);
    BOOST_CHECK_EQUAL(bootstrap->members.size(), 4U);
    // Duplicate identity fails closed.
    const std::string dup = "50:" + uint256S("55").ToString() + ":" + member(0) + ":" + member(1) + ":" +
                            member(2) + ":" + member(0);
    gArgs.ForceSetArg("-pqbootstrap", dup);
    BOOST_CHECK(!pqanchor::InitBootstrap(Params(), reason));
    BOOST_CHECK_EQUAL(reason, "pq-bootstrap-duplicate-member");
    gArgs.ForceSetArg("-pqbootstrap", four);

    // Ancestry: the pinned block must be on the active chain at H0.
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::vector<CBlockIndex> chain;
    std::vector<uint256> hashes;
    MakeChain(chain, hashes, 60);
    BOOST_TEST_MESSAGE("step 2");
    CBlockIndex tip = chain.back(); // copy sharing the ancestor chain
    // Block connection captures registered, mature, key-matched members.
    std::vector<CTransactionRef> registrations;
    for (size_t i = 0; i < 4; ++i) {
        const auto tx = FeeOnly(Registration(i, uint32_t(51000 + i)), i);
        registrations.push_back(tx);
        BOOST_REQUIRE_MESSAGE(index.Apply(*tx, view, 20, reason), "apply " << i << ": " << reason);
    }
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    const auto pinnedMember = [&](size_t i) {
        const std::vector<unsigned char> pub(keys[3 * i + 1].GetPublicKey().begin(),
                                             keys[3 * i + 1].GetPublicKey().end());
        return registrations[i]->GetHash().ToString() + ":" + HexStr(pub);
    };
    BOOST_TEST_MESSAGE("step 3");
    // The pinned hash must match the block at height 50 of our synthetic chain.
    gArgs.ForceSetArg("-pqbootstrap", "55:" + hashes[55].ToString() + ":" + pinnedMember(0) + ":" +
                                          pinnedMember(1) + ":" + pinnedMember(2) + ":" + pinnedMember(3));
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    // The pinned hash and historical committee match independently captured evidence.
    std::string validateReason;
    BOOST_TEST_MESSAGE("step 4");
    BOOST_REQUIRE_MESSAGE(pqanchor::ValidateBootstrap(state, &tip, validateReason),
                          "validate: " << validateReason);
    std::vector<pqquorum::Member> committee;
    BOOST_REQUIRE(state.CommitteeAt(55, committee));
    BOOST_TEST_MESSAGE("step 5");
    BOOST_CHECK_EQUAL(committee.size(), 4U);
    // A pinned identity that is not registered fails lazily without state change.
    gArgs.ForceSetArg("-pqbootstrap", "55:" + hashes[55].ToString() + ":" + member(0) + ":" +
                                          member(1) + ":" + member(2) + ":" + member(3));
    BOOST_REQUIRE(pqanchor::InitBootstrap(Params(), reason));
    BOOST_CHECK(!pqanchor::ValidateBootstrap(state, &tip, validateReason));
    BOOST_CHECK_EQUAL(validateReason, "pq-bootstrap-snapshot-mismatch");
    gArgs.ForceSetArg("-pqbootstrap", "");
}

BOOST_AUTO_TEST_CASE(durable_store_accepts_identical_historical_replay_without_rewinding)
{
    std::string reason;
    auto store = pqanchor::Store::Open(SetDataDir("pq-anchor-replay"), reason);
    BOOST_REQUIRE(store);
    pqanchor::Record first;
    first.height = 20;
    first.blockHash = uint256S("20");
    first.committee = uint256S("40");
    BOOST_REQUIRE(store->Write(first, {}, reason));
    auto next = first;
    next.height = 21;
    next.blockHash = uint256S("21");
    BOOST_REQUIRE(store->Write(next, {}, reason));
    BOOST_CHECK(store->Write(first, {}, reason));
    BOOST_CHECK_EQUAL(store->TipHeight(), 21U);
    first.committee = uint256S("41");
    BOOST_CHECK(!store->Write(first, {}, reason));
    first.committee = uint256S("40");
    first.blockHash = uint256S("22");
    BOOST_CHECK(!store->Write(first, {}, reason));
    BOOST_CHECK_EQUAL(store->TipHeight(), 21U);
}

BOOST_AUTO_TEST_CASE(durable_store_fsyncs_enforces_monotonic_height_and_survives_reopen)
{
    const auto datadir = SetDataDir("pq-anchor-store");
    std::string reason;
    auto store = pqanchor::Store::Open(datadir, reason);
    BOOST_REQUIRE_MESSAGE(store, "open: " << reason);
    BOOST_CHECK_EQUAL(store->TipHeight(), 0U);

    pqquorum::Certificate certificate;
    certificate.statement.purpose = pqquorum::Purpose::PRECOMMIT;
    certificate.statement.genesis = Params().GetConsensus().hashGenesisBlock;
    certificate.statement.anchor = uint256S("77");
    certificate.statement.committee = uint256S("88");
    certificate.statement.height = 100;
    certificate.statement.round = 0;
    certificate.statement.value = uint256S("99");
    // A real certificate needs the quorum threshold of canonical signatures.
    std::vector<pqquorum::Member> fakeMembers;
    for (size_t i = 0; i < 4; ++i) fakeMembers.push_back({uint256S(strprintf("%u", i + 1)), keys[3 * i + 1].GetPublicKey()});
    certificate.statement.committee = pqquorum::Commitment(fakeMembers);
    for (size_t i = 0; i < 3; ++i) {
        pqquorum::Signature vote;
        vote.member = i;
        std::vector<unsigned char> sig;
        BOOST_REQUIRE(keys[3 * i + 1].Sign(pqquorum::Message(certificate.statement), pqquorum::Context(), sig));
        std::copy(sig.begin(), sig.end(), vote.bytes.begin());
        certificate.signatures.push_back(vote);
    }
    auto encoded = pqquorum::Encode(certificate);
    BOOST_REQUIRE(!encoded.empty());

    pqanchor::Record record;
    record.height = 100;
    record.blockHash = uint256S("99");
    record.committee = uint256S("88");
    record.signers = {uint256S("1"), uint256S("2"), uint256S("3")};
    BOOST_REQUIRE(store->Write(record, certificate, reason));
    BOOST_CHECK_EQUAL(store->TipHeight(), 100U);
    // Identical rewrite is an idempotent no-op; anything else at or below the
    // tip is refused.
    pqanchor::Record same = record;
    BOOST_CHECK(store->Write(same, certificate, reason));
    BOOST_CHECK_EQUAL(store->TipHeight(), 100U);
    pqanchor::Record conflicting = record;
    conflicting.signers = {uint256S("9")};
    BOOST_CHECK(store->Write(conflicting, certificate, reason)); // idempotent on identity fields
    pqanchor::Record moved = record;
    moved.blockHash = uint256S("98");
    BOOST_CHECK(!store->Write(moved, certificate, reason));
    BOOST_CHECK_EQUAL(reason, "pq-anchor-store-height");
    pqanchor::Record lower = record;
    lower.height = 99;
    BOOST_CHECK(!store->Write(lower, certificate, reason));

    // Reopen: durable, reads back identical.
    store.reset(); // release the lock before reopening
    store = pqanchor::Store::Open(datadir, reason);
    BOOST_REQUIRE_MESSAGE(store, "reopen: " << reason);
    pqanchor::Record read;
    BOOST_REQUIRE(store->Tip(read));
    BOOST_CHECK_EQUAL(read.height, 100U);
    BOOST_CHECK(read.blockHash == record.blockHash);
    BOOST_CHECK(read.signers == record.signers);
    pqanchor::Record direct;
    BOOST_CHECK(store->Read(100, direct));
    BOOST_CHECK(direct.blockHash == record.blockHash);
    BOOST_CHECK(!store->Read(101, direct));
    pqquorum::Certificate decoded;
    BOOST_REQUIRE(store->ReadCertificate(100, decoded));
    BOOST_CHECK(pqquorum::Encode(decoded) == encoded);
    BOOST_CHECK(!store->ReadCertificate(99, decoded));
}

BOOST_AUTO_TEST_CASE(recovery_configuration_is_test_chain_only_and_exactly_height_hash)
{
    std::string reason;
    gArgs.ForceSetArg("-pqemergencycheckpoint", "20:" + uint256S("20").ToString());
    BOOST_REQUIRE(pqanchor::InitRecovery(Params(), reason));
    const auto* recovery = pqanchor::GetRecovery();
    BOOST_REQUIRE(recovery);
    BOOST_CHECK_EQUAL(recovery->height, 20U);
    BOOST_CHECK(recovery->blockHash == uint256S("20"));

    BOOST_CHECK(pqanchor::InitRecovery(*CreateChainParams("test"), reason));
    BOOST_CHECK(pqanchor::GetRecovery() != nullptr);
    BOOST_CHECK(!pqanchor::InitRecovery(*CreateChainParams("main"), reason));
    BOOST_CHECK(pqanchor::GetRecovery() == nullptr);
    BOOST_REQUIRE(pqanchor::InitRecovery(Params(), reason));

    for (const auto& invalid : {std::string("20"), std::string("20:20:extra"),
                                std::string("0:") + uint256S("20").ToString(),
                                std::string("4294967295:") + uint256S("20").ToString(),
                                std::string("2147483648:") + uint256S("20").ToString(),
                                std::string("20:") + uint256().ToString()}) {
        gArgs.ForceSetArg("-pqemergencycheckpoint", invalid);
        BOOST_CHECK(!pqanchor::InitRecovery(Params(), reason));
        BOOST_CHECK(pqanchor::GetRecovery() == nullptr);
    }
    gArgs.ForceSetArg("-pqemergencycheckpoint", "");
    BOOST_REQUIRE(pqanchor::InitRecovery(Params(), reason));
    BOOST_CHECK(pqanchor::GetRecovery() == nullptr);
}

BOOST_AUTO_TEST_CASE(recovery_store_marker_is_atomic_and_idempotent_after_tip_advances)
{
    const auto datadir = SetDataDir("pq-recovery-marker");
    std::string reason;
    auto store = pqanchor::Store::Open(datadir, reason);
    BOOST_REQUIRE_MESSAGE(store, "open: " << reason);
    pqanchor::Record recovery;
    recovery.height = 20;
    recovery.blockHash = uint256S("20");
    recovery.committee = uint256S("40");
    BOOST_CHECK(!store->WriteRecovery(recovery, reason)); // Not a new trust-root API.
    auto initial = recovery; initial.height = 10; initial.blockHash = uint256S("10");
    BOOST_REQUIRE(store->Write(initial, {}, reason));
    BOOST_REQUIRE(store->WriteRecovery(recovery, reason));
    BOOST_CHECK_EQUAL(store->TipHeight(), 20U);
    pqanchor::Record marker;
    BOOST_REQUIRE(store->ReadRecoveryMarker(20, marker));
    BOOST_CHECK(marker.blockHash == recovery.blockHash);
    pqquorum::Certificate absent;
    BOOST_CHECK(!store->ReadCertificate(20, absent));

    // A normal finalized anchor may advance the durable tip, but replaying the
    // same administrator setting must still find its historical marker.
    pqanchor::Record later = recovery;
    later.height = 21;
    later.blockHash = uint256S("21");
    BOOST_REQUIRE(store->Write(later, {}, reason));
    BOOST_REQUIRE(store->WriteRecovery(recovery, reason));
    pqanchor::Record conflicting = recovery;
    conflicting.blockHash = uint256S("22");
    BOOST_CHECK(!store->WriteRecovery(conflicting, reason));
    BOOST_CHECK_EQUAL(store->TipHeight(), 21U);
}

BOOST_AUTO_TEST_CASE(recovery_validation_requires_restored_chain_derived_committees_and_old_anchor)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    std::vector<CBlockIndex> chain;
    std::vector<uint256> hashes;
    MakeChain(chain, hashes, 40);

    const auto datadir = SetDataDir("pq-recovery-validation");
    auto store = pqanchor::Store::Open(datadir, reason);
    BOOST_REQUIRE(store);
    pqanchor::Record old;
    old.height = 20;
    old.blockHash = hashes[20];
    old.committee = pqquorum::Commitment(pqanchor::SelectCommittee(index, 20));
    BOOST_REQUIRE(store->Write(old, {}, reason));

    // A healthy historical prefix is not an emergency dead-end and must not
    // authorize an arbitrary administrative skip.
    BOOST_REQUIRE(state.CaptureCommittee(index, 30, reason));
    gArgs.ForceSetArg("-pqemergencycheckpoint", "30:" + hashes[30].ToString());
    BOOST_REQUIRE(pqanchor::InitRecovery(Params(), reason));
    BOOST_CHECK(!pqanchor::ValidateRecovery(state, *store, &chain.back(), reason));

    // Replacing the active-chain block or losing the old durable prefix is
    // never an acceptable administrator recovery.
    gArgs.ForceSetArg("-pqemergencycheckpoint", "30:" + hashes[29].ToString());
    BOOST_REQUIRE(pqanchor::InitRecovery(Params(), reason));
    BOOST_CHECK(!pqanchor::ValidateRecovery(state, *store, &chain.back(), reason));
    gArgs.ForceSetArg("-pqemergencycheckpoint", "20:" + hashes[20].ToString());
    BOOST_REQUIRE(pqanchor::InitRecovery(Params(), reason));
    BOOST_CHECK(!pqanchor::ValidateRecovery(state, *store, &chain.back(), reason));

    // A configured checkpoint at the active tip cannot be used because its
    // next voting height is not present in the active chain.
    gArgs.ForceSetArg("-pqemergencycheckpoint", "40:" + hashes[40].ToString());
    BOOST_REQUIRE(pqanchor::InitRecovery(Params(), reason));
    BOOST_CHECK(!pqanchor::ValidateRecovery(state, *store, &chain.back(), reason));

    transaction->Commit();
}

BOOST_AUTO_TEST_CASE(recovery_validation_rejects_known_empty_next_snapshot)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    pqanchor::ChainState state(*evoDb, Params());
    std::string reason;
    RegisterFour(index, reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 20, reason));
    std::vector<CBlockIndex> chain;
    std::vector<uint256> hashes;
    MakeChain(chain, hashes, 40);
    const auto datadir = SetDataDir("pq-recovery-empty-next");
    auto store = pqanchor::Store::Open(datadir, reason);
    BOOST_REQUIRE(store);
    pqanchor::Record old;
    old.height = 20;
    old.blockHash = hashes[20];
    old.committee = pqquorum::Commitment(pqanchor::SelectCommittee(index, 20));
    BOOST_REQUIRE(store->Write(old, {}, reason));

    // Revoke one registration and capture the known-empty snapshot at H+1.
    // The old committee still has four voters, but the next snapshot has less
    // than four. A new fifth registration restores four members later while
    // retaining the empty historical snapshot in the chain-derived journal.
    uint256 revokedRegistration;
    for (const auto& member : pqanchor::SelectCommittee(index, 20))
        if (member.operator_key == keys[1].GetPublicKey()) revokedRegistration = member.registration;
    pqmn::Payload revoke;
    revoke.action = pqmn::Action::REVOKE;
    revoke.registration = revokedRegistration;
    revoke.sequence = 1;
    BOOST_REQUIRE_MESSAGE(index.Apply(*FeeOnly(revoke, 0), view, 21, reason), reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 21, reason));
    BOOST_REQUIRE_MESSAGE(index.Apply(*FeeOnly(Registration(4, 51004), 4), view, 21, reason), reason);
    BOOST_REQUIRE(state.CaptureCommittee(index, 30, reason));
    gArgs.ForceSetArg("-pqemergencycheckpoint", "30:" + hashes[30].ToString());
    BOOST_REQUIRE(pqanchor::InitRecovery(Params(), reason));
    BOOST_REQUIRE(pqanchor::ValidateRecovery(state, *store, &chain.back(), reason));

    pqanchor::Record recovered;
    recovered.height = 30; recovered.blockHash = hashes[30];
    std::vector<pqquorum::Member> restored;
    BOOST_REQUIRE(state.CommitteeAt(30, restored));
    recovered.committee = pqquorum::Commitment(restored);
    BOOST_REQUIRE(store->WriteRecovery(recovered, reason));
    BOOST_REQUIRE(state.RestoreRecovery(*store, 30, &chain.back(), reason));
    BOOST_CHECK_EQUAL(state.TipHeight(), 30U);
    BOOST_REQUIRE(state.RecordAnchor(31, hashes[31], {restored[0].registration}, reason, 40));
    BOOST_REQUIRE(state.UndoBlock(40, 31, reason));
    BOOST_CHECK_EQUAL(state.TipHeight(), 30U);
    BOOST_REQUIRE(state.RestoreRecovery(*store, 30, &chain.back(), reason));
    BOOST_REQUIRE_MESSAGE(pqanchor::ValidateRecovery(state, *store, &chain.back(), reason), reason);
    auto later = recovered; later.height = 31; later.blockHash = hashes[31];
    BOOST_REQUIRE(store->Write(later, {}, reason));
    BOOST_REQUIRE_MESSAGE(pqanchor::ValidateRecovery(state, *store, &chain.back(), reason), reason);

    transaction->Commit();
}

BOOST_AUTO_TEST_SUITE_END()
