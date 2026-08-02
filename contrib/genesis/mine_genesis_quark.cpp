// Copyright (c) 2026 The OrganicLife Coin developers
// Based on PIVX/Bitcoin Core components.
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <limits>
#include <string>
#include <vector>

#include "arith_uint256.h"
#include "crypto/sha256.h"
#include "uint256.h"

#include "crypto/sph_blake.h"
#include "crypto/sph_bmw.h"
#include "crypto/sph_groestl.h"
#include "crypto/sph_jh.h"
#include "crypto/sph_keccak.h"
#include "crypto/sph_skein.h"

static void WriteLE32(std::vector<unsigned char>& out, uint32_t v)
{
    out.push_back((v >> 0) & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
}

static void WriteLE64(std::vector<unsigned char>& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i) out.push_back((v >> (8 * i)) & 0xFF);
}

static void WriteVarInt(std::vector<unsigned char>& out, uint64_t v)
{
    if (v < 0xFD) {
        out.push_back(static_cast<unsigned char>(v));
    } else if (v <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(static_cast<unsigned char>(v & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    } else if (v <= 0xFFFFFFFFULL) {
        out.push_back(0xFE);
        WriteLE32(out, static_cast<uint32_t>(v));
    } else {
        out.push_back(0xFF);
        WriteLE64(out, v);
    }
}

static void ScriptPushData(std::vector<unsigned char>& script, const std::vector<unsigned char>& data)
{
    const size_t n = data.size();
    if (n < 76) {
        script.push_back(static_cast<unsigned char>(n));
    } else if (n <= 0xFF) {
        script.push_back(0x4c); // OP_PUSHDATA1
        script.push_back(static_cast<unsigned char>(n));
    } else if (n <= 0xFFFF) {
        script.push_back(0x4d); // OP_PUSHDATA2
        script.push_back(static_cast<unsigned char>(n & 0xFF));
        script.push_back(static_cast<unsigned char>((n >> 8) & 0xFF));
    } else {
        std::cerr << "PushData too large\n";
        std::exit(1);
    }
    script.insert(script.end(), data.begin(), data.end());
}

// Push a number using CScriptNum minimal encoding (always as a data push).
// This matches `CScript() << CScriptNum(v)` semantics.
static void ScriptPushNum(std::vector<unsigned char>& script, int64_t v)
{
    if (v == 0) {
        script.push_back(0x00); // OP_0 (empty vector push)
        return;
    }

    const bool neg = v < 0;
    uint64_t abs = neg ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);

    std::vector<unsigned char> data;
    while (abs) {
        data.push_back(abs & 0xFF);
        abs >>= 8;
    }
    if (data.back() & 0x80) {
        data.push_back(neg ? 0x80 : 0x00);
    } else if (neg) {
        data.back() |= 0x80;
    }
    ScriptPushData(script, data);
}

static std::vector<unsigned char> ParseHex(const std::string& hex)
{
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string s = hex;
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2);
    if (s.size() % 2 != 0) {
        std::cerr << "Invalid hex length\n";
        std::exit(1);
    }
    std::vector<unsigned char> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nibble(s[i]);
        const int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) {
            std::cerr << "Invalid hex\n";
            std::exit(1);
        }
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}

static std::string HexStrRev(const unsigned char* data, size_t len)
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        ss << std::setw(2) << static_cast<int>(data[len - 1 - i]);
    }
    return ss.str();
}

static std::array<unsigned char, 32> SHA256D(const unsigned char* data, size_t len)
{
    std::array<unsigned char, 32> first{};
    std::array<unsigned char, 32> second{};
    CSHA256().Write(data, len).Finalize(first.data());
    CSHA256().Write(first.data(), first.size()).Finalize(second.data());
    return second;
}

