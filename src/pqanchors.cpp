// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqanchors.h>
#include <chain.h>
#include <clientversion.h>
#include <hash.h>
#include <logging.h>
#include <netaddress.h>
#include <streams.h>
#include <util/system.h>
#include <utilstrencodings.h>
#include <validation.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>

#include <boost/algorithm/string.hpp>

namespace pqanchor {
namespace {
constexpr char HEX_WIDTH = 8; // fixed-width height encoding; lexicographic = numeric

std::string HeightSuffix(uint32_t height)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%08x", height);
    return std::string(buffer);
}

bool ParseHexKey(const std::string& suffix, uint32_t& height)
{
    if (suffix.size() != size_t(HEX_WIDTH)) return false;
    for (const char c : suffix)
        if (!isxdigit(static_cast<unsigned char>(c))) return false;
    height = static_cast<uint32_t>(strtoull(suffix.c_str(), nullptr, 16));
    return true;
}

bool ParseHexUint256(const std::string& hex, uint256& out)
{
    out = uint256S(hex);
    return out.GetHex() == ToLower(hex);
}

template <typename K, typename V>
bool ReadChecked(CEvoDB& db, const K& key, V& value)
{
    if (!db.Read(key, value)) return false;
    return true;
}

template <typename K, typename V>
V ReadDefault(CEvoDB& db, const K& key)
{
    V value;
    db.Read(key, value);
    return value;
}

bool Fail(std::string& reason, const char* message) { reason = message; return false; }

bool ParseMember(const std::string& regidHex, const std::string& keyHex, pqquorum::Member& member,
                 std::string& reason)
{
    if (regidHex.size() != sizeof(uint256) * 2 || keyHex.size() != mldsa44::PUBLIC_KEY_SIZE * 2) {
        reason = "pq-bootstrap-bad-member";
        return false;
    }
    if (!ParseHexUint256(regidHex, member.registration)) {
        reason = "pq-bootstrap-bad-member";
        return false;
    }
    const std::vector<unsigned char> key = ParseHex(keyHex);
    if (key.size() != mldsa44::PUBLIC_KEY_SIZE) {
        reason = "pq-bootstrap-bad-member";
        return false;
    }
    std::copy(key.begin(), key.end(), member.operator_key.begin());
    return true;
}

Bootstrap& BootstrapInstance()
{
    static Bootstrap bootstrap;
    return bootstrap;
}

Recovery& RecoveryInstance()
{
    static Recovery recovery;
    return recovery;
}
} // namespace

uint256 ID(uint32_t height, const uint256& blockHash, const uint256& committee)
{
    CHashWriter hash(SER_GETHASH, 0);
    hash << std::string("OLC/PQ/anchor/v1") << height << blockHash << committee;
    return hash.GetHash();
}

std::vector<pqquorum::Member> SelectCommittee(const pqmn::Index& index, uint32_t height, bool* belowMinimum)
{
    AssertLockHeld(cs_main);
    if (belowMinimum) *belowMinimum = false;
    std::vector<pqquorum::Member> members;
    const uint32_t confirmations = Params().GetConsensus().MasternodeCollateralMinConf();
    for (const auto& entry : index.List()) {
        const pqmn::Record& record = entry.second;
        if (record.revoked || record.service == CService()) continue;
        if (!record.MatureAt(height, confirmations)) continue;
        members.push_back({entry.first, record.operatorKey});
        if (members.size() > pqquorum::MAX_MEMBERS) return {}; // unsupported size: no committee
    }
    std::sort(members.begin(), members.end(),
              [](const pqquorum::Member& a, const pqquorum::Member& b) { return a.registration < b.registration; });
    if (members.size() < pqquorum::MIN_MEMBERS) {
        if (belowMinimum) *belowMinimum = true;
        return {};
    }
    if (pqquorum::Commitment(members).IsNull()) return {};
    return members;
}

std::pair<std::string, uint256> ChainState::Kind(char kind) const
{
    return {std::string("pqmn1") + kind, params.GetConsensus().hashGenesisBlock};
}

std::pair<std::string, uint256> ChainState::AtHeight(char kind, uint32_t height) const
{
    return {std::string("pqmn1") + kind + HeightSuffix(height), params.GetConsensus().hashGenesisBlock};
}

template <typename V>
bool ChainState::Read(const std::pair<std::string, uint256>& key, V& out) const
{
    return ReadChecked(db, key, out);
}

template <typename V>
void ChainState::Write(const std::pair<std::string, uint256>& key, const V& value)
{
    db.Write(key, value);
}

