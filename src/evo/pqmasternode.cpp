// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <evo/pqmasternode.h>
#include <chainparams.h>
#include <coins.h>
#include <hash.h>
#include <pqanchors.h>
#include <pqservice.h>
#include <validation.h>
#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace pqservice {
bool Recent(const pqmn::Record& record, uint32_t height, const CChainParams& params)
{
    return !record.revoked && record.service != CService() &&
        record.MatureAt(height, params.GetConsensus().MasternodeCollateralMinConf()) &&
        record.lastHeartbeatHeight != 0 && record.lastHeartbeatHeight < height &&
        height - record.lastHeartbeatHeight <= uint32_t(params.GetConsensus().nPQServiceWindow);
}

bool Eligible(const pqmn::Record& record, uint32_t height, const CChainParams& params, bool anyHeartbeat)
{
    const auto first = params.GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_SERVICE].nActivationHeight;
    const auto grace = std::max(record.registeredHeight, uint32_t(std::max(0, first)));
    return !anyHeartbeat || (height >= grace && height - grace <= uint32_t(params.GetConsensus().nPQServiceWindow)) ||
        Recent(record, height, params);
}

bool CheckContext(const Heartbeat& heartbeat, const pqmn::Record& record,
                  const CBlockIndex* parent, const CChainParams& params, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason = "bad-pq-service-context";
    if (!parent || !Active(params, parent->nHeight + 1) || heartbeat.registration.IsNull() ||
        !heartbeat.height || heartbeat.height > uint32_t(parent->nHeight) ||
        uint32_t(parent->nHeight) - heartbeat.height >= uint32_t(std::max(1, params.GetConsensus().nPQServiceWindow / 2)) ||
        record.revoked || record.service == CService() || heartbeat.sequence != record.sequence ||
        heartbeat.height <= record.lastHeartbeatHeight ||
        !record.MatureAt(heartbeat.height, params.GetConsensus().MasternodeCollateralMinConf())) return false;
    // TestBlockValidity may provide an index without skip pointers.
    const auto* reference = parent;
    while (reference && reference->nHeight > int(heartbeat.height)) reference = reference->pprev;
    if (!reference || reference->GetBlockHash() != heartbeat.blockHash) return false;
    reason.clear();
    return true;
}

bool Verify(const Heartbeat& heartbeat, const pqmn::Index& index,
            const CBlockIndex* parent, const CChainParams& params, std::string& reason)
{
    pqmn::Record record;
    reason = "bad-pq-service-registration";
    if (!index.Get(heartbeat.registration, record) || !CheckContext(heartbeat, record, parent, params, reason)) return false;
    if (!mldsa44::Verify(record.operatorKey, Message(heartbeat, params.GetConsensus().hashGenesisBlock, record.operatorKey),
                        Context(), heartbeat.signature)) {
        reason = "bad-pq-service-signature";
        return false;
    }
    return true;
}
}

namespace pqmn {
namespace {
constexpr uint8_t PQMN_SCHEMA_VERSION = 4; // Independent heartbeat state; rebuild older indexes.
bool Fail(std::string& reason, const char* error) { reason = error; return false; }
template<typename T> bool Zero(const T& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return c == 0; });
}
bool WireService(const CService& service) {
    return service == CService() || ((service.IsIPv4() || service.IsIPv6()) && service.GetPort() != 0);
}
bool ValidService(const CService& service, const CChainParams& params) {
    static const int mainPort = CreateChainParams(CBaseChainParams::MAIN)->GetDefaultPort();
    return WireService(service) && service.IsValid() && service.GetPort() != 0 &&
           (params.IsRegTestNet() || service.IsRoutable()) &&
           service.GetPort() != mainPort;
}
pq::KeyID KeyID(const mldsa44::PublicKey& key, const CChainParams& params) {
    const auto id = pq::GetID(key, params.NetworkIDString());
    if (!id || Zero(key)) throw std::runtime_error("Invalid PQ masternode key/network");
    return *id;
}
std::array<pq::KeyID, 3> Keys(const Record& record, const CChainParams& params) {
    return {KeyID(record.owner, params), KeyID(record.operatorKey, params), record.collateralKey};
}
template<typename K, typename V> bool ReadChecked(CEvoDB& db, const K& key, V& value) {
    if (db.Read(key, value)) return true;
    if (db.Exists(key)) throw std::runtime_error("Corrupt PQ masternode index");
    value = {};
    return false;
}
struct Previous {
    uint256 id, after;
    bool existed{false};
    Record record;
    SERIALIZE_METHODS(Previous, obj) {
        READWRITE(obj.id, obj.after, obj.existed);
        if (obj.existed) READWRITE(obj.record);
    }
};
struct UndoData {
    std::vector<Previous> changes;
    template<typename Stream> void Serialize(Stream& s) const {
        WriteCompactSize(s, changes.size());
        for (const auto& change : changes) ::Serialize(s, change);
    }
    template<typename Stream> void Unserialize(Stream& s) {
        const auto count = ReadCompactSize(s);
        if (count > pq::MAX_INPUTS + 1) throw std::ios_base::failure("Oversized PQ masternode undo");
        changes.resize(count);
        for (auto& change : changes) ::Unserialize(s, change);
    }
};
bool BlockContext(const CBlock& block, const CBlockIndex& index, int firstHeight) {
    return firstHeight > 0 && index.nHeight >= firstHeight && index.phashBlock && index.pprev &&
           index.pprev->phashBlock && index.pprev->nHeight == index.nHeight - 1 &&
           block.hashPrevBlock == index.pprev->GetBlockHash() && block.GetHash() == index.GetBlockHash() &&
           !block.vtx.empty() && block.vtx[0] && block.vtx[0]->IsCoinBase();
}
}

