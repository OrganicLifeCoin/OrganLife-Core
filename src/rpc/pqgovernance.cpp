// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include "rpc/register.h"

#include "budget/budgetmanager.h"
#include "budget/budgetproposal.h"
#include "chainparams.h"
#include "evo/governancevoteindex.h"
#include "evo/governancevotetx.h"
#include "init.h"
#include "net.h"
#include "pqaddress.h"
#include "pqtransaction.h"
#include "rpc/server.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "wallet/rpcwallet.h"
#include "wallet/wallet.h"

#include <algorithm>
#include <limits>
#include <set>

namespace {
void EnsureTestChain()
{
    if (!Params().IsTestChain())
        throw JSONRPCError(RPC_MISC_ERROR, "PQ governance is available only on testnet and regtest");
}

CWallet* GetUnlockedWallet(const JSONRPCRequest& request)
{
    CWallet* wallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(wallet, request.fHelp)) return nullptr;
    EnsureTestChain();
    wallet->BlockUntilSyncedToCurrentChain();
    EnsureWalletIsUnlocked(wallet);
    return wallet;
}

void EnsureGovernanceActive()
{
    const CBlockIndex* tip = GetChainTip();
    if (!tip) throw JSONRPCError(RPC_IN_WARMUP, "Try again after the active chain is loaded");
    const int nextHeight = tip->nHeight + 1;
    if (!pq::PaymentsActive(Params(), nextHeight) ||
        !Params().GetConsensus().NetworkUpgradeActive(nextHeight, Consensus::UPGRADE_V6_1_GOV))
        throw JSONRPCError(RPC_MISC_ERROR, "PQ governance is not active at the next block");
}

std::string CommitGovernance(CWallet* wallet, const CTxOut& output, uint8_t mode,
                             const std::vector<unsigned char>& data)
{
    CTransactionRef tx;
    CAmount fee{0};
    std::string reason;
    if (!wallet->CreatePQTransaction({output}, mode, data, tx, fee, reason))
        throw JSONRPCError(RPC_WALLET_ERROR, reason);
    const auto result = wallet->CommitTransaction(tx, nullptr, g_connman.get());
    if (result.status != CWallet::CommitStatus::OK)
        throw JSONRPCError(RPC_WALLET_ERROR, result.ToString());
    return result.hashTx.ToString();
}

COutPoint ParseLockRef(const std::string& value)
{
    const auto separator = value.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 == value.size())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "lock reference must be txid:vout");
    const std::string txid = value.substr(0, separator);
    if (!IsHex(txid) || txid.size() != 64)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid lock transaction id");
    uint64_t index;
    try {
        index = std::stoull(value.substr(separator + 1));
    } catch (const std::exception&) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid lock output index");
    }
    if (index > std::numeric_limits<uint32_t>::max())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "lock output index is out of range");
    return {uint256S(txid), static_cast<uint32_t>(index)};
}

bool GetLock(const COutPoint& ref, CGovVoteLockTx& lock)
{
    CGovVoteLockRecord record;
    if (governanceVoteIndex && governanceVoteIndex->GetLockRecord(ref, record)) {
        lock.proposalHash = record.proposalHash;
        lock.lockAmount = record.lockAmount;
        lock.unlockHeight = record.unlockHeight;
        lock.ownerKeyId = record.ownerKeyId;
        return true;
    }
    CTransactionRef tx;
    uint256 blockHash;
    pq::Payload payload;
    return GetTransaction(ref.hash, tx, blockHash, true) && tx && ref.n < tx->vout.size() &&
           pq::DecodePayload(*tx, payload) && DecodeGovernanceData(payload, lock) &&
           tx->vout[ref.n].nValue == lock.lockAmount &&
           tx->vout[ref.n].scriptPubKey == pq::GetScript(lock.ownerKeyId);
}

uint32_t FindLockOutput(const CTransaction& tx, const CGovVoteLockTx& lock)
{
    for (uint32_t i = 0; i < tx.vout.size(); ++i)
        if (tx.vout[i].nValue == lock.lockAmount &&
            tx.vout[i].scriptPubKey == pq::GetScript(lock.ownerKeyId)) return i;
    throw JSONRPCError(RPC_INTERNAL_ERROR, "created transaction is missing its vote lock output");
}

