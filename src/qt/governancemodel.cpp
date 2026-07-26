// Copyright (c) 2021-2022 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "governancemodel.h"

#include "budget/budgetmanager.h"
#include "budget/budgetproposal.h"
#include "budget/budgetutil.h"
#include "destination_io.h"
#include "guiconstants.h"
#include "mnmodel.h"
#include "qt/transactionrecord.h"
#include "qt/transactiontablemodel.h"
#include "rpc/server.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "walletmodel.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <QTimer>
#include <thread>
#include <univalue.h>

namespace {
CallResult<UniValue> ExecuteRPC(const std::string& method, const UniValue& params)
{
    JSONRPCRequest req;
    req.strMethod = method;
    req.params = params;
    req.fHelp = false;

    try {
        return CallResult<UniValue>(tableRPC.execute(req));
    } catch (UniValue& objError) {
        const UniValue& message = find_value(objError, "message");
        return CallResult<UniValue>(message.isStr() ? message.get_str() : "RPC execution failed");
    } catch (const std::exception& e) {
        return CallResult<UniValue>(std::string(e.what()));
    }
}

OperationResult ParseHybridVoteStatus(const UniValue& statusObj, HybridVoteStatus& status)
{
    if (!statusObj.isObject()) {
        return errorOut("invalid getgovvotestatus response");
    }

    const UniValue& mnYes = find_value(statusObj, "mn_yes");
    const UniValue& mnNo = find_value(statusObj, "mn_no");
    const UniValue& coinYes = find_value(statusObj, "coin_yes");
    const UniValue& coinNo = find_value(statusObj, "coin_no");
    const UniValue& k = find_value(statusObj, "k");
    const UniValue& combined = find_value(statusObj, "combined_score");
    const UniValue& cutoffHeight = find_value(statusObj, "cutoff_height");

    if (!mnYes.isNum() || !mnNo.isNum() || !coinYes.isNum() || !coinNo.isNum() ||
            !k.isNum() || !combined.isNum() || !cutoffHeight.isNum()) {
        return errorOut("missing fields in getgovvotestatus response");
    }

    status.mnYes = mnYes.get_int64();
    status.mnNo = mnNo.get_int64();
    status.coinYes = coinYes.get_int64();
    status.coinNo = coinNo.get_int64();
    status.k = k.get_real();
    status.combinedScore = combined.get_real();
    status.cutoffHeight = cutoffHeight.get_int();
    return {true};
}

CAmount ComputeProposalVoteCap(const ProposalInfo& prop)
{
    if (prop.amount <= 0 || prop.remainingPayments <= 0) {
        return 0;
    }
    const auto remainingPayments = static_cast<CAmount>(prop.remainingPayments);
    if (prop.amount > std::numeric_limits<CAmount>::max() / remainingPayments) {
        return std::numeric_limits<CAmount>::max();
    }
    return prop.amount * remainingPayments;
}
} // namespace

std::string ProposalInfo::statusToStr() const
{
    switch(status) {
        case WAITING_FOR_APPROVAL:
            return _("Waiting");
        case PASSING:
            return _("Passing");
        case PASSING_NOT_FUNDED:
            return _("Passing not funded");
        case NOT_PASSING:
            return _("Not Passing");
        case FINISHED:
            if (!isValid) {
                return _("Expired");
            }
            return _("Finished");
    }
    return "";
}

GovernanceModel::GovernanceModel(ClientModel* _clientModel, MNModel* _mnModel) : clientModel(_clientModel), mnModel(_mnModel) {}
GovernanceModel::~GovernanceModel() {}

void GovernanceModel::setWalletModel(WalletModel* _walletModel)
{
    if (walletModel && walletModel->getTransactionTableModel()) {
        disconnect(walletModel->getTransactionTableModel(), &TransactionTableModel::txLoaded, this, &GovernanceModel::txLoaded);
    }

    walletModel = _walletModel;
    if (walletModel && walletModel->getTransactionTableModel()) {
        connect(walletModel->getTransactionTableModel(), &TransactionTableModel::txLoaded, this, &GovernanceModel::txLoaded, Qt::UniqueConnection);
    }
}

