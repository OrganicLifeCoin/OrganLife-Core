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
    auto journal = env.Load();
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

    // Reload replays the identical state.
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

BOOST_AUTO_TEST_CASE(journal_refuses_signer_and_wrong_network)
{
    JournalEnv env{*this};
    auto journal = env.Load();
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
    auto journal = env.Load();
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
    auto journal = env.Load();
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
        FILE* handle = fopen(file.native().c_str(), "r+b");
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
        auto clean = env.Load();
        BOOST_REQUIRE(clean);
        pqjournal::Journal::Signature signature2;
        BOOST_REQUIRE(clean->GetOrSignVote(env.statement(11, 0, pqquorum::Purpose::PREVOTE, JournalEnv::Value(6)),
                                           JournalEnv::Signer, signature2, reason));
        clean.reset();
        FILE* handle = fopen(file.native().c_str(), "ab");
        BOOST_REQUIRE(handle);
        for (int i = 0; i < 80; ++i) fputc(0x7f, handle);
        fclose(handle);
        auto garbage = env.Load();
        BOOST_CHECK(!garbage);
        BOOST_CHECK_EQUAL(env.reason, "pq-journal-corrupt-record");
    }
    // Each sub-case used the same corrupted file deliberately; nothing is restored.
}

BOOST_AUTO_TEST_CASE(journal_rotation_archives_and_starts_fresh)
{
    JournalEnv env{*this};
    auto journal = env.Load();
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
    auto fresh = env.Load(rotated.GetPublicKey());
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
    auto journal = env.Load();
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
    BOOST_CHECK(fs::file_size(file) < 4096);
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
    auto again = env.Load();
    BOOST_REQUIRE(again);
    BOOST_CHECK_EQUAL(again->FinalizedHeight(), 6U);
    BOOST_CHECK(again->GetVote(7, 0, pqquorum::Purpose::PREVOTE, value, voteSignature));
}

BOOST_AUTO_TEST_SUITE_END()
