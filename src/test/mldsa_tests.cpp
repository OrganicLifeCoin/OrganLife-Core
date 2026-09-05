// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifdef MLDSA_STANDALONE_TEST
#define BOOST_TEST_MODULE OrganicLifeMLDSA
#include <boost/test/included/unit_test.hpp>
#else
#include <boost/test/unit_test.hpp>
#endif

#include <crypto/mldsa44.h>
#include <test/data/mldsa44_vectors.h>
#include <boost/algorithm/hex.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <type_traits>

namespace {
std::vector<unsigned char> Bytes(const char* hex)
{
    std::vector<unsigned char> result;
    boost::algorithm::unhex(std::string(hex), std::back_inserter(result));
    return result;
}
const std::vector<unsigned char> MESSAGE{0, 1, 2, 3, 255};
const std::vector<unsigned char> CONTEXT{'O', 'L', 'C', '-', 't', 'e', 's', 't'};
const Span<const unsigned char> EMPTY{};
static_assert(!std::is_copy_constructible<mldsa44::Key>::value, "Secret keys must not be copied");
static_assert(!std::is_copy_assignable<mldsa44::Key>::value, "Secret keys must not be copied");
} // namespace

BOOST_AUTO_TEST_SUITE(mldsa_tests)

BOOST_AUTO_TEST_CASE(nist_key_generation)
{
    mldsa44::Key key;
    const auto seed = Bytes(mldsa44_vectors::KEYGEN_SEED);
    BOOST_REQUIRE(key.SetSeed(seed));
    BOOST_CHECK(key.IsValid());
    const auto expected = Bytes(mldsa44_vectors::KEYGEN_PUBLIC_KEY);
    BOOST_CHECK_EQUAL_COLLECTIONS(key.GetPublicKey().begin(), key.GetPublicKey().end(),
                                 expected.begin(), expected.end());
    mldsa44::Key restored;
    BOOST_REQUIRE(restored.SetSeed(seed));
    BOOST_CHECK(key.GetPublicKey() == restored.GetPublicKey());
}

BOOST_AUTO_TEST_CASE(nist_signature_verification)
{
    BOOST_CHECK(mldsa44::Verify(Bytes(mldsa44_vectors::VALID_PUBLIC_KEY),
                              Bytes(mldsa44_vectors::VALID_MESSAGE),
                              Bytes(mldsa44_vectors::VALID_CONTEXT),
                              Bytes(mldsa44_vectors::VALID_SIGNATURE)));
    BOOST_CHECK(!mldsa44::Verify(Bytes(mldsa44_vectors::INVALID_PUBLIC_KEY),
                               Bytes(mldsa44_vectors::INVALID_MESSAGE),
                               Bytes(mldsa44_vectors::INVALID_CONTEXT),
                               Bytes(mldsa44_vectors::INVALID_SIGNATURE)));
}

BOOST_AUTO_TEST_CASE(random_keys_and_signatures)
{
    mldsa44::Key first, second;
    BOOST_REQUIRE(first.Generate());
    BOOST_REQUIRE(second.Generate());
    BOOST_CHECK(first.GetPublicKey() != second.GetPublicKey());
    std::vector<unsigned char> sig1, sig2;
    BOOST_REQUIRE(first.Sign(MESSAGE, CONTEXT, sig1));
    BOOST_REQUIRE(first.Sign(MESSAGE, CONTEXT, sig2));
    BOOST_CHECK_EQUAL(sig1.size(), mldsa44::SIGNATURE_SIZE);
    BOOST_CHECK(sig1 != sig2);
    BOOST_CHECK(mldsa44::Verify(first.GetPublicKey(), MESSAGE, CONTEXT, sig1));
    BOOST_CHECK(mldsa44::Verify(first.GetPublicKey(), MESSAGE, CONTEXT, sig2));
    BOOST_CHECK(!mldsa44::Verify(second.GetPublicKey(), MESSAGE, CONTEXT, sig1));
}

BOOST_AUTO_TEST_CASE(invalid_and_cleared_keys)
{
    mldsa44::Key key;
    std::vector<unsigned char> signature(10, 0xff);
    BOOST_CHECK(!key.IsValid());
    BOOST_CHECK(!key.Sign(MESSAGE, CONTEXT, signature));
    BOOST_CHECK(signature.empty());
    BOOST_REQUIRE(key.Generate());
    key.Clear();
    BOOST_CHECK(!key.IsValid());
    BOOST_CHECK(std::all_of(key.GetPublicKey().begin(), key.GetPublicKey().end(),
                            [](unsigned char b) { return b == 0; }));
    signature.assign(10, 0xff);
    BOOST_CHECK(!key.Sign(MESSAGE, CONTEXT, signature));
    BOOST_CHECK(signature.empty());
    key.Clear();
    BOOST_REQUIRE(key.Generate());
}

BOOST_AUTO_TEST_CASE(signature_output_can_alias_inputs)
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.Generate());
    auto signature = MESSAGE;
    BOOST_REQUIRE(key.Sign(signature, CONTEXT, signature));
    BOOST_CHECK(mldsa44::Verify(key.GetPublicKey(), MESSAGE, CONTEXT, signature));
    signature = CONTEXT;
    BOOST_REQUIRE(key.Sign(MESSAGE, signature, signature));
    BOOST_CHECK(mldsa44::Verify(key.GetPublicKey(), MESSAGE, CONTEXT, signature));
}

