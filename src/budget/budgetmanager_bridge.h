// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_BUDGET_BUDGETMANAGER_BRIDGE_H
#define PIVX_BUDGET_BUDGETMANAGER_BRIDGE_H

#include "uint256.h"

#include <cstdint>

bool IsBudgetProposalExpired(const uint256& proposalHash, int blockHeight);
void UpdateBudgetProposalCoinVotes(const uint256& proposalHash, int64_t coinAmount, bool fYes);
void DecrementBudgetProposalCoinVotes(const uint256& proposalHash, int64_t coinAmount, bool fYes);

#endif // PIVX_BUDGET_BUDGETMANAGER_BRIDGE_H