UniValue createpqproposal(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 6)
        throw std::runtime_error("createpqproposal \"name\" \"url\" payments start \"pq_address\" amount\n"
                                 "Create and broadcast an on-chain ML-DSA governance proposal.\n");
    CWallet* wallet = GetUnlockedWallet(request);
    if (!wallet) return NullUniValue;
    EnsureGovernanceActive();

    CGovProposalTx data;
    data.name = SanitizeString(request.params[0].get_str());
    data.url = SanitizeString(request.params[1].get_str());
    data.paymentCount = request.params[2].get_int();
    data.blockStart = request.params[3].get_int();
    if (!pq::DecodeAddress(request.params[4].get_str(), Params().NetworkIDString(), data.recipient))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "invalid PQ payment address for this network");
    data.amount = AmountFromValue(request.params[5]);
    CValidationState state;
    if (!data.IsTriviallyValid(state)) throw JSONRPCError(RPC_INVALID_PARAMETER, state.GetRejectReason());

    CBudgetProposal proposal(data.name, data.url, data.paymentCount, pq::GetScript(data.recipient),
                             data.amount, data.blockStart, UINT256_ZERO);
    if (!proposal.IsWellFormed(g_budgetman.GetTotalBudget(data.blockStart)))
        throw JSONRPCError(RPC_INVALID_PARAMETER, proposal.IsInvalidReason());
    const CBlockIndex* tip = GetChainTip();
    if (!tip || data.blockStart <= static_cast<uint32_t>(tip->nHeight + 1))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "proposal start must be a future governance cycle");

    pq::KeyID burnId;
    const uint256 proposalHash = proposal.GetHash();
    std::copy(proposalHash.begin(), proposalHash.end(), burnId.begin());
    const std::string txid = CommitGovernance(wallet, CTxOut(PROPOSAL_FEE_TX, pq::GetScript(burnId)),
                                              pq::GOVERNANCE_PROPOSAL, EncodeGovernanceData(data));
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", txid);
    result.pushKV("proposal_hash", proposalHash.ToString());
    return result;
}

UniValue creategovvotelock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 3)
        throw std::runtime_error("creategovvotelock \"proposal_hash\" amount unlock_height\n"
                                 "Create and broadcast an ML-DSA coin-vote lock.\n");
    CWallet* wallet = GetUnlockedWallet(request);
    if (!wallet) return NullUniValue;
    EnsureGovernanceActive();

    CGovVoteLockTx lock;
    lock.proposalHash = ParseHashV(request.params[0], "proposal_hash");
    lock.lockAmount = AmountFromValue(request.params[1]);
    lock.unlockHeight = request.params[2].get_int();
    CBudgetProposal proposal;
    if (!g_budgetman.GetProposal(lock.proposalHash, proposal))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "unknown proposal hash");
    if (lock.unlockHeight <= static_cast<uint32_t>(proposal.GetBlockEnd()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "unlock height must be after the proposal ends");
    const auto addresses = wallet->GetPQAddresses();
    if (addresses.empty())
        throw JSONRPCError(RPC_WALLET_ERROR, "create and back up a PQ address before voting");
    if (!pq::DecodeAddress(addresses.front(), Params().NetworkIDString(), lock.ownerKeyId))
        throw JSONRPCError(RPC_WALLET_ERROR, "wallet PQ address is invalid");
    CValidationState state;
    if (!lock.IsTriviallyValid(state)) throw JSONRPCError(RPC_INVALID_PARAMETER, state.GetRejectReason());

    CTransactionRef tx;
    CAmount fee{0};
    std::string reason;
    if (!wallet->CreatePQTransaction({CTxOut(lock.lockAmount, pq::GetScript(lock.ownerKeyId))},
                                     pq::GOVERNANCE_LOCK, EncodeGovernanceData(lock), tx, fee, reason))
        throw JSONRPCError(RPC_WALLET_ERROR, reason);
    const uint32_t output = FindLockOutput(*tx, lock);
    const auto result = wallet->CommitTransaction(tx, nullptr, g_connman.get());
    if (result.status != CWallet::CommitStatus::OK)
        throw JSONRPCError(RPC_WALLET_ERROR, result.ToString());
    wallet->LockCoin({result.hashTx, output});

    UniValue response(UniValue::VOBJ);
    response.pushKV("txid", result.hashTx.ToString());
    response.pushKV("vout", static_cast<int64_t>(output));
    response.pushKV("outpoint", strprintf("%s:%u", result.hashTx.ToString(), output));
    response.pushKV("proposal_hash", lock.proposalHash.ToString());
    response.pushKV("amount", ValueFromAmount(lock.lockAmount));
    response.pushKV("unlock_height", static_cast<int64_t>(lock.unlockHeight));
    return response;
}

