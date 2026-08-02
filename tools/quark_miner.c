/* OrganicLife Coin genesis miner.
 *
 * Replicates this codebase's exact genesis GetHash(): CBlockHeader::GetHash()
 * uses HashQuark() over the canonical 80-byte header when nVersion < 4
 * (src/primitives/block.cpp, src/hash.h). Uses the repo's own sphlib sources
 * so the result is byte-identical to what the node computes.
 *
 * Usage:
 *   quark_miner verify <merkle_hex> <ntime> <nbits_hex> <nonce> <expected_hash_hex>
 *   quark_miner mine   <merkle_hex> <ntime> <nbits_hex> [start_nonce]
 *
 * merkle_hex / expected_hash_hex are in display (reversed) order, as printed
 * by uint256.ToString().
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto/sph_blake.h"
#include "crypto/sph_bmw.h"
#include "crypto/sph_groestl.h"
#include "crypto/sph_jh.h"
#include "crypto/sph_keccak.h"
#include "crypto/sph_skein.h"

/* HashQuark from src/hash.h (conditional chain, mask = 8 on byte 0). */
static void hash_quark(const unsigned char* in, size_t len, unsigned char out32[32])
{
    sph_blake512_context ctx_blake;
    sph_bmw512_context ctx_bmw;
    sph_groestl512_context ctx_groestl;
    sph_jh512_context ctx_jh;
    sph_keccak512_context ctx_keccak;
    sph_skein512_context ctx_skein;
    unsigned char hash[9][64];

    sph_blake512_init(&ctx_blake);
    sph_blake512(&ctx_blake, in, len);
    sph_blake512_close(&ctx_blake, hash[0]);

    sph_bmw512_init(&ctx_bmw);
    sph_bmw512(&ctx_bmw, hash[0], 64);
    sph_bmw512_close(&ctx_bmw, hash[1]);

    if (hash[1][0] & 0x08) {
        sph_groestl512_init(&ctx_groestl);
        sph_groestl512(&ctx_groestl, hash[1], 64);
        sph_groestl512_close(&ctx_groestl, hash[2]);
    } else {
        sph_skein512_init(&ctx_skein);
        sph_skein512(&ctx_skein, hash[1], 64);
        sph_skein512_close(&ctx_skein, hash[2]);
    }

    sph_groestl512_init(&ctx_groestl);
    sph_groestl512(&ctx_groestl, hash[2], 64);
    sph_groestl512_close(&ctx_groestl, hash[3]);

    sph_jh512_init(&ctx_jh);
    sph_jh512(&ctx_jh, hash[3], 64);
    sph_jh512_close(&ctx_jh, hash[4]);

    if (hash[4][0] & 0x08) {
        sph_blake512_init(&ctx_blake);
        sph_blake512(&ctx_blake, hash[4], 64);
        sph_blake512_close(&ctx_blake, hash[5]);
    } else {
        sph_bmw512_init(&ctx_bmw);
        sph_bmw512(&ctx_bmw, hash[4], 64);
        sph_bmw512_close(&ctx_bmw, hash[5]);
    }

    sph_keccak512_init(&ctx_keccak);
    sph_keccak512(&ctx_keccak, hash[5], 64);
    sph_keccak512_close(&ctx_keccak, hash[6]);

    sph_skein512_init(&ctx_skein);
    sph_skein512(&ctx_skein, hash[6], 64);
    sph_skein512_close(&ctx_skein, hash[7]);

    if (hash[7][0] & 0x08) {
        sph_keccak512_init(&ctx_keccak);
        sph_keccak512(&ctx_keccak, hash[7], 64);
        sph_keccak512_close(&ctx_keccak, hash[8]);
    } else {
        sph_jh512_init(&ctx_jh);
        sph_jh512(&ctx_jh, hash[7], 64);
        sph_jh512_close(&ctx_jh, hash[8]);
    }

    memcpy(out32, hash[8], 32); /* trim256: low 32 bytes */
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse display-order hex (as ToString prints) into internal LE bytes. */
static void parse_hash_reversed(const char* hex, unsigned char out[32])
{
    unsigned char tmp[32];
    size_t len = strlen(hex);
    size_t i;
    if (len != 64) { fprintf(stderr, "hash hex must be 64 chars\n"); exit(1); }
    for (i = 0; i < 32; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) { fprintf(stderr, "bad hex\n"); exit(1); }
        tmp[i] = (unsigned char)((hi << 4) | lo);
    }
    for (i = 0; i < 32; i++) out[i] = tmp[31 - i];
}

