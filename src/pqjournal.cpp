// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqjournal.h>
#include <fs.h>
#include <hash.h>
#include <logging.h>
#include <streams.h>
#include <utilstrencodings.h>
#include <algorithm>
#include <cstdio>
#include <optional>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
constexpr uint8_t FORMAT_VERSION = 1;
// magic string carries a compact-size length prefix when serialized
constexpr size_t HEADER_SIZE = 1 + MAGIC_SIZE + 1 + 32 + 32 + mldsa44::PUBLIC_KEY_SIZE + 32;
constexpr size_t HASH_SIZE = 32;
constexpr uint8_t RECORD_VOTE = 1;
constexpr uint8_t RECORD_LOCK = 2;
constexpr uint8_t RECORD_FINALIZED = 3;
constexpr size_t MAX_FILE_SIZE = 1u << 30; // Far above any legal journal.

bool ValidStep(pqquorum::Purpose purpose)
{
    return purpose == pqquorum::Purpose::PREVOTE || purpose == pqquorum::Purpose::PRECOMMIT;
}

// Full record size: type byte + payload + trailing hash.
size_t RecordSize(uint8_t type)
{
    switch (type) {
    case RECORD_VOTE: return 1 + 4 + 4 + 1 + 32 + mldsa44::SIGNATURE_SIZE + HASH_SIZE;
    case RECORD_LOCK: return 1 + 4 + 4 + 32 + HASH_SIZE;
    case RECORD_FINALIZED: return 1 + 4 + 32 + 32 + HASH_SIZE;
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

bool WriteAll(int fd, const unsigned char* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::write(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += size_t(count);
    }
    return true;
}

bool SyncDirectory(const fs::path& file)
{
    const int fd = open(file.parent_path().native().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}
} // namespace

bool Journal::VoteKey::operator<(const VoteKey& other) const
{
    if (height != other.height) return height < other.height;
    if (round != other.round) return round < other.round;
    return step < other.step;
}

Journal::Journal(const fs::path& file, const uint256& genesisIn, const uint256& registrationIn,
                 const mldsa44::PublicKey& operatorKeyIn, FILE* append, const uint256& chainHashIn)
    : path(file), genesis(genesisIn), registration(registrationIn), operatorKey(operatorKeyIn),
      file(append), chainHash(chainHashIn)
{
}

Journal::~Journal()
{
    if (file) fclose(file);
}

std::unique_ptr<Journal> Journal::Load(const fs::path& datadir, const uint256& genesis,
                                       const uint256& registration, const mldsa44::PublicKey& key,
                                       std::string& reason)
{
    reason.clear();
    if (genesis.IsNull() || registration.IsNull() ||
        std::all_of(key.begin(), key.end(), [](unsigned char c) { return c == 0; })) {
        reason = "pq-journal-invalid-identity";
        return nullptr;
    }
    try {
        const fs::path directory = datadir / "pqjournal";
        if (!fs::exists(directory)) {
            if (!fs::create_directories(directory)) {
                reason = "pq-journal-directory-unavailable";
                return nullptr;
            }
            fs::permissions(directory, fs::owner_all);
        }
        const fs::path file = directory / FileName(registration, key);
        if (!fs::exists(file)) return Create(directory, file, genesis, registration, key, reason);

        std::optional<std::vector<unsigned char>> bytes;
        {
            FILE* handle = fopen(file.native().c_str(), "rb");
            if (!handle) {
                reason = "pq-journal-unreadable";
                return nullptr;
            }
            bytes.emplace();
            unsigned char buffer[65536];
            size_t count;
            while ((count = fread(buffer, 1, sizeof(buffer), handle)) > 0)
                bytes->insert(bytes->end(), buffer, buffer + count);
            const bool ok = ferror(handle) == 0;
            fclose(handle);
            if (!ok) bytes.reset();
        }
        if (!bytes || bytes->size() < HEADER_SIZE || bytes->size() > MAX_FILE_SIZE) {
            reason = "pq-journal-unreadable";
            return nullptr;
        }
        const auto stored = bytes->data();
        const auto prefix = HeaderPrefix(genesis, registration, key);
        if (std::mismatch(prefix.begin(), prefix.end(), stored).first != prefix.end()) {
            reason = "pq-journal-header-mismatch";
            return nullptr;
        }

        FILE* append = fopen(file.native().c_str(), "a+b"); // O_APPEND: writes always go to the end
        if (!append) {
            reason = "pq-journal-unreadable";
            return nullptr;
        }
        auto journal = std::unique_ptr<Journal>(
            new Journal(file, genesis, registration, key, append, HeaderHash(prefix)));
        if (!journal->Replay(*bytes, reason)) return nullptr;
        return journal;
    } catch (const std::exception&) {
        reason = "pq-journal-unreadable";
        return nullptr;
    }
}

std::unique_ptr<Journal> Journal::Create(const fs::path& directory, const fs::path& file,
                                         const uint256& genesis, const uint256& registration,
                                         const mldsa44::PublicKey& key, std::string& reason)
{
    const auto prefix = HeaderPrefix(genesis, registration, key);
    const uint256 digest = HeaderHash(prefix);
    std::vector<unsigned char> header(prefix);
    header.insert(header.end(), digest.begin(), digest.end());
    FILE* append = fopen(file.native().c_str(), "a+b");
    if (!append) {
        reason = "pq-journal-create-failed";
        return nullptr;
    }
    fs::permissions(file, fs::owner_read | fs::owner_write);
    if (!WriteAll(fileno(append), header.data(), header.size()) || fsync(fileno(append)) != 0 ||
        !SyncDirectory(file)) {
        fclose(append);
        reason = "pq-journal-create-failed";
        return nullptr;
    }
    reason.clear();
    return std::unique_ptr<Journal>(
        new Journal(file, genesis, registration, key, append, digest));
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
        const size_t size = RecordSize(type);
        if (size == 0) {
            reason = "pq-journal-corrupt-record";
            return false;
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
        case RECORD_VOTE: {
            VoteKey key;
            VoteRecord record;
            uint8_t step;
            stream >> key.height >> key.round >> step >> record.value >> record.signature;
            key.step = static_cast<pqquorum::Purpose>(step);
            if (!ValidStep(key.step)) {
                reason = "pq-journal-corrupt-state";
                return false;
            }
            const auto existing = votes.find(key);
            if (existing != votes.end() && existing->second.value != record.value) {
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
    chainHash = previous;
    if (offset != bytes.size()) {
        // Crash mid-append: truncate to the last complete record and fsync.
        if (ftruncate(fileno(file), offset) != 0 || fsync(fileno(file)) != 0) {
            reason = "pq-journal-truncate-failed";
            return false;
        }
        LogPrintf("pqjournal: truncated trailing partial record in %s\n", path.string());
    }
    reason.clear();
    return true;
}

bool Journal::Append(uint8_t type, const std::vector<unsigned char>& payload, std::string& reason)
{
    const uint256 recordHash = ChainHash(chainHash, type, payload);
    std::vector<unsigned char> record;
    record.reserve(1 + payload.size() + HASH_SIZE);
    record.push_back(type);
    record.insert(record.end(), payload.begin(), payload.end());
    record.insert(record.end(), recordHash.begin(), recordHash.end());
    if (!WriteAll(fileno(file), record.data(), record.size()) || fsync(fileno(file)) != 0) {
        reason = "pq-journal-append-failed";
        return false;
    }
    chainHash = recordHash;
    reason.clear();
    return true;
}

bool Journal::GetVote(uint32_t height, uint32_t round, pqquorum::Purpose step,
                      uint256& value, Signature& signature) const
{
    LOCK(mutex);
    if (!ValidStep(step)) return false;
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
        if (existing->second.value != statement.value) {
            reason = "pq-journal-conflicting-vote";
            return false;
        }
        signature = existing->second.signature; // retry reuse, never re-sign
        reason.clear();
        return true;
    }
    Signature fresh;
    if (!signer || !signer(statement, fresh)) {
        reason = "pq-journal-signer-refused";
        return false;
    }
    CDataStream stream(SER_NETWORK, 0);
    stream << statement.height << statement.round << static_cast<uint8_t>(statement.purpose) <<
        statement.value << fresh; // fixed-size fields only: no length prefixes
    if (!Append(RECORD_VOTE, {stream.begin(), stream.end()}, reason)) return false;
    votes[key] = {statement.value, fresh};
    signature = fresh;
    reason.clear();
    return true;
}

bool Journal::GetLock(uint32_t height, uint256& value, uint32_t& lockRound) const
{
    LOCK(mutex);
    const auto it = locks.find(height);
    if (it == locks.end()) return false;
    value = it->second.value;
    lockRound = it->second.round;
    return true;
}

bool Journal::RecordLock(uint32_t height, uint32_t lockRound, const uint256& value, std::string& reason)
{
    LOCK(mutex);
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
    if (!hasFinalized) return false;
    height = finalizedHeight;
    value = finalizedValue;
    anchorID = finalizedAnchor;
    return true;
}

uint32_t Journal::FinalizedHeight() const
{
    LOCK(mutex);
    return finalizedHeight;
}

bool Journal::HighestVoted(uint32_t height, pqquorum::Purpose step, uint32_t& round) const
{
    LOCK(mutex);
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

bool Journal::RecordFinalized(uint32_t height, const uint256& value, const uint256& anchorID, std::string& reason)
{
    LOCK(mutex);
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
    reason.clear();
    if (!Compact()) LogPrintf("pqjournal: compaction failed for %s; append-only file kept\n", path.string());
    return true;
}

bool Journal::Compact()
{
    // Rewrite header + newest finalized record + live votes/locks, re-chained.
    // Atomic: write <file>.tmp, fsync, rename, fsync directory. Any failure
    // keeps the existing append-only file intact; state is unchanged.
    try {
        const fs::path temporary = path.native() + ".tmp";
        FILE* handle = fopen(temporary.native().c_str(), "wb");
        if (!handle) return false;
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
                vote.second.value << vote.second.signature;
            appendRecord(RECORD_VOTE, {stream.begin(), stream.end()});
        }
        for (const auto& lock : locks) {
            CDataStream stream(SER_NETWORK, 0);
            stream << lock.first << lock.second.round << lock.second.value;
            appendRecord(RECORD_LOCK, {stream.begin(), stream.end()});
        }
        bool ok = WriteAll(fileno(handle), content.data(), content.size()) && fsync(fileno(handle)) == 0;
        if (fclose(handle) != 0) ok = false;
        if (!ok) {
            boost::system::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
        boost::system::error_code error;
        fs::rename(temporary, path, error);
        if (error) {
            boost::system::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
        fs::permissions(path, fs::owner_read | fs::owner_write);
        if (!SyncDirectory(path)) return false;
        chainHash = previous;
        FILE* append = fopen(path.native().c_str(), "a+b"); // O_APPEND handle on the compacted file
        if (!append) return false;
        fclose(file);
        file = append;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace pqjournal
