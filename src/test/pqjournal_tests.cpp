// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqjournal.h>
#include <crypto/mldsa44.h>
#include <test/test_organiclife.h>
#include <boost/test/unit_test.hpp>
#include <fs.h>
#include <algorithm>
#include <memory>
#include <stdio.h>
#if !defined(WIN32)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

BOOST_FIXTURE_TEST_SUITE(pqjournal_tests, BasicTestingSetup)

namespace {
struct JournalEnv {
    static size_t Instances;
    mldsa44::Key key;
    uint256 genesis{uint256S("aa")};
    uint256 registration{uint256S("bb")};
    fs::path datadir;
    std::string reason;

    explicit JournalEnv(BasicTestingSetup& base)
    {
        std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
        seed[0] = 9; // Public deterministic test material only.
        BOOST_REQUIRE(key.SetSeed(seed));
        datadir = base.SetDataDir("pqjournal-" + std::to_string(Instances++));
        fs::remove_all(datadir / "pqjournal");
        BOOST_TEST_MESSAGE("env datadir: " << datadir.string());
    }

    std::unique_ptr<pqjournal::Journal> Load()
    {
        return Load(key.GetPublicKey());
    }

    std::unique_ptr<pqjournal::Journal> Initialize()
    {
        return pqjournal::Journal::Initialize(datadir, genesis, registration, key.GetPublicKey(), reason);
    }

    std::unique_ptr<pqjournal::Journal> Load(const mldsa44::PublicKey& publicKey)
    {
        return pqjournal::Journal::Load(datadir, genesis, registration, publicKey, reason);
    }

    pqquorum::Statement statement(uint32_t height, uint32_t round, pqquorum::Purpose purpose,
                                  const uint256& value)
    {
        pqquorum::Statement result;
        result.purpose = purpose;
        result.genesis = genesis;
        result.anchor = uint256S("cc");
        result.committee = uint256S("dd");
        result.height = height;
        result.round = round;
        result.value = value;
        return result;
    }

    static bool Signer(pqquorum::Statement /*unused*/, pqjournal::Journal::Signature& signature)
    {
        signature.fill(0x5a); // Fake signing material; the journal never inspects it.
        return true;
    }
    static bool RefusingSigner(pqquorum::Statement /*unused*/, pqjournal::Journal::Signature& /*unused*/)
    {
        return false;
    }
    static uint256 Value(unsigned char tag)
    {
        std::vector<unsigned char> bytes(32, tag);
        return uint256(bytes);
    }
    fs::path File() const
    {
        // The file name embeds a key fingerprint; a single-key fixture has one.
        fs::path found;
        for (const auto& entry : fs::directory_iterator(datadir / "pqjournal"))
            if (entry.path().extension() == ".journal") found = entry.path();
        BOOST_REQUIRE(!found.empty());
        return found;
    }
};
size_t JournalEnv::Instances = 0;
} // namespace