bool Record::MatureAt(uint32_t height, uint32_t confirmations) const {
    const uint32_t start = std::max(registeredHeight, collateralHeight);
    return confirmations != 0 && start != 0 && height >= start && uint64_t(height) - start + 1 >= confirmations;
}

std::vector<unsigned char> Encode(const Payload& op)
{
    CDataStream s(SER_NETWORK, 0); // Fixed v1 IPv4/IPv6 endpoint encoding, not negotiated peer version.
    s << uint8_t{1} << static_cast<uint8_t>(op.action);
    switch (op.action) {
    case Action::REGISTER:
        if (!op.registration.IsNull() || op.sequence != 0 || !WireService(op.service)) return {};
        s << op.collateral << op.owner << op.operatorKey << op.collateralKey << op.payout <<
            op.operatorReward << op.operatorPayout << op.service << op.ownerSignature << op.operatorSignature << op.collateralSignature;
        break;
    case Action::UPDATE:
        s << op.registration << op.sequence << op.operatorKey << op.payout << op.ownerSignature << op.operatorSignature;
        break;
    case Action::SERVICE:
        if (!WireService(op.service)) return {};
        s << op.registration << op.sequence << op.service << op.operatorPayout << op.operatorSignature;
        break;
    case Action::REVOKE:
        s << op.registration << op.sequence << op.operatorSignature;
        break;
    default: return {};
    }
    if (s.size() > pq::MAX_MASTERNODE_DATA_SIZE) return {};
    return {s.begin(), s.end()};
}

bool Decode(Span<const unsigned char> bytes, Payload& out)
{
    out = {};
    if (!bytes.data() || bytes.size() < 2 || bytes.size() > pq::MAX_MASTERNODE_DATA_SIZE || bytes[0] != 1) return false;
    const char* begin = reinterpret_cast<const char*>(bytes.data());
    CDataStream s(begin + 2, begin + bytes.size(), SER_NETWORK, 0);
    Payload op; op.action = static_cast<Action>(bytes[1]);
    try {
        switch (op.action) {
        case Action::REGISTER:
            s >> op.collateral >> op.owner >> op.operatorKey >> op.collateralKey >> op.payout >>
                op.operatorReward >> op.operatorPayout >> op.service >> op.ownerSignature >> op.operatorSignature >> op.collateralSignature;
            break;
        case Action::UPDATE:
            s >> op.registration >> op.sequence >> op.operatorKey >> op.payout >> op.ownerSignature >> op.operatorSignature;
            break;
        case Action::SERVICE:
            s >> op.registration >> op.sequence >> op.service >> op.operatorPayout >> op.operatorSignature;
            break;
        case Action::REVOKE:
            s >> op.registration >> op.sequence >> op.operatorSignature;
            break;
        default: return false;
        }
    } catch (const std::ios_base::failure&) { return false; }
    if (!s.empty()) return false;
    const auto canonical = Encode(op);
    if (canonical.size() != bytes.size() || !std::equal(canonical.begin(), canonical.end(), bytes.begin())) return false;
    out = std::move(op);
    return true;
}

