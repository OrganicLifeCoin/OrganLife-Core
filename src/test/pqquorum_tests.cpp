// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqquorum.h>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <chrono>

namespace {
struct QuorumFixture {
    std::array<mldsa44::Key, 4> keys;
    std::vector<pqquorum::Member> members;
    pqquorum::Statement statement;
    QuorumFixture()
    {
        for (size_t i = 0; i < keys.size(); ++i) {
            std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
            seed[0] = i + 1; // Public deterministic test material only.
            BOOST_REQUIRE(keys[i].SetSeed(seed));
            members.push_back({uint256S(std::to_string(i + 1)), keys[i].GetPublicKey()});
        }
        statement.genesis = uint256S("11");
        statement.anchor = uint256S("22");
        statement.committee = pqquorum::Commitment(members);
        statement.height = 100;
        statement.round = 7;
        statement.value = uint256S("33");
        statement.purpose = pqquorum::Purpose::PRECOMMIT;
    }
    pqquorum::Certificate certificate(size_t count = 3)
    {
        pqquorum::Certificate result;
        result.statement = statement;
        for (size_t i = 0; i < count; ++i) {
            pqquorum::Signature vote;
            vote.member = i;
            std::vector<unsigned char> signature;
            BOOST_REQUIRE(keys[i].Sign(pqquorum::Message(statement), pqquorum::Context(), signature));
            std::copy(signature.begin(), signature.end(), vote.bytes.begin());
            result.signatures.push_back(vote);
        }
        return result;
    }
};
}

BOOST_FIXTURE_TEST_SUITE(pqquorum_tests, QuorumFixture)

BOOST_AUTO_TEST_CASE(round_locks_survive_timeout_and_require_newer_quorum_to_change)
{
    pqquorum::RoundState round(statement.genesis, statement.anchor, statement.height, members);
    const auto first = statement.value;
    const auto other = uint256S("44");
    pqquorum::Statement vote;
    std::string reason;
    BOOST_REQUIRE(round.Prevote(first, nullptr, vote, reason));
    BOOST_CHECK(vote.value == first);
    BOOST_CHECK_EQUAL(vote.round, 0U);
    BOOST_CHECK(vote.purpose == pqquorum::Purpose::PREVOTE);
    BOOST_CHECK(!round.Prevote(other, nullptr, vote, reason));
    BOOST_CHECK(pqquorum::Message(vote).empty());
    statement.round = 0; statement.purpose = pqquorum::Purpose::PREVOTE;
    auto insufficient = certificate(2);
    BOOST_CHECK(!round.Precommit(&insufficient, first, vote, reason));
    auto proof = certificate();
    BOOST_REQUIRE(round.Precommit(&proof, first, vote, reason));
    BOOST_CHECK(vote.value == first);
    BOOST_CHECK(vote.purpose == pqquorum::Purpose::PRECOMMIT);
    BOOST_CHECK(!round.Prevote(first, nullptr, vote, reason));
    BOOST_CHECK(!round.Advance(0));
    BOOST_REQUIRE(round.Advance(1));
    BOOST_REQUIRE(round.Prevote(other, nullptr, vote, reason));
    BOOST_CHECK(vote.value == first); // A timeout is not an unlock proof.
    BOOST_REQUIRE(round.Precommit(nullptr, {}, vote, reason));
    BOOST_CHECK(vote.value.IsNull());
    BOOST_REQUIRE(round.Advance(2));
    // The older lock survives nil precommit without a nil quorum.
    BOOST_REQUIRE(round.Prevote(other, nullptr, vote, reason));
    BOOST_CHECK(vote.value == first);
    BOOST_REQUIRE(round.Advance(3));
    statement.round = 2; statement.value = other;
    proof = certificate();
    BOOST_REQUIRE(round.Prevote(other, &proof, vote, reason));
    BOOST_CHECK(vote.value == other);
}