void ChainState::Erase(const std::pair<std::string, uint256>& key)
{
    db.Erase(key);
}

bool ChainState::CaptureCommittee(const pqmn::Index& index, uint32_t height, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    bool belowMinimum = false;
    const std::vector<pqquorum::Member> members = SelectCommittee(index, height, &belowMinimum);
    uint256 commitment;
    if (!members.empty()) {
        commitment = pqquorum::Commitment(members);
        if (commitment.IsNull()) return Fail(reason, "bad-pq-committee-commitment");
    }
    return StoreCommittee(height, members, commitment, reason, belowMinimum);
}

bool ChainState::StoreCommittee(uint32_t height, const std::vector<pqquorum::Member>& members,
                                const uint256& commitment, std::string& reason, bool belowMinimum)
{
    uint32_t previous{0};
    const bool hasPrevious = Read(Kind('M'), previous);
    if (hasPrevious) {
        SnapshotEntry latest;
        if (!Read(AtHeight('m', previous), latest)) return Fail(reason, "bad-pq-snapshot-state");
        if (latest.commitment == commitment && latest.belowMinimum == belowMinimum) return true;
    } else if (members.empty()) {
        return true; // no committee now and none before: nothing to capture
    }
    SnapshotEntry entry;
    entry.members = members;
    entry.commitment = commitment;
    entry.belowMinimum = belowMinimum;
    entry.prevHeight = hasPrevious ? previous : 0;
    Write(AtHeight('m', height), entry);
    Write(Kind('M'), height);
    return true;
}

bool ChainState::RecordAnchor(uint32_t height, const uint256& blockHash, std::vector<uint256> signers,
                              std::string& reason, uint32_t carrierHeight)
{
    AssertLockHeld(cs_main);
    reason.clear();
    if (carrierHeight <= height) return Fail(reason, "bad-pq-anchor-carrier");
    uint32_t tip{0};
    const bool hasTip = Read(Kind('F'), tip);
    // Idempotent replay: re-applying the identical anchor (validation-only
    // reconnects, reindex) is a no-op; a conflicting value fails closed.
    if (hasTip && height == tip) {
        Record existing;
        uint32_t target{0};
        if (!Read(AtHeight('f', tip), existing) || existing.blockHash != blockHash ||
            !Read(AtHeight('s', carrierHeight), target) || target != height || existing.signers != signers)
            return Fail(reason, "bad-pq-anchor-conflict");
        reason.clear();
        return true;
    }
    if (hasTip && height != tip + 1) return Fail(reason, "bad-pq-anchor-order");
    uint32_t previousTarget{0};
    if (Read(AtHeight('s', carrierHeight), previousTarget)) return Fail(reason, "bad-pq-anchor-carrier");
    // The anchor governs the NEXT vote, so its committee comes from the
    // finalized height, not the old signers or a later certificate carrier.
    std::vector<pqquorum::Member> members;
    if (!CommitteeAt(height, members)) return Fail(reason, "bad-pq-anchor-committee");
    const uint256 commitment = pqquorum::Commitment(members);
    if (commitment.IsNull()) return Fail(reason, "bad-pq-anchor-committee");
    Record record;
    record.height = height;
    record.blockHash = blockHash;
    record.committee = commitment;
    record.signers = std::move(signers);
    record.prevHeight = hasTip ? tip : 0;
    Write(AtHeight('f', height), record);
    Write(AtHeight('s', carrierHeight), height);
    Write(Kind('F'), height);
    return true;
}

bool ChainState::UndoBlock(uint32_t blockHeight, std::optional<uint32_t> anchorHeight, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    Record anchor;
    if (anchorHeight) {
        uint32_t tip{0};
        uint32_t target{0};
        if (!Read(Kind('F'), tip) || tip != *anchorHeight ||
            !Read(AtHeight('f', *anchorHeight), anchor) || anchor.height != *anchorHeight ||
            !Read(AtHeight('s', blockHeight), target) || target != *anchorHeight)
            return Fail(reason, "bad-pq-anchor-undo-target");
        Erase(AtHeight('f', *anchorHeight));
        Erase(AtHeight('s', blockHeight));
        if (anchor.prevHeight == 0) Erase(Kind('F'));
        else Write(Kind('F'), anchor.prevHeight);
    }
    SnapshotEntry snapshot;
    if (Read(AtHeight('m', blockHeight), snapshot)) {
        Erase(AtHeight('m', blockHeight));
        if (snapshot.prevHeight == 0) Erase(Kind('M'));
        else Write(Kind('M'), snapshot.prevHeight);
    }
    return true;
}