static uint256 HashQuark80(const std::array<unsigned char, 80>& header)
{
    sph_blake512_context ctx_blake;
    sph_bmw512_context ctx_bmw;
    sph_groestl512_context ctx_groestl;
    sph_jh512_context ctx_jh;
    sph_keccak512_context ctx_keccak;
    sph_skein512_context ctx_skein;

    arith_uint512 mask(8);
    arith_uint512 zero(0);
    arith_uint512 hash[9];

    sph_blake512_init(&ctx_blake);
    sph_blake512(&ctx_blake, header.data(), header.size());
    sph_blake512_close(&ctx_blake, static_cast<void*>(&hash[0]));

    sph_bmw512_init(&ctx_bmw);
    sph_bmw512(&ctx_bmw, static_cast<const void*>(&hash[0]), 64);
    sph_bmw512_close(&ctx_bmw, static_cast<void*>(&hash[1]));

    if ((hash[1] & mask) != zero) {
        sph_groestl512_init(&ctx_groestl);
        sph_groestl512(&ctx_groestl, static_cast<const void*>(&hash[1]), 64);
        sph_groestl512_close(&ctx_groestl, static_cast<void*>(&hash[2]));
    } else {
        sph_skein512_init(&ctx_skein);
        sph_skein512(&ctx_skein, static_cast<const void*>(&hash[1]), 64);
        sph_skein512_close(&ctx_skein, static_cast<void*>(&hash[2]));
    }

    sph_groestl512_init(&ctx_groestl);
    sph_groestl512(&ctx_groestl, static_cast<const void*>(&hash[2]), 64);
    sph_groestl512_close(&ctx_groestl, static_cast<void*>(&hash[3]));

    sph_jh512_init(&ctx_jh);
    sph_jh512(&ctx_jh, static_cast<const void*>(&hash[3]), 64);
    sph_jh512_close(&ctx_jh, static_cast<void*>(&hash[4]));

    if ((hash[4] & mask) != zero) {
        sph_blake512_init(&ctx_blake);
        sph_blake512(&ctx_blake, static_cast<const void*>(&hash[4]), 64);
        sph_blake512_close(&ctx_blake, static_cast<void*>(&hash[5]));
    } else {
        sph_bmw512_init(&ctx_bmw);
        sph_bmw512(&ctx_bmw, static_cast<const void*>(&hash[4]), 64);
        sph_bmw512_close(&ctx_bmw, static_cast<void*>(&hash[5]));
    }

    sph_keccak512_init(&ctx_keccak);
    sph_keccak512(&ctx_keccak, static_cast<const void*>(&hash[5]), 64);
    sph_keccak512_close(&ctx_keccak, static_cast<void*>(&hash[6]));

    sph_skein512_init(&ctx_skein);
    sph_skein512(&ctx_skein, static_cast<const void*>(&hash[6]), 64);
    sph_skein512_close(&ctx_skein, static_cast<void*>(&hash[7]));

    if ((hash[7] & mask) != zero) {
        sph_keccak512_init(&ctx_keccak);
        sph_keccak512(&ctx_keccak, static_cast<const void*>(&hash[7]), 64);
        sph_keccak512_close(&ctx_keccak, static_cast<void*>(&hash[8]));
    } else {
        sph_jh512_init(&ctx_jh);
        sph_jh512(&ctx_jh, static_cast<const void*>(&hash[7]), 64);
        sph_jh512_close(&ctx_jh, static_cast<void*>(&hash[8]));
    }

    return hash[8].trim256();
}

struct GenesisResult {
    uint32_t nTime;
    uint32_t nBits;
    int32_t nVersion;
    uint32_t nNonce;
    std::array<unsigned char, 32> merkle_root; // little-endian internal bytes
    std::array<unsigned char, 32> genesis_hash; // little-endian internal bytes
};

static GenesisResult MineGenesis(
    const std::string& timestamp,
    const std::vector<unsigned char>& pubkey,
    uint32_t nTime,
    uint32_t nBits,
    int32_t nVersion,
    uint32_t nonceStart,
    uint64_t genesisRewardSats)
{
    // Build coinbase scriptSig: push(486604799) push(4) push(timestamp)
    std::vector<unsigned char> scriptSig;
    ScriptPushNum(scriptSig, 486604799);
    ScriptPushNum(scriptSig, 4);
    ScriptPushData(scriptSig, std::vector<unsigned char>(timestamp.begin(), timestamp.end()));

    // Build scriptPubKey: push(pubkey) OP_CHECKSIG
    std::vector<unsigned char> scriptPubKey;
    ScriptPushData(scriptPubKey, pubkey);
    scriptPubKey.push_back(0xAC); // OP_CHECKSIG

    // Serialize coinbase tx (version 1)
    std::vector<unsigned char> tx;
    tx.reserve(256);
    WriteLE32(tx, 1);
    WriteVarInt(tx, 1); // vin
    tx.insert(tx.end(), 32, 0x00); // prevout hash
    WriteLE32(tx, 0xFFFFFFFFu);    // prevout n
    WriteVarInt(tx, scriptSig.size());
    tx.insert(tx.end(), scriptSig.begin(), scriptSig.end());
    WriteLE32(tx, 0xFFFFFFFFu); // sequence
    WriteVarInt(tx, 1);         // vout
    WriteLE64(tx, genesisRewardSats);
    WriteVarInt(tx, scriptPubKey.size());
    tx.insert(tx.end(), scriptPubKey.begin(), scriptPubKey.end());
    WriteLE32(tx, 0); // locktime

    const auto txid = SHA256D(tx.data(), tx.size());

    // Merkle root (single tx): txid
    std::array<unsigned char, 32> merkle = txid;

    // Header serialization (80 bytes)
    std::array<unsigned char, 80> header{};
    // version
    header[0] = (nVersion >> 0) & 0xFF;
    header[1] = (nVersion >> 8) & 0xFF;
    header[2] = (nVersion >> 16) & 0xFF;
    header[3] = (nVersion >> 24) & 0xFF;
    // prev block (32 bytes zeros) already
    // merkle root (little-endian internal bytes)
    std::memcpy(header.data() + 36, merkle.data(), 32);
    // time, bits, nonce
    header[68] = (nTime >> 0) & 0xFF;
    header[69] = (nTime >> 8) & 0xFF;
    header[70] = (nTime >> 16) & 0xFF;
    header[71] = (nTime >> 24) & 0xFF;
    header[72] = (nBits >> 0) & 0xFF;
    header[73] = (nBits >> 8) & 0xFF;
    header[74] = (nBits >> 16) & 0xFF;
    header[75] = (nBits >> 24) & 0xFF;

    arith_uint256 target;
    target.SetCompact(nBits);

    uint32_t nonce = nonceStart;
    for (;;) {
        header[76] = (nonce >> 0) & 0xFF;
        header[77] = (nonce >> 8) & 0xFF;
        header[78] = (nonce >> 16) & 0xFF;
        header[79] = (nonce >> 24) & 0xFF;

        const uint256 hash = HashQuark80(header);
        if (UintToArith256(hash) <= target) {
            GenesisResult res;
            res.nTime = nTime;
            res.nBits = nBits;
            res.nVersion = nVersion;
            res.nNonce = nonce;
            res.merkle_root = merkle;
            std::memcpy(res.genesis_hash.data(), hash.begin(), 32);
            return res;
        }
        if (nonce == std::numeric_limits<uint32_t>::max()) {
            std::cerr << "Nonce overflow\n";
            std::exit(1);
        }
        ++nonce;
    }
}

