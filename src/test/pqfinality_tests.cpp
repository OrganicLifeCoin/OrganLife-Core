// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqfinality.h>
#include <test/test_organiclife.h>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <chrono>
#if !defined(WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

BOOST_FIXTURE_TEST_SUITE(pqfinality_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(proposal_without_unlock_proof_roundtrips)
{
    pqfinality::Proposal proposal, decoded;
    proposal.blockHash = uint256S("11");
    proposal.member = 2;
    proposal.signature[0] = 7;
    const auto bytes = pqfinality::EncodeProposal(proposal);
    BOOST_REQUIRE(!bytes.empty());
    BOOST_REQUIRE(pqfinality::DecodeProposal(bytes, decoded));
    BOOST_CHECK(decoded.blockHash == proposal.blockHash);
    BOOST_CHECK_EQUAL(decoded.member, proposal.member);
    BOOST_CHECK(decoded.signature == proposal.signature);
    BOOST_CHECK(decoded.polc.signatures.empty());
    BOOST_CHECK(pqfinality::EncodeProposal(decoded) == bytes);
    auto trailing = bytes;
    trailing.push_back(0);
    BOOST_CHECK(!pqfinality::DecodeProposal(trailing, decoded));
    for (size_t size = 0; size < bytes.size(); ++size)
        BOOST_CHECK(!pqfinality::DecodeProposal({bytes.data(), size}, decoded));
}

BOOST_AUTO_TEST_CASE(proposal_full_size_unlock_proof_roundtrips)
{
    pqfinality::Proposal proposal, decoded;
    proposal.blockHash = uint256S("11");
    proposal.round = 3;
    proposal.polcRound = 2;
    auto& statement = proposal.polc.statement;
    statement.genesis = uint256S("22");
    statement.anchor = uint256S("33");
    statement.committee = uint256S("44");
    statement.height = 9;
    statement.round = 2;
    statement.value = proposal.blockHash;
    statement.purpose = pqquorum::Purpose::PREVOTE;
    for (size_t member = 0; member < pqquorum::MAX_MEMBERS; ++member) {
        pqquorum::Signature signature;
        signature.member = member;
        signature.bytes[0] = member;
        proposal.polc.signatures.push_back(signature);
    }
    const auto bytes = pqfinality::EncodeProposal(proposal);
    BOOST_REQUIRE(bytes.size() > 65535U);
    BOOST_REQUIRE(pqfinality::DecodeProposal(bytes, decoded));
    BOOST_CHECK_EQUAL(decoded.polc.signatures.size(), pqquorum::MAX_MEMBERS);
    BOOST_CHECK_EQUAL(decoded.polcRound, proposal.polcRound);
    BOOST_CHECK(pqfinality::EncodeProposal(decoded) == bytes);
}

BOOST_AUTO_TEST_CASE(inactive_status_clears_previous_snapshot)
{
    bool configured = true, validated = true, locked = true, member = true;
    uint32_t anchor = 1, committee = 4, height = 2, round = 3;
    uint256 anchorHash = uint256S("11"), lockedValue = uint256S("22");
    BOOST_REQUIRE(!pqfinality::Manager::Get().Started());
    pqfinality::Manager::Get().GetStatus(configured, validated, anchor, anchorHash,
        committee, height, round, locked, lockedValue, member);
    BOOST_CHECK(!validated);
    BOOST_CHECK_EQUAL(anchor, 0U);
    BOOST_CHECK(anchorHash.IsNull());
    BOOST_CHECK(lockedValue.IsNull());
    BOOST_CHECK(!locked);
    BOOST_CHECK(!member);
}

BOOST_AUTO_TEST_CASE(vote_requires_exact_local_context_and_voting_purpose)
{
    std::array<mldsa44::Key, 4> keys;
    std::vector<pqquorum::Member> members;
    for (size_t i = 0; i < keys.size(); ++i) {
        std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
        seed[0] = i + 1; // Public deterministic test material only.
        BOOST_REQUIRE(keys[i].SetSeed(seed));
        members.push_back({uint256S(std::to_string(i + 1)), keys[i].GetPublicKey()});
    }
    pqquorum::Statement context;
    context.genesis = uint256S("11");
    context.anchor = uint256S("22");
    context.committee = pqquorum::Commitment(members);
    context.height = 100;
    context.value = uint256S("33");
    const auto sign = [&](const pqquorum::Statement& statement) {
        pqquorum::Certificate vote;
        vote.statement = statement;
        vote.signatures.resize(1);
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(keys[0].Sign(pqquorum::Message(statement), pqquorum::Context(), signature));
        std::copy(signature.begin(), signature.end(), vote.signatures[0].bytes.begin());
        return vote;
    };
    std::string reason;
    auto vote = sign(context);
    BOOST_REQUIRE(pqfinality::VerifyVote(vote, context, members, reason));
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto foreign = context;
        switch (mutation) {
        case 0: foreign.genesis = uint256S("44"); break;
        case 1: foreign.anchor = uint256S("44"); break;
        case 2: foreign.committee = uint256S("44"); break;
        case 3: ++foreign.height; break;
        case 4: foreign.purpose = pqquorum::Purpose::SERVICE; break;
        }
        BOOST_CHECK(!pqfinality::VerifyVote(sign(foreign), context, members, reason));
    }
    // Replacing unsigned wire context must not reconstruct a valid signature.
    vote.statement.anchor = uint256S("55");
    BOOST_CHECK(!pqfinality::VerifyVote(vote, context, members, reason));
    vote = sign(context);
    vote.statement.committee = uint256S("55");
    BOOST_CHECK(!pqfinality::VerifyVote(vote, context, members, reason));
    auto nil = context;
    nil.value.SetNull();
    nil.round = 17;
    nil.purpose = pqquorum::Purpose::PRECOMMIT;
    BOOST_CHECK(pqfinality::VerifyVote(sign(nil), context, members, reason));
}

