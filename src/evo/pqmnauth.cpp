// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <evo/pqmnauth.h>
#include <streams.h>
#include <validation.h>
#include <wallet/pqkey.h>
#include <algorithm>

namespace pqmnauth {
std::unique_ptr<LocalOperator> LocalOperator::Load(const fs::path& directory, const CChainParams& params,
                                                 const uint256& id, std::string& reason)
{
    reason = "PQ operator credentials require regtest PQ masternode activation";
    const int first = params.GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_MASTERNODES].nActivationHeight;
    if (!pq::MasternodesActive(params, first)) return nullptr;
    if (id.IsNull()) { reason = "PQ operator registration must be nonzero"; return nullptr; }
    auto result = std::unique_ptr<LocalOperator>(new LocalOperator(id));
    if (!pqwallet::LoadOperatorCredentials(directory, params.NetworkIDString(),
                                         params.GetConsensus().hashGenesisBlock, result->key, reason)) return nullptr;
    return result;
}

namespace {
bool Fail(std::string& reason, const char* error) { reason = error; return false; }
bool ReadOperator(pqmn::Index& index, const uint256& id, uint32_t height,
                  uint32_t confirmations, pqmn::Record& record, std::string& reason)
{
    try {
        if (!index.Get(id, record)) return Fail(reason, "pq-auth-unknown-registration");
    } catch (const std::exception&) {
        return Fail(reason, "pq-auth-registry-unavailable");
    }
    if (record.revoked || !record.MatureAt(height, confirmations))
        return Fail(reason, "pq-auth-ineligible-registration");
    return true;
}
}

Span<const unsigned char> Context()
{
    static const unsigned char context[] = "OLC/PQ/ML-DSA-44/peer-auth/v1";
    return {context, sizeof(context) - 1};
}

std::vector<unsigned char> Message(const Transcript& transcript, const uint256& registration,
                                   const mldsa44::PublicKey& operatorKey)
{
    if (transcript.genesis.IsNull() || registration.IsNull() || transcript.initiatorChallenge.IsNull() ||
        transcript.responderChallenge.IsNull() || transcript.initiatorChallenge == transcript.responderChallenge ||
        std::all_of(operatorKey.begin(), operatorKey.end(), [](unsigned char c) { return c == 0; })) return {};
    CDataStream stream(SER_NETWORK, 0);
    stream << uint8_t{1} << transcript.genesis << registration << operatorKey <<
        transcript.initiatorChallenge << transcript.responderChallenge << uint8_t(transcript.signerIsInitiator);
    return {stream.begin(), stream.end()};
}

std::vector<unsigned char> Encode(const Proof& proof)
{
    if (proof.registration.IsNull()) return {};
    CDataStream stream(SER_NETWORK, 0);
    stream << uint8_t{1} << proof.registration << proof.signature;
    return {stream.begin(), stream.end()};
}

bool LocalOperator::SignProof(const Transcript& transcript, std::vector<unsigned char>& output, std::string& reason) const
{
    AssertLockHeld(cs_main);
    output.clear(); reason.clear();
    const auto& params = Params();
    if (!pq::MasternodesActive(params, chainActive.Height()) || !evoDb ||
        transcript.genesis != params.GetConsensus().hashGenesisBlock)
        return Fail(reason, "pq-auth-inactive-network");
    try {
        pqmn::Index index(*evoDb, params);
        pqmn::Record record;
        if (!index.MatchesChainTip(chainActive.Tip()) ||
            !ReadOperator(index, registration, chainActive.Height(), params.GetConsensus().MasternodeCollateralMinConf(), record, reason) ||
            record.operatorKey != key.GetPublicKey()) return Fail(reason, "pq-auth-operator-not-current");
        const auto message = Message(transcript, registration, key.GetPublicKey());
        std::vector<unsigned char> signature;
        if (message.empty() || !key.Sign(message, Context(), signature) || signature.size() != mldsa44::SIGNATURE_SIZE)
            return Fail(reason, "pq-auth-signing-failed");
        Proof proof; proof.registration = registration;
        std::copy(signature.begin(), signature.end(), proof.signature.begin());
        output = Encode(proof);
        return true;
    } catch (const std::exception&) {
        return Fail(reason, "pq-auth-registry-unavailable");
    }
}

bool Decode(Span<const unsigned char> bytes, Proof& proof)
{
    proof = {};
    if (bytes.size() != PROOF_SIZE || !bytes.data()) return false;
    const char* begin = reinterpret_cast<const char*>(bytes.data());
    CDataStream stream(begin, begin + bytes.size(), SER_NETWORK, 0);
    uint8_t version; Proof decoded;
    stream >> version >> decoded.registration >> decoded.signature;
    if (version != 1 || decoded.registration.IsNull()) return false;
    proof = decoded;
    return true;
}

bool Session::Authenticate(Span<const unsigned char> bytes, pqmn::Index& index,
                           uint32_t height, uint32_t confirmations, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear(); registration.SetNull(); operatorKey = {};
    if (attempted) return Fail(reason, "pq-auth-already-attempted");
    attempted = true;
    if (transcript.genesis != index.Genesis()) return Fail(reason, "pq-auth-wrong-network");
    Proof proof; pqmn::Record record;
    if (!Decode(bytes, proof)) return Fail(reason, "pq-auth-malformed-proof");
    if (!ReadOperator(index, proof.registration, height, confirmations, record, reason)) return false;
    const auto message = Message(transcript, proof.registration, record.operatorKey);
    if (message.empty()) return Fail(reason, "pq-auth-invalid-transcript");
    if (!mldsa44::Verify(record.operatorKey, message, Context(), proof.signature))
        return Fail(reason, "pq-auth-invalid-signature");
    registration = proof.registration; operatorKey = record.operatorKey;
    return true;
}

uint256 Session::Current(pqmn::Index& index, uint32_t height, uint32_t confirmations, std::string& reason)
{
    AssertLockHeld(cs_main);
    reason.clear();
    if (registration.IsNull()) { reason = "pq-auth-not-authenticated"; return {}; }
    if (transcript.genesis != index.Genesis()) {
        reason = "pq-auth-wrong-network";
        registration.SetNull(); operatorKey = {};
        return {};
    }
    pqmn::Record record;
    if (!ReadOperator(index, registration, height, confirmations, record, reason) || record.operatorKey != operatorKey) {
        if (reason.empty()) reason = "pq-auth-operator-changed";
        registration.SetNull(); operatorKey = {};
    }
    return registration;
}
}
