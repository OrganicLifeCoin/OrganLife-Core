// Copyright (c) 2021-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "governancemodel.h"

#include "budget/budgetmanager.h"
#include "budget/budgetproposal.h"
#include "budget/budgetutil.h"
#include "pqaddress.h"
#include "pqtransaction.h"
#include "rpc/server.h"
#include "utilmoneystr.h"
#include "wallet/wallet.h"
#include "walletmodel.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <set>
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

OperationResult ParseCoinVoteStatus(const UniValue& statusObj, CoinVoteStatus& status)
{
    if (!statusObj.isObject()) {
        return errorOut("invalid getgovvotestatus response");
    }

    const UniValue& coinYes = find_value(statusObj, "coin_yes");
    const UniValue& coinNo = find_value(statusObj, "coin_no");
    const UniValue& netCoinVotes = find_value(statusObj, "net_coin_votes");
    const UniValue& votingCloses = find_value(statusObj, "voting_closes");

    if (!coinYes.isNum() || !coinNo.isNum() || !netCoinVotes.isNum() || !votingCloses.isNum()) {
        return errorOut("missing fields in getgovvotestatus response");
    }

    status.coinYes = coinYes.get_int64();
    status.coinNo = coinNo.get_int64();
    status.netCoinVotes = netCoinVotes.get_int64();
    status.cutoffHeight = votingCloses.get_int();
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

GovernanceModel::GovernanceModel(ClientModel* _clientModel, MNModel* _mnModel) : clientModel(_clientModel)
{
    Q_UNUSED(_mnModel);
}
GovernanceModel::~GovernanceModel() {}

void GovernanceModel::setWalletModel(WalletModel* _walletModel)
{
    walletModel = _walletModel;
}

ProposalInfo GovernanceModel::buildProposalInfo(const CBudgetProposal* prop, bool isPassing)
{
    pq::KeyID recipient{};
    const bool validRecipient = pq::ExtractID(prop->GetPayee(), recipient);
    const int chainHeight = clientModel->getLastBlockProcessedHeight();
    const int remainingPayments = prop->GetRemainingPaymentCount(chainHeight);
    ProposalInfo::Status status;

    if (!prop->IsValid() || chainHeight > prop->GetBlockEnd()) {
        status = ProposalInfo::FINISHED;
    } else if (isPassing) {
        status = ProposalInfo::PASSING;
    } else if (prop->GetNetCoinVotes() > 0) {
        status = ProposalInfo::PASSING_NOT_FUNDED;
    } else {
        status = ProposalInfo::NOT_PASSING;
    }

    return ProposalInfo(prop->GetHash(),
            prop->GetName(),
            prop->GetURL(),
            0,
            0,
            validRecipient ? pq::EncodeAddress(recipient, Params().NetworkIDString()) : "",
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
    std::set<uint256> funded;
    const int nextCycle = getNextSuperblockHeight();
    for (int height = nextCycle; height < nextCycle + getNumBlocksPerBudgetCycle(); ++height) {
        CScript payee;
        CAmount amount;
        uint256 proposalHash;
        if (g_budgetman.GetPQPayment(height, payee, amount, proposalHash)) funded.insert(proposalHash);
    }
    allocatedAmount = 0;
    for (const auto& prop : g_budgetman.GetAllProposalsOrdered()) {
        const bool isPassing = funded.count(prop->GetHash()) != 0;
        ProposalInfo propInfo = buildProposalInfo(prop, isPassing);

        if (filterFinished && propInfo.isFinished()) continue;
        if (!filterByStatus || propInfo.status == *filterByStatus) {
            ret.emplace_back(propInfo);
        }
        if (isPassing) allocatedAmount += prop->GetAmount();
    }

    return ret;
}

bool GovernanceModel::hasProposals()
{
    return g_budgetman.HasAnyProposal();
}

CAmount GovernanceModel::getMaxAvailableBudgetAmount() const
{
    return g_budgetman.GetTotalBudget(getNextSuperblockHeight());
}

int GovernanceModel::getNumBlocksPerBudgetCycle() const
{
    return Params().GetConsensus().nBudgetCycleBlocks;
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
        return {false, strprintf(_("Amount below the minimum of %s OrganicLife"), FormatMoney(PROPOSAL_MIN_AMOUNT))};
    }

    if (amount > PROPOSAL_MAX_AMOUNT) {
        return {false, strprintf(_("Amount exceeding the maximum allowed of %s OrganicLife"), FormatMoney(PROPOSAL_MAX_AMOUNT))};
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

bool GovernanceModel::isChainReady()
{
    return clientModel && !clientModel->inInitialBlockDownload();
}

OperationResult GovernanceModel::createProposal(const std::string& strProposalName,
                                                const std::string& strURL,
                                                int nPaymentCount,
                                                CAmount nAmount,
                                                const std::string& strPaymentAddr)
{
    if (!walletModel) return errorOut("Wallet not loaded");
    pq::KeyID recipient;
    if (!pq::DecodeAddress(strPaymentAddr, Params().NetworkIDString(), recipient))
        return errorOut("Invalid PQ recipient address for this network");

    UniValue params(UniValue::VARR);
    params.push_back(strProposalName);
    params.push_back(strURL);
    params.push_back(nPaymentCount);
    params.push_back(getNextSuperblockHeight());
    params.push_back(strPaymentAddr);
    params.push_back(ValueFromAmount(nAmount));
    const auto result = ExecuteRPC("createpqproposal", params);
    return result ? OperationResult{true} : errorOut(result.getError());
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

    auto lockRes = ExecuteRPC("creategovvotelock", lockParams);
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
    CAmount total{0};
    if (!walletModel || !walletModel->getWallet()) return total;
    for (const COutput& coin : walletModel->getWallet()->GetPQUnspent()) total += coin.Value();
    return total;
}

OperationResult GovernanceModel::getProposalCoinVoteStatus(const ProposalInfo& prop, CoinVoteStatus& status)
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
    return ParseCoinVoteStatus(*statusRes.getObjResult(), status);
}