BOOST_AUTO_TEST_SUITE_END()

namespace pqfinality {
struct ManagerTestAccess : BasicTestingSetup {
    Manager manager;
    Manager::Height height;
    std::array<mldsa44::Key, 5> keys;
    std::string reason;
    ManagerTestAccess()
    {
        height.height = 100;
        height.anchor = uint256S("22");
        height.localBlock = uint256S("33");
        for (size_t i = 0; i < 4; ++i) {
            std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
            seed[0] = i + 1;
            BOOST_REQUIRE(keys[i].SetSeed(seed));
            height.committee.push_back({uint256S(std::to_string(i + 1)), keys[i].GetPublicKey()});
        }
        height.round = std::make_unique<pqquorum::RoundState>(Params().GetConsensus().hashGenesisBlock,
            height.anchor, height.height, height.committee);
    }
    void Advance(uint32_t round) { manager.AdvanceRound(height, round, 1000); }
    uint32_t Round() { return height.round->currentRound(); }
    pqquorum::Certificate SignedVote(uint16_t member, uint32_t round,
                                    pqquorum::Purpose step = pqquorum::Purpose::PREVOTE)
    {
        pqquorum::Certificate vote;
        auto& statement = vote.statement;
        statement.genesis = Params().GetConsensus().hashGenesisBlock;
        statement.anchor = height.anchor;
        statement.committee = pqquorum::Commitment(height.committee);
        statement.height = height.height; statement.round = round;
        statement.value = height.localBlock; statement.purpose = step;
        vote.signatures.resize(1); vote.signatures[0].member = member;
        std::vector<unsigned char> bytes;
        BOOST_REQUIRE(keys[member].Sign(pqquorum::Message(statement), pqquorum::Context(), bytes));
        std::copy(bytes.begin(), bytes.end(), vote.signatures[0].bytes.begin());
        return vote;
    }
    bool Accept(const pqquorum::Certificate& vote) { return manager.AcceptVote(height, vote, 1000, reason); }
    size_t FutureCount() { return height.futureVotes.size(); }
    size_t StoredCount() { return height.votes.size(); }
    void CheckWakeDelay()
    {
        LOCK(cs_main);
        LOCK(manager.cs);
        manager.working = std::move(height);
        auto& state = manager.working;
        state.selfMember = 0;
        state.deadlinePropose = 1000;
        state.deadlinePrevote = 2000;
        state.deadlinePrecommit = 3000;
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(0), 500);
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(900), 100);
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(1000), 10);
        pqquorum::Statement vote;
        BOOST_REQUIRE(state.round->Prevote({}, nullptr, vote, reason));
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(1500), 500);
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(1900), 100);
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(2000), 10);
        BOOST_REQUIRE(state.round->Precommit(nullptr, {}, vote, reason));
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(2500), 500);
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(2900), 100);
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(3000), 10);
        state.round = pqquorum::RoundState::Restore(Params().GetConsensus().hashGenesisBlock,
            state.anchor, state.height, state.committee, 0, {}, 0, std::nullopt, uint256(), reason);
        BOOST_REQUIRE(state.round);
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(2900), 100); // Recovered precommit without prevote.
        manager.AdvanceRound(state, 1, 3000);
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(state.deadlinePropose - 100), 100);
        state.selfMember = -1;
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(state.deadlinePrecommit + 1000), 500);
        state.round.reset();
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(state.deadlinePrecommit + 1000), 500);
        state.selfMember = 0;
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(state.deadlinePrecommit + 1000), 500);
        state.selfMember = -1;
        manager.inbox.push_back(InboxItem{});
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(0), 10);
        state.committed = true;
        BOOST_CHECK_EQUAL(manager.NextWakeDelay(0), 50);
    }
    void CheckInboxBatches()
    {
        LOCK(cs_main);
        LOCK(manager.cs);
        const auto queue = [&](const pqquorum::Certificate& vote) {
            InboxItem item{};
            item.certificate = vote;
            item.step = vote.statement.purpose;
            manager.inbox.push_back(std::move(item));
        };
        const auto first = SignedVote(0, 0);
        auto invalid = SignedVote(2, 0);
        invalid.signatures[0].bytes.back() ^= 1;
        for (size_t i = 0; i < MAX_INBOX - 2; ++i) queue(first);
        queue(invalid);
        queue(SignedVote(1, 0));
        for (size_t left : {48U, 32U, 16U, 0U}) {
            manager.DrainInbox(height, 1000);
            BOOST_CHECK_EQUAL(manager.inbox.size(), left);
            BOOST_CHECK_EQUAL(StoredCount(), left ? 1U : 2U);
        }
        // Oversized declared work must not drain the entire inbox even when
        // an invalid proof is cheap to reject. One maximum item still fits.
        auto large = first;
        large.statement.purpose = pqquorum::Purpose::PRECOMMIT;
        large.signatures.resize(pqquorum::MAX_MEMBERS);
        queue(first);
        queue(first);
        queue(large);
        manager.DrainInbox(height, 1000);
        BOOST_CHECK_EQUAL(manager.inbox.size(), 1U);
        manager.DrainInbox(height, 1000);
        BOOST_CHECK(manager.inbox.empty());
        BOOST_CHECK_EQUAL(StoredCount(), 2U);
        for (int i = 0; i < 2; ++i) {
            InboxItem item{};
            item.proposal = std::make_unique<Proposal>();
            item.proposal->polc = large;
            manager.inbox.push_back(std::move(item));
        }
        manager.DrainInbox(height, 1000);
        BOOST_CHECK_EQUAL(manager.inbox.size(), 1U);
        manager.DrainInbox(height, 1000);
        BOOST_CHECK(manager.inbox.empty());
        BOOST_CHECK_EQUAL(ProposalCount(), 0U);
        queue(first); // Stale once the driver advances to the next height.
        ++height.height;
        queue(SignedVote(3, 0));
        --height.height;
        height.committed = true;
        manager.DrainInbox(height, 1000);
        BOOST_CHECK_EQUAL(manager.inbox.size(), 2U);
        ++height.height;
        height.committed = false;
        height.votes.clear();
        height.round = std::make_unique<pqquorum::RoundState>(Params().GetConsensus().hashGenesisBlock,
            height.anchor, height.height, height.committee);
        manager.DrainInbox(height, 1000);
        BOOST_CHECK(manager.inbox.empty());
        BOOST_CHECK_EQUAL(StoredCount(), 1U); // Deferred work is verified against the new context.
    }
    void CheckMaximumProofBatch()
    {
        LOCK(cs_main);
        LOCK(manager.cs);
        std::vector<mldsa44::Key> largeKeys(pqquorum::MAX_MEMBERS);
        height.committee.clear();
        for (size_t i = 0; i < largeKeys.size(); ++i) {
            std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
            seed[0] = i & 255; seed[1] = i >> 8;
            BOOST_REQUIRE(largeKeys[i].SetSeed(seed));
            height.committee.push_back({uint256S(strprintf("%x", i + 1)), largeKeys[i].GetPublicKey()});
        }
        height.round = std::make_unique<pqquorum::RoundState>(Params().GetConsensus().hashGenesisBlock,
            height.anchor, height.height, height.committee);
        Proposal proposal;
        proposal.round = 1;
        proposal.member = (height.height + proposal.round) % height.committee.size();
        proposal.blockHash = height.localBlock;
        auto& statement = proposal.polc.statement;
        statement.genesis = Params().GetConsensus().hashGenesisBlock;
        statement.anchor = height.anchor;
        statement.committee = pqquorum::Commitment(height.committee);
        statement.height = height.height;
        statement.value = height.localBlock;
        statement.purpose = pqquorum::Purpose::PREVOTE;
        for (size_t i = 0; i < largeKeys.size(); ++i) {
            pqquorum::Signature signature;
            signature.member = i;
            std::vector<unsigned char> bytes;
            BOOST_REQUIRE(largeKeys[i].Sign(pqquorum::Message(statement), pqquorum::Context(), bytes));
            std::copy(bytes.begin(), bytes.end(), signature.bytes.begin());
            proposal.polc.signatures.push_back(signature);
        }
        auto authorization = statement;
        authorization.round = proposal.round;
        std::vector<unsigned char> bytes;
        BOOST_REQUIRE(largeKeys[proposal.member].Sign(pqquorum::Message(authorization), pqquorum::Context(), bytes));
        std::copy(bytes.begin(), bytes.end(), proposal.signature.begin());
        for (size_t i = 0; i < MAX_INBOX; ++i) {
            InboxItem item{};
            item.proposal = std::make_unique<Proposal>(proposal);
            if (i == 0) item.proposal->polc.signatures.back().bytes.back() ^= 1;
            manager.inbox.push_back(std::move(item));
        }
        const auto start = std::chrono::steady_clock::now();
        manager.DrainInbox(height, 1000);
        BOOST_TEST_MESSAGE("Maximum proof batch elapsed microseconds: " <<
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        BOOST_CHECK_EQUAL(manager.inbox.size(), MAX_INBOX - 1);
        BOOST_CHECK(!height.futureProposal); // The last of400 signatures is checked.
        manager.DrainInbox(height, 1000);
        BOOST_CHECK_EQUAL(manager.inbox.size(), MAX_INBOX - 2);
        BOOST_REQUIRE(height.futureProposal);
        BOOST_CHECK_EQUAL(height.futureProposal->polc.signatures.size(), pqquorum::MAX_MEMBERS);
        Advance(1);
        BOOST_CHECK_EQUAL(ProposalCount(), 1U);
        BOOST_CHECK_EQUAL(ProofSigners(), pqquorum::MAX_MEMBERS);
    }
    uint32_t ProofRound() { return height.bestPolc.statement.round; }
    size_t ProofSigners() { return height.bestPolc.signatures.size(); }
    int64_t LastDeadline() { return height.deadlinePrecommit; }
    void FifthMember()
    {
        std::array<unsigned char, mldsa44::SEED_SIZE> seed{}; seed[0] = 5;
        BOOST_REQUIRE(keys[4].SetSeed(seed));
        height.committee.push_back({uint256S("5"), keys[4].GetPublicKey()});
        height.round = std::make_unique<pqquorum::RoundState>(Params().GetConsensus().hashGenesisBlock,
            height.anchor, height.height, height.committee);
    }
    bool CheckProposal(uint32_t round, const pqquorum::Certificate* proof = nullptr, bool receive = false)
    {
        pqfinality::Proposal proposal;
        proposal.blockHash = height.localBlock;
        proposal.round = round;
        proposal.member = (uint64_t(height.height) + round) % height.committee.size();
        pqquorum::Statement statement;
        statement.genesis = Params().GetConsensus().hashGenesisBlock;
        statement.anchor = height.anchor;
        statement.committee = pqquorum::Commitment(height.committee);
        statement.height = height.height; statement.round = round; statement.value = height.localBlock;
        std::vector<unsigned char> bytes;
        BOOST_REQUIRE(keys[proposal.member].Sign(pqquorum::Message(statement), pqquorum::Context(), bytes));
        std::copy(bytes.begin(), bytes.end(), proposal.signature.begin());
        if (proof) {
            proposal.polc = *proof;
            proposal.polcRound = proof->statement.round;
        }
        return receive ? manager.AcceptProposal(height, proposal, reason) : manager.VerifyProposal(proposal, height, reason);
    }
    size_t ProposalCount() { return height.proposals.size(); }
    uint32_t FutureProposalRound() { return height.futureProposal ? height.futureProposal->round : 0; }

    void CheckDurableProof()
    {
        const auto directory = SetDataDir("pq-proof");
        const auto open = [&](bool initialize) {
            return initialize ? pqjournal::Journal::Initialize(directory, Params().GetConsensus().hashGenesisBlock,
                height.committee[0].registration, keys[0].GetPublicKey(), reason) :
                pqjournal::Journal::Load(directory, Params().GetConsensus().hashGenesisBlock,
                height.committee[0].registration, keys[0].GetPublicKey(), reason);
        };
        manager.journal = open(true);
        BOOST_REQUIRE(manager.journal);
        for (uint16_t i = 0; i < 3; ++i) BOOST_REQUIRE(Accept(SignedVote(i, 0)));
        pqquorum::Certificate saved;
        BOOST_REQUIRE(manager.journal->GetProof(height.height, saved));
        BOOST_CHECK_EQUAL(saved.signatures.size(), 3U);
        Advance(6); // Ordinary vote history is pruned; public proof must survive disk restart too.
        BOOST_CHECK_EQUAL(StoredCount(), 0U);
        height.bestPolc = {};
        manager.journal.reset();
        manager.journal = open(false);
        BOOST_REQUIRE(manager.journal);
        BOOST_REQUIRE(manager.RestoreProof(height, reason));
        BOOST_CHECK(pqquorum::Encode(height.bestPolc) == pqquorum::Encode(saved));
        // Journal hashes are storage integrity, not proof authority. Restore
        // must reject even structurally valid but unauthentic public material.
        ++saved.statement.round;
        BOOST_REQUIRE(manager.journal->RecordProof(saved, reason));
        height.bestPolc = {};
        BOOST_CHECK(!manager.RestoreProof(height, reason));
        BOOST_CHECK(height.bestPolc.signatures.empty());
    }

    void CheckBatchedProofWrites(bool failWrite, bool advanceRound = false)
    {
        LOCK(cs_main);
        LOCK(manager.cs);
        const auto directory = SetDataDir("pq-batched-proof");
        const auto reference = SetDataDir("pq-single-proof");
        const auto open = [&](const fs::path& path, bool initialize) {
            return initialize ? pqjournal::Journal::Initialize(path, Params().GetConsensus().hashGenesisBlock,
                height.committee[0].registration, keys[0].GetPublicKey(), reason) :
                pqjournal::Journal::Load(path, Params().GetConsensus().hashGenesisBlock,
                height.committee[0].registration, keys[0].GetPublicKey(), reason);
        };
        const auto file = [](const fs::path& path) {
            for (const auto& entry : fs::directory_iterator(path / "pqjournal"))
                if (entry.path().extension() == ".journal") return entry.path();
            return fs::path();
        };
        manager.journal = open(directory, true);
        auto expectedJournal = open(reference, true);
        BOOST_REQUIRE(manager.journal && expectedJournal);
        pqquorum::Certificate expected;
        for (uint16_t member = 0; member < 4; ++member) {
            auto vote = SignedVote(member, 0);
            expected.statement = vote.statement;
            expected.signatures.push_back(vote.signatures[0]);
            InboxItem item{};
            item.step = pqquorum::Purpose::PREVOTE;
            item.certificate = vote;
            manager.inbox.push_back(std::move(item));
        }
        BOOST_REQUIRE(expectedJournal->RecordProof(expected, reason));
        if (advanceRound) {
            // A future PRECOMMIT quorum can prune the current PREVOTEs
            // without supplying a newer proof to replace their recovery data.
            for (uint16_t member = 0; member < 3; ++member) {
                InboxItem item{};
                item.step = pqquorum::Purpose::PRECOMMIT;
                item.certificate = SignedVote(member, ROUND_HISTORY, item.step);
                manager.inbox.push_back(std::move(item));
            }
        }
        if (failWrite) fs::resize_file(file(directory), 1u << 30); // Sparse disposable test journal.
        manager.DrainInbox(height, 1000);
        BOOST_CHECK(manager.inbox.empty());
        if (advanceRound) {
            BOOST_CHECK_EQUAL(Round(), ROUND_HISTORY);
            BOOST_CHECK_EQUAL(StoredCount(), 3U);
        }
        pqquorum::Certificate recovered;
        if (failWrite) {
            BOOST_CHECK(height.bestPolc.signatures.empty());
            BOOST_CHECK(!manager.journal->GetProof(height.height, recovered));
            bool signedVote = false;
            pqjournal::Journal::Signature signature;
            BOOST_CHECK(!manager.journal->GetOrSignVote(expected.statement,
                [&](const pqquorum::Statement&, pqjournal::Journal::Signature&) { signedVote = true; return true; },
                signature, reason));
            BOOST_CHECK(!signedVote);
            BOOST_CHECK_EQUAL(fs::file_size(file(directory)), 1u << 30);
            return;
        }
        BOOST_REQUIRE(manager.journal->GetProof(height.height, recovered));
        BOOST_CHECK(pqquorum::Encode(recovered) == pqquorum::Encode(expected));
        BOOST_CHECK_EQUAL(fs::file_size(file(directory)), fs::file_size(file(reference)));
        manager.journal.reset();
        manager.journal = open(directory, false);
        BOOST_REQUIRE(manager.journal);
        height.bestPolc = {};
        BOOST_REQUIRE(manager.RestoreProof(height, reason));
        BOOST_CHECK(pqquorum::Encode(height.bestPolc) == pqquorum::Encode(expected));
    }

    void CheckReportedState()
    {
        LOCK(cs_main);
        pqanchor::ChainState mirror(*evoDb, Params());
        BOOST_REQUIRE(mirror.SetInitialCommittee(99, height.committee, reason));
        BOOST_REQUIRE(mirror.RecordAnchor(99, uint256S("99"), {}, reason, 100));
        const auto directory = SetDataDir("pq-status");
        manager.journal = pqjournal::Journal::Initialize(directory, Params().GetConsensus().hashGenesisBlock,
            height.committee[0].registration, keys[0].GetPublicKey(), reason);
        BOOST_REQUIRE(manager.journal);
        manager.started = true;
        manager.working = std::move(height);
        bool configured, validated, locked, member;
        uint32_t anchor, size, voting, round;
        uint256 anchorHash, lock;
        const auto query = [&] { manager.GetStatus(configured, validated, anchor, anchorHash,
                                                  size, voting, round, locked, lock, member); };
        query();
        BOOST_CHECK(!validated);
        BOOST_CHECK_EQUAL(anchor, 99U); // Stored protection exists before the driver's first pass.
        manager.validated = true;
        BOOST_REQUIRE(manager.working.round->Advance(4));
        BOOST_REQUIRE(manager.journal->RecordLock(100, 4, uint256S("33"), reason));
        query();
        BOOST_CHECK_EQUAL(voting, 100U);
        BOOST_CHECK_EQUAL(round, 4U); // Actual runtime round, even before its first signature.
        BOOST_CHECK(locked); // Does not require a previous journal FINALIZED record.
        BOOST_CHECK(lock == uint256S("33"));
        BOOST_REQUIRE(manager.working.round->Advance(5));
        BOOST_REQUIRE(manager.journal->RecordLock(100, 5, {}, reason));
        query();
        BOOST_CHECK(!locked); // A persisted nil-unlock is not an active lock.
        BOOST_CHECK(lock.IsNull());
    }
};
}