BOOST_AUTO_TEST_CASE(round_commit_requires_bound_nonnil_precommits_and_is_terminal)
{
    pqquorum::RoundState round(statement.genesis, statement.anchor, statement.height, members);
    uint256 finalized;
    std::string reason;
    statement.purpose = pqquorum::Purpose::PREVOTE;
    BOOST_CHECK(!round.Commit(certificate(), statement.value, finalized, reason));
    BOOST_CHECK(finalized.IsNull());
    statement.purpose = pqquorum::Purpose::PRECOMMIT;
    const auto value = statement.value;
    statement.value.SetNull();
    BOOST_CHECK(!round.Commit(certificate(), statement.value, finalized, reason));
    statement.value = value;
    ++statement.height;
    BOOST_CHECK(!round.Commit(certificate(), statement.value, finalized, reason));
    --statement.height;
    BOOST_CHECK(!round.Commit(certificate(2), statement.value, finalized, reason));
    const auto committed = certificate();
    BOOST_REQUIRE(round.Commit(committed, value, finalized, reason));
    BOOST_CHECK(finalized == value);
    BOOST_CHECK(round.Commit(committed, value, finalized, reason));
    statement.value = uint256S("55");
    BOOST_CHECK(!round.Commit(certificate(), statement.value, finalized, reason));
    BOOST_CHECK(finalized.IsNull());
    BOOST_CHECK(!round.Advance(99));
    pqquorum::Statement vote;
    BOOST_CHECK(!round.Prevote(value, nullptr, vote, reason));
    BOOST_CHECK(!round.Precommit(nullptr, {}, vote, reason));
}

BOOST_AUTO_TEST_CASE(round_never_endorses_unvalidated_block_data)
{
    pqquorum::RoundState round(statement.genesis, statement.anchor, statement.height, members);
    pqquorum::Statement vote;
    uint256 finalized;
    std::string reason;
    statement.round = 0; statement.purpose = pqquorum::Purpose::PREVOTE;
    const auto proof = certificate();
    BOOST_CHECK(!round.Precommit(&proof, {}, vote, reason));
    BOOST_CHECK(!round.Precommit(&proof, uint256S("99"), vote, reason));
    BOOST_CHECK(pqquorum::Message(vote).empty());
    BOOST_REQUIRE(round.Precommit(&proof, statement.value, vote, reason));
    statement.purpose = pqquorum::Purpose::PRECOMMIT;
    const auto commit = certificate();
    BOOST_CHECK(!round.Commit(commit, {}, finalized, reason));
    BOOST_CHECK(!round.Commit(commit, uint256S("99"), finalized, reason));
    BOOST_CHECK(finalized.IsNull());
    BOOST_REQUIRE(round.Commit(commit, statement.value, finalized, reason));
}

BOOST_AUTO_TEST_CASE(round_nil_quorum_unlocks_but_stale_or_invalid_proofs_do_not)
{
    const auto first = statement.value;
    const auto other = uint256S("44");
    pqquorum::RoundState round(statement.genesis, statement.anchor, statement.height, members);
    std::string reason;
    pqquorum::Statement vote;
    statement.round = 0; statement.purpose = pqquorum::Purpose::PREVOTE;
    const auto oldProof = certificate();
    BOOST_REQUIRE(round.Precommit(&oldProof, first, vote, reason));
    BOOST_REQUIRE(round.Advance(1));
    statement.round = 1; statement.value.SetNull();
    auto nilProof = certificate();
    BOOST_REQUIRE(round.Precommit(&nilProof, {}, vote, reason));
    BOOST_CHECK(vote.value.IsNull());
    BOOST_REQUIRE(round.Advance(2));
    BOOST_REQUIRE(round.Prevote(other, nullptr, vote, reason));
    BOOST_CHECK(vote.value == other);
    statement.round = 2; statement.value = other;
    auto proof = certificate();
    BOOST_REQUIRE(round.Precommit(&proof, other, vote, reason));
    BOOST_CHECK(!round.Precommit(nullptr, {}, vote, reason));
    BOOST_REQUIRE(round.Precommit(&proof, other, vote, reason)); // exact retry
    BOOST_REQUIRE(round.Advance(3));
    BOOST_REQUIRE(round.Prevote(first, &oldProof, vote, reason));
    BOOST_CHECK(vote.value == other); // round 0 cannot release a round 2 lock
    BOOST_REQUIRE(round.Advance(4));
    statement.round = 3; statement.value = first;
    proof = certificate();
    proof.signatures[0].bytes[0] ^= 1;
    BOOST_CHECK(!round.Prevote(first, &proof, vote, reason));
    BOOST_REQUIRE(round.Prevote(first, nullptr, vote, reason));
    BOOST_CHECK(vote.value == other);
}

