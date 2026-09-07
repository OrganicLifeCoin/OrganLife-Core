// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqquorum.h>
#include <hash.h>
#include <streams.h>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace pqquorum {
namespace {
constexpr uint8_t VERSION = 1;
bool ValidStatement(const Statement& statement)
{
    if (statement.genesis.IsNull() || statement.anchor.IsNull() ||
        statement.committee.IsNull() || statement.height == 0) return false;
    switch (statement.purpose) {
    case Purpose::PREVOTE:
    case Purpose::PRECOMMIT:
        return true;
    case Purpose::SERVICE:
    case Purpose::HANDOFF:
        return !statement.value.IsNull();
    }
    return false;
}

void WriteStatement(CDataStream& stream, const Statement& statement)
{
    stream << VERSION << static_cast<uint8_t>(statement.purpose) << statement.genesis <<
        statement.anchor << statement.committee << statement.height << statement.round << statement.value;
}

bool CanonicalSigners(const std::vector<Signature>& signatures)
{
    if (signatures.empty() || signatures.size() > MAX_MEMBERS) return false;
    for (size_t i = 0; i < signatures.size(); ++i) {
        if (signatures[i].member >= MAX_MEMBERS ||
            (i && signatures[i - 1].member >= signatures[i].member)) return false;
    }
    return true;
}
}

RoundState::RoundState(const uint256& genesis, const uint256& anchor, uint32_t height,
                       const std::vector<Member>& snapshot) : members(snapshot)
{
    current.genesis = genesis;
    current.anchor = anchor;
    current.committee = Commitment(members);
    current.height = height;
    if (!ValidStatement(current)) throw std::invalid_argument("Invalid PQ finality context");
}

std::unique_ptr<RoundState> RoundState::Restore(const uint256& genesis, const uint256& anchor,
                                                uint32_t height, const std::vector<Member>& snapshot,
                                                uint32_t round, const uint256& locked, uint32_t lockRound,
                                                const uint256& prevote, const uint256& precommit,
                                                std::string& reason)
{
    reason.clear();
    try {
        std::unique_ptr<RoundState> state(new RoundState(genesis, anchor, height, snapshot));
        state->current.round = round;
        state->locked = locked;
        state->lockRound = lockRound;
        if (!prevote.IsNull()) {
            state->prevote = prevote;
            state->prevoted = true;
        }
        if (!precommit.IsNull()) {
            state->precommit = precommit;
            state->precommitted = true;
        }
        return state;
    } catch (const std::exception&) {
        reason = "bad-pq-restore-context";
        return nullptr;
    }
}

bool RoundState::Advance(uint32_t round)
{
    if (!committed.IsNull() || round <= current.round) return false;
    current.round = round;
    prevoted = precommitted = false;
    prevote.SetNull(); precommit.SetNull();
    // Advancing time/round does not release a quorum lock.
    return true;
}

bool RoundState::Prevote(const uint256& proposal, const Certificate* proof,
                         Statement& vote, std::string& reason)
{
    vote = {}; reason.clear();
    if (!committed.IsNull() || precommitted) { reason = "bad-pq-vote-step"; return false; }
    bool unlock = false;
    if (proof) {
        Statement expected = current;
        expected.round = proof->statement.round;
        expected.value = proof->statement.value;
        if (expected.round >= current.round) {
            reason = "bad-pq-unlock-proof"; return false;
        }
        if (!Verify(*proof, expected, members, reason)) return false;
        unlock = !locked.IsNull() && expected.value != locked && expected.round > lockRound;
    }
    const uint256 value = locked.IsNull() || unlock ? proposal : locked;
    if (prevoted && value != prevote) { reason = "bad-pq-double-prevote"; return false; }
    if (!prevoted) {
        if (unlock) locked.SetNull();
        prevote = value;
        prevoted = true;
    }
    vote = current; vote.value = prevote;
    return true;
}

bool RoundState::Precommit(const Certificate* proof, const uint256& validatedBlock,
                           Statement& vote, std::string& reason)
{
    vote = {}; reason.clear();
    if (!committed.IsNull()) { reason = "bad-pq-vote-step"; return false; }
    uint256 value;
    if (proof) {
        Statement expected = current;
        expected.value = proof->statement.value;
        if (!expected.value.IsNull() && expected.value != validatedBlock) {
            reason = "bad-pq-unvalidated-block"; return false;
        }
        if (!Verify(*proof, expected, members, reason)) return false;
        value = expected.value;
    }
    if (precommitted && value != precommit) { reason = "bad-pq-double-precommit"; return false; }
    if (!precommitted) {
        if (proof) { locked = value; lockRound = current.round; }
        precommit = value;
        precommitted = true;
    }
    vote = current; vote.purpose = Purpose::PRECOMMIT; vote.value = precommit;
    return true;
}