BOOST_FIXTURE_TEST_SUITE(pqfinality_round_tests, pqfinality::ManagerTestAccess)
BOOST_AUTO_TEST_CASE(idle_delay_tracks_only_unfinished_voting_steps)
{
    CheckWakeDelay();
}
BOOST_AUTO_TEST_CASE(inbox_batches_preserve_fifo_and_signature_validation)
{
    CheckInboxBatches();
}
BOOST_AUTO_TEST_CASE(maximum_proof_batch_checks_last_signature_and_defers_remaining_work)
{
    CheckMaximumProofBatch();
}
BOOST_AUTO_TEST_CASE(runtime_restores_and_revalidates_durable_quorum_proof)
{
    CheckDurableProof();
}
BOOST_AUTO_TEST_CASE(inbox_persists_complete_quorum_once_before_returning)
{
    CheckBatchedProofWrites(false);
}
BOOST_AUTO_TEST_CASE(failed_batched_proof_write_still_denies_signing)
{
    CheckBatchedProofWrites(true);
}
BOOST_AUTO_TEST_CASE(inbox_preserves_quorum_before_same_batch_round_pruning)
{
    CheckBatchedProofWrites(false, true);
}

BOOST_AUTO_TEST_CASE(status_reports_stored_anchors_and_actual_runtime_locks)
{
    CheckReportedState();
}