ProposalInfo GovernanceModel::buildProposalInfo(const CBudgetProposal* prop, bool isPassing, bool isPending)
{
    CTxDestination recipient;
    ExtractDestination(prop->GetPayee(), recipient);

    // Calculate status
    int votesYes = prop->GetYeas();
    int votesNo = prop->GetNays();
    int mnCount = clientModel->getMasternodesCount();
    const int chainHeight = clientModel->getLastBlockProcessedHeight();
    const int nBlocksPerCycle = getNumBlocksPerBudgetCycle();
    const int nBlockStartBudget = chainHeight - chainHeight % nBlocksPerCycle + nBlocksPerCycle;
    const int nBlockEndBudget = nBlockStartBudget + nBlocksPerCycle - 1;
    const bool passesVotesThreshold = votesYes - votesNo > mnCount / 10;
    const bool passingWithoutBudgetCap = prop->IsValid()
            && prop->GetBlockStart() <= nBlockStartBudget
            && prop->GetBlockEnd() >= nBlockEndBudget
            && passesVotesThreshold
            && prop->IsEstablished();
    int remainingPayments = prop->GetRemainingPaymentCount(clientModel->getLastBlockProcessedHeight());
    ProposalInfo::Status status;

    if (isPending) {
        // Proposal waiting for confirmation to be broadcasted.
        status = ProposalInfo::WAITING_FOR_APPROVAL;
    } else {
        if (remainingPayments < 0) {
            status = ProposalInfo::FINISHED;
        } else if (isPassing) {
            status = ProposalInfo::PASSING;
        } else if (passingWithoutBudgetCap && allocatedAmount + prop->GetAmount() > getMaxAvailableBudgetAmount()) {
            status = ProposalInfo::PASSING_NOT_FUNDED;
        } else {
            status = ProposalInfo::NOT_PASSING;
        }
    }

    return ProposalInfo(prop->GetHash(),
            prop->GetName(),
            prop->GetURL(),
            votesYes,
            votesNo,
            Standard::EncodeDestination(recipient),
            prop->GetAmount(),
            prop->GetTotalPaymentCount(),
            remainingPayments,
            status,
            prop->GetBlockStart(),
            prop->GetBlockEnd(),
            prop->GetCoinYeas(),
            prop->GetCoinNays(),
            prop->IsValid());
}

std::list<ProposalInfo> GovernanceModel::getProposals(const ProposalInfo::Status* filterByStatus, bool filterFinished)
{
    if (!clientModel) return {};
    std::list<ProposalInfo> ret;
    std::vector<CBudgetProposal> budget = g_budgetman.GetBudget();
    allocatedAmount = 0;
    for (const auto& prop : g_budgetman.GetAllProposalsOrdered()) {
        bool isPassing = std::find(budget.begin(), budget.end(), *prop) != budget.end();
        ProposalInfo propInfo = buildProposalInfo(prop, isPassing, false);

        if (filterFinished && propInfo.isFinished()) continue;
        if (!filterByStatus || propInfo.status == *filterByStatus) {
            ret.emplace_back(propInfo);
        }
        if (isPassing) allocatedAmount += prop->GetAmount();
    }

    // Add pending proposals
    for (const auto& prop : waitingPropsForConfirmations) {
        ProposalInfo propInfo = buildProposalInfo(&prop, false, true);
        if (!filterByStatus || propInfo.status == *filterByStatus) {
            ret.emplace_back(propInfo);
        }
    }
    return ret;
}

bool GovernanceModel::hasProposals()
{
    return g_budgetman.HasAnyProposal() || !waitingPropsForConfirmations.empty();
}

CAmount GovernanceModel::getMaxAvailableBudgetAmount() const
{
    return g_budgetman.GetTotalBudget(getNextSuperblockHeight());
}

int GovernanceModel::getNumBlocksPerBudgetCycle() const
{
    return Params().GetConsensus().nBudgetCycleBlocks;
}

int GovernanceModel::getProposalVoteUpdateMinTime() const
{
    return BUDGET_VOTE_UPDATE_MIN;
}

int GovernanceModel::getPropMaxPaymentsCount() const
{
    return Params().GetConsensus().nMaxProposalPayments;
}

CAmount GovernanceModel::getProposalFeeAmount() const
{
    return PROPOSAL_FEE_TX;
}

int GovernanceModel::getNextSuperblockHeight() const
{
    if (!clientModel) return 1;
    const int nBlocksPerCycle = getNumBlocksPerBudgetCycle();
    const int chainHeight = clientModel->getNumBlocks();
    return chainHeight - chainHeight % nBlocksPerCycle + nBlocksPerCycle;
}