Span<const unsigned char> Context(Role role)
{
    static const unsigned char owner[] = "OLC/PQ/ML-DSA-44/masternode/owner/v1";
    static const unsigned char oper[] = "OLC/PQ/ML-DSA-44/masternode/operator/v1";
    static const unsigned char collateral[] = "OLC/PQ/ML-DSA-44/masternode/collateral/v1";
    switch (role) {
    case Role::OWNER: return {owner, sizeof(owner)-1};
    case Role::OPERATOR: return {oper, sizeof(oper)-1};
    case Role::COLLATERAL: return {collateral, sizeof(collateral)-1};
    }
    return {};
}

std::vector<unsigned char> SigningMessage(const CTransaction& tx, const std::vector<CTxOut>& prevouts, const uint256& genesis)
{
    pq::Payload envelope; Payload op;
    if (genesis.IsNull() || !pq::DecodePayload(tx, envelope) || envelope.mode != pq::MASTERNODE || !Decode(envelope.data, op)) return {};
    op.ownerSignature = {}; op.operatorSignature = {}; op.collateralSignature = {};
    envelope.data = Encode(op);
    // The registration ID is not part of REGISTER data; later actions bind the existing ID.
    return pq::SignatureMessage(tx, prevouts, envelope, genesis, 0);
}

std::pair<std::string, uint256> Index::Prefix(char kind) const {
    return {std::string("pqmn1") + kind, params.GetConsensus().hashGenesisBlock};
}
const uint256& Index::Genesis() const { return params.GetConsensus().hashGenesisBlock; }
bool Index::Get(const uint256& id, Record& out) const {
    AssertLockHeld(cs_main);
    return ReadChecked(db, std::make_pair(Prefix('r'), id), out);
}
bool Index::FindCollateral(const COutPoint& collateral, uint256& id) const {
    AssertLockHeld(cs_main);
    if (!ReadChecked(db, std::make_pair(Prefix('c'), collateral), id)) return false;
    Record record;
    if (!Get(id, record) || record.collateral != collateral) throw std::runtime_error("Inconsistent PQ masternode collateral index");
    return true;
}
std::vector<std::pair<uint256, Record>> Index::List() const {
    AssertLockHeld(cs_main);
    LOCK(db.cs);
    std::vector<std::pair<uint256, Record>> result;
    auto it = db.GetCurTransaction().NewIteratorUniquePtr();
    CDataStream prefix(SER_DISK, 0); prefix << Prefix('r');
    for (it->Seek(std::make_pair(Prefix('r'), uint256())); it->Valid(); it->Next()) {
        auto key = it->GetKey();
        // Check the namespace before decoding the suffix: adjacent index keys have other types.
        if (key.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), key.begin())) break;
        key.ignore(prefix.size()); uint256 id; key >> id;
        if (!key.empty()) throw std::runtime_error("Corrupt PQ masternode record key");
        Record record;
        if (!it->GetValue(record)) throw std::runtime_error("Corrupt PQ masternode record");
        result.emplace_back(id, record);
    }
    return result;
}
void Index::Erase(const uint256& id, const Record& record) {
    const auto owned = [&](const auto& key) {
        uint256 owner;
        if (!ReadChecked(db, key, owner) || owner != id) throw std::runtime_error("Inconsistent PQ masternode reverse index");
    };
    owned(std::make_pair(Prefix('c'), record.collateral));
    for (const auto& key : Keys(record, params)) owned(std::make_pair(Prefix('k'), key));
    if (record.service != CService()) owned(std::make_pair(Prefix('s'), record.service));
    db.Erase(std::make_pair(Prefix('r'), id));
    db.Erase(std::make_pair(Prefix('c'), record.collateral));
    for (const auto& key : Keys(record, params)) db.Erase(std::make_pair(Prefix('k'), key));
    if (record.service != CService()) db.Erase(std::make_pair(Prefix('s'), record.service));
}
void Index::Put(const uint256& id, const Record& record) {
    const auto available = [&](const auto& key) {
        uint256 owner;
        if (ReadChecked(db, key, owner) && owner != id) throw std::runtime_error("Occupied PQ masternode reverse property");
    };
    available(std::make_pair(Prefix('c'), record.collateral));
    for (const auto& key : Keys(record, params)) available(std::make_pair(Prefix('k'), key));
    if (record.service != CService()) available(std::make_pair(Prefix('s'), record.service));
    db.Write(std::make_pair(Prefix('r'), id), record);
    db.Write(std::make_pair(Prefix('c'), record.collateral), id);
    for (const auto& key : Keys(record, params)) db.Write(std::make_pair(Prefix('k'), key), id);
    if (record.service != CService()) db.Write(std::make_pair(Prefix('s'), record.service), id);
}