BOOST_AUTO_TEST_CASE(runtime_can_continue_beyond_round_99)
{
    Advance(99);
    BOOST_REQUIRE_EQUAL(Round(), 99U);
    Advance(100);
    BOOST_CHECK_EQUAL(Round(), 100U);
    Advance(101);
    BOOST_CHECK_EQUAL(Round(), 101U);
    BOOST_CHECK(CheckProposal(100));
    BOOST_CHECK(CheckProposal(UINT32_MAX)); // Rotation addition must not overflow.
    Advance(UINT32_MAX);
    BOOST_CHECK(LastDeadline() > 1000);
    Advance(0);
    BOOST_CHECK_EQUAL(Round(), UINT32_MAX); // Never wrap to a used round.
}

BOOST_AUTO_TEST_CASE(future_proposal_is_bounded_and_used_only_after_round_entry)
{
    BOOST_REQUIRE(CheckProposal(7, nullptr, true));
    BOOST_CHECK_EQUAL(Round(), 0U); // A proposal alone is never round-change authority.
    BOOST_CHECK_EQUAL(ProposalCount(), 0U);
    BOOST_CHECK_EQUAL(FutureProposalRound(), 7U);
    BOOST_CHECK(!CheckProposal(8, nullptr, true)); // Retain the nearest future round, not a flood.
    BOOST_REQUIRE(CheckProposal(3, nullptr, true));
    BOOST_CHECK_EQUAL(FutureProposalRound(), 3U);
    BOOST_REQUIRE(CheckProposal(0, nullptr, true));
    BOOST_CHECK_EQUAL(ProposalCount(), 1U);
    Advance(3);
    BOOST_CHECK_EQUAL(ProposalCount(), 1U);
    BOOST_CHECK_EQUAL(FutureProposalRound(), 0U);
    BOOST_REQUIRE(CheckProposal(5, nullptr, true));
    Advance(6);
    BOOST_CHECK_EQUAL(ProposalCount(), 0U);
    BOOST_CHECK_EQUAL(FutureProposalRound(), 0U);
}