bool ChainState::RestoreRecovery(const Store& store, uint32_t height, const CBlockIndex* tip, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    Record marker;
    if (!store.ReadRecoveryMarker(height, marker)) return true;
    if (!tip || height > uint32_t(tip->nHeight) || marker.height != height || !marker.signers.empty())
        return Fail(reason, "pq-recovery-mirror-context");
    const auto* ancestor = tip;
    while (ancestor && ancestor->nHeight > int(height)) ancestor = ancestor->pprev;
    std::vector<pqquorum::Member> members;
    if (!ancestor || ancestor->GetBlockHash() != marker.blockHash || !CommitteeAt(height, members) ||
        pqquorum::Commitment(members) != marker.committee)
        return Fail(reason, "pq-recovery-mirror-ancestry");
    uint32_t previous = TipHeight();
    Record existing;
    if (previous >= height) {
        if (!GetAnchor(height, existing) || existing.blockHash != marker.blockHash || existing.committee != marker.committee)
            return Fail(reason, "pq-recovery-mirror-conflict");
        return true;
    }
    if (Read(AtHeight('f', height), existing)) return Fail(reason, "pq-recovery-mirror-conflict");
    marker.prevHeight = previous;
    Write(AtHeight('f', height), marker);
    Write(Kind('F'), height);
    return true;
}

bool ChainState::GetAnchor(uint32_t height, Record& out) const
{
    AssertLockHeld(cs_main);
    return Read(AtHeight('f', height), out);
}

bool ChainState::SignersAtCarrier(uint32_t height, std::vector<uint256>& signers) const
{
    AssertLockHeld(cs_main);
    signers.clear();
    uint32_t target{0};
    if (!Read(AtHeight('s', height), target)) return false;
    Record anchor;
    if (target >= height || !GetAnchor(target, anchor) || anchor.height != target)
        throw std::runtime_error("Corrupt PQ certificate carrier index");
    signers = anchor.signers;
    return true;
}

bool ChainState::TipAnchor(Record& out) const
{
    AssertLockHeld(cs_main);
    uint32_t tip{0};
    if (!Read(Kind('F'), tip)) return false;
    return Read(AtHeight('f', tip), out);
}

bool ChainState::CommitteeAt(uint32_t height, std::vector<pqquorum::Member>& members) const
{
    AssertLockHeld(cs_main);
    bool snapshotFound = false;
    return CommitteeAtStatus(height, members, snapshotFound);
}

bool ChainState::CommitteeAtStatus(uint32_t height, std::vector<pqquorum::Member>& members,
                                   bool& snapshotFound, bool* belowMinimum) const
{
    AssertLockHeld(cs_main);
    members.clear();
    snapshotFound = false;
    if (belowMinimum) *belowMinimum = false;
    uint32_t snapshotHeight{0};
    if (!Read(Kind('M'), snapshotHeight)) return false;
    // Snapshots only exist at or below the tip; walk back if the latest is above.
    while (snapshotHeight > height) {
        SnapshotEntry previous;
        if (!Read(AtHeight('m', snapshotHeight), previous) || previous.prevHeight == 0) return false;
        snapshotHeight = previous.prevHeight;
    }
    SnapshotEntry snapshot;
    if (!Read(AtHeight('m', snapshotHeight), snapshot)) return false;
    snapshotFound = true;
    if (belowMinimum) *belowMinimum = snapshot.belowMinimum;
    members = snapshot.members;
    return !members.empty();
}

uint32_t ChainState::TipHeight() const
{
    AssertLockHeld(cs_main);
    uint32_t tip{0};
    return Read(Kind('F'), tip) ? tip : 0;
}

bool ChainState::SetInitialCommittee(uint32_t height, const std::vector<pqquorum::Member>& members,
                                     std::string& reason)
{
    AssertLockHeld(cs_main);
    const uint256 commitment = pqquorum::Commitment(members);
    if (commitment.IsNull()) return Fail(reason, "bad-pq-committee-commitment");
    return StoreCommittee(height, members, commitment, reason);
}

