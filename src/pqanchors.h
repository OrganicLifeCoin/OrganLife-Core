// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_PQANCHORS_H
#define ORGANICLIFE_PQANCHORS_H

#include <chainparams.h>
#include <consensus/params.h>
#include <crypto/mldsa44.h>
#include <evo/evodb.h>
#include <evo/pqmasternode.h>
#include <fs.h>
#include <pqquorum.h>
#include <uint256.h>
#include <memory>
#include <string>
#include <vector>

class CBlockIndex;

// Finality anchor chain state. Two stores with different rollback semantics:
//
// 1. ChainState (EvoDB-backed): the deterministic chain mirror — committee
//    snapshots and anchor records written inside the caller's block-connect
//    transaction, atomically undone on disconnect and rebuilt identically on
//    reindex. This is consensus state.
//
// 2. Store (separate pqanchors/ LevelDB): every locally validated anchor and
//    certificate, fsynced before use. NEVER reset by -reindex; it is the
//    non-rollbackable enforcement source. Startup reconciliation refuses to
//    start when its tip is not an ancestor of the active chain.
//
// The anchor chain is cryptographic: an anchor's ID commits to its height,
// block hash and committee commitment, and each certificate statement names
// the anchor it votes under. A peer certificate can never choose the trust
// root or committee; verification always uses caller-supplied snapshots.
namespace pqanchor {
// AnchorID = Hash(canonical serialization of {height, blockHash, committee}).
uint256 ID(uint32_t height, const uint256& blockHash, const uint256& committee);

// Chain-state mirror record for one finalized height.
struct Record {
    uint32_t height{0};
    uint256 blockHash;
    uint256 committee; // commitment of the voting committee
    std::vector<uint256> signers; // registration IDs endorsing, from the cert
    uint32_t prevHeight{0}; // previous anchor height (O(1) undo)
    SERIALIZE_METHODS(Record, obj) { READWRITE(obj.height, obj.blockHash, obj.committee, obj.signers, obj.prevHeight); }
};

struct SnapshotEntry {
    std::vector<pqquorum::Member> members;
    uint256 commitment;
    uint32_t prevHeight{0};
    SERIALIZE_METHODS(SnapshotEntry, obj) { READWRITE(obj.members, obj.commitment, obj.prevHeight); }
};

// Deterministic committee selection from registry state at a height: mature,
// non-revoked registrations with a non-empty service endpoint, ordered by
// registration ID ascending, capped at MAX_MEMBERS. Fewer than MIN_MEMBERS
// means NO committee (finality stalls; the threshold is never lowered).
std::vector<pqquorum::Member> SelectCommittee(const pqmn::Index& index, uint32_t height);

// Publicly verifiable positive service evidence: collects the registration IDs
// that signed in-block finality certificates within [upToHeight - window + 1,
// upToHeight]. anyCert reports whether the window contains any certificate at
// all (a committee-level stall is never attributed to individuals).
// Caller holds cs_main.
bool SignersInWindow(CEvoDB& db, const CChainParams& params, uint32_t upToHeight, uint32_t window,
                     std::vector<uint256>& signers, bool& anyCert);

class ChainState {
public:
    ChainState(CEvoDB& db, const CChainParams& params) : db(db), params(params) {}
    // Captures a committee snapshot at height when the commitment changes.
    // Called from ConnectBlock after registry updates, inside the caller's
    // EvoDB transaction. False means corrupt state (fail closed).
    bool CaptureCommittee(const pqmn::Index& index, uint32_t height, std::string& reason);
    // Records a validated in-block anchor. Height must be TipHeight() + 1 and
    // the value must extend the previous anchor (verified by the caller).
    bool RecordAnchor(uint32_t height, const uint256& blockHash, std::vector<uint256> signers,
                      std::string& reason);
    // Undoes snapshot/anchor records for the block on disconnect.
    bool UndoBlock(uint32_t height, std::string& reason);
    bool GetAnchor(uint32_t height, Record& out) const;
    bool TipAnchor(Record& out) const;
    // Latest committee snapshot at or below height; false = none (finality
    // inactive: no bootstrap or no qualifying committee yet).
    bool CommitteeAt(uint32_t height, std::vector<pqquorum::Member>& members) const;
    uint32_t TipHeight() const;
    const CChainParams& GetParams() const { return params; }
    // Writes the pinned bootstrap committee as the initial snapshot (H0).
    bool SetInitialCommittee(uint32_t height, const std::vector<pqquorum::Member>& members, std::string& reason);

private:
    CEvoDB& db;
    const CChainParams& params;
    std::pair<std::string, uint256> Kind(char kind) const;
    std::pair<std::string, uint256> AtHeight(char kind, uint32_t height) const;
    template <typename V> bool Read(const std::pair<std::string, uint256>& key, V& out) const;
    template <typename V> void Write(const std::pair<std::string, uint256>& key, const V& value);
    void Erase(const std::pair<std::string, uint256>& key);
    bool StoreCommittee(uint32_t height, const std::vector<pqquorum::Member>& members,
                        const uint256& commitment, std::string& reason);
};

// Pinned initial checkpoint (-pqbootstrap=height:blockhash:regid:pubkey:...).
// Regtest/qualification only; main/testnet carry no bootstrap until coordinated
// activation supplies real values. No bootstrap => finality fully inactive.
struct Bootstrap {
    uint32_t height{0};
    uint256 blockHash;
    std::vector<pqquorum::Member> members;
    bool Configured() const { return !members.empty(); }
};
// Parses the regtest-only argument at init; false fails startup.
bool InitBootstrap(const CChainParams& params, std::string& reason);
const Bootstrap* GetBootstrap();
// Lazy validation, re-checked until it passes; failure keeps finality inactive
// but never rejects the chain. On first success the pinned committee is stored
// as the initial snapshot at the pinned height. Caller holds cs_main.
bool ValidateBootstrap(ChainState& state, const pqmn::Index& index, const CBlockIndex* tip,
                       std::string& reason);

// Non-rollbackable durable store under <datadir>/pqanchors/. Never reset by
// reindex; every write is fsynced before returning. Heights strictly increase.
class Store {
public:
    static std::unique_ptr<Store> Open(const fs::path& datadir, std::string& reason);
    // Records a locally validated anchor and its certificate, fsynced. Refuses
    // a height at or below the current tip (finality is never overwritten).
    bool Write(const Record& record, const pqquorum::Certificate& certificate, std::string& reason);
    bool Read(uint32_t height, Record& out) const;
    bool ReadCertificate(uint32_t height, pqquorum::Certificate& out) const;
    bool Tip(Record& out) const;
    uint32_t TipHeight() const;

private:
    Store(std::unique_ptr<CDBWrapper> db) : db(std::move(db)) {}
    std::unique_ptr<CDBWrapper> db;
    static constexpr const char* ANCHOR_PREFIX = "pqanchor1a";
    static constexpr const char* CERT_PREFIX = "pqanchor1c";
    static constexpr const char* TIP_KEY = "pqanchor1t";
};
} // namespace pqanchor
#endif