bool RoundState::Commit(const Certificate& proof, const uint256& validatedBlock,
                        uint256& finalized, std::string& reason)
{
    finalized.SetNull(); reason.clear();
    Statement expected = current;
    expected.purpose = Purpose::PRECOMMIT;
    expected.round = proof.statement.round;
    expected.value = proof.statement.value;
    if (expected.value != validatedBlock) { reason = "bad-pq-unvalidated-block"; return false; }
    if (expected.value.IsNull() || (!committed.IsNull() && committed != expected.value)) {
        reason = "bad-pq-finality-value"; return false;
    }
    if (!Verify(proof, expected, members, reason)) return false;
    committed = expected.value;
    finalized = committed;
    return true;
}

size_t Threshold(size_t members)
{
    return members < MIN_MEMBERS || members > MAX_MEMBERS ? 0 : (2 * members) / 3 + 1;
}

uint256 Commitment(const std::vector<Member>& members)
{
    if (!Threshold(members.size())) return {};
    std::set<uint256> registrations;
    std::set<mldsa44::PublicKey> keys;
    CHashWriter hash(SER_GETHASH, 0);
    hash << std::string("OLC/PQ/committee/v1") << static_cast<uint16_t>(members.size());
    for (const auto& member : members) {
        if (member.registration.IsNull() || !registrations.insert(member.registration).second ||
            !keys.insert(member.operator_key).second ||
            std::all_of(member.operator_key.begin(), member.operator_key.end(), [](unsigned char c) { return c == 0; })) return {};
        hash << member.registration << member.operator_key;
    }
    return hash.GetHash();
}

Span<const unsigned char> Context()
{
    static const unsigned char context[] = "OLC/PQ/ML-DSA-44/quorum/v1";
    return {context, sizeof(context) - 1};
}

std::vector<unsigned char> Message(const Statement& statement)
{
    if (!ValidStatement(statement)) return {};
    CDataStream stream(SER_NETWORK, 0);
    WriteStatement(stream, statement);
    return {stream.begin(), stream.end()};
}

std::vector<unsigned char> Encode(const Certificate& certificate)
{
    if (!ValidStatement(certificate.statement) || !CanonicalSigners(certificate.signatures)) return {};
    CDataStream stream(SER_NETWORK, 0);
    WriteStatement(stream, certificate.statement);
    stream << static_cast<uint16_t>(certificate.signatures.size());
    for (const auto& signature : certificate.signatures) stream << signature.member << signature.bytes;
    return {stream.begin(), stream.end()};
}

bool Decode(Span<const unsigned char> bytes, Certificate& certificate)
{
    certificate = {};
    if (bytes.size() < HEADER_SIZE || bytes.size() > MAX_CERTIFICATE_SIZE || !bytes.data()) return false;
    const char* begin = reinterpret_cast<const char*>(bytes.data());
    CDataStream stream(begin, begin + bytes.size(), SER_NETWORK, 0);
    Certificate decoded;
    uint8_t version, purpose;
    uint16_t count;
    try {
        stream >> version >> purpose >> decoded.statement.genesis >> decoded.statement.anchor >>
            decoded.statement.committee >> decoded.statement.height >> decoded.statement.round >> decoded.statement.value >> count;
        decoded.statement.purpose = static_cast<Purpose>(purpose);
        if (version != VERSION || !ValidStatement(decoded.statement) || count == 0 || count > MAX_MEMBERS ||
            stream.size() != size_t(count) * SIGNATURE_RECORD_SIZE) return false;
        decoded.signatures.resize(count);
        for (auto& signature : decoded.signatures) stream >> signature.member >> signature.bytes;
    } catch (const std::ios_base::failure&) {
        return false;
    }
    if (!stream.empty() || !CanonicalSigners(decoded.signatures)) return false;
    certificate = std::move(decoded);
    return true;
}

bool Verify(const Certificate& certificate, const Statement& expected,
            const std::vector<Member>& members, std::string& reason)
{
    reason.clear();
    const auto fail = [&](const char* message) { reason = message; return false; };
    const size_t threshold = Threshold(members.size());
    if (!threshold || certificate.signatures.size() < threshold ||
        certificate.signatures.size() > members.size() || !CanonicalSigners(certificate.signatures))
        return fail("bad-pq-quorum-signers");
    if (!ValidStatement(expected) || Message(certificate.statement) != Message(expected))
        return fail("bad-pq-quorum-statement");
    const uint256 commitment = Commitment(members);
    if (commitment.IsNull() || commitment != expected.committee) return fail("bad-pq-quorum-committee");
    const auto message = Message(expected);
    // Validate every index before any signature work; no attacker-controlled trust root.
    for (const auto& signature : certificate.signatures) {
        if (signature.member >= members.size()) return fail("bad-pq-quorum-member");
    }
    for (const auto& signature : certificate.signatures) {
        if (!mldsa44::Verify(members[signature.member].operator_key, message, Context(), signature.bytes))
            return fail("bad-pq-quorum-signature");
    }
    return true;
}
}
