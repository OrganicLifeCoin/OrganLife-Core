// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqservice.h>
#include <chainparams.h>
#include <pqtransaction.h>
#include <streams.h>
#include <set>

namespace pqservice {
std::vector<unsigned char> Encode(const Carrier& carrier)
{
    if (carrier.heartbeats.size() > MAX_HEARTBEATS ||
        carrier.certificate.size() > pqquorum::MAX_CERTIFICATE_SIZE ||
        (carrier.heartbeats.empty() && carrier.certificate.empty())) return {};
    CDataStream bytes(SER_NETWORK, 0);
    bytes << uint8_t{1} << carrier.certificate << uint8_t(carrier.heartbeats.size());
    for (const auto& heartbeat : carrier.heartbeats) bytes << heartbeat;
    return {bytes.begin(), bytes.end()};
}

bool Decode(Span<const unsigned char> bytes, Carrier& carrier)
{
    carrier = {};
    if (bytes.size() > MAX_CARRIER_SIZE) return false;
    try {
        CDataStream stream(std::vector<unsigned char>(bytes.begin(), bytes.end()), SER_NETWORK, 0);
        uint8_t version, count;
        stream >> version;
        if (version != 1) return false;
        const auto size = ReadCompactSize(stream);
        if (size > pqquorum::MAX_CERTIFICATE_SIZE || size > stream.size()) return false;
        Carrier decoded;
        decoded.certificate.resize(size);
        if (size) stream.read(reinterpret_cast<char*>(decoded.certificate.data()), size);
        stream >> count;
        if (count > MAX_HEARTBEATS || stream.size() != count * HEARTBEAT_SIZE || (!size && !count)) return false;
        std::set<uint256> seen;
        for (unsigned int i = 0; i < count; ++i) {
            Heartbeat heartbeat;
            stream >> heartbeat;
            if (heartbeat.registration.IsNull() || !seen.insert(heartbeat.registration).second) return false;
            decoded.heartbeats.push_back(heartbeat);
        }
        if (size) {
            pqquorum::Certificate certificate;
            if (!pqquorum::Decode(decoded.certificate, certificate)) return false;
        }
        carrier = std::move(decoded);
        return true;
    } catch (const std::exception&) { return false; }
}

Span<const unsigned char> Context()
{
    static const unsigned char context[] = "OLC/PQ/ML-DSA-44/service/v1";
    return {context, sizeof(context) - 1};
}

std::vector<unsigned char> Message(const Heartbeat& heartbeat, const uint256& genesis,
                                   const mldsa44::PublicKey& key)
{
    CDataStream bytes(SER_NETWORK, 0);
    bytes << uint8_t{1} << genesis << heartbeat.registration << key << heartbeat.sequence
          << heartbeat.height << heartbeat.blockHash;
    return {bytes.begin(), bytes.end()};
}

bool Active(const CChainParams& params, int height)
{
    return pq::MasternodesActive(params, height) &&
        params.GetConsensus().NetworkUpgradeActive(height, Consensus::UPGRADE_PQ_SERVICE);
}

}