BOOST_AUTO_TEST_CASE(round_context_and_unlock_proof_fields_are_not_peer_controlled)
{
    BOOST_CHECK_THROW(pqquorum::RoundState({}, statement.anchor, statement.height, members), std::invalid_argument);
    BOOST_CHECK_THROW(pqquorum::RoundState(statement.genesis, {}, statement.height, members), std::invalid_argument);
    BOOST_CHECK_THROW(pqquorum::RoundState(statement.genesis, statement.anchor, 0, members), std::invalid_argument);
    auto tooFew = members; tooFew.pop_back();
    BOOST_CHECK_THROW(pqquorum::RoundState(statement.genesis, statement.anchor, statement.height, tooFew), std::invalid_argument);
    const auto base = statement;
    for (int mutation = 0; mutation < 7; ++mutation) {
        statement = base;
        pqquorum::RoundState round(base.genesis, base.anchor, base.height, members);
        BOOST_REQUIRE(round.Advance(2));
        statement.round = 1; statement.purpose = pqquorum::Purpose::PREVOTE;
        if (mutation == 0) statement.genesis = uint256S("66");
        if (mutation == 1) statement.anchor = uint256S("66");
        if (mutation == 2) statement.committee = uint256S("66");
        if (mutation == 3) ++statement.height;
        if (mutation == 4) statement.round = 2;
        if (mutation == 5) statement.purpose = pqquorum::Purpose::PRECOMMIT;
        const auto proof = certificate(mutation == 6 ? 2 : 3);
        std::string reason;
        pqquorum::Statement vote;
        BOOST_CHECK(!round.Prevote(base.value, &proof, vote, reason));
        BOOST_CHECK(!reason.empty());
        BOOST_CHECK(pqquorum::Message(vote).empty());
        BOOST_REQUIRE(round.Prevote(base.value, nullptr, vote, reason));
        BOOST_CHECK(vote.value == base.value);
        BOOST_CHECK(reason.empty());
    }
}

BOOST_AUTO_TEST_CASE(round_unlock_proof_is_not_a_proposal_and_same_value_keeps_lock)
{
    const auto original = statement.value;
    for (bool sameValue : {false, true}) {
        pqquorum::RoundState round(statement.genesis, statement.anchor, statement.height, members);
        std::string reason;
        pqquorum::Statement vote;
        statement.round = 0; statement.value = original; statement.purpose = pqquorum::Purpose::PREVOTE;
        auto proof = certificate();
        BOOST_REQUIRE(round.Precommit(&proof, original, vote, reason));
        BOOST_REQUIRE(round.Advance(2));
        statement.round = 1; statement.value = sameValue ? original : uint256S("55");
        proof = certificate();
        const auto proposal = uint256S("66");
        BOOST_REQUIRE(round.Prevote(proposal, &proof, vote, reason));
        BOOST_CHECK(vote.value == (sameValue ? original : proposal));
        BOOST_REQUIRE(round.Prevote(proposal, &proof, vote, reason)); // exact decision is idempotent
    }
}

BOOST_AUTO_TEST_CASE(strict_quorum_and_roundtrip)
{
    BOOST_CHECK_EQUAL(pqquorum::Threshold(4), 3U);
    BOOST_CHECK_EQUAL(pqquorum::Threshold(3), 0U);
    BOOST_CHECK_EQUAL(pqquorum::Threshold(50), 34U);
    BOOST_CHECK_EQUAL(pqquorum::Threshold(400), 267U);
    BOOST_CHECK_EQUAL(pqquorum::Threshold(0), 0U);
    BOOST_CHECK_EQUAL(pqquorum::Threshold(1), 0U);
    BOOST_CHECK_EQUAL(pqquorum::Threshold(2), 0U);
    BOOST_CHECK_EQUAL(pqquorum::Threshold(401), 0U);
    auto cert = certificate();
    std::string reason;
    BOOST_REQUIRE(pqquorum::Verify(cert, statement, members, reason));
    BOOST_CHECK(reason.empty());
    const auto encoded = pqquorum::Encode(cert);
    BOOST_CHECK_EQUAL(encoded.size(), pqquorum::HEADER_SIZE + 3 * pqquorum::SIGNATURE_RECORD_SIZE);
    pqquorum::Certificate decoded;
    BOOST_REQUIRE(pqquorum::Decode(encoded, decoded));
    BOOST_CHECK(pqquorum::Encode(decoded) == encoded);
    BOOST_CHECK(pqquorum::Verify(decoded, statement, members, reason));
    BOOST_CHECK(!pqquorum::Verify(certificate(2), statement, members, reason));
    BOOST_CHECK(pqquorum::Verify(certificate(4), statement, members, reason));
}

