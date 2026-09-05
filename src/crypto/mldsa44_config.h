// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ORGANICLIFE_MLDSA44_CONFIG_H
#define ORGANICLIFE_MLDSA44_CONFIG_H

#define MLD_CONFIG_PARAMETER_SET 44
#define MLD_CONFIG_NAMESPACE_PREFIX olc_mldsa44
#define MLD_CONFIG_NO_SUPERCOP
// Portable C only; retain the upstream constant-time compiler barriers.
#define MLD_CONFIG_SERIAL_FIPS202_ONLY
#define MLD_CONFIG_CUSTOM_RANDOMBYTES
#define MLD_CONFIG_CUSTOM_ZEROIZE

#include <stddef.h>
#include <stdint.h>
#include <sodium.h>

static inline int mld_randombytes(uint8_t* out, size_t len)
{
    if (sodium_init() < 0) return -1;
    // libsodium aborts if OS entropy is unavailable; there is no fallback RNG.
    randombytes_buf(out, len);
    return 0;
}

static inline void mld_zeroize(void* ptr, size_t len)
{
    sodium_memzero(ptr, len);
}

#endif // ORGANICLIFE_MLDSA44_CONFIG_H
