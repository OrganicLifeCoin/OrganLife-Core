// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_PQJOURNAL_H
#define ORGANICLIFE_PQJOURNAL_H

#include <crypto/mldsa44.h>
#include <fs.h>
#include <pqquorum.h>
#include <sync.h>
#include <uint256.h>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdio.h>
#include <string>

// Per-identity durable anti-equivocation journal, not a finality runtime: an
// append-only file of hash-chained vote/lock/finalized records, each flushed to
// durable storage before the corresponding decision may be signed or broadcast.
// On finalize the file is compacted atomically to header + retained finalized
// records + live records above the finalized height, keeping it bounded without
// ever discarding a decision that a used key could still contradict. The
// journal lives outside chainstate/EvoDB and is never rollback or reindex
// state. Deleting the pqjournal directory DESTROYS equivocation protection for
// the identity (the Tendermint priv_validator_state operational boundary); no
// code path ever silently reinitializes a journal over existing content.
namespace pqjournal {
class Journal {
public:
    using Signature = std::array<unsigned char, mldsa44::SIGNATURE_SIZE>;
    // A false return refuses the statement; the journal then appends nothing.
    using Signer = std::function<bool(const pqquorum::Statement&, Signature&)>;

    // Opens <datadir>/pqjournal/<registration-id-hex>-<key-fingerprint>.journal
    // for exactly this genesis, registration and operator key. The file is per
    // (registration, operator key): a rotated key has never signed anything and
    // naturally starts a fresh file, while the retired key's journal is
    // retained under its own name. The caller passes the already network-
    // specific datadir; no network subdirectory is appended here. Both the
    // journal and its lifetime-lock marker must already exist. An
    // existing file must bind the identical header; any mismatch, unreadable
    // content, or corruption that is not a trailing partial record fails closed
    // (nullptr, never sign). A trailing partial record from a crash mid-append
    // is truncated back to the last valid record, fsynced, and replay continues.
    // A separate .lock file is never replaced by compaction and stays locked
    // until destruction, even after poisoning. Preserve it with the journal:
    // either missing artifact fails closed, without recreating anything.
    // A copied/old pair is not safe recovery: local leases cannot detect another
    // host or a stale backup. Rotate the operator key when history is uncertain.
    static std::unique_ptr<Journal> Load(const fs::path& datadir, const uint256& genesis,
                                         const uint256& registration, const mldsa44::PublicKey& key,
                                         std::string& reason);
    // Explicit FIRST USE only. Refuses if either journal or marker exists.
    // Never use to recover a lost/old signing history: rotate the operator key.
    static std::unique_ptr<Journal> Initialize(const fs::path& datadir, const uint256& genesis,
                                               const uint256& registration, const mldsa44::PublicKey& key,
                                               std::string& reason);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    // Journaled vote lookup; step must be PREVOTE or PRECOMMIT.
    bool GetVote(uint32_t height, uint32_t round, pqquorum::Purpose step,
                 uint256& value, Signature& signature) const;
    // Anti-equivocation signing gate. A journaled vote at (height, round, purpose)
    // with the same value returns its signature WITHOUT invoking signer (retry
    // reuse); a different value fails with "conflicting journaled vote". With no
    // journaled vote, signer is invoked and the VOTE record is durable before
    // true is returned. Statement purpose must be PREVOTE or PRECOMMIT and its
    // genesis must match this journal; anything else fails without signing.
    bool GetOrSignVote(const pqquorum::Statement& statement, const Signer& signer,
                       Signature& signature, std::string& reason);
    // Latest lock for the height; a null value is a journaled nil-unlock.
    // False means no lock record exists for the height at all.
    bool GetLock(uint32_t height, uint256& value, uint32_t& lockRound) const;
    // Appends a LOCK record (value may be null for a nil-unlock). Refuses when
    // height <= FinalizedHeight() and when lockRound is below the journaled
    // lockRound for that height; an identical retry is a durable no-op success.
    bool RecordLock(uint32_t height, uint32_t lockRound, const uint256& value, std::string& reason);
    bool GetFinalized(uint32_t& height, uint256& value, uint256& anchorID) const;
    // The finalized height must strictly increase; anything else fails closed.
    bool RecordFinalized(uint32_t height, const uint256& value, const uint256& anchorID, std::string& reason);
    uint32_t FinalizedHeight() const;
    // Highest journaled round for (height, step) so a restarted driver resumes.
    bool HighestVoted(uint32_t height, pqquorum::Purpose step, uint32_t& round) const;
    // Actual persisted finality and highest retained vote; zero means absent.
    // False (with cleared output) means the journal is unavailable/poisoned.
    bool GetProgress(uint32_t& finalized, uint32_t& votedHeight, uint32_t& votedRound) const;
    // Public quorum material, already verified by the caller. Retain only the
    // highest PREVOTE proof per live height; fsync before use. On restore the
    // runtime must reverify against its trusted committee and anchor.
    bool RecordProof(const pqquorum::Certificate& proof, std::string& reason);
    bool GetProof(uint32_t height, pqquorum::Certificate& proof) const;
    // Refuse restoration into another provisional bootstrap context. Old
    // height-only locks require a context-bound proof; no history is erased.
    bool CheckContext(uint32_t height, const uint256& anchor, const uint256& committee, std::string& reason) const;

private:
    static std::unique_ptr<Journal> Open(const fs::path& datadir, const uint256& genesis,
                                         const uint256& registration, const mldsa44::PublicKey& key,
                                         bool initialize, std::string& reason);
    struct VoteKey {
        uint32_t height, round;
        pqquorum::Purpose step;
        bool operator<(const VoteKey& other) const;
    };
    struct VoteRecord {
        uint256 anchor;
        uint256 committee;
        uint256 value;
        Signature signature;
    };
    struct LockRecord {
        uint32_t round;
        uint256 value;
    };
    Journal(const fs::path& file, const uint256& genesisIn, const uint256& registrationIn,
            const mldsa44::PublicKey& operatorKeyIn, int leaseIn);
    // Replays records into state, truncating a trailing partial record; fails
    // closed on any other inconsistency.
    bool Replay(const std::vector<unsigned char>& bytes, std::string& reason);
    // Atomically rewrites the file to header + retained finalized records +
    // votes/locks above the finalized height. A write, fsync, rename, or
    // post-rename reopen failure poisons this instance; reopen verifies the
    // journal before signing again.
    bool Compact();
    bool Append(uint8_t type, const std::vector<unsigned char>& payload, std::string& reason);
    bool HasSpace(size_t bytes, std::string& reason);
    bool CheckProof(const pqquorum::Certificate& proof, std::string& reason) const;
    void Poison();

    mutable Mutex mutex;
    const fs::path path;
    const uint256 genesis, registration;
    const mldsa44::PublicKey operatorKey;
    const int lease; // OS-held lock on the stable sidecar, not the replaced inode.
    FILE* file{nullptr}; // Append handle; writes are durable before any success return.
    uint256 chainHash; // Last durable chain hash (header hash when empty).
    bool poisoned{false};
    std::map<VoteKey, VoteRecord> votes;
    std::map<uint32_t, LockRecord> locks;
    std::map<uint32_t, pqquorum::Certificate> proofs;
    uint32_t finalizedHeight{0};
    uint256 finalizedValue, finalizedAnchor;
    bool hasFinalized{false};
};
} // namespace pqjournal
#endif
