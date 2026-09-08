// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqjournal.h>
#include <pqjournal_io.h>
#include <crypto/common.h>
#include <fs.h>
#include <hash.h>
#include <logging.h>
#include <streams.h>
#include <utilstrencodings.h>
#include <algorithm>
#include <cstdio>
#include <optional>


// Durable anti-equivocation journal. Every decision is fsynced to this file
// before the corresponding message may be signed or broadcast; the recorded
// signature is reused on retries so a restart can never produce a conflicting
// vote for the same height/round/step. The file is hash-chained: mid-chain
// corruption, a wrong identity/genesis/key header or an unreadable file fails
// closed. Only a trailing partial record (crash mid-append) is truncated back
// to the last valid record. On finalize the file is compacted atomically to
// header + newest finalized record + live votes/locks above the finalized
// height, which keeps the journal bounded without ever discarding a decision a
// used key could still contradict. Deleting the pqjournal directory DESTROYS
// equivocation protection for the identity, the same operational boundary as
// Tendermint priv_validator_state.
namespace pqjournal {
namespace {
constexpr char MAGIC[] = "OLC-PQJRNL";
constexpr size_t MAGIC_SIZE = sizeof(MAGIC) - 1;
constexpr uint8_t FORMAT_VERSION = 2;
// magic string carries a compact-size length prefix when serialized
constexpr size_t HEADER_SIZE = 1 + MAGIC_SIZE + 1 + 32 + 32 + mldsa44::PUBLIC_KEY_SIZE + 32;
constexpr size_t HASH_SIZE = 32;
constexpr uint8_t RECORD_VOTE = 1;
constexpr uint8_t RECORD_LOCK = 2;
constexpr uint8_t RECORD_FINALIZED = 3;
constexpr uint8_t RECORD_PROOF = 4; // uint32 length + length hash + canonical certificate
constexpr size_t MAX_FILE_SIZE = 1u << 30; // Hard bound, also enforced before signing/appending.

bool ValidStep(pqquorum::Purpose purpose)
{
    return purpose == pqquorum::Purpose::PREVOTE || purpose == pqquorum::Purpose::PRECOMMIT;
}

// Full record size: type byte + payload + trailing hash.
size_t RecordSize(uint8_t type)
{
    switch (type) {
    case RECORD_VOTE: return 1 + 4 + 4 + 1 + 32 + 32 + 32 + mldsa44::SIGNATURE_SIZE + HASH_SIZE;
    case RECORD_LOCK: return 1 + 4 + 4 + 32 + HASH_SIZE;
    case RECORD_FINALIZED: return 1 + 4 + 32 + 32 + HASH_SIZE;
    case RECORD_PROOF: return 1 + 4 + HASH_SIZE + HASH_SIZE; // plus the bounded encoded length
    }
    return 0;
}

std::vector<unsigned char> HeaderPrefix(const uint256& genesis, const uint256& registration,
                                        const mldsa44::PublicKey& key)
{
    CDataStream stream(SER_NETWORK, 0);
    stream << std::string(MAGIC, MAGIC_SIZE) << FORMAT_VERSION << genesis << registration << key;
    return {stream.begin(), stream.end()};
}

uint256 HeaderHash(const std::vector<unsigned char>& prefix)
{
    CHashWriter hash(SER_GETHASH, 0);
    hash.write(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    return hash.GetHash();
}

uint256 ChainHash(const uint256& previous, uint8_t type, const std::vector<unsigned char>& payload)
{
    CHashWriter writer(SER_GETHASH, 0);
    writer << previous;
    writer.write(reinterpret_cast<const char*>(&type), 1);
    writer << payload;
    return writer.GetHash();
}

std::vector<unsigned char> ProofPayload(const pqquorum::Certificate& proof, const uint256& previous)
{
    const auto encoded = pqquorum::Encode(proof);
    CDataStream stream(SER_NETWORK, 0);
    stream << uint32_t(encoded.size());
    // Check the length independently, before deciding that a record is partial.
    // Bitrot in this field must not discard later durable signing decisions.
    stream << ChainHash(previous, RECORD_PROOF, {stream.begin(), stream.end()});
    stream.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    return {stream.begin(), stream.end()};
}

// Journal files are per (registration, operator key): a rotated key has never
// signed anything and naturally starts a fresh file, while the retired key's
// journal is retained under its own name.
std::string FileName(const uint256& registration, const mldsa44::PublicKey& key)
{
    CHashWriter hash(SER_GETHASH, 0);
    hash << registration << key;
    const uint256 digest = hash.GetHash();
    const std::vector<unsigned char> fingerprint(digest.begin(), digest.begin() + 8);
    return registration.ToString() + "-" + HexStr(fingerprint) + ".journal";
}

} // namespace

bool Journal::VoteKey::operator<(const VoteKey& other) const
{
    if (height != other.height) return height < other.height;
    if (round != other.round) return round < other.round;
    return step < other.step;
}

Journal::Journal(const fs::path& file, const uint256& genesisIn, const uint256& registrationIn,
                 const mldsa44::PublicKey& operatorKeyIn, int leaseIn)
    : path(file), genesis(genesisIn), registration(registrationIn), operatorKey(operatorKeyIn),
      lease(leaseIn)
{
}

Journal::~Journal()
{
    if (file) fclose(file);
    io::Close(lease);
}

std::unique_ptr<Journal> Journal::Load(const fs::path& datadir, const uint256& genesis,
                                       const uint256& registration, const mldsa44::PublicKey& key,
                                       std::string& reason)
{
    return Open(datadir, genesis, registration, key, false, reason);
}

std::unique_ptr<Journal> Journal::Initialize(const fs::path& datadir, const uint256& genesis,
                                             const uint256& registration, const mldsa44::PublicKey& key,
                                             std::string& reason)
{
    return Open(datadir, genesis, registration, key, true, reason);
}

std::unique_ptr<Journal> Journal::Open(const fs::path& datadir, const uint256& genesis,
                                       const uint256& registration, const mldsa44::PublicKey& key,
                                       bool initialize, std::string& reason)
{
    reason.clear();
    if (genesis.IsNull() || registration.IsNull() ||
        std::all_of(key.begin(), key.end(), [](unsigned char c) { return c == 0; })) {
        reason = "pq-journal-invalid-identity";
        return nullptr;
    }
    try {
        const fs::path directory = datadir / "pqjournal";
        if (fs::is_symlink(directory)) {
            reason = "pq-journal-directory-symlink";
            return nullptr;
        }
        if (!fs::exists(directory)) {
            if (!initialize) {
                reason = "pq-journal-not-initialized";
                return nullptr;
            }
            if (!fs::create_directories(directory)) {
                reason = "pq-journal-directory-unavailable";
                return nullptr;
            }
            fs::permissions(directory, fs::owner_all);
        }
        if (!io::DirectorySafe(directory)) {
            reason = "pq-journal-directory-unavailable";
            return nullptr;
        }
        const fs::path file = directory / FileName(registration, key);
        fs::path lockPath = file;
        lockPath += ".lock";
        if (initialize && (fs::exists(file) || fs::is_symlink(file) ||
                           fs::exists(lockPath) || fs::is_symlink(lockPath))) {
            reason = "pq-journal-already-initialized-or-incomplete";
            return nullptr;
        }
        const int lease = io::Open(lockPath, initialize ? io::Mode::LOCK_CREATE : io::Mode::LOCK_EXISTING);
        if (lease < 0) {
            reason = "pq-journal-locked-or-unavailable";
            return nullptr;
        }
        auto journal = std::unique_ptr<Journal>(new Journal(file, genesis, registration, key, lease));
        // Sync the marker before any identity can sign, including a retry after
        // interrupted creation. It is deliberately never unlinked on failure.
        if (!io::Sync(lease) || !io::SyncParent(lockPath) || !io::SyncParent(directory)) {
            reason = "pq-journal-create-failed";
            return nullptr;
        }
        const int fd = io::Open(file, initialize ? io::Mode::CREATE : io::Mode::APPEND);
        if (fd < 0) {
            reason = "pq-journal-unreadable";
            return nullptr;
        }
        journal->file = io::Stream(fd, "a+b");
        if (!journal->file) {
            io::Close(fd);
            reason = "pq-journal-unreadable";
            return nullptr;
        }
        const auto prefix = HeaderPrefix(genesis, registration, key);
        journal->chainHash = HeaderHash(prefix);
        if (initialize) {
            std::vector<unsigned char> header(prefix);
            header.insert(header.end(), journal->chainHash.begin(), journal->chainHash.end());
            if (!io::WriteAll(fd, header.data(), header.size()) || !io::Sync(fd) || !io::SyncParent(file)) {
                reason = "pq-journal-create-failed";
                return nullptr;
            }
            return journal;
        }
        const int64_t size = io::Size(fd);
        if (size < int64_t(HEADER_SIZE) || size > int64_t(MAX_FILE_SIZE)) {
            reason = "pq-journal-unreadable";
            return nullptr;
        }
        std::vector<unsigned char> bytes(size);
        rewind(journal->file);
        if (fread(bytes.data(), 1, bytes.size(), journal->file) != bytes.size()) {
            reason = "pq-journal-unreadable";
            return nullptr;
        }
        const auto stored = bytes.data();
        if (std::mismatch(prefix.begin(), prefix.end(), stored).first != prefix.end()) {
            reason = "pq-journal-header-mismatch";
            return nullptr;
        }
        const uint256 storedDigest(std::vector<unsigned char>(stored + prefix.size(),
                                                               stored + HEADER_SIZE));
        if (storedDigest != HeaderHash(prefix)) {
            reason = "pq-journal-header-corrupt";
            return nullptr;
        }

        if (!journal->Replay(bytes, reason)) return nullptr;
        // A failed prior flush may leave complete records only in the OS cache.
        // Do not expose their cached signatures until durability is reestablished.
        if (!io::Sync(fd)) {
            reason = "pq-journal-recovery-flush-failed";
            return nullptr;
        }
        return journal;
    } catch (const std::exception&) {
        reason = "pq-journal-unreadable";
        return nullptr;
    }
}

bool Journal::Replay(const std::vector<unsigned char>& bytes, std::string& reason)
{
    const auto prefix = HeaderPrefix(genesis, registration, operatorKey);
    uint256 previous = HeaderHash(prefix);
    size_t offset = HEADER_SIZE;
    uint32_t finalized = 0;
    uint256 value, anchor;
    bool has = false;
    while (offset < bytes.size()) {
        const size_t remaining = bytes.size() - offset;
        const uint8_t type = bytes[offset];
        size_t size = RecordSize(type);
        if (size == 0) {
            reason = "pq-journal-corrupt-record";
            return false;
        }
        if (type == RECORD_PROOF) {
            if (remaining < 5) break;
            const uint32_t length = ReadLE32(bytes.data() + offset + 1);
            if (length < pqquorum::HEADER_SIZE || length > pqquorum::MAX_CERTIFICATE_SIZE) {
                reason = "pq-journal-corrupt-proof-length";
                return false;
            }
            if (remaining < 5 + HASH_SIZE) break;
            const uint256 lengthHash(std::vector<unsigned char>(bytes.begin() + offset + 5,
                                                                 bytes.begin() + offset + 5 + HASH_SIZE));
            if (ChainHash(previous, type, {bytes.begin() + offset + 1, bytes.begin() + offset + 5}) != lengthHash) {
                reason = "pq-journal-corrupt-proof-length-hash";
                return false;
            }
            size += length;
        }
        if (remaining < size) break; // trailing partial record: truncate below
        const std::vector<unsigned char> payload(bytes.begin() + offset + 1,
                                                 bytes.begin() + offset + size - HASH_SIZE);
        const uint256 stored(std::vector<unsigned char>(bytes.begin() + offset + size - HASH_SIZE,
                                                        bytes.begin() + offset + size));
        if (ChainHash(previous, type, payload) != stored) {
            reason = "pq-journal-corrupt-chain";
            return false;
        }
        CDataStream stream(SER_NETWORK, 0);
        stream.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        switch (type) {
        case RECORD_PROOF: {
            pqquorum::Certificate proof;
            if (!pqquorum::Decode({payload.data() + 4 + HASH_SIZE, payload.size() - 4 - HASH_SIZE}, proof) ||
                !CheckProof(proof, reason) || (has && proof.statement.height <= finalized)) {
                reason = "pq-journal-corrupt-proof";
                return false;
            }
            proofs[proof.statement.height] = std::move(proof);
            break;
        }
        case RECORD_VOTE: {
            VoteKey key;
            VoteRecord record;
            uint8_t step;
            stream >> key.height >> key.round >> step >> record.anchor >> record.committee >>
                record.value >> record.signature;
            key.step = static_cast<pqquorum::Purpose>(step);
            if (!ValidStep(key.step)) {
                reason = "pq-journal-corrupt-state";
                return false;
            }
            const auto existing = votes.find(key);
            if (existing != votes.end() && (existing->second.anchor != record.anchor ||
                                             existing->second.committee != record.committee ||
                                             existing->second.value != record.value)) {
                reason = "pq-journal-corrupt-state";
                return false;
            }
            votes[key] = record;
            break;
        }
        case RECORD_LOCK: {
            uint32_t height, round;
            uint256 lockValue;
            stream >> height >> round >> lockValue;
            const auto existing = locks.find(height);
            if (existing != locks.end()) {
                if (round < existing->second.round ||
                    (round == existing->second.round && lockValue != existing->second.value)) {
                    reason = "pq-journal-corrupt-state";
                    return false;
                }
                if (round == existing->second.round) break; // identical retry
            }
            locks[height] = {round, lockValue};
            break;
        }
        case RECORD_FINALIZED: {
            uint32_t height;
            uint256 finalValue, anchorID;
            stream >> height >> finalValue >> anchorID;
            if (finalValue.IsNull() || anchorID.IsNull() || (has && height <= finalized)) {
                reason = "pq-journal-corrupt-state";
                return false;
            }
            finalized = height;
            value = finalValue;
            anchor = anchorID;
            has = true;
            break;
        }
        default:
            reason = "pq-journal-corrupt-record";
            return false;
        }
        previous = ChainHash(previous, type, payload);
        offset += size;
    }
    finalizedHeight = finalized;
    finalizedValue = value;
    finalizedAnchor = anchor;
    hasFinalized = has;
    proofs.erase(proofs.begin(), proofs.upper_bound(finalized));
    chainHash = previous;
    if (offset != bytes.size()) {
        // Crash mid-append: truncate to the last complete record and fsync.
        if (!io::Truncate(io::Descriptor(file), offset) || !io::Sync(io::Descriptor(file))) {
            reason = "pq-journal-truncate-failed";
            return false;
        }
        LogPrintf("pqjournal: truncated trailing partial record in %s\n", path.string());
    }
    reason.clear();
    return true;
}

bool Journal::HasSpace(size_t bytes, std::string& reason)
{
    const int64_t size = file ? io::Size(io::Descriptor(file)) : -1;
    if (size < 0 || bytes > MAX_FILE_SIZE || uint64_t(size) > MAX_FILE_SIZE - bytes) {
        reason = "pq-journal-capacity-or-stat-failed";
        Poison();
        return false;
    }
    return true;
}

bool Journal::Append(uint8_t type, const std::vector<unsigned char>& payload, std::string& reason)
{
    if (poisoned || !file) {
        reason = "pq-journal-poisoned";
        return false;
    }
    const uint256 recordHash = ChainHash(chainHash, type, payload);
    std::vector<unsigned char> record;
    record.reserve(1 + payload.size() + HASH_SIZE);
    record.push_back(type);
    record.insert(record.end(), payload.begin(), payload.end());
    record.insert(record.end(), recordHash.begin(), recordHash.end());
    if (!HasSpace(record.size(), reason)) return false;
    if (!io::WriteAll(io::Descriptor(file), record.data(), record.size()) || !io::Sync(io::Descriptor(file))) {
        reason = "pq-journal-append-failed";
        Poison();
        return false;
    }
    chainHash = recordHash;
    reason.clear();
    return true;
}

void Journal::Poison()
{
    if (file) {
        fclose(file);
        file = nullptr;
    }
    poisoned = true;
}

bool Journal::GetVote(uint32_t height, uint32_t round, pqquorum::Purpose step,
                      uint256& value, Signature& signature) const
{
    LOCK(mutex);
    if (poisoned || !ValidStep(step)) return false;
    const auto it = votes.find({height, round, step});
    if (it == votes.end()) return false;
    value = it->second.value;
    signature = it->second.signature;
    return true;
}

bool Journal::GetOrSignVote(const pqquorum::Statement& statement, const Signer& signer,
                            Signature& signature, std::string& reason)
{
    LOCK(mutex);
    signature = {};
    if (poisoned) {
        reason = "pq-journal-poisoned";
        return false;
    }
    if (!ValidStep(statement.purpose) || statement.genesis != genesis) {
        reason = "pq-journal-invalid-statement";
        return false;
    }
    if (hasFinalized && statement.height <= finalizedHeight) {
        reason = "pq-journal-height-finalized";
        return false;
    }
    const VoteKey key{statement.height, statement.round, statement.purpose};
    const auto existing = votes.find(key);
    if (existing != votes.end()) {
        if (existing->second.anchor != statement.anchor ||
            existing->second.committee != statement.committee ||
            existing->second.value != statement.value) {
            reason = "pq-journal-conflicting-vote";
            return false;
        }
        signature = existing->second.signature; // retry reuse, never re-sign
        reason.clear();
        return true;
    }
    Signature fresh;
    if (!HasSpace(RecordSize(RECORD_VOTE), reason)) return false;
    if (!signer || !signer(statement, fresh)) {
        reason = "pq-journal-signer-refused";
        return false;
    }
    CDataStream stream(SER_NETWORK, 0);
    stream << statement.height << statement.round << static_cast<uint8_t>(statement.purpose) <<
        statement.anchor << statement.committee << statement.value << fresh; // fixed-size fields only: no length prefixes
    if (!Append(RECORD_VOTE, {stream.begin(), stream.end()}, reason)) return false;
    votes[key] = {statement.anchor, statement.committee, statement.value, fresh};
    signature = fresh;
    reason.clear();
    return true;
}

bool Journal::GetLock(uint32_t height, uint256& value, uint32_t& lockRound) const
{
    LOCK(mutex);
    if (poisoned) return false;
    const auto it = locks.find(height);
    if (it == locks.end()) return false;
    value = it->second.value;
    lockRound = it->second.round;
    return true;
}

bool Journal::RecordLock(uint32_t height, uint32_t lockRound, const uint256& value, std::string& reason)
{
    LOCK(mutex);
    if (poisoned) {
        reason = "pq-journal-poisoned";
        return false;
    }
    if (hasFinalized && height <= finalizedHeight) {
        reason = "pq-journal-height-finalized";
        return false;
    }
    const auto existing = locks.find(height);
    if (existing != locks.end()) {
        if (lockRound < existing->second.round ||
            (lockRound == existing->second.round && value != existing->second.value)) {
            reason = "pq-journal-conflicting-lock";
            return false;
        }
        if (lockRound == existing->second.round) {
            reason.clear(); // identical retry: durable no-op
            return true;
        }
    }
    CDataStream stream(SER_NETWORK, 0);
    stream << height << lockRound << value;
    if (!Append(RECORD_LOCK, {stream.begin(), stream.end()}, reason)) return false;
    locks[height] = {lockRound, value};
    reason.clear();
    return true;
}

bool Journal::GetFinalized(uint32_t& height, uint256& value, uint256& anchorID) const
{
    LOCK(mutex);
    if (poisoned || !hasFinalized) return false;
    height = finalizedHeight;
    value = finalizedValue;
    anchorID = finalizedAnchor;
    return true;
}

uint32_t Journal::FinalizedHeight() const
{
    LOCK(mutex);
    if (poisoned) return 0;
    return finalizedHeight;
}

bool Journal::HighestVoted(uint32_t height, pqquorum::Purpose step, uint32_t& round) const
{
    LOCK(mutex);
    if (poisoned) return false;
    if (!ValidStep(step)) return false;
    bool found = false;
    round = 0;
    for (auto it = votes.lower_bound({height, 0, step}); it != votes.end() && it->first.height == height; ++it) {
        if (it->first.step == step && (!found || it->first.round > round)) {
            round = it->first.round;
            found = true;
        }
    }
    return found;
}

bool Journal::GetProgress(uint32_t& finalized, uint32_t& votedHeight, uint32_t& votedRound) const
{
    LOCK(mutex);
    finalized = votedHeight = votedRound = 0;
    if (poisoned || !file) return false;
    if (hasFinalized) finalized = finalizedHeight;
    if (!votes.empty()) {
        votedHeight = votes.rbegin()->first.height;
        votedRound = votes.rbegin()->first.round;
    }
    return true;
}

bool Journal::CheckProof(const pqquorum::Certificate& proof, std::string& reason) const
{
    const auto& statement = proof.statement;
    if (statement.genesis != genesis || statement.purpose != pqquorum::Purpose::PREVOTE ||
        statement.height == 0 || statement.height <= finalizedHeight || statement.anchor.IsNull() ||
        statement.committee.IsNull() || proof.signatures.empty() || pqquorum::Encode(proof).empty()) {
        reason = "pq-journal-invalid-proof";
        return false;
    }
    const auto it = proofs.find(statement.height);
    if (it != proofs.end()) {
        const auto& old = it->second.statement;
        if (statement.anchor != old.anchor || statement.committee != old.committee ||
            statement.round < old.round || (statement.round == old.round && statement.value != old.value)) {
            reason = "pq-journal-conflicting-proof";
            return false;
        }
    }
    return true;
}

bool Journal::RecordProof(const pqquorum::Certificate& proof, std::string& reason)
{
    LOCK(mutex);
    if (poisoned) {
        reason = "pq-journal-poisoned";
        return false;
    }
    if (!CheckProof(proof, reason)) return false;
    const auto it = proofs.find(proof.statement.height);
    if (it != proofs.end() && it->second.statement.round == proof.statement.round &&
        it->second.signatures.size() >= proof.signatures.size()) {
        reason.clear();
        return true;
    }
    if (!Append(RECORD_PROOF, ProofPayload(proof, chainHash), reason)) return false;
    proofs[proof.statement.height] = proof;
    return true;
}

bool Journal::GetProof(uint32_t height, pqquorum::Certificate& proof) const
{
    LOCK(mutex);
    proof = {};
    if (poisoned || height <= finalizedHeight) return false;
    const auto it = proofs.find(height);
    if (it == proofs.end()) return false;
    proof = it->second;
    return true;
}

bool Journal::RecordFinalized(uint32_t height, const uint256& value, const uint256& anchorID, std::string& reason)
{
    LOCK(mutex);
    if (poisoned) {
        reason = "pq-journal-poisoned";
        return false;
    }
    if (value.IsNull() || anchorID.IsNull() || (hasFinalized && height <= finalizedHeight)) {
        reason = "pq-journal-invalid-finalized";
        return false;
    }
    CDataStream stream(SER_NETWORK, 0);
    stream << height << value << anchorID;
    if (!Append(RECORD_FINALIZED, {stream.begin(), stream.end()}, reason)) return false;
    finalizedHeight = height;
    finalizedValue = value;
    finalizedAnchor = anchorID;
    hasFinalized = true;
    // Drop live state at or below the finalized height; it can never be re-voted.
    votes.erase(votes.begin(), votes.upper_bound({height, UINT32_MAX, pqquorum::Purpose::PRECOMMIT}));
    locks.erase(locks.begin(), locks.upper_bound(height));
    proofs.erase(proofs.begin(), proofs.upper_bound(height));
    reason.clear();
    if (!Compact()) {
        reason = "pq-journal-compaction-failed";
        return false;
    }
    return true;
}

bool Journal::Compact()
{
    // Rewrite header + newest finalized record + live votes/locks, re-chained.
    // Atomic: write <file>.tmp, fsync, rename, fsync directory. Any failure
    // poisons this instance; only a verified reopen may use the durable path.
    try {
        fs::path temporary = path;
        temporary += ".tmp";
        const int fd = io::Open(temporary, io::Mode::REWRITE);
        FILE* handle = fd >= 0 ? io::Stream(fd, "wb") : nullptr;
        if (!handle) {
            if (fd >= 0) io::Close(fd);
            Poison();
            return false;
        }
        const auto fail = [&]() {
            if (handle) {
                fclose(handle);
                handle = nullptr;
            }
            boost::system::error_code ignored;
            fs::remove(temporary, ignored);
            Poison();
            return false;
        };
        const auto prefix = HeaderPrefix(genesis, registration, operatorKey);
        uint256 previous = HeaderHash(prefix);
        std::vector<unsigned char> content(prefix);
        content.insert(content.end(), previous.begin(), previous.end()); // full header incl. hash
        const auto appendRecord = [&](uint8_t type, const std::vector<unsigned char>& payload) {
            const uint256 recordHash = ChainHash(previous, type, payload);
            content.push_back(type);
            content.insert(content.end(), payload.begin(), payload.end());
            content.insert(content.end(), recordHash.begin(), recordHash.end());
            previous = recordHash;
        };
        {
            CDataStream stream(SER_NETWORK, 0);
            stream << finalizedHeight << finalizedValue << finalizedAnchor;
            appendRecord(RECORD_FINALIZED, {stream.begin(), stream.end()});
        }
        for (const auto& vote : votes) {
            CDataStream stream(SER_NETWORK, 0);
            stream << vote.first.height << vote.first.round << static_cast<uint8_t>(vote.first.step) <<
                vote.second.anchor << vote.second.committee << vote.second.value << vote.second.signature;
            appendRecord(RECORD_VOTE, {stream.begin(), stream.end()});
        }
        for (const auto& lock : locks) {
            CDataStream stream(SER_NETWORK, 0);
            stream << lock.first << lock.second.round << lock.second.value;
            appendRecord(RECORD_LOCK, {stream.begin(), stream.end()});
        }
        for (const auto& proof : proofs)
            appendRecord(RECORD_PROOF, ProofPayload(proof.second, previous));
        bool ok = io::WriteAll(io::Descriptor(handle), content.data(), content.size()) && io::Sync(io::Descriptor(handle));
        if (fclose(handle) != 0) ok = false;
        handle = nullptr;
        if (!ok) {
            return fail();
        }
        // Windows cannot replace this open append target. The independent
        // marker lease remains held, including on close/rename/reopen failure.
        const bool closed = fclose(file) == 0;
        file = nullptr;
        if (!closed) return fail();
        if (!io::Replace(temporary, path)) {
            return fail();
        }
        if (!io::SyncParent(path)) {
            Poison();
            return false;
        }
        chainHash = previous;
        const int appendFd = io::Open(path, io::Mode::APPEND);
        FILE* append = appendFd >= 0 ? io::Stream(appendFd, "a+b") : nullptr;
        if (!append) {
            if (appendFd >= 0) io::Close(appendFd);
            Poison();
            return false;
        }
        file = append;
        return true;
    } catch (const std::exception&) {
        Poison();
        return false;
    }
}
} // namespace pqjournal