Optional<std::pair<uint256, Record>> Index::FindPayee(uint32_t height) const
{
    Optional<std::pair<uint256, Record>> winner;
    const auto records = List();
    const bool heartbeatsActive = pqservice::Active(params, height);
    const bool anyHeartbeat = std::any_of(records.begin(), records.end(), [&](const auto& item) {
        return pqservice::Recent(item.second, height, params);
    });
    // Service evidence: with a pinned bootstrap and active finality, a node is
    // payout-eligible when it signed an in-block certificate within the
    // service window, or is within the window of its registration height, or
    // when the window holds no certificate at all (stall is not attributed to
    // individuals). Without finality the previous rules are unchanged.
    const bool finalityConfigured = pqanchor::GetBootstrap() != nullptr;
    std::map<uint256, uint32_t> signers;
    bool anyCert = false;
    if (finalityConfigured && !heartbeatsActive)
        pqanchor::SignersInWindow(db, params, height ? height - 1 : 0,
                                 params.GetConsensus().nPQServiceWindow, signers, anyCert);
    const auto eligible = [&](const uint256& id, const Record& record) {
        if (heartbeatsActive) return pqservice::Eligible(record, height, params, anyHeartbeat);
        if (!finalityConfigured || !anyCert) return true;
        if (record.registeredHeight != 0 && height - record.registeredHeight <= params.GetConsensus().nPQServiceWindow)
            return true; // bootstrap grace
        return signers.count(id) != 0;
    };
    for (const auto& item : records) {
        const auto& record = item.second;
        if (record.revoked || record.service == CService()) continue;
        if (Zero(record.payout) || record.operatorReward > 10000 ||
            (record.operatorReward != 0 && Zero(record.operatorPayout)) ||
            (record.operatorReward == 0 && !Zero(record.operatorPayout)))
            throw std::runtime_error("Corrupt PQ masternode payout fields");
        if (!record.MatureAt(height, params.GetConsensus().MasternodeCollateralMinConf())) continue;
        if (!eligible(item.first, record)) continue;
        const auto effective = std::max(record.lastPaidHeight, record.revivedHeight);
        const auto age = effective != 0 ? effective : record.registeredHeight;
        const auto winnerEffective = std::max(winner ? winner->second.lastPaidHeight : 0U,
                                               winner ? winner->second.revivedHeight : 0U);
        const auto winnerAge = winnerEffective != 0 ? winnerEffective : (winner ? winner->second.registeredHeight : 0U);
        if (!winner || std::make_tuple(age, item.first) < std::make_tuple(winnerAge, winner->first))
            winner = item;
    }
    return winner;
}

bool Index::GetPayee(uint32_t height, uint256& registration, Record& record) const
{
    AssertLockHeld(cs_main);
    registration.SetNull();
    record = {};
    const auto winner = FindPayee(height);
    if (!winner) return false;
    registration = winner->first;
    record = winner->second;
    return true;
}

bool Index::Apply(const CTransaction& tx, const CCoinsViewCache& view, uint32_t height, std::string& reason)
{
    return Process(tx, view, height, reason, nullptr);
}

bool Index::Check(const CTransaction& tx, const CCoinsViewCache& view, uint32_t height,
                  Record& replacement, std::string& reason)
{
    replacement = {};
    return Process(tx, view, height, reason, &replacement);
}

