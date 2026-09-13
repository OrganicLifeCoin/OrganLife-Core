// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <pqaddress.h>
#include <bech32.h>
#include <crypto/sha256.h>
#include <utilstrencodings.h>
#include <algorithm>
#include <cstring>

namespace pq {
namespace {
const char* AddressHRP(const std::string& network)
{
    if (network == "main") return "olcpq";
    if (network == "regtest") return "olcpqregtest";
    if (network == "test") return "olcpqtest";
    return nullptr;
}
}

Optional<KeyID> GetID(const mldsa44::PublicKey& public_key, const std::string& network)
{
    const char* domain = network == "regtest" ? "OLC/PQ/ML-DSA-44/regtest/address/v1" :
                         network == "test" ? "OLC/PQ/ML-DSA-44/testnet/address/v1" :
                         network == "main" ? "OLC/PQ/ML-DSA-44/mainnet/address/v1" : nullptr;
    if (!domain) return nullopt;
    KeyID id;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(domain), std::strlen(domain))
        .Write(public_key.data(), public_key.size()).Finalize(id.data());
    return id;
}

std::string EncodeAddress(const KeyID& id, const std::string& network)
{
    const auto* hrp = AddressHRP(network);
    if (!hrp) return {};
    std::vector<unsigned char> data{1};
    ConvertBits<8, 5, true>([&](unsigned char c) { data.push_back(c); }, id.begin(), id.end());
    return bech32::EncodeM(hrp, data);
}

bool DecodeAddress(const std::string& address, const std::string& network, KeyID& id)
{
    id = {};
    const auto* hrp = AddressHRP(network);
    if (!hrp || address.size() != std::strlen(hrp) + 1 + 53 + 6) return false;
    const auto decoded = bech32::DecodeM(address);
    if (decoded.first != hrp || decoded.second.size() != 53 || decoded.second[0] != 1) return false;
    std::vector<unsigned char> bytes;
    if (!ConvertBits<5, 8, false>([&](unsigned char c) { bytes.push_back(c); },
                                  decoded.second.begin() + 1, decoded.second.end()) || bytes.size() != id.size()) return false;
    KeyID result;
    std::copy(bytes.begin(), bytes.end(), result.begin());
    if (EncodeAddress(result, network) != address) return false;
    id = result;
    return true;
}

} // namespace pq