static void Usage(const char* argv0)
{
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " --time <unix> --bits <hex> [--version <n>] [--nonce <start>] [--reward <sats>]\n"
        << "        [--timestamp <str>] [--pubkey <hex>]\n"
        << "\n"
        << "Defaults:\n"
        << "  --timestamp \"OrganicLife Genesis 2026-01-21\"\n"
        << "  --pubkey (PIVX genesis pubkey)\n"
        << "  --version 1\n"
        << "  --nonce 0\n"
        << "  --reward 0\n";
}

static bool ParseArg(const std::string& a, const std::string& k) { return a == k; }

int main(int argc, char** argv)
{
    std::string timestamp = "OrganicLife Genesis 2026-02-02";
    std::string pubkeyHex =
        "04c10e83b2703ccf322f7dbd62dd5855ac7c10bd055814ce121ba32607d573b8810c02c0582aed05b4deb9c4b77b26d92428c61256cd42774babea0a073b2ed0c9";
    uint32_t nTime = 0;
    uint32_t nBits = 0;
    int32_t nVersion = 1;
    uint32_t nonceStart = 0;
    uint64_t rewardSats = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        auto requireValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                Usage(argv[0]);
                std::exit(1);
            }
            return std::string(argv[++i]);
        };

        if (ParseArg(arg, "--time")) {
            nTime = static_cast<uint32_t>(std::stoul(requireValue("--time")));
        } else if (ParseArg(arg, "--bits")) {
            const std::string v = requireValue("--bits");
            nBits = static_cast<uint32_t>(std::stoul(v, nullptr, 16));
        } else if (ParseArg(arg, "--version")) {
            nVersion = static_cast<int32_t>(std::stol(requireValue("--version")));
        } else if (ParseArg(arg, "--nonce")) {
            nonceStart = static_cast<uint32_t>(std::stoul(requireValue("--nonce")));
        } else if (ParseArg(arg, "--reward")) {
            rewardSats = static_cast<uint64_t>(std::stoull(requireValue("--reward")));
        } else if (ParseArg(arg, "--timestamp")) {
            timestamp = requireValue("--timestamp");
        } else if (ParseArg(arg, "--pubkey")) {
            pubkeyHex = requireValue("--pubkey");
        } else if (ParseArg(arg, "-h") || ParseArg(arg, "--help")) {
            Usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown arg: " << arg << "\n";
            Usage(argv[0]);
            return 1;
        }
    }

    if (nTime == 0 || nBits == 0) {
        Usage(argv[0]);
        return 1;
    }

    const std::vector<unsigned char> pubkey = ParseHex(pubkeyHex);
    if (pubkey.size() != 65 || pubkey[0] != 0x04) {
        std::cerr << "Expected an uncompressed 65-byte pubkey (starts with 04)\n";
        return 1;
    }

    const GenesisResult res = MineGenesis(timestamp, pubkey, nTime, nBits, nVersion, nonceStart, rewardSats);

    std::cout << "timestamp: " << timestamp << "\n";
    std::cout << "nTime:     " << res.nTime << "\n";
    std::cout << "nBits:     0x" << std::hex << std::setw(8) << std::setfill('0') << res.nBits << std::dec << "\n";
    std::cout << "nVersion:  " << res.nVersion << "\n";
    std::cout << "nNonce:    " << res.nNonce << "\n";
    std::cout << "merkle:    0x" << HexStrRev(res.merkle_root.data(), res.merkle_root.size()) << "\n";
    std::cout << "genesis:   0x" << HexStrRev(res.genesis_hash.data(), res.genesis_hash.size()) << "\n";
    return 0;
}
