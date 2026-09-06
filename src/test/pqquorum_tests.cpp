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

BOOST_AUTO_TEST_CASE(strict_quorum_and_roundtrip)
{
    BOOST_CHECK_EQUAL(pqquorum::Threshold(4), 3U);
    BOOST_CHECK_EQUAL(pqquorum::Threshold(3), 3U);
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