BOOST_AUTO_TEST_CASE(journal_create_record_and_reuse)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    BOOST_CHECK_EQUAL(journal->FinalizedHeight(), 0U);

    pqjournal::Journal::Signature signature;
    std::string reason;
    const auto vote = env.statement(100, 1, pqquorum::Purpose::PREVOTE, JournalEnv::Value(1));
    BOOST_REQUIRE(journal->GetOrSignVote(vote, JournalEnv::Signer, signature, reason));

    // Retry reuse: the signer is not invoked, the recorded signature returns.
    pqjournal::Journal::Signature reused;
    bool invoked = false;
    const auto counting = [&](pqquorum::Statement, pqjournal::Journal::Signature& result) {
        invoked = true;
        result.fill(0x5a);
        return true;
    };
    BOOST_REQUIRE(journal->GetOrSignVote(vote, counting, reused, reason));
    BOOST_CHECK(!invoked);
    BOOST_CHECK(reused == signature);

    // A different value at the same height/round/step is refused forever.
    const auto conflict = env.statement(100, 1, pqquorum::Purpose::PREVOTE, JournalEnv::Value(2));
    pqjournal::Journal::Signature refused;
    BOOST_CHECK(!journal->GetOrSignVote(conflict, JournalEnv::Signer, refused, reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-conflicting-vote");
    // The precommit step is a distinct decision slot.
    const auto precommit = env.statement(100, 1, pqquorum::Purpose::PRECOMMIT, JournalEnv::Value(1));
    BOOST_REQUIRE(journal->GetOrSignVote(precommit, JournalEnv::Signer, refused, reason));
    // A vote above the height survives finalization of the height below.
    const auto next = env.statement(101, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(9));
    pqjournal::Journal::Signature nextSignature;
    BOOST_REQUIRE(journal->GetOrSignVote(next, JournalEnv::Signer, nextSignature, reason));

    // Locks and finalization persist with the same rules.
    BOOST_REQUIRE(journal->RecordLock(100, 1, JournalEnv::Value(1), reason));
    BOOST_CHECK(!journal->RecordLock(100, 0, JournalEnv::Value(1), reason));
    BOOST_REQUIRE(journal->RecordFinalized(100, JournalEnv::Value(1), uint256S("ee"), reason));
    BOOST_CHECK_EQUAL(journal->FinalizedHeight(), 100U);
    BOOST_CHECK(!journal->GetOrSignVote(vote, JournalEnv::Signer, refused, reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-height-finalized");
    BOOST_CHECK(!journal->RecordFinalized(100, JournalEnv::Value(1), uint256S("ee"), reason));
    BOOST_CHECK(!journal->RecordLock(100, 2, JournalEnv::Value(1), reason));

    // Reload replays the identical state after the writer closes.
    journal.reset();
    auto reloaded = env.Load();
    BOOST_REQUIRE(reloaded);
    BOOST_CHECK_EQUAL(reloaded->FinalizedHeight(), 100U);
    uint256 value;
    uint32_t round = 0;
    BOOST_CHECK(!reloaded->GetLock(100, value, round)); // erased with finalization
    BOOST_CHECK(!reloaded->GetVote(100, 1, pqquorum::Purpose::PREVOTE, value, reused));
    uint256 anchor;
    uint32_t height = 0;
    BOOST_CHECK(reloaded->GetFinalized(height, value, anchor));
    BOOST_CHECK_EQUAL(height, 100U);
    BOOST_CHECK(value == JournalEnv::Value(1));
    BOOST_CHECK(anchor == uint256S("ee"));
    pqjournal::Journal::Signature recovered;
    BOOST_CHECK(reloaded->GetVote(101, 0, pqquorum::Purpose::PREVOTE, value, recovered));
    BOOST_CHECK(value == JournalEnv::Value(9));
    BOOST_CHECK(recovered == nextSignature);
    uint32_t highest = 0;
    BOOST_CHECK(reloaded->HighestVoted(101, pqquorum::Purpose::PREVOTE, highest));
    BOOST_CHECK_EQUAL(highest, 0U);
    BOOST_CHECK(!reloaded->HighestVoted(100, pqquorum::Purpose::PREVOTE, highest));
}

BOOST_AUTO_TEST_CASE(journal_preserves_highest_proof_across_restart_and_compaction)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    pqquorum::Certificate proof, recovered;
    proof.statement = env.statement(20, 4, pqquorum::Purpose::PREVOTE, JournalEnv::Value(3));
    for (uint16_t i = 0; i < 3; ++i) {
        pqquorum::Signature signature;
        signature.member = i;
        proof.signatures.push_back(signature); // Storage API receives already-verified public proof material.
    }
    BOOST_CHECK(!journal->GetProof(20, recovered));
    BOOST_REQUIRE(journal->RecordProof(proof, env.reason));
    const auto size = fs::file_size(env.File());
    BOOST_REQUIRE(journal->RecordProof(proof, env.reason));
    BOOST_CHECK_EQUAL(fs::file_size(env.File()), size);
    auto other = proof;
    other.statement.round = 3;
    BOOST_CHECK(!journal->RecordProof(other, env.reason));
    other = proof; other.statement.value.SetNull();
    BOOST_CHECK(!journal->RecordProof(other, env.reason));
    other = proof; ++other.statement.round; other.statement.anchor.SetNull();
    BOOST_CHECK(!journal->RecordProof(other, env.reason));
    other = proof; other.statement.genesis.SetNull();
    BOOST_CHECK(!journal->RecordProof(other, env.reason));
    other = proof; other.statement.purpose = pqquorum::Purpose::PRECOMMIT;
    BOOST_CHECK(!journal->RecordProof(other, env.reason));
    ++proof.statement.round;
    proof.statement.value.SetNull(); // A nil quorum is a real unlock proof.
    BOOST_REQUIRE(journal->RecordProof(proof, env.reason));
    journal.reset();
    journal = env.Load();
    BOOST_REQUIRE(journal);
    BOOST_REQUIRE(journal->GetProof(20, recovered));
    BOOST_CHECK(pqquorum::Encode(recovered) == pqquorum::Encode(proof));
    BOOST_REQUIRE(journal->RecordFinalized(19, JournalEnv::Value(4), JournalEnv::Value(5), env.reason));
    journal.reset();
    journal = env.Load();
    BOOST_REQUIRE(journal);
    BOOST_REQUIRE(journal->GetProof(20, recovered));
    BOOST_CHECK(pqquorum::Encode(recovered) == pqquorum::Encode(proof));
    BOOST_REQUIRE(journal->RecordFinalized(20, JournalEnv::Value(4), JournalEnv::Value(5), env.reason));
    BOOST_CHECK(!journal->RecordProof(proof, env.reason));
    journal.reset();
    journal = env.Load();
    BOOST_REQUIRE(journal);
    BOOST_CHECK(!journal->GetProof(20, recovered));
}

BOOST_AUTO_TEST_CASE(journal_proof_truncation_and_invalid_lengths_fail_safely)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    const auto headerSize = fs::file_size(env.File());
    pqquorum::Certificate proof, recovered;
    proof.statement = env.statement(20, 4, pqquorum::Purpose::PREVOTE, JournalEnv::Value(3));
    proof.signatures.resize(3);
    for (uint16_t i = 0; i < 3; ++i) proof.signatures[i].member = i;
    BOOST_REQUIRE(journal->RecordProof(proof, env.reason));
    const auto completeSize = fs::file_size(env.File());
    ++proof.statement.round;
    BOOST_REQUIRE(journal->RecordProof(proof, env.reason));
    journal.reset();
    fs::resize_file(env.File(), fs::file_size(env.File()) - 10);
    journal = env.Load();
    BOOST_REQUIRE(journal);
    BOOST_CHECK_EQUAL(fs::file_size(env.File()), completeSize);
    BOOST_REQUIRE(journal->GetProof(20, recovered));
    BOOST_CHECK_EQUAL(recovered.statement.round, 4U);
    BOOST_REQUIRE(journal->RecordProof(proof, env.reason));
    journal.reset();
    // A complete but unreasonable length is corruption, never an allocation
    // request and never silently treated as a trailing partial append.
    FILE* handle = fsbridge::fopen(env.File(), "r+b");
    BOOST_REQUIRE(handle);
    BOOST_REQUIRE(fseek(handle, headerSize + 1, SEEK_SET) == 0);
    for (int i = 0; i < 4; ++i) BOOST_REQUIRE(fputc(0xff, handle) != EOF);
    BOOST_REQUIRE(fclose(handle) == 0);
    BOOST_CHECK(!env.Load());
    BOOST_CHECK_EQUAL(env.reason, "pq-journal-corrupt-proof-length");
}

BOOST_AUTO_TEST_CASE(journal_corrupt_in_range_proof_length_cannot_discard_later_votes)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    const auto headerSize = fs::file_size(env.File());
    pqquorum::Certificate proof;
    proof.statement = env.statement(20, 4, pqquorum::Purpose::PREVOTE, JournalEnv::Value(3));
    proof.signatures.resize(3);
    for (uint16_t i = 0; i < 3; ++i) proof.signatures[i].member = i;
    BOOST_REQUIRE(journal->RecordProof(proof, env.reason));
    pqjournal::Journal::Signature signature;
    BOOST_REQUIRE(journal->GetOrSignVote(proof.statement, JournalEnv::Signer, signature, env.reason));
    journal.reset();
    const auto originalSize = fs::file_size(env.File());
    FILE* handle = fsbridge::fopen(env.File(), "r+b");
    BOOST_REQUIRE(handle);
    BOOST_REQUIRE(fseek(handle, headerSize + 1, SEEK_SET) == 0);
    for (const int byte : {0, 0, 8, 0}) BOOST_REQUIRE(fputc(byte, handle) != EOF); // In-range524288.
    BOOST_REQUIRE(fclose(handle) == 0);
    BOOST_CHECK(!env.Load());
    BOOST_CHECK_EQUAL(fs::file_size(env.File()), originalSize);
}

