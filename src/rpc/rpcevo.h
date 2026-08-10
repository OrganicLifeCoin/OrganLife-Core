// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_RPCEVO_H
#define PIVX_RPCEVO_H

#include <string>

#include "masternodeconfig.h"
#include "script/standard.h"

class CWallet;

// Returns (generating if needed) the transparent owner address for the given
// alias. The owner address doubles as voting address and payout address in the
// simple flow. Requires the wallet to be unlocked.
CTxDestination GetOrCreateOwnerAddress(CWallet* pwallet, const std::string& alias);

// Starts a deterministic masternode from the controller wallet.
// Generates (or reuses) the owner+voting key pair for the alias, builds the
// ProReg payload from the conf entry (IP, BLS operator key), funds the
// registration (4000 OLC collateral created inside the registration tx),
// signs, submits, and locks the collateral output.
//
// Preconditions: wallet unlocked, v6_evo upgrade active.
// On success: txidOut = registration txid, errorOut untouched, returns true.
// On failure: errorOut = user-facing message, returns false.
bool StartDeterministicMasternode(CWallet& wallet,
                                  const CMasternodeConfig::CMasternodeEntry& entry,
                                  std::string& txidOut,
                                  std::string& errorOut);

#endif // PIVX_RPCEVO_H
