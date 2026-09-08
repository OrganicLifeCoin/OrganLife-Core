// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_PQSERVICE_H
#define ORGANICLIFE_PQSERVICE_H

#include <pqquorum.h>

class CBlockIndex;
class CChainParams;
class CNode;
namespace pqmn { class Index; struct Record; }

namespace pqservice {
constexpr size_t MAX_HEARTBEATS = 16;
constexpr size_t HEARTBEAT_SIZE = 32 + 8 + 4 + 32 + mldsa44::SIGNATURE_SIZE;
constexpr size_t MAX_CARRIER_SIZE = 1 + 9 + pqquorum::MAX_CERTIFICATE_SIZE + 1 + MAX_HEARTBEATS * HEARTBEAT_SIZE;
struct Heartbeat {
    uint256 registration;
    uint64_t sequence{0};
    uint32_t height{0};
    uint256 blockHash;
    std::array<unsigned char, mldsa44::SIGNATURE_SIZE> signature{};
    SERIALIZE_METHODS(Heartbeat, obj) {
        READWRITE(obj.registration, obj.sequence, obj.height, obj.blockHash, obj.signature);
    }
};
struct Carrier {
    std::vector<unsigned char> certificate;
    std::vector<Heartbeat> heartbeats;
};
std::vector<unsigned char> Encode(const Carrier& carrier);
bool Decode(Span<const unsigned char> bytes, Carrier& carrier);
Span<const unsigned char> Context();
std::vector<unsigned char> Message(const Heartbeat& heartbeat, const uint256& genesis,
                                   const mldsa44::PublicKey& key);
bool Active(const CChainParams& params, int height);
bool Recent(const pqmn::Record& record, uint32_t height, const CChainParams& params);
bool Eligible(const pqmn::Record& record, uint32_t height, const CChainParams& params, bool anyHeartbeat);
// Positive signed activity, not proof of complete service. The caller holds
// cs_main and supplies the parent registry/chain, never peer-claimed state.
bool CheckContext(const Heartbeat& heartbeat, const pqmn::Record& record,
                  const CBlockIndex* parent, const CChainParams& params, std::string& reason);
bool Verify(const Heartbeat& heartbeat, const pqmn::Index& index,
            const CBlockIndex* parent, const CChainParams& params, std::string& reason);
// Bounded walletless relay/mining pool. All calls require cs_main.
bool Receive(Span<const unsigned char> bytes);
void Send(CNode& peer, int64_t& nextRelay, uint256& cursor);
std::vector<Heartbeat> Pending(const CBlockIndex* parent);
}
#endif
