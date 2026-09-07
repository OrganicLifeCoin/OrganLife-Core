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
} // namespace

uint256 ID(uint32_t height, const uint256& blockHash, const uint256& committee)
{
    CHashWriter hash(SER_GETHASH, 0);
    hash << std::string("OLC/PQ/anchor/v1") << height << blockHash << committee;
    return hash.GetHash();
}

std::vector<pqquorum::Member> SelectCommittee(const pqmn::Index& index, uint32_t height)
{
    AssertLockHeld(cs_main);
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
    if (members.size() < pqquorum::MIN_MEMBERS || pqquorum::Commitment(members).IsNull()) return {};
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
    const std::vector<pqquorum::Member> members = SelectCommittee(index, height);
    uint256 commitment;
    if (!members.empty()) {
        commitment = pqquorum::Commitment(members);
        if (commitment.IsNull()) return Fail(reason, "bad-pq-committee-commitment");
    }
    return StoreCommittee(height, members, commitment, reason);
}

bool ChainState::StoreCommittee(uint32_t height, const std::vector<pqquorum::Member>& members,
                                const uint256& commitment, std::string& reason)
{
    uint32_t previous{0};
    const bool hasPrevious = Read(Kind('M'), previous);
    if (hasPrevious) {
        SnapshotEntry latest;
        if (!Read(AtHeight('m', previous), latest)) return Fail(reason, "bad-pq-snapshot-state");
        if (latest.commitment == commitment) return true; // unchanged: no new snapshot
    } else if (members.empty()) {
        return true; // no committee now and none before: nothing to capture
    }
    SnapshotEntry entry;
    entry.members = members;
    entry.commitment = commitment;
    entry.prevHeight = hasPrevious ? previous : 0;
    Write(AtHeight('m', height), entry);
    Write(Kind('M'), height);
    return true;
}

bool ChainState::RecordAnchor(uint32_t height, const uint256& blockHash, std::vector<uint256> signers,
                              std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    uint32_t tip{0};
    const bool hasTip = Read(Kind('F'), tip);
    if (hasTip && height != tip + 1) return Fail(reason, "bad-pq-anchor-order");
    uint32_t snapshotHeight{0};
    const bool hasSnapshot = Read(Kind('M'), snapshotHeight);
    if (!hasSnapshot) return Fail(reason, "bad-pq-anchor-committee");
    SnapshotEntry snapshot;
    if (!Read(AtHeight('m', snapshotHeight), snapshot) || snapshot.commitment.IsNull())
        return Fail(reason, "bad-pq-anchor-committee");
    if (height <= snapshotHeight) return Fail(reason, "bad-pq-anchor-height");
    Record record;
    record.height = height;
    record.blockHash = blockHash;
    record.committee = snapshot.commitment;
    record.signers = std::move(signers);
    record.prevHeight = hasTip ? tip : 0;
    Write(AtHeight('f', height), record);
    Write(Kind('F'), height);
    return true;
}

bool ChainState::UndoBlock(uint32_t height, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    Record anchor;
    if (Read(AtHeight('f', height), anchor)) {
        Erase(AtHeight('f', height));
        if (anchor.prevHeight == 0) Erase(Kind('F'));
        else Write(Kind('F'), anchor.prevHeight);
    }
    SnapshotEntry snapshot;
    if (Read(AtHeight('m', height), snapshot)) {
        Erase(AtHeight('m', height));
        if (snapshot.prevHeight == 0) Erase(Kind('M'));
        else Write(Kind('M'), snapshot.prevHeight);
    }
    return true;
}