std::vector<VoteInfo> GovernanceModel::getLocalMNsVotesForProposal(const ProposalInfo& propInfo)
{
    if (!mnModel) return {};

    // First, get the local masternodes
    std::vector<std::pair<COutPoint, std::string>> vecLocalMn;
    for (int i = 0; i < mnModel->rowCount(); ++i) {
        vecLocalMn.emplace_back(std::make_pair(
                COutPoint(uint256S(mnModel->index(i, MNModel::COLLATERAL_ID, QModelIndex()).data().toString().toStdString()),
                mnModel->index(i, MNModel::COLLATERAL_OUT_INDEX, QModelIndex()).data().toInt()),
                mnModel->index(i, MNModel::ALIAS, QModelIndex()).data().toString().toStdString())
        );
    }

    std::vector<VoteInfo> localVotes;
    {
        LOCK(g_budgetman.cs_proposals); // future: encapsulate this mutex lock.
        // Get the budget proposal, get the votes, then loop over it and return the ones that correspond to the local masternodes here.
        CBudgetProposal* prop = g_budgetman.FindProposal(propInfo.id);
        if (!prop) return localVotes;
        const auto& mapVotes = prop->GetVotes();
        for (const auto& it : mapVotes) {
            for (const auto& mn : vecLocalMn) {
                if (it.first == mn.first && it.second.IsValid()) {
                    localVotes.emplace_back(mn.first, (VoteInfo::VoteDirection) it.second.GetDirection(), mn.second, it.second.GetTime());
                    break;
                }
            }
        }
    }
    return localVotes;
}

OperationResult GovernanceModel::validatePropName(const QString& name) const
{
    std::string strName = SanitizeString(name.toStdString());
    if (strName != name.toStdString()) { // invalid characters
        return {false, _("Invalid name, invalid characters")};
    }
    if (strName.size() > (int)PROP_NAME_MAX_SIZE) { // limit
        return {false, strprintf(_("Invalid name, maximum size of %d exceeded"), PROP_NAME_MAX_SIZE)};
    }
    return {true};
}

OperationResult GovernanceModel::validatePropURL(const QString& url) const
{
    std::string strURL = SanitizeString(url.toStdString());
    if (strURL != url.toStdString()) {
        return {false, _("Invalid URL, invalid characters")};
    }
    std::string strError;
    return {validateURL(strURL, strError, PROP_URL_MAX_SIZE), strError};
}

OperationResult GovernanceModel::validatePropAmount(CAmount amount) const
{
    if (amount < PROPOSAL_MIN_AMOUNT) { // Future: move constant to a budget interface.
        return {false, strprintf(_("Amount below the minimum of %s CTEAM"), FormatMoney(PROPOSAL_MIN_AMOUNT))};
    }

    if (amount > PROPOSAL_MAX_AMOUNT) {
        return {false, strprintf(_("Amount exceeding the maximum allowed of %s CTEAM"), FormatMoney(PROPOSAL_MAX_AMOUNT))};
    }
    return {true};
}

OperationResult GovernanceModel::validatePropPaymentCount(int paymentCount) const
{
    if (paymentCount < 1) return { false, _("Invalid payment count, must be greater than zero.")};
    int nMaxPayments = getPropMaxPaymentsCount();
    if (paymentCount > nMaxPayments) {
        return { false, strprintf(_("Invalid payment count, cannot be greater than %d"), nMaxPayments)};
    }
    return {true};
}

bool GovernanceModel::isTierTwoSync()
{
    return g_tiertwo_sync_state.IsSynced();
}

OperationResult GovernanceModel::createProposal(const std::string& strProposalName,
                                                const std::string& strURL,
                                                int nPaymentCount,
                                                CAmount nAmount,
                                                const std::string& strPaymentAddr)
{
    // First get the next superblock height
    int nBlockStart = getNextSuperblockHeight();

    // Parse address
    const CTxDestination* dest = Standard::GetTransparentDestination(Standard::DecodeDestination(strPaymentAddr));
    if (!dest) return {false, _("invalid recipient address for the proposal")};
    CScript scriptPubKey = GetScriptForDestination(*dest);

    // Validate proposal
    CBudgetProposal proposal(strProposalName, strURL, nPaymentCount, scriptPubKey, nAmount, nBlockStart, UINT256_ZERO);
    if (!proposal.IsWellFormed(g_budgetman.GetTotalBudget(proposal.GetBlockStart()))) {
        return {false, strprintf(_("Proposal is not valid %s"), proposal.IsInvalidReason())};
    }

    // Craft and send transaction.
    auto opRes = walletModel->createAndSendProposalFeeTx(proposal);
    if (!opRes) return opRes;
    scheduleBroadcast(proposal);

    return {true};
}