bool Index::Process(const CTransaction& tx, const CCoinsViewCache& view, uint32_t height,
                    std::string& reason, Record* checked)
{
    AssertLockHeld(cs_main);
    reason.clear();
    if (!params.IsTestChain() || height == 0 || tx.IsCoinBase() || tx.nType != CTransaction::PQ)
        return Fail(reason, "bad-pqmn-context");
    const uint256 txid = tx.GetHash();
    if (db.Exists(std::make_pair(Prefix('u'), txid))) return Fail(reason, "bad-pqmn-already-applied");
    pq::Payload envelope;
    if (!pq::CheckStructure(tx, params, reason) || !pq::DecodePayload(tx, envelope)) return false;
    std::vector<CTxOut> prevouts;
    std::set<COutPoint> inputs;
    std::map<uint256, Record> removed;
    for (const auto& input : tx.vin) {
        Coin coin;
        if (!inputs.insert(input.prevout).second || !view.GetUTXOCoin(input.prevout, coin) || coin.nHeight > height)
            return Fail(reason, "bad-pqmn-input");
        prevouts.push_back(coin.out);
        uint256 id;
        if (FindCollateral(input.prevout, id)) {
            Record record; if (!Get(id, record)) throw std::runtime_error("Missing PQ masternode record");
            removed.emplace(id, record);
        }
    }
    if (!pq::VerifyInputs(tx, prevouts, params, reason)) return false;
    Payload op;
    Record replacement, original;
    uint256 id;
    bool replace = envelope.mode == pq::MASTERNODE;
    bool existed = false;
    if (replace) {
        if (!Decode(envelope.data, op)) return Fail(reason, "bad-pqmn-payload");
        const auto message = SigningMessage(tx, prevouts, params.GetConsensus().hashGenesisBlock);
        const auto verify = [&](const mldsa44::PublicKey& key, Role role, const Signature& signature) {
            return !Zero(key) && !message.empty() && mldsa44::Verify(key, message, Context(role), signature);
        };
        if (op.action == Action::REGISTER) {
            id = txid;
            if (Get(id, original)) return Fail(reason, "bad-pqmn-duplicate-registration");
            Coin coin;
            COutPoint collateral = op.collateral;
            if (collateral.hash.IsNull()) {
                if (collateral.n >= tx.vout.size()) return Fail(reason, "bad-pqmn-collateral-index");
                coin = Coin(tx.vout[collateral.n], height, false, false);
                collateral.hash = txid;
            } else if (!view.GetUTXOCoin(collateral, coin)) return Fail(reason, "bad-pqmn-collateral-missing");
            pq::KeyID collateralKey;
            const auto expected = pq::GetID(op.collateralKey, params.NetworkIDString());
            if (inputs.count(collateral) || coin.nHeight == 0 || coin.nHeight > height ||
                coin.out.nValue != params.GetConsensus().nMNCollateralAmt ||
                !pq::ExtractID(coin.out.scriptPubKey, collateralKey) || !expected || *expected != collateralKey)
                return Fail(reason, "bad-pqmn-collateral");
            if (op.operatorReward > 10000 || ((op.operatorReward == 0) != Zero(op.operatorPayout)) || Zero(op.payout) ||
                (op.service != CService() && !ValidService(op.service, params))) return Fail(reason, "bad-pqmn-registration-fields");
            if (!verify(op.owner, Role::OWNER, op.ownerSignature) ||
                !verify(op.operatorKey, Role::OPERATOR, op.operatorSignature) ||
                !verify(op.collateralKey, Role::COLLATERAL, op.collateralSignature)) return Fail(reason, "bad-pqmn-registration-signature");
            replacement.collateral = collateral; replacement.owner = op.owner; replacement.operatorKey = op.operatorKey;
            replacement.collateralKey = collateralKey; replacement.payout = op.payout; replacement.operatorPayout = op.operatorPayout;
            replacement.operatorReward = op.operatorReward; replacement.service = op.service;
            replacement.registeredHeight = height; replacement.collateralHeight = coin.nHeight;
        } else {
            id = op.registration;
            if (id.IsNull() || !Get(id, original) || removed.count(id)) return Fail(reason, "bad-pqmn-registration");
            Coin collateral;
            pq::KeyID collateralKey;
            if (!view.GetUTXOCoin(original.collateral, collateral) || collateral.nHeight != original.collateralHeight ||
                collateral.nHeight > height || collateral.out.nValue != params.GetConsensus().nMNCollateralAmt ||
                !pq::ExtractID(collateral.out.scriptPubKey, collateralKey) || collateralKey != original.collateralKey)
                return Fail(reason, "bad-pqmn-collateral");
            existed = true;
            if (original.sequence == std::numeric_limits<uint64_t>::max() || op.sequence != original.sequence + 1)
                return Fail(reason, "bad-pqmn-sequence");
            replacement = original; replacement.sequence = op.sequence;
            replacement.lastHeartbeatHeight = 0; // New sequence requires fresh activity.
            switch (op.action) {
            case Action::UPDATE:
                if (Zero(op.payout) || !verify(original.owner, Role::OWNER, op.ownerSignature)) return Fail(reason, "bad-pqmn-owner-signature");
                if (op.operatorKey != original.operatorKey) {
                    if (!verify(op.operatorKey, Role::OPERATOR, op.operatorSignature)) return Fail(reason, "bad-pqmn-operator-proof");
                    replacement.operatorKey = op.operatorKey; replacement.service = {};
                    replacement.operatorPayout = {}; replacement.revoked = false;
                } else if (!Zero(op.operatorSignature)) return Fail(reason, "bad-pqmn-unexpected-proof");
                replacement.payout = op.payout;
                break;
            case Action::SERVICE:
                if (original.revoked || !ValidService(op.service, params) ||
                    (original.operatorReward == 0 && !Zero(op.operatorPayout)) ||
                    (original.operatorReward != 0 && Zero(op.operatorPayout))) return Fail(reason, "bad-pqmn-service");
                if (!verify(original.operatorKey, Role::OPERATOR, op.operatorSignature)) return Fail(reason, "bad-pqmn-operator-signature");
                replacement.service = op.service; replacement.operatorPayout = op.operatorPayout;
                if (original.service == CService()) replacement.revivedHeight = height;
                break;
            case Action::REVOKE:
                if (original.revoked || !verify(original.operatorKey, Role::OPERATOR, op.operatorSignature)) return Fail(reason, "bad-pqmn-revoke");
                replacement.revoked = true; replacement.service = {}; replacement.operatorPayout = {};
                break;
            default: return Fail(reason, "bad-pqmn-action");
            }
        }
        const auto freeProperty = [&](const auto& key) {
            uint256 occupied;
            return !ReadChecked(db, key, occupied) || occupied == id || removed.count(occupied);
        };
        const auto keys = Keys(replacement, params);
        if (keys[0] == keys[1] || keys[0] == keys[2] || keys[1] == keys[2]) return Fail(reason, "bad-pqmn-key-reuse");
        for (const auto& key : keys)
            if (!freeProperty(std::make_pair(Prefix('k'), key))) return Fail(reason, "bad-pqmn-duplicate-key");
        if (!freeProperty(std::make_pair(Prefix('c'), replacement.collateral))) return Fail(reason, "bad-pqmn-duplicate-collateral");
        if (replacement.service != CService() && !freeProperty(std::make_pair(Prefix('s'), replacement.service)))
            return Fail(reason, "bad-pqmn-duplicate-service");
    }
    UndoData undo;
    for (const auto& item : removed) undo.changes.push_back({item.first, {}, true, item.second});
    if (replace) undo.changes.push_back({id, SerializeHash(replacement), existed, original});
    if (undo.changes.size() > pq::MAX_INPUTS + 1) return Fail(reason, "bad-pqmn-undo-size");
    if (checked) {
        *checked = replacement;
        return true;
    }
    // Validation has finished. All writes belong to the caller's CEvoDB committer.
    for (const auto& item : removed) Erase(item.first, item.second);
    if (replace) {
        if (existed) Erase(id, original);
        Put(id, replacement);
    }
    db.Write(std::make_pair(Prefix('u'), txid), undo);
    return true;
}

