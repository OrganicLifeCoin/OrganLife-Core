// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqtransaction.h>
#include <chainparams.h>
#include <streams.h>
#include <algorithm>

namespace pq {
bool IsMasternode(const CTransaction& tx)
{
    return tx.nType == CTransaction::PQ && tx.extraPayload && tx.extraPayload->size() >= 2 &&
           (*tx.extraPayload)[1] == MASTERNODE;
}
namespace {
bool Fail(std::string& reason, const char* message) { reason = message; return false; }
bool HasData(uint8_t mode) { return IsGovernanceMode(mode) || mode == MASTERNODE || mode == FINALITY; }
size_t DataLimit(uint8_t mode)
{
    return mode == MASTERNODE ? MAX_MASTERNODE_DATA_SIZE :
           mode == FINALITY ? pqquorum::MAX_CERTIFICATE_SIZE : MAX_GOVERNANCE_DATA_SIZE;
}
bool ValidCount(uint8_t mode, size_t count)
{
    return ((mode == TRANSFER || HasData(mode)) && count >= 1 && count <= MAX_INPUTS) ||
           (mode == STAKE && count == 1) || (mode == FINALITY && count == 0);
}
} // namespace

bool IsGovernanceMode(uint8_t mode)
{
    return mode == GOVERNANCE_PROPOSAL || mode == GOVERNANCE_LOCK || mode == GOVERNANCE_CAST;
}

bool IsFinality(const CTransaction& tx)
{
    return tx.nType == CTransaction::PQ && tx.IsCoinBase() && tx.extraPayload &&
           tx.extraPayload->size() >= 2 && (*tx.extraPayload)[1] == FINALITY;
}

bool HasMarker(const CScript& script) { return !script.empty() && script[0] == OP_INVALIDOPCODE; }

CScript GetScript(const KeyID& id)
{
    return CScript() << OP_INVALIDOPCODE << OP_1 << std::vector<unsigned char>(id.begin(), id.end());
}

bool ExtractID(const CScript& script, KeyID& id)
{
    id = {};
    if (script.size() != 35 || script[0] != OP_INVALIDOPCODE || script[1] != OP_1 || script[2] != 32) return false;
    std::copy(script.begin() + 3, script.end(), id.begin());
    return true;
}

std::vector<unsigned char> EncodePayload(const Payload& payload)
{
    if (!ValidCount(payload.mode, payload.authorizations.size()) ||
        (HasData(payload.mode) != !payload.data.empty()) ||
        payload.data.size() > DataLimit(payload.mode)) return {};
    CDataStream bytes(SER_NETWORK, 0);
    bytes << uint8_t{1} << payload.mode << uint8_t(payload.authorizations.size());
    for (const auto& auth : payload.authorizations) bytes << auth.public_key << auth.signature;
    if (HasData(payload.mode)) bytes << payload.data;
    return {bytes.begin(), bytes.end()};
}

bool DecodePayload(const CTransaction& tx, Payload& payload)
{
    payload = {};
    if (tx.nType != CTransaction::PQ || tx.nVersion != 3 || !tx.extraPayload) return false;
    const auto& bytes = *tx.extraPayload;
    if (bytes.size() < 3 || bytes[0] != 1 || !ValidCount(bytes[1], bytes[2])) return false;
    const size_t authorization_size = 3 + AUTH_SIZE * bytes[2];
    if ((!HasData(bytes[1]) && bytes.size() != authorization_size) ||
        (HasData(bytes[1]) && (bytes.size() <= authorization_size ||
         bytes.size() > authorization_size + DataLimit(bytes[1]) + 3))) return false;
    Payload decoded;
    decoded.mode = bytes[1];
    decoded.authorizations.resize(bytes[2]);
    auto it = bytes.begin() + 3;
    for (auto& auth : decoded.authorizations) {
        std::copy_n(it, auth.public_key.size(), auth.public_key.begin()); it += auth.public_key.size();
        std::copy_n(it, auth.signature.size(), auth.signature.begin()); it += auth.signature.size();
    }
    if (HasData(decoded.mode)) {
        try {
            const std::vector<unsigned char> encoded_data(it, bytes.end());
            CDataStream data_stream(encoded_data, SER_NETWORK, 0);
            const uint64_t size = ReadCompactSize(data_stream);
            if (size == 0 || size > DataLimit(decoded.mode) || size != data_stream.size()) return false;
            decoded.data.resize(size);
            data_stream.read(reinterpret_cast<char*>(decoded.data.data()), size);
        } catch (const std::exception&) {
            return false;
        }
    }
    payload = std::move(decoded);
    return true;
}

bool CheckStructure(const CTransaction& tx, const CChainParams& params, std::string& reason)
{
    reason.clear();
    if (tx.nType != CTransaction::PQ) return true;
    if (!params.IsTestChain()) return Fail(reason, "bad-pq-network");
    Payload payload;
    if (tx.nVersion != 3 || tx.sapData || !DecodePayload(tx, payload)) return Fail(reason, "bad-pq-payload");
    // A finality commit certificate is carried ONLY in the coinbase envelope.
    if (tx.IsCoinBase()) {
        if (payload.mode != FINALITY || !payload.authorizations.empty())
            return Fail(reason, "bad-pq-generation");
        return true;
    }
    if (payload.mode == FINALITY) return Fail(reason, "bad-pq-finality-not-coinbase");
    const bool stake = tx.IsCoinStake();
    if ((payload.mode == STAKE) != stake) return Fail(reason, "bad-pq-mode");
    const size_t sizeLimit = payload.mode == MASTERNODE ? MAX_MASTERNODE_TX_SIZE : MAX_TX_SIZE;
    if (tx.vin.empty() || tx.vout.empty() || tx.GetTotalSize() > sizeLimit) return Fail(reason, "bad-pq-size");
    if ((stake && (tx.vin.size() != 1 || tx.vout.size() < 2 || tx.vout.size() > 3 || !tx.vout[0].IsEmpty())) ||
        (!stake && tx.vout.size() > 2)) return Fail(reason, "bad-pq-size");
    if (tx.vin.size() > MAX_INPUTS || payload.authorizations.size() != tx.vin.size())
        return Fail(reason, "bad-pq-input-count");
    for (const auto& input : tx.vin)
        if (!input.scriptSig.empty()) return Fail(reason, "bad-pq-scriptsig");
    for (size_t i = stake ? 1 : 0; i < tx.vout.size(); ++i) {
        KeyID id;
        if (!ExtractID(tx.vout[i].scriptPubKey, id)) return Fail(reason, "bad-pq-output");
    }
    return true;
}

bool PaymentsActive(const CChainParams& params, int height)
{
    return height >= 0 && params.IsTestChain() &&
        params.GetConsensus().NetworkUpgradeActive(height, Consensus::UPGRADE_PQ);
}

bool MasternodesActive(const CChainParams& params, int height)
{
    const int first = params.GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_MASTERNODES].nActivationHeight;
    return params.IsRegTestNet() && first > 0 && height >= first && PaymentsActive(params, first);
}