BOOST_AUTO_TEST_CASE(three_operators_cannot_bootstrap_a_committee)
{
    // Even unanimous signatures cannot lower the network's minimum membership.
    members.resize(3);
    statement.committee = pqquorum::Commitment(members);
    BOOST_CHECK(statement.committee.IsNull());
    // A peer can still supply a nonnull purported committee commitment.
    if (statement.committee.IsNull()) statement.committee = uint256S("44");
    const auto cert = certificate(3);
    std::string reason;
    BOOST_CHECK(!pqquorum::Verify(cert, statement, members, reason));
    BOOST_CHECK(!reason.empty());
}

BOOST_AUTO_TEST_CASE(rejects_signer_substitution_duplicates_and_order)
{
    const auto valid = certificate();
    std::string reason;
    for (int change = 0; change < 5; ++change) {
        auto cert = valid;
        if (change == 0) cert.signatures[1] = cert.signatures[0];
        if (change == 1) std::swap(cert.signatures[0], cert.signatures[1]);
        if (change == 2) cert.signatures[2].member = members.size();
        if (change == 3) cert.signatures[2].member = 3;
        if (change == 4) cert.signatures[0].bytes[0] ^= 1;
        BOOST_CHECK(!pqquorum::Verify(cert, statement, members, reason));
        BOOST_CHECK(!reason.empty());
    }
    auto duplicate = members;
    duplicate[1].registration = duplicate[0].registration;
    BOOST_CHECK(pqquorum::Commitment(duplicate).IsNull());
    BOOST_CHECK(!pqquorum::Verify(valid, statement, duplicate, reason));
    duplicate = members;
    duplicate[1].operator_key = duplicate[0].operator_key;
    BOOST_CHECK(pqquorum::Commitment(duplicate).IsNull());
    BOOST_CHECK(!pqquorum::Verify(valid, statement, duplicate, reason));
    duplicate = members;
    duplicate[0].registration.SetNull();
    BOOST_CHECK(pqquorum::Commitment(duplicate).IsNull());
    std::reverse(duplicate.begin(), duplicate.end());
    BOOST_CHECK(!pqquorum::Verify(valid, statement, duplicate, reason));
}

BOOST_AUTO_TEST_CASE(binds_every_statement_field_and_trusted_committee)
{
    auto cert = certificate();
    std::string reason;
    for (int change = 0; change < 7; ++change) {
        auto other = statement;
        if (change == 0) other.genesis = uint256S("44");
        if (change == 1) other.anchor = uint256S("44");
        if (change == 2) other.committee = uint256S("44");
        if (change == 3) ++other.height;
        if (change == 4) ++other.round;
        if (change == 5) other.value = uint256S("44");
        if (change == 6) other.purpose = pqquorum::Purpose::PREVOTE;
        BOOST_CHECK(!pqquorum::Verify(cert, other, members, reason));
        cert.statement = other;
        BOOST_CHECK(!pqquorum::Verify(cert, other, members, reason));
        cert.statement = statement;
    }
    auto reordered = members;
    std::reverse(reordered.begin(), reordered.end());
    BOOST_CHECK(pqquorum::Commitment(reordered) != statement.committee);
    BOOST_CHECK(!pqquorum::Verify(cert, statement, reordered, reason));
    cert.statement.purpose = static_cast<pqquorum::Purpose>(255);
    BOOST_CHECK(pqquorum::Encode(cert).empty());
}

BOOST_AUTO_TEST_CASE(bounded_canonical_decoder_clears_partial_output)
{
    const auto valid = certificate();
    const auto bytes = pqquorum::Encode(valid);
    BOOST_REQUIRE(bytes.size() >= pqquorum::HEADER_SIZE);
    pqquorum::Certificate decoded = valid;
    for (const size_t size : {size_t(0), pqquorum::HEADER_SIZE - 1, pqquorum::HEADER_SIZE, bytes.size() - 1}) {
        BOOST_CHECK(!pqquorum::Decode({bytes.data(), size}, decoded));
        BOOST_CHECK(decoded.signatures.empty());
        BOOST_CHECK(decoded.statement.genesis.IsNull());
    }
    auto bad = bytes;
    bad.push_back(0);
    BOOST_CHECK(!pqquorum::Decode(bad, decoded));
    bad = bytes;
    bad[0] = 2; // Unknown encoding version.
    BOOST_CHECK(!pqquorum::Decode(bad, decoded));
    bad.assign(pqquorum::MAX_CERTIFICATE_SIZE + 1, 0);
    BOOST_CHECK(!pqquorum::Decode(bad, decoded));
    BOOST_CHECK(decoded.signatures.empty());
    auto cert = valid;
    cert.signatures[1].member = cert.signatures[0].member;
    BOOST_CHECK(pqquorum::Encode(cert).empty());
    cert = valid;
    cert.signatures.resize(pqquorum::MAX_MEMBERS + 1);
    BOOST_CHECK(pqquorum::Encode(cert).empty());
}