UniValue castgovvote(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 3)
        throw std::runtime_error("castgovvote \"proposal_hash\" \"yes|no\" [\"txid:vout\",...]\n"
                                 "Create and broadcast an ML-DSA coin vote.\n");
    CWallet* wallet = GetUnlockedWallet(request);
    if (!wallet) return NullUniValue;
    EnsureGovernanceActive();

    CGovVoteCastTx cast;
    cast.proposalHash = ParseHashV(request.params[0], "proposal_hash");
    const std::string direction = request.params[1].get_str();
    if (direction == "yes") cast.voteDirection = CGovVoteCastTx::VOTE_YES;
    else if (direction == "no") cast.voteDirection = CGovVoteCastTx::VOTE_NO;
    else throw JSONRPCError(RPC_INVALID_PARAMETER, "vote must be 'yes' or 'no'");
    const UniValue refs = request.params[2].get_array();
    if (refs.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "lock_refs must not be empty");
    for (size_t i = 0; i < refs.size(); ++i) cast.lockRefs.push_back(ParseLockRef(refs[i].get_str()));

    pq::KeyID owner{};
    bool haveOwner = false;
    for (const auto& ref : cast.lockRefs) {
        CGovVoteLockTx lock;
        if (!GetLock(ref, lock)) throw JSONRPCError(RPC_INVALID_PARAMETER, "governance lock was not found");
        if (lock.proposalHash != cast.proposalHash)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "lock belongs to a different proposal");
        if (!haveOwner) { owner = lock.ownerKeyId; haveOwner = true; }
        else if (owner != lock.ownerKeyId)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "all locks must have the same owner");
    }
    mldsa44::Key key;
    if (!wallet->GetPQKey(owner, key, false))
        throw JSONRPCError(RPC_WALLET_ERROR, "wallet does not own the vote lock key");
    cast.ownerPublicKey = key.GetPublicKey();
    const auto context = pq::GovernanceSignatureContext(Params().NetworkIDString());
    std::vector<unsigned char> signature;
    if (!context || !key.Sign(cast.GetSignatureMessage(Params().GetConsensus().hashGenesisBlock),
                              *context, signature) || signature.size() != cast.sig.size())
        throw JSONRPCError(RPC_INTERNAL_ERROR, "failed to sign governance vote");
    std::copy(signature.begin(), signature.end(), cast.sig.begin());
    return CommitGovernance(wallet, CTxOut(COIN, pq::GetScript(owner)), pq::GOVERNANCE_CAST,
                            EncodeGovernanceData(cast));
}

UniValue listgovlocks(const JSONRPCRequest& request)
{
    EnsureTestChain();
    CWallet* wallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(wallet, request.fHelp)) return NullUniValue;
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error("listgovlocks ( \"proposal_hash\" )\nList wallet PQ governance locks.\n");
    Optional<uint256> filter;
    if (!request.params.empty()) filter = ParseHashV(request.params[0], "proposal_hash");
    UniValue response(UniValue::VARR);
    LOCK2(cs_main, wallet->cs_wallet);
    for (const auto& entry : wallet->mapWallet) {
        pq::Payload envelope;
        CGovVoteLockTx lock;
        if (!entry.second.tx || !pq::DecodePayload(*entry.second.tx, envelope) ||
            !DecodeGovernanceData(envelope, lock) || (filter && lock.proposalHash != *filter)) continue;
        const uint32_t output = FindLockOutput(*entry.second.tx, lock);
        const COutPoint ref(entry.first, output);
        UniValue item(UniValue::VOBJ);
        item.pushKV("txid", entry.first.ToString());
        item.pushKV("vout", static_cast<int64_t>(output));
        item.pushKV("outpoint", strprintf("%s:%u", entry.first.ToString(), output));
        item.pushKV("proposal_hash", lock.proposalHash.ToString());
        item.pushKV("amount", ValueFromAmount(lock.lockAmount));
        item.pushKV("unlock_height", static_cast<int64_t>(lock.unlockHeight));
        item.pushKV("owner", pq::EncodeAddress(lock.ownerKeyId, Params().NetworkIDString()));
        item.pushKV("confirmations", entry.second.GetDepthInMainChain());
        item.pushKV("used", governanceVoteIndex && governanceVoteIndex->IsLockUsedForProposal(ref, lock.proposalHash));
        response.push_back(item);
    }
    return response;
}

