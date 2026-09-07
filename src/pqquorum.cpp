// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqquorum.h>
#include <hash.h>
#include <streams.h>
#include <algorithm>
#include <set>

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