BOOST_AUTO_TEST_CASE(real_signature_verification_sample)
{
    const auto cert = certificate();
    std::string reason;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i)
        BOOST_REQUIRE(pqquorum::Verify(cert, statement, members, reason));
    BOOST_TEST_MESSAGE("PQ certificate: 30 ML-DSA verifications in " <<
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() << " us");
}

BOOST_AUTO_TEST_CASE(nil_values_are_only_vote_statements)
{
    statement.value.SetNull();
    BOOST_CHECK(!pqquorum::Message(statement).empty());
    statement.purpose = pqquorum::Purpose::PREVOTE;
    BOOST_CHECK(!pqquorum::Message(statement).empty());
    statement.purpose = pqquorum::Purpose::SERVICE;
    BOOST_CHECK(pqquorum::Message(statement).empty());
    statement.purpose = pqquorum::Purpose::HANDOFF;
    BOOST_CHECK(pqquorum::Message(statement).empty());
}

BOOST_AUTO_TEST_CASE(decoder_rejects_noncanonical_fields_and_accepts_exact_maximum)
{
    auto cert = certificate();
    auto bytes = pqquorum::Encode(cert);
    BOOST_REQUIRE_EQUAL(bytes.size(), pqquorum::HEADER_SIZE + 3 * pqquorum::SIGNATURE_RECORD_SIZE);
    for (const auto field : {size_t(1), pqquorum::HEADER_SIZE - 2, pqquorum::HEADER_SIZE}) {
        auto bad = bytes;
        bad[field] = 255;
        if (field != 1) bad[field + 1] = 255;
        pqquorum::Certificate decoded = cert;
        BOOST_CHECK(!pqquorum::Decode(bad, decoded));
        BOOST_CHECK(decoded.signatures.empty());
        BOOST_CHECK(decoded.statement.genesis.IsNull());
    }
    auto duplicate = bytes;
    const size_t second = pqquorum::HEADER_SIZE + pqquorum::SIGNATURE_RECORD_SIZE;
    duplicate[second] = duplicate[second + 1] = 0;
    pqquorum::Certificate decoded;
    BOOST_CHECK(!pqquorum::Decode(duplicate, decoded));
    cert.signatures.resize(pqquorum::MAX_MEMBERS);
    for (size_t i = 0; i < cert.signatures.size(); ++i) cert.signatures[i].member = i;
    bytes = pqquorum::Encode(cert);
    BOOST_REQUIRE_EQUAL(bytes.size(), pqquorum::MAX_CERTIFICATE_SIZE);
    BOOST_REQUIRE(pqquorum::Decode(bytes, decoded));
    BOOST_CHECK_EQUAL(decoded.signatures.size(), pqquorum::MAX_MEMBERS);
    BOOST_CHECK(pqquorum::Encode(decoded) == bytes);
}

BOOST_AUTO_TEST_CASE(rejects_other_signature_context_and_every_header_mutation)
{
    auto cert = certificate();
    std::string reason;
    const auto bytes = pqquorum::Encode(cert);
    for (size_t i = 0; i < pqquorum::HEADER_SIZE; ++i) {
        auto changed = bytes;
        changed[i] ^= 1;
        pqquorum::Certificate decoded;
        if (pqquorum::Decode(changed, decoded))
            BOOST_CHECK(!pqquorum::Verify(decoded, statement, members, reason));
    }
    const std::vector<unsigned char> wrongContext{'o', 't', 'h', 'e', 'r'};
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(keys[0].Sign(pqquorum::Message(statement), wrongContext, signature));
    std::copy(signature.begin(), signature.end(), cert.signatures[0].bytes.begin());
    BOOST_CHECK(!pqquorum::Verify(cert, statement, members, reason));
    BOOST_REQUIRE(pqquorum::Verify(certificate(), statement, members, reason));
    BOOST_CHECK(reason.empty());
}

