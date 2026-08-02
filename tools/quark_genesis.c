// OrganicLife Coin genesis miner — replicates src/hash.h HashQuark exactly.
// Usage:
//   quark_genesis verify <merkle_hex> <time> <bits_hex> <nonce>   -> print block hash
//   quark_genesis mine   <merkle_hex> <time> <bits_hex> [start_nonce] -> find valid nonce
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sph_blake.h"
#include "sph_bmw.h"
#include "sph_groestl.h"
#include "sph_jh.h"
#include "sph_keccak.h"
#include "sph_skein.h"

typedef unsigned char u8;
typedef uint32_t u32;
typedef uint64_t u64;

// hash[9] stored as flat 64-byte buffers; mask check = (byte0 & 0x08)
static void hash_quark(const u8 *in, size_t len, u8 out[32])
{
    u8 h[9][64];
    sph_blake512_context ctx_blake;
    sph_bmw512_context ctx_bmw;
    sph_groestl512_context ctx_groestl;
    sph_jh512_context ctx_jh;
    sph_keccak512_context ctx_keccak;
    sph_skein512_context ctx_skein;

    sph_blake512_init(&ctx_blake);
    sph_blake512(&ctx_blake, in, len);
    sph_blake512_close(&ctx_blake, h[0]);

    sph_bmw512_init(&ctx_bmw);
    sph_bmw512(&ctx_bmw, h[0], 64);
    sph_bmw512_close(&ctx_bmw, h[1]);

    if (h[1][0] & 0x08) {
        sph_groestl512_init(&ctx_groestl);
        sph_groestl512(&ctx_groestl, h[1], 64);
        sph_groestl512_close(&ctx_groestl, h[2]);
    } else {
        sph_skein512_init(&ctx_skein);
        sph_skein512(&ctx_skein, h[1], 64);
        sph_skein512_close(&ctx_skein, h[2]);
    }

    sph_groestl512_init(&ctx_groestl);
    sph_groestl512(&ctx_groestl, h[2], 64);
    sph_groestl512_close(&ctx_groestl, h[3]);

    sph_jh512_init(&ctx_jh);
    sph_jh512(&ctx_jh, h[3], 64);
    sph_jh512_close(&ctx_jh, h[4]);

    if (h[4][0] & 0x08) {
        sph_blake512_init(&ctx_blake);
        sph_blake512(&ctx_blake, h[4], 64);
        sph_blake512_close(&ctx_blake, h[5]);
    } else {
        sph_bmw512_init(&ctx_bmw);
        sph_bmw512(&ctx_bmw, h[4], 64);
        sph_bmw512_close(&ctx_bmw, h[5]);
    }

    sph_keccak512_init(&ctx_keccak);
    sph_keccak512(&ctx_keccak, h[5], 64);
    sph_keccak512_close(&ctx_keccak, h[6]);

    sph_skein512_init(&ctx_skein);
    sph_skein512(&ctx_skein, h[6], 64);
    sph_skein512_close(&ctx_skein, h[7]);

    if (h[7][0] & 0x08) {
        sph_keccak512_init(&ctx_keccak);
        sph_keccak512(&ctx_keccak, h[7], 64);
        sph_keccak512_close(&ctx_keccak, h[8]);
    } else {
        sph_jh512_init(&ctx_jh);
        sph_jh512(&ctx_jh, h[7], 64);
        sph_jh512_close(&ctx_jh, h[8]);
    }
    memcpy(out, h[8], 32); // trim256
}

static void hex_to_bytes(const char *hex, u8 *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        sscanf(hex + 2 * i, "%2hhx", &out[i]);
}

static void print_hex_rev(const u8 *b, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", b[n - 1 - i]);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s verify|mine <merkle_hex> <time> <bits_hex> [nonce|start]\n", argv[0]);
        return 1;
    }
    const char *mode = argv[1];
    u8 merkle_be[32], merkle[32];
    hex_to_bytes(argv[2], merkle_be, 32);
    for (int i = 0; i < 32; i++) merkle[i] = merkle_be[31 - i]; // internal LE
    u32 ntime = (u32)strtoul(argv[3], NULL, 0);
    u32 nbits = (u32)strtoul(argv[4], NULL, 0);

    // target from compact bits
    u64 mantissa = nbits & 0xFFFFFF;
    int shift = 8 * ((nbits >> 24) - 3);
    // compare as 256-bit: build target bytes (LE)
    u8 target[32] = {0};
    {
        u8 m[3] = {(u8)(mantissa & 0xFF), (u8)((mantissa >> 8) & 0xFF), (u8)((mantissa >> 16) & 0xFF)};
        int byteoff = shift / 8;
        for (int i = 0; i < 3 && byteoff + i < 32; i++) target[byteoff + i] = m[i];
    }

    u8 header[80];
    memset(header, 0, 80);
    header[0] = 1; // nVersion = 1 (int32 LE)
    memcpy(header + 4 + 32, merkle, 32);
    memcpy(header + 68, &ntime, 4);
    memcpy(header + 72, &nbits, 4);

    if (strcmp(mode, "verify") == 0) {
        u32 nonce = (u32)strtoul(argv[5], NULL, 0);
        memcpy(header + 76, &nonce, 4);
        u8 out[32];
        hash_quark(header, 80, out);
        print_hex_rev(out, 32);
        printf("\n");
        return 0;
    }

    u64 nonce = argc > 5 ? strtoull(argv[5], NULL, 0) : 0;
    u8 out[32];
    for (;; nonce++) {
        u32 n32 = (u32)nonce;
        memcpy(header + 76, &n32, 4);
        hash_quark(header, 80, out);
        int le = 1;
        for (int i = 31; i >= 0; i--) {   // 256-bit compare, MSB first
            if (out[i] != target[i]) { le = out[i] < target[i]; break; }
        }
        if (le) {
            printf("FOUND nonce=%u hash=", n32);
            print_hex_rev(out, 32);
            printf("\n");
            return 0;
        }
        if (nonce == 0xFFFFFFFFULL) {
            fprintf(stderr, "nonce space exhausted\n");
            return 2;
        }
    }
}