static void print_hash_display(const unsigned char internal[32])
{
    int i;
    for (i = 31; i >= 0; i--) printf("%02x", internal[i]);
}

static void put_le32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* uint256::SetCompact: nbits -> 32-byte LE target. */
static void bits_to_target(uint32_t nbits, unsigned char target[32])
{
    unsigned int nsize = nbits >> 24;
    uint32_t nword = nbits & 0x007fffff;
    memset(target, 0, 32);
    if (nsize <= 3) {
        nword >>= 8 * (3 - nsize);
        put_le32(target, nword);
    } else {
        put_le32(target, nword);
        /* shift left by 8*(nsize-3): move 4 bytes up */
        unsigned int shift = nsize - 3; /* in bytes */
        int i;
        for (i = 3; i >= 0; i--) {
            if ((unsigned int)i + shift < 32)
                target[i + shift] = target[i];
        }
        for (i = 0; i < 4; i++) {
            if ((unsigned int)i < shift || (unsigned int)i + shift >= 32)
                target[i] = 0;
        }
    }
}

/* hash (LE) <= target (LE) ? */
static int hash_meets_target(const unsigned char hash[32], const unsigned char target[32])
{
    int i;
    for (i = 31; i >= 0; i--) {
        if (hash[i] < target[i]) return 1;
        if (hash[i] > target[i]) return 0;
    }
    return 1;
}

static uint32_t parse_u32(const char* s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (uint32_t)strtoul(s, NULL, 16);
    return (uint32_t)strtoul(s, NULL, 10);
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s verify|mine <merkle_hex> <ntime> <nbits> [nonce|start_nonce] [expected]\n", argv[0]);
        return 1;
    }
    const char* mode = argv[1];
    unsigned char merkle[32];
    parse_hash_reversed(argv[2], merkle);
    uint32_t ntime = parse_u32(argv[3]);
    uint32_t nbits = parse_u32(argv[4]);

    unsigned char header[80];
    memset(header, 0, sizeof(header));
    put_le32(header + 0, 1); /* nVersion = 1 */
    /* hashPrevBlock = null (32 zero bytes) */
    memcpy(header + 36, merkle, 32);
    put_le32(header + 68, ntime);
    put_le32(header + 72, nbits);

    unsigned char target[32];
    bits_to_target(nbits, target);

    if (strcmp(mode, "verify") == 0) {
        if (argc < 7) { fprintf(stderr, "verify needs <nonce> <expected_hash_hex>\n"); return 1; }
        uint32_t nonce = parse_u32(argv[5]);
        unsigned char expected[32], got[32];
        parse_hash_reversed(argv[6], expected);
        put_le32(header + 76, nonce);
        hash_quark(header, 80, got);
        printf("computed: ");
        print_hash_display(got);
        printf("\nexpected: %s\n", argv[6]);
        if (memcmp(got, expected, 32) == 0) {
            printf("MATCH\n");
            return 0;
        }
        printf("MISMATCH\n");
        return 2;
    }

    if (strcmp(mode, "mine") == 0) {
        uint32_t nonce = argc > 5 ? parse_u32(argv[5]) : 0;
        unsigned char got[32];
        for (;; nonce++) {
            put_le32(header + 76, nonce);
            hash_quark(header, 80, got);
            if (hash_meets_target(got, target)) {
                printf("nonce=%u\nhash=", nonce);
                print_hash_display(got);
                printf("\n");
                return 0;
            }
            if (nonce == 0xFFFFFFFF) {
                fprintf(stderr, "nonce space exhausted\n");
                return 3;
            }
        }
    }

    fprintf(stderr, "unknown mode\n");
    return 1;
}