bool ChainState::GetAnchor(uint32_t height, Record& out) const
{
    AssertLockHeld(cs_main);
    return Read(AtHeight('f', height), out);
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
    members.clear();
    uint32_t snapshotHeight{0};
    if (!Read(Kind('M'), snapshotHeight)) return false;
    // Snapshots only exist at or below the tip; walk back if the latest is above.
    while (snapshotHeight > height) {
        SnapshotEntry previous;
        if (!Read(AtHeight('m', snapshotHeight), previous) || previous.prevHeight == 0) return false;
        snapshotHeight = previous.prevHeight;
    }
    SnapshotEntry snapshot;
    if (!Read(AtHeight('m', snapshotHeight), snapshot) || snapshot.members.empty()) return false;
    members = snapshot.members;
    return true;
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
    Bootstrap& bootstrap = BootstrapInstance();
    if (!gArgs.IsArgSet("-pqbootstrap") || gArgs.GetArg("-pqbootstrap", "").empty()) {
        reason.clear();
        return true;
    }
    if (!params.IsRegTestNet()) {
        reason = "pq-bootstrap-regtest-only";
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
    if (!ParseUInt32(parts[0], &height) || height == 0) {
        reason = "pq-bootstrap-height";
        return false;
    }
    bootstrap.height = height;
    if (!ParseHexUint256(parts[1], bootstrap.blockHash)) {
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
    if (bootstrap.members.size() < pqquorum::MIN_MEMBERS || bootstrap.members.size() > pqquorum::MAX_MEMBERS) {
        reason = "pq-bootstrap-members";
        return false;
    }
    reason.clear();
    return true;
}

const Bootstrap* GetBootstrap()
{
    Bootstrap& bootstrap = BootstrapInstance();
    return bootstrap.Configured() ? &bootstrap : nullptr;
}

bool ValidateBootstrap(ChainState& state, const pqmn::Index& index, const CBlockIndex* tip, std::string& reason)
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
    pqmn::Record record;
    const uint32_t confirmations = state.GetParams().GetConsensus().MasternodeCollateralMinConf();
    // Registry state is current; maturity is checked at the pinned height when
    // the chain has not reached it yet, otherwise at the current tip. Lazy:
    // re-checked on every call until it passes; failure keeps finality inactive.
    const uint32_t maturityHeight = std::max<uint32_t>(bootstrap->height, uint32_t(tip->nHeight));
    for (const auto& member : bootstrap->members) {
        try {
            if (!index.Get(member.registration, record)) {
                reason = "pq-bootstrap-unknown-registration";
                return false;
            }
        } catch (const std::exception&) {
            reason = "pq-bootstrap-registry-unavailable";
            return false;
        }
        if (record.revoked || !record.MatureAt(maturityHeight, confirmations) ||
            record.operatorKey != member.operator_key) {
            reason = "pq-bootstrap-registration-mismatch";
            return false;
        }
    }
    // First success: the pinned committee becomes the initial snapshot at H0.
    if (state.TipHeight() == 0) {
        std::vector<pqquorum::Member> existing;
        if (!state.CommitteeAt(bootstrap->height, existing))
            return state.SetInitialCommittee(bootstrap->height, bootstrap->members, reason);
    }
    reason.clear();
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
    if (record.height <= TipHeight()) {
        reason = "pq-anchor-store-height";
        return false;
    }
    const auto key = std::make_pair(std::string(ANCHOR_PREFIX), HeightSuffix(record.height));
    const auto certKey = std::make_pair(std::string(CERT_PREFIX), HeightSuffix(record.height));
    try {
        CDBBatch batch(db->GetSerializationVersion());
        batch.Write(key, record);
        // Certificates are stored in their canonical wire encoding; they are
        // public, verifiable data.
        const std::vector<unsigned char> encoded = pqquorum::Encode(certificate);
        if (encoded.empty()) {
            reason = "pq-anchor-store-invalid";
            return false;
        }
        batch.Write(certKey, encoded);
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

bool Store::Read(uint32_t height, Record& out) const
{
    return db->Read(std::make_pair(std::string(ANCHOR_PREFIX), HeightSuffix(height)), out);
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
                     std::vector<uint256>& signers, bool& anyCert)
{
    AssertLockHeld(cs_main);
    signers.clear();
    anyCert = false;
    if (window == 0) return true;
    const auto low = upToHeight >= window ? upToHeight - window + 1 : 1;
    ChainState state(db, params);
    for (uint32_t height = low; height <= upToHeight; ++height) {
        Record anchor;
        if (!state.GetAnchor(height, anchor)) continue;
        anyCert = true;
        signers.insert(signers.end(), anchor.signers.begin(), anchor.signers.end());
    }
    return true;
}

} // namespace pqanchor
