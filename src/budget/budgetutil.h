// Copyright (c) 2021 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_BUDGET_BUDGETUTIL_H
#define PIVX_BUDGET_BUDGETUTIL_H

#include "uint256.h"
#include "budget/budgetvote.h"

#include <list>
#include <string>

// Future: Decouple UniValue usage. Should be used only in the RPC server files for the inputs/outputs values.

class CWallet;

// vote on a finalized budget with the active local masternode
// Note: finalized budget voting is allowed only with the operator key
UniValue mnLocalBudgetVoteInner(const uint256& budgetHash);

// vote on a proposal with the wallet (voting key) of all masternodes, or a single one (mnAliasFilter)
UniValue mnBudgetVoteInner(CWallet* const pwallet, const uint256& budgetHash, bool fFinal,
                                  const CBudgetVote::VoteDirection& nVote, const Optional<std::string>& mnAliasFilter);

#endif // PIVX_BUDGET_BUDGETUTIL_H