BOOST_AUTO_TEST_CASE(seed_sizes_and_rekey)
{
    mldsa44::Key key;
    for (size_t size : {size_t{0}, size_t{31}, size_t{33}}) {
        BOOST_REQUIRE(key.Generate());
        BOOST_CHECK(!key.SetSeed(std::vector<unsigned char>(size)));
        BOOST_CHECK(!key.IsValid());
    }
    auto seed = Bytes(mldsa44_vectors::KEYGEN_SEED);
    BOOST_REQUIRE(key.SetSeed(seed));
    const auto previous = key.GetPublicKey();
    seed[0] ^= 1;
    BOOST_REQUIRE(key.SetSeed(seed));
    BOOST_CHECK(previous != key.GetPublicKey());
}

BOOST_AUTO_TEST_CASE(message_context_key_and_signature_tampering)
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.Generate());
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(MESSAGE, CONTEXT, signature));
    auto message = MESSAGE;
    message[0] ^= 1;
    BOOST_CHECK(!mldsa44::Verify(key.GetPublicKey(), message, CONTEXT, signature));
    auto context = CONTEXT;
    context[0] ^= 1;
    BOOST_CHECK(!mldsa44::Verify(key.GetPublicKey(), MESSAGE, context, signature));
    auto public_key = key.GetPublicKey();
    public_key[0] ^= 1;
    BOOST_CHECK(!mldsa44::Verify(public_key, MESSAGE, CONTEXT, signature));
    for (size_t i : {size_t{0}, size_t{1000}, signature.size() - 1}) {
        auto changed = signature;
        changed[i] ^= 1;
        BOOST_CHECK(!mldsa44::Verify(key.GetPublicKey(), MESSAGE, CONTEXT, changed));
    }
}

BOOST_AUTO_TEST_CASE(empty_message_and_context_bounds)
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.Generate());
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(EMPTY, EMPTY, signature));
    BOOST_CHECK(mldsa44::Verify(key.GetPublicKey(), EMPTY, EMPTY, signature));
    std::vector<unsigned char> context(255, 7);
    BOOST_REQUIRE(key.Sign(EMPTY, context, signature));
    BOOST_CHECK(mldsa44::Verify(key.GetPublicKey(), EMPTY, context, signature));
    context.push_back(7);
    BOOST_CHECK(!mldsa44::Verify(key.GetPublicKey(), EMPTY, context, signature));
    BOOST_CHECK(!key.Sign(EMPTY, context, signature));
    BOOST_CHECK(signature.empty());
}

BOOST_AUTO_TEST_CASE(malformed_lengths)
{
    const auto pk = Bytes(mldsa44_vectors::VALID_PUBLIC_KEY);
    const auto msg = Bytes(mldsa44_vectors::VALID_MESSAGE);
    const auto ctx = Bytes(mldsa44_vectors::VALID_CONTEXT);
    const auto sig = Bytes(mldsa44_vectors::VALID_SIGNATURE);
    for (size_t n : {size_t{0}, size_t{1}, pk.size() - 1, pk.size() + 1}) {
        auto changed = pk;
        changed.resize(n);
        BOOST_CHECK(!mldsa44::Verify(changed, msg, ctx, sig));
    }
    for (size_t n : {size_t{0}, size_t{1}, sig.size() - 1, sig.size() + 1}) {
        auto changed = sig;
        changed.resize(n);
        BOOST_CHECK(!mldsa44::Verify(pk, msg, ctx, changed));
    }
}

BOOST_AUTO_TEST_CASE(bounded_mutation_corpus)
{
    auto pk = Bytes(mldsa44_vectors::VALID_PUBLIC_KEY);
    const auto msg = Bytes(mldsa44_vectors::VALID_MESSAGE);
    const auto ctx = Bytes(mldsa44_vectors::VALID_CONTEXT);
    const auto sig = Bytes(mldsa44_vectors::VALID_SIGNATURE);
    for (size_t i = 0; i < 256; ++i) {
        auto changed = sig;
        changed[(i * 31) % changed.size()] ^= static_cast<unsigned char>(1 + i % 255);
        BOOST_CHECK(!mldsa44::Verify(pk, msg, ctx, changed));
    }
    BOOST_CHECK(!mldsa44::Verify(std::vector<unsigned char>(pk.size(), 0xff), msg, ctx,
                               std::vector<unsigned char>(sig.size(), 0xff)));
}

BOOST_AUTO_TEST_CASE(repeated_operations)
{
    using Clock = std::chrono::steady_clock;
    std::chrono::nanoseconds key_time{0}, sign_time{0}, verify_time{0};
    const int count = 32;
    for (int i = 0; i < count; ++i) {
        mldsa44::Key key;
        auto start = Clock::now();
        BOOST_REQUIRE(key.Generate());
        key_time += Clock::now() - start;
        std::vector<unsigned char> signature;
        start = Clock::now();
        BOOST_REQUIRE(key.Sign(MESSAGE, CONTEXT, signature));
        sign_time += Clock::now() - start;
        start = Clock::now();
        BOOST_REQUIRE(mldsa44::Verify(key.GetPublicKey(), MESSAGE, CONTEXT, signature));
        verify_time += Clock::now() - start;
    }
    BOOST_TEST_MESSAGE("ML-DSA-44 mean microseconds (32 iterations): keygen="
                       << key_time.count() / (1000.0 * count)
                       << " sign=" << sign_time.count() / (1000.0 * count)
                       << " verify=" << verify_time.count() / (1000.0 * count));
}

BOOST_AUTO_TEST_SUITE_END()