BOOST_AUTO_TEST_CASE(catchup_requires_distinct_authenticated_supermajority)
{
    BOOST_REQUIRE(Accept(SignedVote(0, 100)));
    BOOST_CHECK_EQUAL(Round(), 0U);
    Accept(SignedVote(0, 100, pqquorum::Purpose::PRECOMMIT));
    BOOST_CHECK_EQUAL(FutureCount(), 1U);
    auto bad = SignedVote(1, 100);
    bad.signatures[0].bytes[0] ^= 1;
    BOOST_CHECK(!Accept(bad));
    bad = SignedVote(1, 100); ++bad.statement.height;
    BOOST_CHECK(!Accept(bad));
    BOOST_REQUIRE(Accept(SignedVote(1, 100)));
    BOOST_CHECK_EQUAL(Round(), 0U);
    BOOST_REQUIRE(Accept(SignedVote(2, 100)));
    BOOST_CHECK_EQUAL(Round(), 100U);
    BOOST_CHECK_EQUAL(FutureCount(), 0U);
    BOOST_CHECK_EQUAL(StoredCount(), 3U);
    BOOST_CHECK_EQUAL(ProofRound(), 100U);
    BOOST_REQUIRE(Accept(SignedVote(3, 100)));
    BOOST_CHECK_EQUAL(ProofSigners(), 4U); // Preserve every known signer, not just three.
}

