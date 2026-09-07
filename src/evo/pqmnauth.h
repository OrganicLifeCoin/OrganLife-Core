// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_EVO_PQMNAUTH_H
#define ORGANICLIFE_EVO_PQMNAUTH_H

#include <evo/pqmasternode.h>
#include <fs.h>
#include <memory>

// Registered peer identity only: no connection privilege, service or finality authority.
namespace pqmnauth {
struct Transcript;
// Loading is not registry readiness. Only the typed, current-registry-checked
// peer proof signer is exposed; no key export or arbitrary signing.
class LocalOperator {
    const uint256 registration;
    mldsa44::Key key;
    explicit LocalOperator(const uint256& id) : registration(id) {}
public:
    static std::unique_ptr<LocalOperator> Load(const fs::path& directory, const CChainParams& params,
                                              const uint256& id, std::string& reason);
    const uint256& Registration() const { return registration; }
    const mldsa44::PublicKey& PublicKey() const { return key.GetPublicKey(); }
    // Peer authentication only, never arbitrary messages or finality votes.
    // Caller holds cs_main; checks active/current registry and key on every use.
    bool SignProof(const Transcript& transcript, std::vector<unsigned char>& proof, std::string& reason) const;
};

constexpr size_t PROOF_SIZE = 1 + 32 + mldsa44::SIGNATURE_SIZE;
struct Transcript {
    uint256 genesis, initiatorChallenge, responderChallenge;
    bool signerIsInitiator{false};
};
struct Proof {
    uint256 registration;
    pqmn::Signature signature{};
};
Span<const unsigned char> Context();
std::vector<unsigned char> Message(const Transcript& transcript, const uint256& registration,
                                   const mldsa44::PublicKey& operatorKey);
std::vector<unsigned char> Encode(const Proof& proof);
bool Decode(Span<const unsigned char> bytes, Proof& proof);

// Caller owns one instance per connection and serializes all access. Challenges
// come from the local handshake, never the proof; generate fresh random values.
// Caller holds cs_main and supplies the current confirmed registry and policy.
// Key control is NOT encrypted/channel-bound transport or evidence of service.
class Session {
    const Transcript transcript;
    bool attempted{false};
    uint256 registration;
    mldsa44::PublicKey operatorKey{};
public:
    explicit Session(const Transcript& transcript) : transcript(transcript) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Even malformed/unknown-identity attempts exhaust the connection's budget.
    bool Authenticate(Span<const unsigned char> bytes, pqmn::Index& index,
                      uint32_t height, uint32_t confirmations, std::string& reason);
    // Recheck before use; invalidation is permanent even if a later reorg restores it.
    uint256 Current(pqmn::Index& index, uint32_t height, uint32_t confirmations, std::string& reason);
};
}
#endif
