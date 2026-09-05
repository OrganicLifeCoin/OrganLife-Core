// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/mldsa44.h>

#define MLD_CONFIG_FILE "crypto/mldsa44_config.h"
#include <crypto/mldsa-native/mldsa_native.h>

namespace mldsa44 {
static_assert(SEED_SIZE == MLDSA_SEEDBYTES, "ML-DSA seed size mismatch");
static_assert(PUBLIC_KEY_SIZE == MLDSA_PUBLICKEYBYTES(44), "ML-DSA public key size mismatch");
static_assert(SECRET_KEY_SIZE == MLDSA_SECRETKEYBYTES(44), "ML-DSA secret key size mismatch");
static_assert(SIGNATURE_SIZE == MLDSA_BYTES(44), "ML-DSA signature size mismatch");

namespace {
const unsigned char* Data(Span<const unsigned char> bytes)
{
    static const unsigned char empty = 0;
    return bytes.size() == 0 ? &empty : bytes.data();
}
} // namespace

bool Key::Generate()
{
    Clear();
    m_secret.resize(SECRET_KEY_SIZE);
    if (olc_mldsa44_keypair(m_public.data(), m_secret.data()) != 0) {
        Clear();
        return false;
    }
    return true;
}

bool Key::SetSeed(Span<const unsigned char> seed)
{
    Clear();
    if (seed.size() != SEED_SIZE) return false;
    m_secret.resize(SECRET_KEY_SIZE);
    if (olc_mldsa44_keypair_internal(m_public.data(), m_secret.data(), seed.data()) != 0) {
        Clear();
        return false;
    }
    return true;
}

bool Key::IsValid() const
{
    return m_secret.size() == SECRET_KEY_SIZE;
}

void Key::Clear()
{
    if (!m_secret.empty()) memory_cleanse(m_secret.data(), m_secret.size());
    m_secret.clear();
    m_public.fill(0);
}

bool Key::Sign(Span<const unsigned char> message, Span<const unsigned char> context,
               std::vector<unsigned char>& signature) const
{
    if (!IsValid() || context.size() > MAX_CONTEXT_SIZE) {
        signature.clear();
        return false;
    }
    // Keep inputs valid even when the caller reuses the output vector as input.
    std::array<unsigned char, SIGNATURE_SIZE> result{};
    size_t size = 0;
    if (olc_mldsa44_signature(result.data(), &size, Data(message), message.size(),
                              Data(context), context.size(), m_secret.data()) != 0 ||
        size != SIGNATURE_SIZE) {
        signature.clear();
        return false;
    }
    signature.assign(result.begin(), result.end());
    return true;
}

bool Verify(Span<const unsigned char> public_key, Span<const unsigned char> message,
            Span<const unsigned char> context, Span<const unsigned char> signature)
{
    if (public_key.size() != PUBLIC_KEY_SIZE || signature.size() != SIGNATURE_SIZE ||
        context.size() > MAX_CONTEXT_SIZE) return false;
    return olc_mldsa44_verify(signature.data(), signature.size(), Data(message), message.size(),
                              Data(context), context.size(), public_key.data()) == 0;
}
} // namespace mldsa44