bool Index::MatchesChainTip(const CBlockIndex* tip) const
{
    AssertLockHeld(cs_main);
    uint256 best; int first{0};
    const bool hasTip = ReadChecked(db, Prefix('b'), best);
    const bool hasActivation = ReadChecked(db, Prefix('a'), first);
    int serviceActivation{0};
    const bool hasServiceActivation = ReadChecked(db, Prefix('j'), serviceActivation);
    uint8_t schema{0};
    const bool hasSchema = ReadChecked(db, Prefix('v'), schema);
    if (!tip || !pq::MasternodesActive(params, tip->nHeight)) return !hasTip && !hasActivation && !hasServiceActivation && !hasSchema;
    return hasTip && hasActivation && hasServiceActivation && hasSchema &&
        serviceActivation == params.GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_SERVICE].nActivationHeight &&
        schema == PQMN_SCHEMA_VERSION && best == tip->GetBlockHash() &&
        first == params.GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_MASTERNODES].nActivationHeight;
}

bool Index::ConnectBlock(const CBlock& block, const CBlockIndex& index, CCoinsViewCache& view,
                         int firstHeight, std::string& reason, CAmount reward)
{
    AssertLockHeld(cs_main);
    reason.clear();
    if (reward < 0) return Fail(reason, "bad-pqmn-negative-reward");
    if (!params.IsTestChain() || !BlockContext(block, index, firstHeight)) return Fail(reason, "bad-pqmn-block-context");
    if (!view.GetHeadBlocks().empty() || view.GetBestBlock() != block.hashPrevBlock || !db.VerifyBestBlock(block.hashPrevBlock))
        return Fail(reason, "bad-pqmn-chain-tip");
    uint256 best; int first{0};
    const bool exists = ReadChecked(db, Prefix('b'), best);
    const bool hasActivation = ReadChecked(db, Prefix('a'), first);
    if (index.nHeight == firstHeight) {
        if (exists || hasActivation) return Fail(reason, "bad-pqmn-registry-tip");
        LOCK(db.cs);
        auto it = db.GetCurTransaction().NewIteratorUniquePtr();
        for (char kind : {'c', 'h', 'j', 'k', 'r', 's', 'u', 'v', 'w'}) {
            CDataStream prefix(SER_DISK, 0); prefix << Prefix(kind);
            it->Seek(Prefix(kind));
            if (!it->Valid()) continue;
            const auto key = it->GetKey();
            if (key.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), key.begin()))
                return Fail(reason, "bad-pqmn-registry-not-empty");
        }
    } else if (!exists || best != block.hashPrevBlock || !hasActivation || first != firstHeight)
        return Fail(reason, "bad-pqmn-registry-tip");
    if (index.nHeight != firstHeight) {
        uint8_t schema{0};
        int serviceActivation{0};
        if (!ReadChecked(db, Prefix('j'), serviceActivation) ||
            serviceActivation != params.GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_SERVICE].nActivationHeight)
            return Fail(reason, "bad-pqmn-service-activation");
        if (!ReadChecked(db, Prefix('v'), schema) || schema != PQMN_SCHEMA_VERSION)
            return Fail(reason, "bad-pqmn-schema");
    }

    // Select and advance from the parent state before same-block updates/spends.
    // The saved pre-state is restored after transaction undos on disconnect.
    Previous rewardMarker;
    if (reward > 0) {
        const auto winner = FindPayee(index.nHeight);
        if (winner) {
            Record paid = winner->second;
            paid.lastPaidHeight = index.nHeight;
            const Previous before{winner->first, SerializeHash(paid), true, winner->second};
            Erase(winner->first, winner->second);
            Put(winner->first, paid);
            rewardMarker = before;
        }
    }
    db.Write(std::make_pair(Prefix('w'), index.GetBlockHash()), rewardMarker);
    if (pqservice::Active(params, index.nHeight)) {
        std::vector<Previous> heartbeatUndo;
        pq::Payload payload;
        if (pq::DecodePayload(*block.vtx[0], payload) && payload.mode == pq::SERVICE) {
            pqservice::Carrier carrier;
            if (!pqservice::Decode(payload.data, carrier)) return Fail(reason, "bad-pq-service-payload");
            // Verify the complete batch before writing, against the parent registry.
            for (const auto& heartbeat : carrier.heartbeats)
                if (!pqservice::Verify(heartbeat, *this, index.pprev, params, reason)) return false;
            for (const auto& heartbeat : carrier.heartbeats) {
                Record before;
                if (!Get(heartbeat.registration, before)) return Fail(reason, "bad-pq-service-registration");
                Record after = before;
                after.lastHeartbeatHeight = heartbeat.height;
                heartbeatUndo.push_back({heartbeat.registration, SerializeHash(after), true, before});
                Erase(heartbeat.registration, before);
                Put(heartbeat.registration, after);
            }
        }
        db.Write(std::make_pair(Prefix('h'), index.GetBlockHash()), heartbeatUndo);
    }
    if (index.nHeight == firstHeight) {
        db.Write(Prefix('v'), PQMN_SCHEMA_VERSION);
        db.Write(Prefix('j'), params.GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_SERVICE].nActivationHeight);
    }
    // Never flush: later transactions see earlier spends/outputs without mutating the caller's view.
    CCoinsViewCache inputs(&view);
    for (const auto& tx : block.vtx) {
        if (!tx) return Fail(reason, "bad-pqmn-block-transaction");
        if (!tx->IsCoinBase() && !Apply(*tx, inputs, index.nHeight, reason)) return false;
        UpdateCoins(*tx, inputs, index.nHeight);
    }
    db.Write(Prefix('b'), index.GetBlockHash());
    if (index.nHeight == firstHeight) db.Write(Prefix('a'), firstHeight);
    return true;
}