bool InitBootstrap(const CChainParams& params, std::string& reason)
{
    BootstrapInstance() = {};
    Bootstrap bootstrap;
    if (!gArgs.IsArgSet("-pqbootstrap") || gArgs.GetArg("-pqbootstrap", "").empty()) {
        reason.clear();
        return true;
    }
    if (!params.IsTestChain()) {
        reason = "pq-bootstrap-test-chain-only";
        return false;
    }
    // Format: height:blockhash:regid:pubkey[:regid:pubkey ...]
    std::vector<std::string> parts;
    boost::split(parts, gArgs.GetArg("-pqbootstrap", ""), boost::is_any_of(":"));
    if (parts.size() < 3 || (parts.size() - 2) % 2 != 0) {
        reason = "pq-bootstrap-format";
        return false;
    }
    uint32_t height = 0;
    if (!ParseUInt32(parts[0], &height) || height == 0 || height > uint32_t(std::numeric_limits<int>::max())) {
        reason = "pq-bootstrap-height";
        return false;
    }
    bootstrap.height = height;
    if (!ParseHexUint256(parts[1], bootstrap.blockHash) || bootstrap.blockHash.IsNull()) {
        reason = "pq-bootstrap-blockhash";
        return false;
    }
    bootstrap.members.clear();
    for (size_t i = 2; i + 1 < parts.size(); i += 2) {
        pqquorum::Member member;
        if (!ParseMember(parts[i], parts[i + 1], member, reason)) return false;
        bootstrap.members.push_back(member);
    }
    // Unique identities and unique keys are required up front.
    for (size_t i = 0; i < bootstrap.members.size(); ++i) {
        if (bootstrap.members[i].registration.IsNull()) {
            reason = "pq-bootstrap-null-identity";
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (bootstrap.members[i].registration == bootstrap.members[j].registration ||
                bootstrap.members[i].operator_key == bootstrap.members[j].operator_key) {
                reason = "pq-bootstrap-duplicate-member";
                return false;
            }
        }
    }
    if (bootstrap.members.size() != pqquorum::MIN_MEMBERS) {
        reason = "pq-bootstrap-members";
        return false;
    }
    std::sort(bootstrap.members.begin(), bootstrap.members.end(),
              [](const pqquorum::Member& a, const pqquorum::Member& b) { return a.registration < b.registration; });
    if (pqquorum::Commitment(bootstrap.members).IsNull()) return Fail(reason, "pq-bootstrap-bad-member");
    BootstrapInstance() = std::move(bootstrap);
    reason.clear();
    return true;
}

const Bootstrap* GetBootstrap()
{
    Bootstrap& bootstrap = BootstrapInstance();
    return bootstrap.Configured() ? &bootstrap : nullptr;
}

bool InitRecovery(const CChainParams& params, std::string& reason)
{
    RecoveryInstance() = {};
    Recovery recovery;
    const auto args = gArgs.GetArgs("-pqemergencycheckpoint");
    if (args.empty() || (args.size() == 1 && args.front().empty())) {
        reason.clear();
        return true;
    }
    if (args.size() != 1) return Fail(reason, "pq-recovery-duplicate");
    if (!params.IsTestChain()) {
        reason = "pq-recovery-test-chain-only";
        return false;
    }
    std::vector<std::string> parts;
    boost::split(parts, args.front(), boost::is_any_of(":"));
    if (parts.size() != 2) return Fail(reason, "pq-recovery-format");
    if (!ParseUInt32(parts[0], &recovery.height) || recovery.height == 0 ||
        recovery.height >= uint32_t(std::numeric_limits<int>::max()))
        return Fail(reason, "pq-recovery-height");
    if (!ParseHexUint256(parts[1], recovery.blockHash) || recovery.blockHash.IsNull())
        return Fail(reason, "pq-recovery-blockhash");
    RecoveryInstance() = recovery;
    reason.clear();
    return true;
}

const Recovery* GetRecovery()
{
    Recovery& recovery = RecoveryInstance();
    return recovery.Configured() ? &recovery : nullptr;
}