BOOST_AUTO_TEST_CASE(normal_load_never_provisions_a_missing_identity)
{
    JournalEnv env{*this};
    BOOST_CHECK(!env.Load());
    BOOST_CHECK(!fs::exists(env.datadir / "pqjournal"));
}

BOOST_AUTO_TEST_CASE(missing_sidecar_is_not_recreated_around_existing_journal)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    const auto marker = fs::path(env.File().string() + ".lock");
    journal.reset();
    BOOST_REQUIRE(fs::remove(marker)); // Disposable public test identity only.
    BOOST_CHECK(!env.Load());
    BOOST_CHECK(!fs::exists(marker));
    BOOST_CHECK(!env.Initialize());
    BOOST_CHECK(!fs::exists(marker));
}

BOOST_AUTO_TEST_CASE(explicit_initialization_never_overwrites_existing_state)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    const auto file = env.File();
    const auto size = fs::file_size(file);
    BOOST_CHECK(!env.Initialize());
    BOOST_CHECK_EQUAL(fs::file_size(file), size);
    journal.reset();
    BOOST_CHECK(!env.Initialize());
    BOOST_REQUIRE(fs::remove(file)); // Disposable test fixture, not a recovery procedure.
    BOOST_CHECK(!env.Initialize());
    BOOST_CHECK(!fs::exists(file));
}