bool Index::DisconnectBlock(const CBlock& block, const CBlockIndex& index, const CCoinsViewCache& view,
                            int firstHeight, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    if (!params.IsTestChain() || !BlockContext(block, index, firstHeight)) return Fail(reason, "bad-pqmn-block-context");
    uint256 best; int first{0};
    if (!view.GetHeadBlocks().empty() || view.GetBestBlock() != index.GetBlockHash() || !db.VerifyBestBlock(index.GetBlockHash()))
        return Fail(reason, "bad-pqmn-chain-tip");
    if (!ReadChecked(db, Prefix('b'), best) || best != index.GetBlockHash() ||
        !ReadChecked(db, Prefix('a'), first) || first != firstHeight) return Fail(reason, "bad-pqmn-registry-tip");
    uint8_t schema{0};
    if (!ReadChecked(db, Prefix('v'), schema) || schema != PQMN_SCHEMA_VERSION)
        return Fail(reason, "bad-pqmn-schema");
    for (auto it = block.vtx.rbegin(); it != block.vtx.rend(); ++it) {
        if (!*it) return Fail(reason, "bad-pqmn-block-transaction");
        if (!(*it)->IsCoinBase() && !Undo((*it)->GetHash(), reason)) return false;
    }
    if (pqservice::Active(params, index.nHeight)) {
        const auto key = std::make_pair(Prefix('h'), index.GetBlockHash());
        std::vector<Previous> heartbeatUndo;
        if (!ReadChecked(db, key, heartbeatUndo) || heartbeatUndo.size() > pqservice::MAX_HEARTBEATS)
            return Fail(reason, "bad-pq-service-undo");
        for (const auto& change : heartbeatUndo) {
            Record current;
            if (!Get(change.id, current) || SerializeHash(current) != change.after)
                return Fail(reason, "bad-pq-service-undo-order");
            Erase(change.id, current);
            Put(change.id, change.record);
        }
        db.Erase(key);
    }
    Previous rewardUndo;
    const auto rewardKey = std::make_pair(Prefix('w'), index.GetBlockHash());
    if (!ReadChecked(db, rewardKey, rewardUndo)) return Fail(reason, "bad-pqmn-missing-reward-undo");
    {
        if (rewardUndo.existed) {
            Record current;
            if (!Get(rewardUndo.id, current) || SerializeHash(current) != rewardUndo.after)
                return Fail(reason, "bad-pqmn-reward-undo-order");
            Erase(rewardUndo.id, current);
            Put(rewardUndo.id, rewardUndo.record);
        }
        db.Erase(rewardKey);
        if (index.nHeight == firstHeight) { db.Erase(Prefix('v')); db.Erase(Prefix('j')); }
    }
    if (index.nHeight == firstHeight) { db.Erase(Prefix('b')); db.Erase(Prefix('a')); }
    else db.Write(Prefix('b'), block.hashPrevBlock);
    return true;
}

