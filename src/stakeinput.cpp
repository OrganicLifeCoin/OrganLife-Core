// Copyright (c) 2017-2022 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "stakeinput.h"

#include "chain.h"
#include "txdb.h"
#include "undo.h"
#include "validation.h"

static bool HasStakeMinAgeOrDepth(int nHeight, uint32_t nTime, const CBlockIndex* pindex)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    if (!consensus.HasStakeMinAgeOrDepth(nHeight, nTime, pindex->nHeight, pindex->nTime)) {
        return error("%s : min age violation - height=%d - time=%d, nHeightBlockFrom=%d, nTimeBlockFrom=%d",
                     __func__, nHeight, nTime, pindex->nHeight, pindex->nTime);
    }
    return true;
}

CPivStake* CPivStake::NewPivStake(const CTxIn& txin, const CBlockIndex* pindexPrev, uint32_t nTime)
{
    if (!pindexPrev) return nullptr;
    const int nHeight = pindexPrev->nHeight + 1;
    // The active UTXO cache is usable only for an origin on this parent's ancestry.
    const Coin& coin = pcoinsTip->AccessCoin(txin.prevout);
    if (!coin.IsSpent()) {
        const CBlockIndex* pindexFrom = chainActive[coin.nHeight];
        if (pindexFrom && pindexPrev->GetAncestor(pindexFrom->nHeight) == pindexFrom) {
            if (!HasStakeMinAgeOrDepth(nHeight, nTime, pindexFrom)) return nullptr;
            return new CPivStake(coin.out, txin.prevout, pindexFrom);
        }
    }

    // Side-branch transactions are not in the active coins cache or tx index.
    // Use the same fork bound as AcceptBlock; input-spend checks still run there.
    int readBlocks = 0;
    for (const CBlockIndex* origin = pindexPrev; origin && !chainActive.Contains(origin); origin = origin->pprev) {
        if (++readBlocks >= gArgs.GetArg("-maxreorg", DEFAULT_MAX_REORG_DEPTH)) return nullptr;
        CBlock block;
        if (!ReadBlockFromDisk(block, origin)) return nullptr;
        for (const auto& tx : block.vtx) {
            if (tx->GetHash() != txin.prevout.hash) continue;
            if (txin.prevout.n >= tx->vout.size() || !HasStakeMinAgeOrDepth(nHeight, nTime, origin))
                return nullptr;
            return new CPivStake(tx->vout[txin.prevout.n], txin.prevout, origin);
        }
    }

    // Otherwise find the previous transaction in database
    uint256 hashBlock;
    CTransactionRef txPrev;
    if (!GetTransaction(txin.prevout.hash, txPrev, hashBlock, true)) {
        // Without a tx index, recover a common-ancestor coin from the undo
        // record of its competing active-chain spend. Never rewind live state.
        const CBlockIndex* split = chainActive.FindFork(pindexPrev);
        if (!split || chainActive.Height() - split->nHeight > gArgs.GetArg("-maxreorg", DEFAULT_MAX_REORG_DEPTH))
            return nullptr;
        for (const CBlockIndex* spent = chainActive.Tip(); spent != split; spent = spent->pprev) {
            CBlock block;
            if (!ReadBlockFromDisk(block, spent)) return nullptr;
            for (size_t i = 1; i < block.vtx.size(); ++i) {
                const auto& inputs = block.vtx[i]->vin;
                for (size_t j = 0; j < inputs.size(); ++j) {
                    if (inputs[j].prevout != txin.prevout) continue;
                    CBlockUndo undo;
                    const FlatFilePos pos = spent->GetUndoPos();
                    if (pos.IsNull() || !UndoReadFromDisk(undo, pos, spent->pprev->GetBlockHash()) ||
                        undo.vtxundo.size() + 1 != block.vtx.size() || undo.vtxundo[i - 1].vprevout.size() != inputs.size())
                        return nullptr;
                    const Coin& previous = undo.vtxundo[i - 1].vprevout[j];
                    const CBlockIndex* origin = chainActive[previous.nHeight];
                    if (previous.IsSpent() || !origin || pindexPrev->GetAncestor(origin->nHeight) != origin ||
                        !HasStakeMinAgeOrDepth(nHeight, nTime, origin)) return nullptr;
                    return new CPivStake(previous.out, txin.prevout, origin);
                }
            }
        }
        error("%s : INFO: read txPrev failed, tx id prev: %s", __func__, txin.prevout.hash.GetHex());
        return nullptr;
    }
    const CBlockIndex* pindexFrom = nullptr;
    if (mapBlockIndex.count(hashBlock)) {
        CBlockIndex* pindex = mapBlockIndex.at(hashBlock);
        if (pindexPrev->GetAncestor(pindex->nHeight) == pindex) pindexFrom = pindex;
    }
    // An indexed transaction on a sibling/future block is not a valid origin.
    if (!pindexFrom || txin.prevout.n >= txPrev->vout.size()) {
        error("%s : Failed to find the block index for stake origin", __func__);
        return nullptr;
    }
    // Check that the stake has the required depth/age
    if (!HasStakeMinAgeOrDepth(nHeight, nTime, pindexFrom)) {
        return nullptr;
    }
    // All good
    return new CPivStake(txPrev->vout[txin.prevout.n], txin.prevout, pindexFrom);
}

bool CPivStake::GetTxOutFrom(CTxOut& out) const
{
    out = outputFrom;
    return true;
}

CTxIn CPivStake::GetTxIn() const
{
    return CTxIn(outpointFrom.hash, outpointFrom.n);
}

CAmount CPivStake::GetValue() const
{
    return outputFrom.nValue;
}

CDataStream CPivStake::GetUniqueness() const
{
    //The unique identifier for a PIV stake is the outpoint
    CDataStream ss(SER_NETWORK, 0);
    ss << outpointFrom.n << outpointFrom.hash;
    return ss;
}

//The block that the UTXO was added to the chain
const CBlockIndex* CPivStake::GetIndexFrom() const
{
    // Sanity check, pindexFrom is set on the constructor.
    if (!pindexFrom) throw std::runtime_error("CPivStake: uninitialized pindexFrom");
    return pindexFrom;
}