BOOST_AUTO_TEST_CASE(journal_refuses_same_slot_with_different_context)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    std::string reason;
    pqjournal::Journal::Signature signature;
    const auto vote = env.statement(20, 2, pqquorum::Purpose::PREVOTE, JournalEnv::Value(8));
    BOOST_REQUIRE(journal->GetOrSignVote(vote, JournalEnv::Signer, signature, reason));

    bool invoked = false;
    const auto counting = [&](pqquorum::Statement, pqjournal::Journal::Signature&) {
        invoked = true;
        return true;
    };
    pqquorum::Statement differentAnchor = vote;
    differentAnchor.anchor = uint256S("ce");
    pqjournal::Journal::Signature refused;
    BOOST_CHECK(!journal->GetOrSignVote(differentAnchor, counting, refused, reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-conflicting-vote");
    BOOST_CHECK(!invoked);

    pqquorum::Statement differentCommittee = vote;
    differentCommittee.committee = uint256S("de");
    BOOST_CHECK(!journal->GetOrSignVote(differentCommittee, counting, refused, reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-conflicting-vote");
    BOOST_CHECK(!invoked);

    journal.reset();
    auto reloaded = env.Load();
    BOOST_REQUIRE(reloaded);
    BOOST_CHECK(!reloaded->GetOrSignVote(differentAnchor, counting, refused, reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-conflicting-vote");
    BOOST_CHECK(!invoked);
}

BOOST_AUTO_TEST_CASE(journal_capacity_is_checked_before_signing)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    fs::resize_file(env.File(), 1u << 30); // Sparse disposable file, not a 1GB allocation.
    bool invoked = false;
    pqjournal::Journal::Signature signature;
    const auto signer = [&](pqquorum::Statement, pqjournal::Journal::Signature&) {
        invoked = true;
        return true;
    };
    BOOST_CHECK(!journal->GetOrSignVote(env.statement(1, 0, pqquorum::Purpose::PREVOTE, {}),
                                      signer, signature, env.reason));
    BOOST_CHECK(!invoked);
    BOOST_CHECK_EQUAL(fs::file_size(env.File()), 1u << 30);
}

BOOST_AUTO_TEST_CASE(journal_reports_only_actual_persisted_progress)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    uint32_t finalized = 123, height = 123, round = 123;
    BOOST_REQUIRE(journal->GetProgress(finalized, height, round));
    BOOST_CHECK_EQUAL(finalized, 0U);
    BOOST_CHECK_EQUAL(height, 0U);
    BOOST_CHECK_EQUAL(round, 0U);
    pqjournal::Journal::Signature signature;
    BOOST_REQUIRE(journal->GetOrSignVote(env.statement(20, 7, pqquorum::Purpose::PREVOTE, {}),
                                        JournalEnv::Signer, signature, env.reason));
    BOOST_REQUIRE(journal->GetProgress(finalized, height, round));
    BOOST_CHECK_EQUAL(finalized, 0U);
    BOOST_CHECK_EQUAL(height, 20U);
    BOOST_CHECK_EQUAL(round, 7U);
    BOOST_REQUIRE(journal->RecordFinalized(20, uint256S("20"), uint256S("21"), env.reason));
    BOOST_REQUIRE(journal->GetProgress(finalized, height, round));
    BOOST_CHECK_EQUAL(finalized, 20U);
    BOOST_CHECK_EQUAL(height, 0U); // Compacted votes must not be inferred from finality.
}

BOOST_AUTO_TEST_CASE(journal_refuses_signer_and_wrong_network)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    pqjournal::Journal::Signature signature;
    std::string reason;
    const auto vote = env.statement(5, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(4));
    BOOST_CHECK(!journal->GetOrSignVote(vote, JournalEnv::RefusingSigner, signature, reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-signer-refused");
    // Nothing was journaled: a later working signer succeeds.
    BOOST_REQUIRE(journal->GetOrSignVote(vote, JournalEnv::Signer, signature, reason));

    pqquorum::Statement foreign = vote;
    foreign.genesis = uint256S("ff");
    BOOST_CHECK(!journal->GetOrSignVote(foreign, JournalEnv::Signer, signature, reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-invalid-statement");
    // SERVICE/HANDOFF statements are never journaled votes.
    pqquorum::Statement service = vote;
    service.purpose = pqquorum::Purpose::SERVICE;
    BOOST_CHECK(!journal->GetOrSignVote(service, JournalEnv::Signer, signature, reason));
}

BOOST_AUTO_TEST_CASE(journal_truncated_tail_recovers_last_good_record)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    pqjournal::Journal::Signature signature;
    std::string reason;
    const auto vote = env.statement(10, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(3));
    BOOST_REQUIRE(journal->GetOrSignVote(vote, JournalEnv::Signer, signature, reason));
    BOOST_REQUIRE(journal->RecordLock(10, 0, JournalEnv::Value(3), reason));
    journal.reset();

    const fs::path file = env.File();
    const auto original = fs::file_size(file);
    // A crash mid-append leaves a trailing partial record: the partial tail
    // record is truncated away and the last complete records replay.
    fs::resize_file(file, original - 10);
    auto partial = env.Load();
    BOOST_REQUIRE(partial);
    pqjournal::Journal::Signature recovered;
    uint256 value;
    uint32_t round = 0;
    BOOST_CHECK(partial->GetVote(10, 0, pqquorum::Purpose::PREVOTE, value, recovered));
    BOOST_CHECK(recovered == signature);
    BOOST_CHECK(!partial->GetLock(10, value, round)); // the partial lock record was discarded
    // Appended records continue from the truncation point without conflict.
    BOOST_REQUIRE(partial->RecordLock(10, 0, JournalEnv::Value(3), reason));
    partial.reset();
    auto again = env.Load();
    BOOST_REQUIRE(again);
    BOOST_CHECK(again->GetLock(10, value, round));
}

BOOST_AUTO_TEST_CASE(journal_fails_closed_on_corruption_and_mismatch)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    pqjournal::Journal::Signature signature;
    std::string reason;
    BOOST_REQUIRE(journal->GetOrSignVote(env.statement(10, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(3)),
                                         JournalEnv::Signer, signature, reason));
    journal.reset();
    const fs::path file = env.File();
    const auto original = fs::file_size(file);

    // Mid-chain corruption fails closed.
    {
        FILE* handle = fsbridge::fopen(file, "r+b");
        BOOST_REQUIRE(handle);
        BOOST_REQUIRE(fseek(handle, 0, SEEK_END) == 0);
        const long size = ftell(handle);
        BOOST_REQUIRE(fseek(handle, size - 40, SEEK_SET) == 0);
        BOOST_REQUIRE(fputc(0xa5, handle) != EOF);
        fclose(handle);
        auto corrupt = env.Load();
        BOOST_CHECK(!corrupt);
        BOOST_CHECK_EQUAL(env.reason, "pq-journal-corrupt-chain");
    }
    // A different genesis for the same registration fails closed.
    {
        JournalEnv wrong{*this};
        wrong.datadir = env.datadir; // same directory, different genesis header
        wrong.genesis = uint256S("ab");
        auto mismatch = wrong.Load();
        BOOST_CHECK(!mismatch);
        BOOST_CHECK_EQUAL(wrong.reason, "pq-journal-header-mismatch");
    }
    // Trailing garbage of unknown record type on an otherwise valid journal
    // fails closed: rebuild a clean journal first.
    {
        fs::remove(file);
        fs::remove(file.string() + ".lock"); // Explicitly reset this isolated fixture only.
        auto clean = env.Initialize();
        BOOST_REQUIRE(clean);
        pqjournal::Journal::Signature signature2;
        BOOST_REQUIRE(clean->GetOrSignVote(env.statement(11, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(6)),
                                           JournalEnv::Signer, signature2, reason));
        clean.reset();
        FILE* handle = fsbridge::fopen(file, "ab");
        BOOST_REQUIRE(handle);
        for (int i = 0; i < 80; ++i) fputc(0x7f, handle);
        fclose(handle);
        auto garbage = env.Load();
        BOOST_CHECK(!garbage);
        BOOST_CHECK_EQUAL(env.reason, "pq-journal-corrupt-record");
    }
    // Each sub-case used the same corrupted file deliberately; nothing is restored.
}

BOOST_AUTO_TEST_CASE(journal_fails_closed_on_header_digest_corruption)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    journal.reset();

    const fs::path file = env.File();
    FILE* handle = fsbridge::fopen(file, "r+b");
    BOOST_REQUIRE(handle);
    BOOST_REQUIRE(fseek(handle, -1, SEEK_END) == 0);
    BOOST_REQUIRE(fputc(0xa5, handle) != EOF);
    fclose(handle);

    auto corrupt = env.Load();
    BOOST_CHECK(!corrupt);
    BOOST_CHECK_EQUAL(env.reason, "pq-journal-header-corrupt");
}

BOOST_AUTO_TEST_CASE(journal_rotation_archives_and_starts_fresh)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    pqjournal::Journal::Signature signature;
    std::string reason;
    BOOST_REQUIRE(journal->GetOrSignVote(env.statement(7, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(5)),
                                         JournalEnv::Signer, signature, reason));
    journal.reset();

    mldsa44::Key rotated;
    std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
    seed[0] = 10; // Public deterministic test material only.
    BOOST_REQUIRE(rotated.SetSeed(seed));
    auto fresh = pqjournal::Journal::Initialize(env.datadir, env.genesis, env.registration,
                                                 rotated.GetPublicKey(), env.reason);
    BOOST_REQUIRE(fresh);
    BOOST_CHECK_EQUAL(fresh->FinalizedHeight(), 0U);
    pqjournal::Journal::Signature unused;
    uint256 value;
    BOOST_CHECK(!fresh->GetVote(7, 0, pqquorum::Purpose::PREVOTE, value, unused));
    // The retired key's journal is retained and still replays under its key.
    auto original = env.Load();
    BOOST_REQUIRE(original);
    BOOST_CHECK(original->GetVote(7, 0, pqquorum::Purpose::PREVOTE, value, unused));
}

BOOST_AUTO_TEST_CASE(journal_compaction_preserves_state_and_bounds_file)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    std::string reason;
    pqjournal::Journal::Signature signature;
    for (uint32_t height = 1; height <= 6; ++height) {
        BOOST_REQUIRE(journal->GetOrSignVote(
            env.statement(height, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(height)),
            JournalEnv::Signer, signature, reason));
        BOOST_REQUIRE(journal->RecordLock(height, 0, JournalEnv::Value(height), reason));
        BOOST_REQUIRE(journal->RecordFinalized(height, JournalEnv::Value(height), uint256S("ee"), reason));
    }
    // Live state above the finalized height survives compaction.
    BOOST_REQUIRE(journal->GetOrSignVote(
        env.statement(7, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(7)), JournalEnv::Signer,
        signature, reason));
    BOOST_REQUIRE(journal->RecordLock(7, 0, JournalEnv::Value(7), reason));
    journal.reset();

    const fs::path file = env.File();
    const auto expectedSize = 1 + 10 + 1 + 32 + 32 + mldsa44::PUBLIC_KEY_SIZE + 32 +
        (1 + 4 + 32 + 32 + 32) +
        (1 + 4 + 4 + 1 + 32 + 32 + 32 + mldsa44::SIGNATURE_SIZE + 32) +
        (1 + 4 + 4 + 32 + 32);
    BOOST_CHECK_EQUAL(fs::file_size(file), expectedSize);
    auto reloaded = env.Load();
    BOOST_REQUIRE(reloaded);
    BOOST_CHECK_EQUAL(reloaded->FinalizedHeight(), 6U);
    uint256 value;
    uint32_t round = 0;
    BOOST_CHECK(reloaded->GetLock(7, value, round));
    BOOST_CHECK(value == JournalEnv::Value(7));
    pqjournal::Journal::Signature voteSignature;
    BOOST_CHECK(reloaded->GetVote(7, 0, pqquorum::Purpose::PREVOTE, value, voteSignature));
    BOOST_CHECK(value == JournalEnv::Value(7));
    // Heights at or below finalization are gone from the live state.
    BOOST_CHECK(!reloaded->GetVote(1, 0, pqquorum::Purpose::PREVOTE, value, voteSignature));
    BOOST_CHECK(!reloaded->GetLock(1, value, round));
    // The compacted file replays identically a second time.
    reloaded.reset();
    auto again = env.Load();
    BOOST_REQUIRE(again);
    BOOST_CHECK_EQUAL(again->FinalizedHeight(), 6U);
    BOOST_CHECK(again->GetVote(7, 0, pqquorum::Purpose::PREVOTE, value, voteSignature));
}

BOOST_AUTO_TEST_CASE(journal_compaction_preserves_vote_context)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    std::string reason;
    pqjournal::Journal::Signature signature;
    const auto vote = env.statement(7, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(7));
    BOOST_REQUIRE(journal->GetOrSignVote(vote, JournalEnv::Signer, signature, reason));
    BOOST_REQUIRE(journal->RecordFinalized(6, JournalEnv::Value(6), uint256S("ee"), reason));
    journal.reset();

    auto reloaded = env.Load();
    BOOST_REQUIRE(reloaded);
    bool invoked = false;
    const auto counting = [&](pqquorum::Statement, pqjournal::Journal::Signature&) {
        invoked = true;
        return true;
    };
    pqquorum::Statement differentAnchor = vote;
    differentAnchor.anchor = uint256S("ce");
    pqjournal::Journal::Signature refused;
    BOOST_CHECK(!reloaded->GetOrSignVote(differentAnchor, counting, refused, reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-conflicting-vote");
    BOOST_CHECK(!invoked);

    pqquorum::Statement differentCommittee = vote;
    differentCommittee.committee = uint256S("de");
    BOOST_CHECK(!reloaded->GetOrSignVote(differentCommittee, counting, refused, reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-conflicting-vote");
    BOOST_CHECK(!invoked);

    pqjournal::Journal::Signature reused;
    BOOST_REQUIRE(reloaded->GetOrSignVote(vote, counting, reused, reason));
    BOOST_CHECK(!invoked);
    BOOST_CHECK(reused == signature);
}

#if !defined(WIN32)
BOOST_AUTO_TEST_CASE(journal_excludes_other_writers_across_compaction)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    BOOST_CHECK(!env.Load());
    std::string reason;
    BOOST_REQUIRE(journal->RecordFinalized(1, JournalEnv::Value(1), uint256S("ee"), reason));
    BOOST_CHECK(!env.Load());
    const pid_t child = fork();
    BOOST_REQUIRE(child >= 0);
    if (child == 0) _exit(env.Load() ? 10 : 0);
    int status = 0;
    BOOST_REQUIRE(waitpid(child, &status, 0) == child);
    BOOST_REQUIRE(WIFEXITED(status));
    BOOST_CHECK_EQUAL(WEXITSTATUS(status), 0);
    journal.reset();
    BOOST_REQUIRE(env.Load());
}

BOOST_AUTO_TEST_CASE(journal_rejects_symlink_and_missing_used_file)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    const auto file = env.File();
    journal.reset();
    const fs::path saved = file.string() + ".saved";
    fs::rename(file, saved);
    BOOST_CHECK(!env.Load()); // Never silently start this identity over.
    // The failed open must not recreate a header-only journal.
    BOOST_CHECK(!fs::exists(file));
    if (fs::exists(file)) fs::remove(file); // Only the isolated test fixture.
    fs::create_symlink(saved, file);
    BOOST_CHECK(!env.Load());
    fs::remove(file);
    fs::rename(saved, file);
    BOOST_REQUIRE(env.Load());
}

BOOST_AUTO_TEST_CASE(journal_compaction_never_follows_temporary_symlink)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    const auto file = env.File();
    const fs::path sentinel = file.string() + ".sentinel";
    fs::copy_file(file, sentinel);
    const auto originalSize = fs::file_size(sentinel);
    fs::create_symlink(sentinel, file.string() + ".tmp");
    std::string reason;
    BOOST_CHECK(!journal->RecordFinalized(1, JournalEnv::Value(1), uint256S("ee"), reason));
    BOOST_CHECK_EQUAL(fs::file_size(sentinel), originalSize);
    // Even a poisoned writer must retain its exclusive lease until closed.
    BOOST_CHECK(!env.Load());
}

BOOST_AUTO_TEST_CASE(journal_append_failure_poisons_instance)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    std::string reason;
    pqjournal::Journal::Signature cachedSignature;
    const auto cached = env.statement(31, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(2));
    BOOST_REQUIRE(journal->GetOrSignVote(cached, JournalEnv::Signer, cachedSignature, reason));
    const auto originalSize = fs::file_size(env.File());
    const pid_t child = fork();
    BOOST_REQUIRE(child >= 0);
    if (child == 0) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit {originalSize, originalSize};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(10);
        pqjournal::Journal::Signature signature;
        const auto first = env.statement(30, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(1));
        if (journal->GetOrSignVote(first, JournalEnv::Signer, signature, reason)) _exit(11);
        bool invoked = false;
        const auto counting = [&](pqquorum::Statement, pqjournal::Journal::Signature&) {
            invoked = true;
            return true;
        };
        const auto second = env.statement(31, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(2));
        uint256 value;
        if (journal->GetVote(31, 0, pqquorum::Purpose::PREVOTE, value, signature)) _exit(12);
        if (journal->GetOrSignVote(second, counting, signature, reason) || invoked) _exit(13);
        _exit(0);
    }
    int status = 0;
    BOOST_REQUIRE(waitpid(child, &status, 0) == child);
    BOOST_CHECK(WIFEXITED(status));
    BOOST_CHECK_EQUAL(WEXITSTATUS(status), 0);
    journal.reset();
    auto reopened = env.Load();
    BOOST_REQUIRE(reopened);
    uint256 reopenedValue;
    pqjournal::Journal::Signature reopenedSignature;
    BOOST_CHECK(reopened->GetVote(31, 0, pqquorum::Purpose::PREVOTE, reopenedValue,
                                  reopenedSignature));
    BOOST_CHECK(reopenedValue == cached.value);
    BOOST_CHECK(reopenedSignature == cachedSignature);
}
#endif