OperationResult GovernanceModel::voteForProposal(const ProposalInfo& prop,
                                                 bool isVotePositive,
                                                 const std::vector<std::string>& mnVotingAlias)
{
    UniValue ret; // future: don't use UniValue here.
    for (const auto& mnAlias : mnVotingAlias) {
        bool fLegacyMN = true; // For now, only legacy MNs
        ret = mnBudgetVoteInner(nullptr,
                          fLegacyMN,
                          prop.id,
                          false,
                          isVotePositive ? CBudgetVote::VoteDirection::VOTE_YES : CBudgetVote::VoteDirection::VOTE_NO,
                          mnAlias);
        if (ret.exists("detail") && ret["detail"].isArray()) {
            const UniValue& obj = ret["detail"].get_array()[0];
            if (obj["result"].getValStr() != "success") {
                return {false, obj["error"].getValStr()};
            }
        }
    }
    // add more information with ret["overall"]
    return {true};
}

OperationResult GovernanceModel::createVoteLockAndCast(const ProposalInfo& prop,
                                                       bool isVotePositive,
                                                       CAmount lockAmount,
                                                       uint32_t unlockHeight)
{
    const CAmount proposalCap = ComputeProposalVoteCap(prop);
    if (proposalCap <= 0) {
        return errorOut("Invalid proposal payout cap");
    }
    if (lockAmount <= 0) {
        return errorOut("Invalid lock amount");
    }
    if (lockAmount > proposalCap) {
        return errorOut("Lock amount exceeds proposal cap");
    }
    if (!walletModel) {
        return errorOut("Wallet not loaded");
    }
    if (unlockHeight == 0) {
        return errorOut("Invalid unlock height");
    }

    UniValue lockParams(UniValue::VARR);
    lockParams.push_back(prop.id.ToString());
    lockParams.push_back(ValueFromAmount(lockAmount));
    lockParams.push_back(static_cast<int64_t>(unlockHeight));
    lockParams.push_back(true); // request lock reference details when supported

    auto lockRes = ExecuteRPC("creategovvotelock", lockParams);
    if (!lockRes) {
        // Backward compatibility with older daemons that only accept 3 params.
        UniValue legacyLockParams(UniValue::VARR);
        legacyLockParams.push_back(prop.id.ToString());
        legacyLockParams.push_back(ValueFromAmount(lockAmount));
        legacyLockParams.push_back(static_cast<int64_t>(unlockHeight));
        lockRes = ExecuteRPC("creategovvotelock", legacyLockParams);
    }
    if (!lockRes) {
        return errorOut(lockRes.getError());
    }
    if (!lockRes.getObjResult()) {
        return errorOut("Unexpected creategovvotelock response");
    }

    std::string lockTxId;
    std::string lockOutpoint;
    if (lockRes.getObjResult()->isStr()) {
        lockTxId = lockRes.getObjResult()->get_str();
    } else if (lockRes.getObjResult()->isObject()) {
        const UniValue& txid = find_value(*lockRes.getObjResult(), "txid");
        const UniValue& outpoint = find_value(*lockRes.getObjResult(), "outpoint");
        const UniValue& vout = find_value(*lockRes.getObjResult(), "vout");
        if (txid.isStr()) {
            lockTxId = txid.get_str();
        }
        if (outpoint.isStr()) {
            lockOutpoint = outpoint.get_str();
        } else if (txid.isStr() && vout.isNum()) {
            lockOutpoint = strprintf("%s:%d", txid.get_str(), vout.get_int());
        }
    } else {
        return errorOut("Unexpected creategovvotelock response");
    }

    if (lockTxId.empty()) {
        return errorOut("Missing lock transaction id from creategovvotelock");
    }
    if (lockOutpoint.empty()) {
        lockOutpoint = strprintf("%s:%d", lockTxId, 0);
    }

    UniValue castParams(UniValue::VARR);
    castParams.push_back(prop.id.ToString());
    castParams.push_back(isVotePositive ? "yes" : "no");
    UniValue lockRefs(UniValue::VARR);
    lockRefs.push_back(lockOutpoint);
    castParams.push_back(lockRefs);

    auto castRes = ExecuteRPC("castgovvote", castParams);
    if (!castRes) {
        const std::string error = castRes.getError();
        const std::string loweredError = ToLower(error);
        const bool looksLikeLockPropagationRace =
                loweredError.find("lock") != std::string::npos &&
                (loweredError.find("missing") != std::string::npos ||
                 loweredError.find("not found") != std::string::npos);
        if (looksLikeLockPropagationRace) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            castRes = ExecuteRPC("castgovvote", castParams);
        }
    }
    if (!castRes) {
        return errorOut(strprintf("Vote lock created (%s), but vote cast failed for %s: %s",
                                  lockTxId,
                                  lockOutpoint,
                                  castRes.getError()));
    }
    return {true};
}