BOOST_AUTO_TEST_CASE(five_members_require_four_for_catchup)
{
    FifthMember();
    for (uint16_t member = 0; member < 3; ++member) BOOST_REQUIRE(Accept(SignedVote(member, 150)));
    BOOST_CHECK_EQUAL(Round(), 0U);
    BOOST_REQUIRE(Accept(SignedVote(3, 150)));
    BOOST_CHECK_EQUAL(Round(), 150U);
    BOOST_CHECK_EQUAL(ProofSigners(), 4U);
}

BOOST_AUTO_TEST_CASE(proposal_unlock_proof_is_verified_before_caching)
{
    auto proof = SignedVote(0, 5);
    BOOST_CHECK(!CheckProposal(6, &proof)); // One signature is not a PoLC.
    for (uint16_t member = 1; member < 3; ++member)
        proof.signatures.push_back(SignedVote(member, 5).signatures[0]);
    BOOST_REQUIRE(CheckProposal(6, &proof));
    BOOST_CHECK(!CheckProposal(5, &proof)); // Proof must precede proposal round.
    proof.signatures[0].bytes[0] ^= 1;
    BOOST_CHECK(!CheckProposal(6, &proof));
}

BOOST_AUTO_TEST_CASE(future_flood_is_bounded_and_cannot_choose_catchup_round)
{
    for (uint32_t round = 100; round < 120; ++round) Accept(SignedVote(0, round));
    BOOST_REQUIRE(Accept(SignedVote(0, UINT32_MAX)));
    BOOST_CHECK_EQUAL(Round(), 0U);
    BOOST_CHECK_EQUAL(FutureCount(), 1U);
    BOOST_REQUIRE(Accept(SignedVote(1, 150)));
    BOOST_CHECK_EQUAL(Round(), 0U);
    BOOST_REQUIRE(Accept(SignedVote(2, 152)));
    BOOST_CHECK_EQUAL(Round(), 150U); // Not the malicious member's maximum.
    BOOST_CHECK_EQUAL(FutureCount(), 2U);
}

