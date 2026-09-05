// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#ifndef PIVX_BLOCKSIGNATURE_H
#define PIVX_BLOCKSIGNATURE_H

#include "crypto/mldsa44.h"
#include "primitives/block.h"

class CWallet;

bool SignBlockWithPQKey(CBlock& block, const mldsa44::Key& key);
bool SignBlock(CBlock& block, const CWallet& wallet);
bool CheckBlockSignature(const CBlock& block);

#endif // PIVX_BLOCKSIGNATURE_H