bool ValidateRecovery(const ChainState& state, const Store& store, const CBlockIndex* tip,
                      std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    const Recovery* recovery = GetRecovery();
    if (!recovery || !tip) return false;

    Record durable;
    if (!store.Tip(durable)) return Fail(reason, "pq-recovery-no-durable-anchor");
    const CBlockIndex* durableAncestor = tip->GetAncestor(int(durable.height));
    if (!durableAncestor || durableAncestor->GetBlockHash() != durable.blockHash)
        return Fail(reason, "pq-recovery-durable-ancestry");
    const CBlockIndex* checkpoint = tip->GetAncestor(int(recovery->height));
    if (!checkpoint || checkpoint->GetBlockHash() != recovery->blockHash)
        return Fail(reason, "pq-recovery-ancestry");
    if (tip->nHeight <= int(recovery->height))
        return Fail(reason, "pq-recovery-next-ancestry");

    Record marker;
    const bool marked = store.ReadRecoveryMarker(recovery->height, marker);
    if (recovery->height <= durable.height) {
        if (!marked || marker.blockHash != recovery->blockHash)
            return Fail(reason, "pq-recovery-stale");
    }

    // Existing exact recovery markers retain approval after finality advances.
    // New recovery is allowed only at the actual next-height dead end, not
    // merely because some later historical interval once had fewer voters.
    if (!marked) {
        std::vector<pqquorum::Member> historical;
        bool found = false, belowMinimum = false;
        state.CommitteeAtStatus(durable.height + 1, historical, found, &belowMinimum);
        if (!found || !historical.empty() || !belowMinimum) return Fail(reason, "pq-recovery-no-dead-end");
    }

    std::vector<pqquorum::Member> members;
    bool snapshotFound = false;
    state.CommitteeAtStatus(recovery->height, members, snapshotFound);
    if (!snapshotFound)
        return Fail(reason, "pq-recovery-committee-unavailable");
    if (members.size() < pqquorum::MIN_MEMBERS)
        return Fail(reason, "pq-recovery-committee-empty");
    std::vector<pqquorum::Member> nextMembers;
    snapshotFound = false;
    state.CommitteeAtStatus(recovery->height + 1, nextMembers, snapshotFound);
    if (!snapshotFound)
        return Fail(reason, "pq-recovery-next-committee-unavailable");
    if (nextMembers.size() < pqquorum::MIN_MEMBERS)
        return Fail(reason, "pq-recovery-committee-empty");
    if (marked && (marker.committee != pqquorum::Commitment(members) || marker.blockHash != recovery->blockHash))
        return Fail(reason, "pq-recovery-marker-conflict");
    return true;
}

bool ValidateBootstrap(const ChainState& state, const CBlockIndex* tip, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    const Bootstrap* bootstrap = GetBootstrap();
    if (!bootstrap || !tip) return false;
    // The pinned block must be an ancestor of the active chain at the pinned
    // height; local validity was established by ordinary block validation.
    const CBlockIndex* pinned = tip->GetAncestor(bootstrap->height);
    if (!pinned || pinned->GetBlockHash() != bootstrap->blockHash) {
        reason = "pq-bootstrap-ancestry";
        return false;
    }
    // Block connection already captured maturity, revocation, service and keys
    // at this height. Current registry state is not historical evidence, and
    // configuration must never manufacture or replace a chain snapshot.
    std::vector<pqquorum::Member> historical;
    if (!state.CommitteeAt(bootstrap->height, historical))
        return Fail(reason, "pq-bootstrap-snapshot-unavailable");
    if (pqquorum::Commitment(historical) != pqquorum::Commitment(bootstrap->members))
        return Fail(reason, "pq-bootstrap-snapshot-mismatch");
    return true;
}

std::unique_ptr<Store> Store::Open(const fs::path& datadir, std::string& reason)
{
    try {
        const fs::path path = datadir / "pqanchors";
        if (!fs::exists(path)) fs::create_directories(path);
        // fWipe is NEVER set: this store survives -reindex and -reindex-chainstate.
        return std::unique_ptr<Store>(new Store(std::unique_ptr<CDBWrapper>(new CDBWrapper(
            path, 4 << 20, false, false, CLIENT_VERSION | ADDRV2_FORMAT))));
    } catch (const std::exception&) {
        reason = "pq-anchor-store-unavailable";
        return nullptr;
    }
}

bool Store::Write(const Record& record, const pqquorum::Certificate& certificate, std::string& reason)
{
    reason.clear();
    if (record.height == 0 || record.blockHash.IsNull() || record.committee.IsNull()) {
        reason = "pq-anchor-store-invalid";
        return false;
    }
    // Idempotent replay: re-recording the identical finalized anchor (e.g. the
    // in-block certificate for a height this node already committed locally)
    // is a no-op. A different value at a recorded height is never accepted.
    if (record.height <= TipHeight()) {
        Record existing;
        if (Read(record.height, existing) && existing.height == record.height &&
            existing.blockHash == record.blockHash && existing.committee == record.committee)
            return true;
        reason = "pq-anchor-store-height";
        return false;
    }
    const auto key = std::make_pair(std::string(ANCHOR_PREFIX), HeightSuffix(record.height));
    const auto certKey = std::make_pair(std::string(CERT_PREFIX), HeightSuffix(record.height));
    try {
        const std::vector<unsigned char> encoded = pqquorum::Encode(certificate);
        CDBBatch batch(db->GetSerializationVersion());
        batch.Write(key, record);
        if (!encoded.empty()) batch.Write(certKey, encoded); // the pinned a0 has no certificate
        batch.Write(std::string(TIP_KEY), record.height);
        if (!db->WriteBatch(batch, true)) { // fsync before any caller may rely on it
            reason = "pq-anchor-store-write-failed";
            return false;
        }
        return true;
    } catch (const std::exception&) {
        reason = "pq-anchor-store-write-failed";
        return false;
    }
}