BOOST_AUTO_TEST_CASE(history_pruning_keeps_latest_quorum_proof)
{
    for (uint32_t round = 100; round < 112; ++round) {
        Advance(round);
        for (uint16_t member = 0; member < 3; ++member) BOOST_REQUIRE(Accept(SignedVote(member, round)));
        BOOST_CHECK(StoredCount() <= 12U); // Four rounds, three signers per round.
    }
    BOOST_CHECK_EQUAL(ProofRound(), 111U);
    Advance(1000);
    BOOST_CHECK_EQUAL(StoredCount(), 0U);
    BOOST_CHECK_EQUAL(ProofRound(), 111U); // Unlock evidence is independent of the rolling history.
}
BOOST_AUTO_TEST_SUITE_END()

namespace {
struct VotingFixture : BasicTestingSetup {
    std::array<mldsa44::Key, 4> keys;
    std::vector<pqquorum::Member> members;
    pqquorum::Statement context;
    std::unique_ptr<pqquorum::RoundState> state;
    std::unique_ptr<pqjournal::Journal> journal;
    fs::path directory;
    std::string reason;
    size_t signedVotes{0};
    VotingFixture()
    {
        for (size_t i = 0; i < keys.size(); ++i) {
            std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
            seed[0] = i + 1; // Public test material.
            BOOST_REQUIRE(keys[i].SetSeed(seed));
            members.push_back({uint256S(std::to_string(i + 1)), keys[i].GetPublicKey()});
        }
        context.genesis = uint256S("11"); context.anchor = uint256S("22");
        context.committee = pqquorum::Commitment(members); context.height = 100;
        context.value = uint256S("33");
        directory = SetDataDir("finality-voting");
        journal = pqjournal::Journal::Initialize(directory, context.genesis, members[0].registration,
            keys[0].GetPublicKey(), reason);
        BOOST_REQUIRE(journal);
        state = std::make_unique<pqquorum::RoundState>(context.genesis, context.anchor, context.height, members);
    }
    pqquorum::Certificate Proof(uint32_t round, const uint256& value)
    {
        pqquorum::Certificate cert;
        cert.statement = context; cert.statement.round = round; cert.statement.value = value;
        for (size_t i = 0; i < 3; ++i) {
            pqquorum::Signature sig; sig.member = i;
            std::vector<unsigned char> bytes;
            BOOST_REQUIRE(keys[i].Sign(pqquorum::Message(cert.statement), pqquorum::Context(), bytes));
            std::copy(bytes.begin(), bytes.end(), sig.bytes.begin());
            cert.signatures.push_back(sig);
        }
        return cert;
    }
    bool Vote(pqquorum::Purpose step, const uint256& value, const pqquorum::Certificate* proof = nullptr)
    {
        pqquorum::Statement decision;
        pqjournal::Journal::Signature signature;
        return pqfinality::JournalVote(*state, *journal, step, value, proof,
            [&](const pqquorum::Statement& statement, pqjournal::Journal::Signature& out) {
                ++signedVotes;
                std::vector<unsigned char> bytes;
                if (!keys[0].Sign(pqquorum::Message(statement), pqquorum::Context(), bytes)) return false;
                std::copy(bytes.begin(), bytes.end(), out.begin());
                return true;
            }, decision, signature, reason);
    }
    void Restart()
    {
        journal.reset();
        journal = pqjournal::Journal::Load(directory, context.genesis, members[0].registration,
            keys[0].GetPublicKey(), reason);
        BOOST_REQUIRE(journal);
        uint256 locked; uint32_t lockRound = 0;
        journal->GetLock(context.height, locked, lockRound);
        uint32_t round = lockRound, votedRound;
        if (journal->HighestVoted(context.height, pqquorum::Purpose::PREVOTE, votedRound)) round = std::max(round, votedRound);
        if (journal->HighestVoted(context.height, pqquorum::Purpose::PRECOMMIT, votedRound)) round = std::max(round, votedRound);
        std::optional<uint256> prevote, precommit;
        uint256 value; pqjournal::Journal::Signature signature;
        if (journal->GetVote(context.height, round, pqquorum::Purpose::PREVOTE, value, signature)) prevote = value;
        if (journal->GetVote(context.height, round, pqquorum::Purpose::PRECOMMIT, value, signature)) precommit = value;
        state = pqquorum::RoundState::Restore(context.genesis, context.anchor, context.height,
            members, round, locked, lockRound, prevote, precommit, reason);
        BOOST_REQUIRE(state);
    }
};
}