BOOST_AUTO_TEST_CASE(journal_compaction_failure_poisons_instance)
{
    JournalEnv env{*this};
    auto journal = env.Initialize();
    BOOST_REQUIRE(journal);
    std::string reason;
    pqjournal::Journal::Signature signature;
    BOOST_REQUIRE(journal->GetOrSignVote(
        env.statement(40, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(4)), JournalEnv::Signer,
        signature, reason));
    const auto cached = env.statement(41, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(5));
    pqjournal::Journal::Signature cachedSignature;
    BOOST_REQUIRE(journal->GetOrSignVote(cached, JournalEnv::Signer, cachedSignature, reason));
    fs::path temporary = env.File();
    temporary += ".tmp";
    BOOST_REQUIRE(fs::create_directory(temporary));
    BOOST_CHECK(!journal->RecordFinalized(40, JournalEnv::Value(4), uint256S("ee"), reason));
    BOOST_CHECK_EQUAL(reason, "pq-journal-compaction-failed");
    bool invoked = false;
    const auto counting = [&](pqquorum::Statement, pqjournal::Journal::Signature&) {
        invoked = true;
        return true;
    };
    pqjournal::Journal::Signature refused;
    uint256 cachedValue;
    BOOST_CHECK(!journal->GetVote(41, 0, pqquorum::Purpose::PREVOTE, cachedValue, refused));
    BOOST_CHECK(!journal->GetOrSignVote(
        cached, counting, refused, reason));
    BOOST_CHECK(!invoked);
    journal.reset();
    BOOST_REQUIRE(fs::remove_all(temporary) > 0);
    auto reopened = env.Load();
    BOOST_REQUIRE(reopened);
    BOOST_CHECK(reopened->GetVote(41, 0, pqquorum::Purpose::PREVOTE, cachedValue, refused));
    BOOST_CHECK(cachedValue == cached.value);
    BOOST_CHECK(refused == cachedSignature);
}

BOOST_AUTO_TEST_SUITE_END()