BOOST_AUTO_TEST_CASE(decoded_signer_outside_snapshot_and_unowned_key_fail)
{
    auto cert = certificate();
    cert.signatures.back().member = members.size(); // Within wire limit, outside this snapshot.
    pqquorum::Certificate decoded;
    BOOST_REQUIRE(pqquorum::Decode(pqquorum::Encode(cert), decoded));
    std::string reason;
    BOOST_CHECK(!pqquorum::Verify(decoded, statement, members, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pq-quorum-member");

    members[0].operator_key.fill(1); // Hashing a key is not proof of possession.
    statement.committee = pqquorum::Commitment(members);
    BOOST_REQUIRE(!statement.committee.IsNull());
    cert = certificate(); // Signs the new statement with the old, non-matching key.
    BOOST_CHECK(!pqquorum::Verify(cert, statement, members, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pq-quorum-signature");
    members[0].operator_key.fill(0);
    BOOST_CHECK(pqquorum::Commitment(members).IsNull());
}

BOOST_AUTO_TEST_CASE(real_maximum_committee_threshold_and_full_certificate)
{
    std::vector<mldsa44::Key> largeKeys(pqquorum::MAX_MEMBERS);
    members.clear();
    for (size_t i = 0; i < largeKeys.size(); ++i) {
        std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
        seed[0] = i & 255;
        seed[1] = i >> 8;
        seed[2] = 99; // Public deterministic test material only.
        BOOST_REQUIRE(largeKeys[i].SetSeed(seed));
        members.push_back({uint256S(std::to_string(i + 1)), largeKeys[i].GetPublicKey()});
    }
    statement.committee = pqquorum::Commitment(members);
    BOOST_REQUIRE(!statement.committee.IsNull());
    pqquorum::Certificate cert;
    cert.statement = statement;
    for (size_t i = 0; i < largeKeys.size(); ++i) {
        pqquorum::Signature vote;
        vote.member = i;
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(largeKeys[i].Sign(pqquorum::Message(statement), pqquorum::Context(), signature));
        std::copy(signature.begin(), signature.end(), vote.bytes.begin());
        cert.signatures.push_back(vote);
    }
    const auto bytes = pqquorum::Encode(cert);
    BOOST_REQUIRE_EQUAL(bytes.size(), pqquorum::MAX_CERTIFICATE_SIZE);
    pqquorum::Certificate decoded;
    BOOST_REQUIRE(pqquorum::Decode(bytes, decoded));
    std::string reason;
    const auto start = std::chrono::steady_clock::now();
    BOOST_REQUIRE(pqquorum::Verify(decoded, statement, members, reason));
    BOOST_TEST_MESSAGE("PQ maximum certificate: 400 ML-DSA verifications in " <<
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() << " us");
    decoded.signatures.resize(pqquorum::Threshold(members.size()));
    BOOST_CHECK(pqquorum::Verify(decoded, statement, members, reason));
    decoded.signatures.pop_back();
    BOOST_CHECK(!pqquorum::Verify(decoded, statement, members, reason));
}

BOOST_AUTO_TEST_CASE(statement_has_fixed_little_endian_wire_layout)
{
    std::vector<unsigned char> expected(138, 0);
    expected[0] = 1; // Version.
    expected[1] = 2; // PRECOMMIT.
    expected[2] = 0x11; // Genesis (32 bytes).
    expected[34] = 0x22; // Anchor (32 bytes).
    statement.committee = uint256S("55");
    expected[66] = 0x55; // Committee (32 bytes).
    statement.height = 0x01020304;
    expected[98] = 4; expected[99] = 3; expected[100] = 2; expected[101] = 1;
    expected[102] = 7; // Round (4 bytes).
    expected[106] = 0x33; // Value (32 bytes).
    BOOST_CHECK(pqquorum::Message(statement) == expected);
    BOOST_CHECK_EQUAL(pqquorum::HEADER_SIZE, expected.size() + 2);
}

BOOST_AUTO_TEST_CASE(null_input_is_rejected_without_reading)
{
    auto decoded = certificate();
    const Span<const unsigned char> invalid(static_cast<const unsigned char*>(nullptr), pqquorum::HEADER_SIZE);
    BOOST_CHECK(!pqquorum::Decode(invalid, decoded));
    BOOST_CHECK(decoded.signatures.empty());
}

BOOST_AUTO_TEST_SUITE_END()