BOOST_FIXTURE_TEST_SUITE(pqfinality_voting_tests, VotingFixture)
BOOST_AUTO_TEST_CASE(refused_transition_never_signs)
{
    BOOST_REQUIRE(Vote(pqquorum::Purpose::PRECOMMIT, {}));
    BOOST_CHECK(!Vote(pqquorum::Purpose::PREVOTE, context.value));
    BOOST_CHECK_EQUAL(signedVotes, 1U);
    BOOST_REQUIRE(state->Advance(1));
    auto invalid = Proof(1, context.value); // Unlock proof must precede current round.
    BOOST_CHECK(!Vote(pqquorum::Purpose::PREVOTE, context.value, &invalid));
    BOOST_CHECK_EQUAL(signedVotes, 1U);
    BOOST_CHECK(!state->hasPrevoted());
}

BOOST_AUTO_TEST_CASE(missing_durable_lock_never_signs)
{
    for (const auto locked : {context.value, uint256()}) {
        state = pqquorum::RoundState::Restore(context.genesis, context.anchor, context.height,
            members, 2, locked, 1, std::nullopt, std::nullopt, reason);
        BOOST_REQUIRE(state);
        BOOST_CHECK(!Vote(pqquorum::Purpose::PREVOTE, context.value));
        BOOST_CHECK_EQUAL(signedVotes, 0U);
    }
}

BOOST_AUTO_TEST_CASE(nil_quorum_unlock_is_durable_but_timeout_preserves_lock)
{
    auto proof = Proof(0, context.value);
    BOOST_REQUIRE(Vote(pqquorum::Purpose::PRECOMMIT, context.value, &proof));
    BOOST_REQUIRE(state->Advance(1));
    BOOST_REQUIRE(Vote(pqquorum::Purpose::PRECOMMIT, {})); // Timeout is NOT an unlock.
    Restart();
    BOOST_CHECK(state->lockedValue() == context.value);
    BOOST_REQUIRE(state->Advance(2));
    auto nil = Proof(2, {});
    BOOST_REQUIRE(Vote(pqquorum::Purpose::PRECOMMIT, {}, &nil));
    Restart();
    BOOST_CHECK(state->lockedValue().IsNull());
    BOOST_CHECK_EQUAL(state->lockedRound(), 2U);
}

BOOST_AUTO_TEST_CASE(polc_unlock_survives_restart_and_allows_later_relock)
{
    auto proof = Proof(0, context.value);
    BOOST_REQUIRE(Vote(pqquorum::Purpose::PRECOMMIT, context.value, &proof));
    BOOST_REQUIRE(state->Advance(2));
    const auto other = uint256S("44");
    auto polc = Proof(1, other);
    BOOST_REQUIRE(Vote(pqquorum::Purpose::PREVOTE, other, &polc));
    Restart();
    BOOST_CHECK(state->lockedValue().IsNull());
    BOOST_CHECK_EQUAL(state->lockedRound(), 1U);
    auto relock = Proof(2, other);
    BOOST_REQUIRE(Vote(pqquorum::Purpose::PRECOMMIT, other, &relock));
    Restart();
    BOOST_CHECK(state->lockedValue() == other);
    BOOST_CHECK_EQUAL(state->lockedRound(), 2U);
}

#if !defined(WIN32)
BOOST_AUTO_TEST_CASE(crash_before_signature_preserves_lock_and_both_unlock_paths)
{
    auto initial = Proof(0, context.value);
    BOOST_REQUIRE(Vote(pqquorum::Purpose::PRECOMMIT, context.value, &initial));
    for (const bool polc : {false, true}) {
        BOOST_REQUIRE(state->Advance(polc ? 4 : 1));
        const auto other = uint256S("44");
        auto proof = Proof(polc ? 3 : 1, polc ? other : uint256());
        const auto step = polc ? pqquorum::Purpose::PREVOTE : pqquorum::Purpose::PRECOMMIT;
        const pid_t child = fork();
        BOOST_REQUIRE(child >= 0);
        if (child == 0) {
            pqquorum::Statement decision;
            pqjournal::Journal::Signature signature;
            pqfinality::JournalVote(*state, *journal, step, polc ? other : uint256(), &proof,
                [](const pqquorum::Statement&, pqjournal::Journal::Signature&) -> bool {
                    _exit(0); // Crash exactly when signing would start.
                }, decision, signature, reason);
            _exit(10);
        }
        int status = 0;
        BOOST_REQUIRE(waitpid(child, &status, 0) == child);
        BOOST_REQUIRE(WIFEXITED(status));
        BOOST_REQUIRE_EQUAL(WEXITSTATUS(status), 0);
        Restart();
        BOOST_CHECK(state->lockedValue().IsNull());
        BOOST_CHECK_EQUAL(state->lockedRound(), polc ? 3U : 1U);
        BOOST_CHECK(!state->hasPrevoted());
        BOOST_CHECK(!state->hasPrecommitted());
        if (!polc) {
            BOOST_REQUIRE(state->Advance(2));
            auto relock = Proof(2, context.value);
            BOOST_REQUIRE(Vote(pqquorum::Purpose::PRECOMMIT, context.value, &relock));
        }
    }
}
#endif
BOOST_AUTO_TEST_SUITE_END()
