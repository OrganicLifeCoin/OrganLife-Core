// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_PQTRANSACTION_H
#define ORGANICLIFE_PQTRANSACTION_H

#include <pqaddress.h>
#include <primitives/transaction.h>

class CChainParams;
namespace pq {
constexpr uint8_t TRANSFER = 1;
constexpr uint8_t STAKE = 2;
constexpr uint8_t GOVERNANCE_PROPOSAL = 3;
constexpr uint8_t GOVERNANCE_LOCK = 4;
constexpr uint8_t GOVERNANCE_CAST = 5;
// Requires opt-in regtest registry activation; public networks remain disabled.
constexpr uint8_t MASTERNODE = 6;
constexpr size_t MAX_MASTERNODE_DATA_SIZE = 12000;
constexpr size_t MAX_MASTERNODE_TX_SIZE = 24000;
constexpr size_t MAX_INPUTS = 2;
constexpr size_t MAX_TX_SIZE = 10000;
constexpr size_t MAX_GOVERNANCE_DATA_SIZE = 8192;
constexpr unsigned int SIGOP_COST = 400;
constexpr size_t AUTH_SIZE = mldsa44::PUBLIC_KEY_SIZE + mldsa44::SIGNATURE_SIZE;
struct Authorization {
    mldsa44::PublicKey public_key{};
    std::array<unsigned char, mldsa44::SIGNATURE_SIZE> signature{};
};
struct Payload {
    uint8_t mode{TRANSFER};
    std::vector<Authorization> authorizations;
    std::vector<unsigned char> data;
};

bool IsGovernanceMode(uint8_t mode);
// Envelope classification only; does not replace structure/context/signature validation.
bool IsMasternode(const CTransaction& tx);

bool HasMarker(const CScript& script);
CScript GetScript(const KeyID& id);
bool ExtractID(const CScript& script, KeyID& id);
std::vector<unsigned char> EncodePayload(const Payload& payload);
bool DecodePayload(const CTransaction& tx, Payload& payload);
bool CheckStructure(const CTransaction& tx, const CChainParams& params, std::string& reason);
// Pass the actual block height, or the candidate next-block height for mempool/wallet use.
bool PaymentsActive(const CChainParams& params, int height);
// Local registry qualification only; public networks remain disabled.
bool MasternodesActive(const CChainParams& params, int height);
bool CheckContext(const CTransaction& tx, const CChainParams& params, int height, std::string& reason);
// prevouts are complete spent outputs, in input order, from the trusted UTXO view.
std::vector<unsigned char> SignatureMessage(const CTransaction& tx, const std::vector<CTxOut>& prevouts,
                                          const Payload& payload, const uint256& genesis, uint32_t input);
Optional<Span<const unsigned char>> SignatureContext(const std::string& network);
Optional<Span<const unsigned char>> BlockSignatureContext(const std::string& network);
Optional<Span<const unsigned char>> GovernanceSignatureContext(const std::string& network);
bool VerifyInputs(const CTransaction& tx, const std::vector<CTxOut>& prevouts,
                  const CChainParams& params, std::string& reason);
unsigned int GetSigOpCost(const CTransaction& tx);
} // namespace pq
#endif