bool Store::WriteRecovery(const Record& record, std::string& reason)
{
    reason.clear();
    if (record.height == 0 || record.height >= uint32_t(std::numeric_limits<int>::max()) ||
        record.blockHash.IsNull() || record.committee.IsNull() || !record.signers.empty())
        return Fail(reason, "pq-anchor-store-invalid");
    Record marker;
    if (ReadRecoveryMarker(record.height, marker)) {
        Record anchor;
        if (marker.blockHash == record.blockHash && marker.committee == record.committee &&
            Read(record.height, anchor) && anchor.blockHash == marker.blockHash &&
            anchor.committee == marker.committee && TipHeight() >= record.height) return true;
        return Fail(reason, "pq-anchor-store-height");
    }
    Record previous;
    if (!Tip(previous) || record.height <= previous.height) return Fail(reason, "pq-anchor-store-height");
    const auto key = std::make_pair(std::string(ANCHOR_PREFIX), HeightSuffix(record.height));
    if (db->Exists(key) || db->Exists(std::make_pair(std::string(CERT_PREFIX), HeightSuffix(record.height))))
        return Fail(reason, "pq-anchor-store-height");
    const auto recoveryKey = std::make_pair(std::string(RECOVERY_PREFIX), HeightSuffix(record.height));
    try {
        CDBBatch batch(db->GetSerializationVersion());
        Record saved = record;
        saved.prevHeight = previous.height;
        batch.Write(key, saved);
        batch.Write(recoveryKey, saved);
        batch.Write(std::string(TIP_KEY), record.height);
        if (!db->WriteBatch(batch, true)) return Fail(reason, "pq-anchor-store-write-failed");
        return true;
    } catch (const std::exception&) {
        return Fail(reason, "pq-anchor-store-write-failed");
    }
}

bool Store::Read(uint32_t height, Record& out) const
{
    return db->Read(std::make_pair(std::string(ANCHOR_PREFIX), HeightSuffix(height)), out);
}

bool Store::ReadRecoveryMarker(uint32_t height, Record& out) const
{
    const auto key = std::make_pair(std::string(RECOVERY_PREFIX), HeightSuffix(height));
    if (db->Read(key, out)) return true;
    if (db->Exists(key)) throw std::runtime_error("Corrupt PQ recovery marker");
    return false;
}

bool Store::ReadCertificate(uint32_t height, pqquorum::Certificate& out) const
{
    std::vector<unsigned char> encoded;
    if (!db->Read(std::make_pair(std::string(CERT_PREFIX), HeightSuffix(height)), encoded)) return false;
    return pqquorum::Decode(encoded, out);
}

bool Store::Tip(Record& out) const
{
    uint32_t tip{0};
    if (!db->Read(std::string(TIP_KEY), tip)) return false;
    return Read(tip, out);
}

uint32_t Store::TipHeight() const
{
    uint32_t tip{0};
    return db->Read(std::string(TIP_KEY), tip) ? tip : 0;
}
bool SignersInWindow(CEvoDB& db, const CChainParams& params, uint32_t upToHeight, uint32_t window,
                     std::map<uint256, uint32_t>& lastCarrier, bool& anyCert)
{
    AssertLockHeld(cs_main);
    lastCarrier.clear();
    anyCert = false;
    if (window == 0) return true;
    const auto low = upToHeight >= window ? upToHeight - window + 1 : 1;
    ChainState state(db, params);
    for (uint64_t height = low; height <= upToHeight; ++height) {
        std::vector<uint256> signers;
        if (!state.SignersAtCarrier(uint32_t(height), signers)) continue;
        anyCert = true;
        for (const auto& signer : signers) lastCarrier[signer] = uint32_t(height);
    }
    return true;
}

} // namespace pqanchor