bool Index::Undo(const uint256& txid, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    UndoData undo;
    if (!ReadChecked(db, std::make_pair(Prefix('u'), txid), undo)) return Fail(reason, "bad-pqmn-missing-undo");
    std::set<uint256> seen;
    for (const auto& change : undo.changes) {
        Record current;
        const bool exists = Get(change.id, current);
        if (!seen.insert(change.id).second || (exists ? SerializeHash(current) : uint256()) != change.after)
            return Fail(reason, "bad-pqmn-undo-order");
    }
    const auto available = [&](const auto& key) {
        uint256 owner;
        return !ReadChecked(db, key, owner) || seen.count(owner);
    };
    for (const auto& change : undo.changes) {
        if (!change.existed) continue;
        if (!available(std::make_pair(Prefix('c'), change.record.collateral))) return Fail(reason, "bad-pqmn-undo-property");
        for (const auto& key : Keys(change.record, params))
            if (!available(std::make_pair(Prefix('k'), key))) return Fail(reason, "bad-pqmn-undo-property");
        if (change.record.service != CService() && !available(std::make_pair(Prefix('s'), change.record.service)))
            return Fail(reason, "bad-pqmn-undo-property");
    }
    // Remove every after-state before restoring any before-state (properties may have moved).
    for (const auto& change : undo.changes) {
        Record current;
        if (Get(change.id, current)) Erase(change.id, current);
    }
    for (const auto& change : undo.changes) if (change.existed) Put(change.id, change.record);
    db.Erase(std::make_pair(Prefix('u'), txid));
    return true;
}
}