bool CheckContext(const CTransaction& tx, const CChainParams& params, int height, std::string& reason)
{
    reason.clear();
    if (IsMasternode(tx) && !MasternodesActive(params, height))
        return Fail(reason, "bad-pq-masternode-not-active");
    // The pre-launch genesis coinbase is fixed by the network identity and is
    // never spendable. It predates PQ activation even on always-active regtest.
    if (height == 0 && tx.IsCoinBase()) return true;
    if (tx.nType == CTransaction::PQ && !PaymentsActive(params, height))
        return Fail(reason, "bad-pq-not-active");
    if (PaymentsActive(params, height)) {
        if (tx.IsCoinBase()) {
            for (const auto& out : tx.vout) {
                KeyID id;
                if (!out.IsEmpty() && !ExtractID(out.scriptPubKey, id))
                    return Fail(reason, "bad-pq-only-output");
            }
            return true;
        }
        if (tx.nType != CTransaction::PQ)
            return Fail(reason, tx.IsCoinStake() ? "bad-pq-only-stake" : "bad-pq-only-transaction");
        Payload payload;
        if (!DecodePayload(tx, payload) ||
            (tx.IsCoinStake() ? payload.mode != STAKE :
             (payload.mode != TRANSFER && !IsGovernanceMode(payload.mode) && payload.mode != MASTERNODE)))
            return Fail(reason, tx.IsCoinStake() ? "bad-pq-only-stake" : "bad-pq-only-transaction");
    }
    return true;
}

Optional<Span<const unsigned char>> SignatureContext(const std::string& network)
{
    static const unsigned char regtest[] = "OLC/PQ/ML-DSA-44/regtest/tx/v1";
    static const unsigned char testnet[] = "OLC/PQ/ML-DSA-44/testnet/tx/v1";
    if (network == "regtest") return Span<const unsigned char>{regtest, sizeof(regtest) - 1};
    if (network == "test") return Span<const unsigned char>{testnet, sizeof(testnet) - 1};
    return nullopt;
}

Optional<Span<const unsigned char>> BlockSignatureContext(const std::string& network)
{
    static const unsigned char regtest[] = "OLC/PQ/ML-DSA-44/regtest/block/v1";
    static const unsigned char testnet[] = "OLC/PQ/ML-DSA-44/testnet/block/v1";
    if (network == "regtest") return Span<const unsigned char>{regtest, sizeof(regtest) - 1};
    if (network == "test") return Span<const unsigned char>{testnet, sizeof(testnet) - 1};
    return nullopt;
}

