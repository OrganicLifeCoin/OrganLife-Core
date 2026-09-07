// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include "blocksignature.h"

#include "chainparams.h"
#include "pqtransaction.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <algorithm>

namespace {
constexpr uint8_t BLOCK_SIGNATURE_VERSION = 1;
constexpr size_t BLOCK_SIGNATURE_SIZE = 1 + mldsa44::PUBLIC_KEY_SIZE + mldsa44::SIGNATURE_SIZE;

std::vector<unsigned char> BlockMessage(const CBlock& block)
{
    const uint256 hash = block.GetHash();
    return {hash.begin(), hash.end()};
}
}

bool SignBlockWithPQKey(CBlock& block, const mldsa44::Key& key)
{
    const auto context = pq::BlockSignatureContext(Params().NetworkIDString());
    std::vector<unsigned char> signature;
    if (!context || !key.Sign(BlockMessage(block), *context, signature) ||
        signature.size() != mldsa44::SIGNATURE_SIZE) return false;
    const auto public_key = key.GetPublicKey();
    block.vchBlockSig.clear();
    block.vchBlockSig.reserve(BLOCK_SIGNATURE_SIZE);
    block.vchBlockSig.push_back(BLOCK_SIGNATURE_VERSION);
    block.vchBlockSig.insert(block.vchBlockSig.end(), public_key.begin(), public_key.end());
    block.vchBlockSig.insert(block.vchBlockSig.end(), signature.begin(), signature.end());
    return true;
}

bool SignBlock(CBlock& block, const CWallet& wallet)
{
#ifdef ENABLE_WALLET
    if (!block.IsProofOfStake() || block.vtx.size() < 2 || block.vtx[1]->vout.size() < 2 ||
        block.vtx[1]->vout.size() > 3) return false;
    pq::KeyID id;
    mldsa44::Key key;
    return pq::ExtractID(block.vtx[1]->vout[1].scriptPubKey, id) &&
           wallet.GetPQKey(id, key, true) && SignBlockWithPQKey(block, key);
#else
    return false;
#endif
}

bool CheckBlockSignature(const CBlock& block)
{
    if (block.IsProofOfWork()) return block.vchBlockSig.empty();
    if (!Params().IsTestChain() || block.vtx.size() < 2 || block.vtx[1]->vout.size() < 2 ||
        block.vtx[1]->vout.size() > 3 ||
        block.vchBlockSig.size() != BLOCK_SIGNATURE_SIZE || block.vchBlockSig[0] != BLOCK_SIGNATURE_VERSION)
        return false;
    pq::Payload payload;
    if (!pq::DecodePayload(*block.vtx[1], payload) || payload.mode != pq::STAKE) return false;
    mldsa44::PublicKey public_key;
    std::copy_n(block.vchBlockSig.begin() + 1, public_key.size(), public_key.begin());
    pq::KeyID output_id;
    const auto signer_id = pq::GetID(public_key, Params().NetworkIDString());
    if (!pq::ExtractID(block.vtx[1]->vout[1].scriptPubKey, output_id) || !signer_id || *signer_id != output_id)
        return false;
    const auto context = pq::BlockSignatureContext(Params().NetworkIDString());
    if (!context) return false;
    const Span<const unsigned char> signature(block.vchBlockSig.data() + 1 + public_key.size(), mldsa44::SIGNATURE_SIZE);
    return mldsa44::Verify(public_key, BlockMessage(block), *context, signature);
}
