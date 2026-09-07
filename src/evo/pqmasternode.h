// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_EVO_PQMASTERNODE_H
#define ORGANICLIFE_EVO_PQMASTERNODE_H

#include <evo/evodb.h>
#include <netaddress.h>
#include <optional.h>
#include <pqtransaction.h>

class CCoinsViewCache;
class CBlock;
class CBlockIndex;
namespace pqmn {
enum class Action : uint8_t { REGISTER = 1, UPDATE = 2, SERVICE = 3, REVOKE = 4 };
enum class Role { OWNER, OPERATOR, COLLATERAL };
using Signature = std::array<unsigned char, mldsa44::SIGNATURE_SIZE>;
struct Payload {
    Action action{Action::REGISTER};
    uint256 registration;
    uint64_t sequence{0};
    COutPoint collateral;
    mldsa44::PublicKey owner{}, operatorKey{}, collateralKey{};
    pq::KeyID payout{}, operatorPayout{};
    uint16_t operatorReward{0}; // Immutable basis points, 0..10000.
    CService service;
    Signature ownerSignature{}, operatorSignature{}, collateralSignature{};
};
struct Record {
    static constexpr uint8_t SERIALIZATION_VERSION = 2;
    COutPoint collateral;
    mldsa44::PublicKey owner{}, operatorKey{};
    pq::KeyID collateralKey{}, payout{}, operatorPayout{};
    CService service;
    uint16_t operatorReward{0};
    uint64_t sequence{0};
    uint32_t registeredHeight{0}, collateralHeight{0};
    uint32_t lastPaidHeight{0}, revivedHeight{0};
    bool revoked{false};
    // Maturity alone does not establish service, voting or reward eligibility.
    bool MatureAt(uint32_t height, uint32_t confirmations) const;
    SERIALIZE_METHODS(Record, obj) {
        uint8_t version = SERIALIZATION_VERSION;
        READWRITE(version);
        if (ser_action.ForRead() && version != SERIALIZATION_VERSION)
            throw std::ios_base::failure("Unsupported PQ masternode record version");
        READWRITE(obj.collateral, obj.owner, obj.operatorKey, obj.collateralKey, obj.payout,
                  obj.operatorPayout, obj.service, obj.operatorReward, obj.sequence,
                  obj.registeredHeight, obj.collateralHeight, obj.lastPaidHeight,
                  obj.revivedHeight, obj.revoked);
    }
};

std::vector<unsigned char> Encode(const Payload& payload);
bool Decode(Span<const unsigned char> bytes, Payload& payload);
Span<const unsigned char> Context(Role role);
// Normalizes role signatures only; fee signatures bind the completed payload.
std::vector<unsigned char> SigningMessage(const CTransaction& tx, const std::vector<CTxOut>& prevouts,
                                         const uint256& genesis);

// Registry and payout runtime are opt-in regtest only; quorum service/finality are not installed.
// Caller holds cs_main, owns the CEvoDB transaction and supplies the pre-spend view.
// Caller also validates ordinary transaction values/issuance, maturity and locktime.
class Index {
    CEvoDB& db;
    const CChainParams& params;
    std::pair<std::string, uint256> Prefix(char kind) const;
    void Erase(const uint256& id, const Record& record);
    void Put(const uint256& id, const Record& record);
    Optional<std::pair<uint256, Record>> FindPayee(uint32_t height) const;
    bool Process(const CTransaction& tx, const CCoinsViewCache& view, uint32_t height,
                 std::string& reason, Record* checked);
public:
    Index(CEvoDB& database, const CChainParams& chainParams) : db(database), params(chainParams) {}
    const uint256& Genesis() const;
    bool Get(const uint256& id, Record& record) const;
    bool FindCollateral(const COutPoint& collateral, uint256& id) const;
    std::vector<std::pair<uint256, Record>> List() const;
    // Selects the deterministic configured, mature payee from the parent state.
    // This does not prove quorum service or derive a monetary reward.
    bool GetPayee(uint32_t height, uint256& registration, Record& record) const;
    // Read-only startup check, including activation changes and inactive chains.
    bool MatchesChainTip(const CBlockIndex* tip) const;
    // Failure writes nothing. Exceptions require caller to roll back its transaction.
    bool Apply(const CTransaction& tx, const CCoinsViewCache& view, uint32_t height, std::string& reason);
    // Identical validation, no writes/undo or transaction ownership. Output cleared on failure.
    bool Check(const CTransaction& tx, const CCoinsViewCache& view, uint32_t height,
               Record& replacement, std::string& reason);
    // Missing undo or a changed after-state fails closed. Undo in reverse transaction order.
    bool Undo(const uint256& transaction, std::string& reason);
    // Isolated block lifecycle: caller validates ordinary block/transaction consensus,
    // supplies a trusted first active height, and owns the outer database transaction.
    // Every failure/exception requires rollback. The pre-spend view is never flushed.
    // Caller advances EVODB_BEST_BLOCK in that transaction and the corresponding UTXO cache.
    // reward is the already-derived positive PQ masternode payment for this block; zero skips queue advance.
    // This is not an atomic disk commit across the two databases.
    bool ConnectBlock(const CBlock& block, const CBlockIndex& index, CCoinsViewCache& view,
                      int firstHeight, std::string& reason, CAmount reward = 0);
    bool DisconnectBlock(const CBlock& block, const CBlockIndex& index, const CCoinsViewCache& view,
                         int firstHeight, std::string& reason);
};
} // namespace pqmn
#endif