Optional<Span<const unsigned char>> GovernanceSignatureContext(const std::string& network)
{
    static const unsigned char regtest[] = "OLC/PQ/ML-DSA-44/regtest/governance/v1";
    static const unsigned char testnet[] = "OLC/PQ/ML-DSA-44/testnet/governance/v1";
    if (network == "regtest") return Span<const unsigned char>{regtest, sizeof(regtest) - 1};
    if (network == "test") return Span<const unsigned char>{testnet, sizeof(testnet) - 1};
    return nullopt;
}

std::vector<unsigned char> SignatureMessage(const CTransaction& tx, const std::vector<CTxOut>& prevouts,
                                          const Payload& payload, const uint256& genesis, uint32_t input)
{
    if (tx.nType != CTransaction::PQ || tx.nVersion != 3 || tx.sapData ||
        (payload.mode != TRANSFER && payload.mode != STAKE && !HasData(payload.mode)) ||
        tx.vin.empty() || tx.vin.size() > MAX_INPUTS || tx.vout.empty() || tx.vout.size() > 3 ||
        prevouts.size() != tx.vin.size() || payload.authorizations.size() != tx.vin.size() || input >= tx.vin.size()) return {};
    const bool stake = payload.mode == STAKE;
    if ((!stake && tx.vout.size() > 2) || stake != tx.IsCoinStake() ||
        (stake && (tx.vin.size() != 1 || tx.vout.size() < 2 || tx.vout.size() > 3 || !tx.vout[0].IsEmpty()))) return {};
    KeyID id;
    for (const auto& out : prevouts) if (!ExtractID(out.scriptPubKey, id)) return {};
    for (size_t i = stake ? 1 : 0; i < tx.vout.size(); ++i)
        if (!ExtractID(tx.vout[i].scriptPubKey, id)) return {};
    CDataStream bytes(SER_NETWORK, 0);
    bytes << genesis << tx.nVersion << tx.nType << uint8_t{1} << payload.mode;
    WriteCompactSize(bytes, tx.vin.size());
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        if (!tx.vin[i].scriptSig.empty()) return {};
        bytes << tx.vin[i].prevout << tx.vin[i].nSequence << prevouts[i];
    }
    WriteCompactSize(bytes, tx.vout.size());
    for (const auto& out : tx.vout) bytes << out;
    bytes << tx.nLockTime << uint8_t(payload.authorizations.size());
    for (const auto& auth : payload.authorizations) bytes << auth.public_key;
    if (HasData(payload.mode)) bytes << payload.data;
    bytes << input;
    return {bytes.begin(), bytes.end()};
}

bool VerifyInputs(const CTransaction& tx, const std::vector<CTxOut>& prevouts,
                  const CChainParams& params, std::string& reason)
{
    if (!CheckStructure(tx, params, reason)) return false;
    if (prevouts.size() != tx.vin.size()) return Fail(reason, "bad-pq-prevout-count");
    if (tx.nType != CTransaction::PQ) {
        if (params.IsTestChain()) {
            for (const auto& out : prevouts)
                if (HasMarker(out.scriptPubKey)) return Fail(reason, "bad-pq-spend-type");
        }
        return true;
    }
    Payload payload;
    if (!DecodePayload(tx, payload)) return Fail(reason, "bad-pq-payload");
    const auto context = SignatureContext(params.NetworkIDString());
    if (!context) return Fail(reason, "bad-pq-network");
    for (size_t i = 0; i < prevouts.size(); ++i) {
        KeyID id;
        const auto expected_id = GetID(payload.authorizations[i].public_key, params.NetworkIDString());
        if (!ExtractID(prevouts[i].scriptPubKey, id) || !expected_id || *expected_id != id)
            return Fail(reason, "bad-pq-key");
    }
    for (size_t i = 0; i < payload.authorizations.size(); ++i) {
        const auto message = SignatureMessage(tx, prevouts, payload, params.GetConsensus().hashGenesisBlock, i);
        const auto& auth = payload.authorizations[i];
        if (message.empty() || !mldsa44::Verify(auth.public_key, message, *context, auth.signature))
            return Fail(reason, "bad-pq-signature");
    }
    return true;
}

unsigned int GetSigOpCost(const CTransaction& tx)
{
    Payload payload;
    return DecodePayload(tx, payload) ? SIGOP_COST * (payload.authorizations.size() + (payload.mode == MASTERNODE ? 3 : 0)) : 0;
}
}