CAmount GovernanceModel::getCoinLockableBalance() const
{
    if (!walletModel) {
        return 0;
    }
    return walletModel->GetWalletBalances().balance;
}

OperationResult GovernanceModel::getProposalHybridVoteStatus(const ProposalInfo& prop, HybridVoteStatus& status)
{
    UniValue params(UniValue::VARR);
    params.push_back(prop.id.ToString());
    auto statusRes = ExecuteRPC("getgovvotestatus", params);
    if (!statusRes) {
        return errorOut(statusRes.getError());
    }
    if (!statusRes.getObjResult()) {
        return errorOut("Unexpected getgovvotestatus response");
    }
    return ParseHybridVoteStatus(*statusRes.getObjResult(), status);
}

void GovernanceModel::scheduleBroadcast(const CBudgetProposal& proposal)
{
    // Cache the proposal to be sent as soon as it gets the minimum required confirmations
    // without requiring user interaction
    waitingPropsForConfirmations.emplace_back(proposal);

    // Launch timer if it's not already running
    if (!pollTimer) pollTimer = new QTimer(this);
    if (!pollTimer->isActive()) {
        connect(pollTimer, &QTimer::timeout, this, &GovernanceModel::pollGovernanceChanged);
        pollTimer->start(MODEL_UPDATE_DELAY * 60 * (walletModel->isTestNetwork() ? 0.5 : 3.5)); // Every 3.5 minutes
    }
}

void GovernanceModel::pollGovernanceChanged()
{
    if (!isTierTwoSync()) return;

    int chainHeight = clientModel->getNumBlocks();
    // Try to broadcast any pending for confirmations proposal
    auto it = waitingPropsForConfirmations.begin();
    while (it != waitingPropsForConfirmations.end()) {
        // Remove expired proposals
        if (it->IsExpired(clientModel->getNumBlocks())) {
            it = waitingPropsForConfirmations.erase(it);
            continue;
        }

        // Try to add it
        if (!g_budgetman.AddProposal(*it)) {
            LogPrint(BCLog::QT, "Cannot broadcast budget proposal - %s\n", it->IsInvalidReason());
            // Remove proposals which due a reorg lost their fee tx
            if (it->IsInvalidReason().find("Can't find collateral tx") != std::string::npos) {
                // future: notify the user about it.
                it = waitingPropsForConfirmations.erase(it);
                continue;
            }
            // Check if the proposal didn't exceed the superblock start height
            if (chainHeight >= it->GetBlockStart()) {
                // Edge case, the proposal was never broadcasted before the next superblock, can be removed.
                // future: notify the user about it.
                it = waitingPropsForConfirmations.erase(it);
            } else {
                it++;
            }
            continue;
        }
        it->Relay();
        it = waitingPropsForConfirmations.erase(it);
    }

    // If there are no more waiting proposals, turn the timer off.
    if (waitingPropsForConfirmations.empty()) {
        pollTimer->stop();
    }
}

void GovernanceModel::stop()
{
    if (pollTimer && pollTimer->isActive()) {
        pollTimer->stop();
    }
}

void GovernanceModel::txLoaded(const QString& id, const int txType, const int txStatus)
{
    if (txType != TransactionRecord::SendToNobody) {
        return;
    }
    // If the tx is not longer available in the mainchain, drop it.
    if (txStatus == TransactionStatus::Conflicted ||
        txStatus == TransactionStatus::NotAccepted) {
        return;
    }
    if (!walletModel || !clientModel) {
        return;
    }

    try {
        const CWalletTx* wtx = walletModel->getTx(uint256S(id.toStdString()));
        if (!wtx) {
            return;
        }

        const auto& it = wtx->mapValue.find("proposal");
        if (it == wtx->mapValue.end()) {
            return;
        }

        const std::vector<unsigned char> vec = ParseHex(it->second);
        if (vec.empty()) {
            return;
        }

        CDataStream ss(vec, SER_DISK, CLIENT_VERSION);
        CBudgetProposal proposal;
        ss >> proposal;
        proposal.SetFeeTxHash(wtx->GetHash());
        if (!g_budgetman.HaveProposal(proposal.GetHash()) &&
            !proposal.IsExpired(clientModel->getNumBlocks()) &&
            proposal.GetBlockStart() > clientModel->getNumBlocks()) {
            scheduleBroadcast(proposal);
        }
    } catch (const std::exception&) {
        // Silently ignore deserialization errors - don't let exceptions propagate to Qt event loop
    }
}