UniValue getgovvotestatus(const JSONRPCRequest& request)
{
    EnsureTestChain();
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error("getgovvotestatus \"proposal_hash\"\nReturn PQ coin-vote totals.\n");
    const uint256 hash = ParseHashV(request.params[0], "proposal_hash");
    CBudgetProposal proposal;
    if (!g_budgetman.GetProposal(hash, proposal))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "unknown proposal hash");
    UniValue response(UniValue::VOBJ);
    response.pushKV("coin_yes", proposal.GetCoinYeas());
    response.pushKV("coin_no", proposal.GetCoinNays());
    response.pushKV("net_coin_votes", proposal.GetNetCoinVotes());
    response.pushKV("voting_closes", proposal.GetBlockStart() - 1);
    return response;
}

UniValue getnextsuperblock(const JSONRPCRequest& request)
{
    EnsureTestChain();
    if (request.fHelp || !request.params.empty())
        throw std::runtime_error("getnextsuperblock\nReturn the next governance-cycle height.\n");
    const int height = WITH_LOCK(cs_main, return chainActive.Height());
    if (height < 0) return "unknown";
    const int cycle = Params().GetConsensus().nBudgetCycleBlocks;
    return height - height % cycle + cycle;
}

UniValue getbudgetinfo(const JSONRPCRequest& request)
{
    EnsureTestChain();
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error("getbudgetinfo ( \"name\" )\nList confirmed PQ governance proposals.\n");
    const std::string filter = request.params.empty() ? "" : SanitizeString(request.params[0].get_str());
    UniValue response(UniValue::VARR);
    for (const CBudgetProposal* proposal : g_budgetman.GetAllProposalsOrdered()) {
        if (!filter.empty() && proposal->GetName() != filter) continue;
        pq::KeyID recipient;
        UniValue item(UniValue::VOBJ);
        item.pushKV("Name", proposal->GetName());
        item.pushKV("URL", proposal->GetURL());
        item.pushKV("Hash", proposal->GetHash().ToString());
        item.pushKV("BlockStart", proposal->GetBlockStart());
        item.pushKV("BlockEnd", proposal->GetBlockEnd());
        item.pushKV("PaymentAddress", pq::ExtractID(proposal->GetPayee(), recipient) ?
                    pq::EncodeAddress(recipient, Params().NetworkIDString()) : "");
        item.pushKV("PaymentAmount", ValueFromAmount(proposal->GetAmount()));
        item.pushKV("CoinYeas", proposal->GetCoinYeas());
        item.pushKV("CoinNays", proposal->GetCoinNays());
        item.pushKV("IsValid", proposal->IsValid());
        response.push_back(item);
    }
    return response;
}
} // namespace

void RegisterPQGovernanceRPCCommands(CRPCTable& table)
{
    static const CRPCCommand commands[] = {
        {"governance", "createpqproposal", &createpqproposal, true, {"name", "url", "payments", "start", "pq_address", "amount"}},
        {"governance", "creategovvotelock", &creategovvotelock, true, {"proposal_hash", "amount", "unlock_height"}},
        {"governance", "castgovvote", &castgovvote, true, {"proposal_hash", "vote", "lock_refs"}},
        {"governance", "listgovlocks", &listgovlocks, true, {"proposal_hash"}},
        {"governance", "getgovvotestatus", &getgovvotestatus, true, {"proposal_hash"}},
        {"governance", "getnextsuperblock", &getnextsuperblock, true, {}},
        {"governance", "getbudgetinfo", &getbudgetinfo, true, {"name"}},
    };
    for (const auto& command : commands) table.appendCommand(command.name, &command);
}
