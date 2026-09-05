// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ORGANICLIFE_CRYPTO_MLDSA44_H
#define ORGANICLIFE_CRYPTO_MLDSA44_H

#include <span.h>
#include <support/allocators/secure.h>

#include <array>
#include <vector>

namespace mldsa44 {
constexpr size_t SEED_SIZE = 32;
constexpr size_t PUBLIC_KEY_SIZE = 1312;
constexpr size_t SECRET_KEY_SIZE = 2560;
constexpr size_t SIGNATURE_SIZE = 2420;
constexpr size_t MAX_CONTEXT_SIZE = 255;
using PublicKey = std::array<unsigned char, PUBLIC_KEY_SIZE>;

// Experimental cryptography only: no wallet or consensus integration.
class Key
{
    std::vector<unsigned char, secure_allocator<unsigned char>> m_secret;
    PublicKey m_public{};

public:
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    ~Key() = default;

    bool Generate();
    // The caller owns and must securely erase the supplied seed.
    bool SetSeed(Span<const unsigned char> seed);
    bool IsValid() const;
    void Clear();
    const PublicKey& GetPublicKey() const { return m_public; }
    // Standard randomized ML-DSA with a FIPS 204 context, not a transaction format.
    // A false return clears signature. Allocation exceptions propagate.
    bool Sign(Span<const unsigned char> message, Span<const unsigned char> context,
              std::vector<unsigned char>& signature) const;
};

bool Verify(Span<const unsigned char> public_key, Span<const unsigned char> message,
            Span<const unsigned char> context, Span<const unsigned char> signature);
} // namespace mldsa44

#endif // ORGANICLIFE_CRYPTO_MLDSA44_H
