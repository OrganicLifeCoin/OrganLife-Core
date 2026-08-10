// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "bls/key_io.h"
#include "db.h"
#include "evo/deterministicmns.h"
#include "key_io.h"
#include "masternode-payments.h"
#include "masternodeconfig.h"
#include "netaddress.h"
#include "netbase.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "rpc/rpcevo.h"
#include "rpc/server.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#include "wallet/rpcwallet.h"
#endif

#include <univalue.h>

#include <boost/tokenizer.hpp>

static UniValue DmnToJson(const CDeterministicMNCPtr dmn)
{
    UniValue ret(UniValue::VOBJ);
    dmn->ToJson(ret);
    Coin coin;
    if (!WITH_LOCK(cs_main, return pcoinsTip->GetUTXOCoin(dmn->collateralOutpoint, coin); )) {
        return ret;
    }
    CTxDestination dest;
    if (!ExtractDestination(coin.out.scriptPubKey, dest)) {
        return ret;
    }
    ret.pushKV("collateralAddress", EncodeDestination(dest));
    return ret;
}

UniValue initmasternode(const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() != 1)) {
        throw std::runtime_error(
                "initmasternode \"privkey\"\n"
                "\nInitialize masternode on demand if it's not already initialized.\n"
                "\nArguments:\n"
                "1. privkey          (string, required) The masternode BLS operator private key.\n"

                "\nResult:\n"
                " success            (string) if the masternode initialization succeeded.\n"

                "\nExamples:\n" +
                HelpExampleCli("initmasternode", "\"bls-sk1xye8es37kk7y2mz7mad6yz7fdygttexqwhypa0u86hzw2crqgxfqy29ajm\"") +
                HelpExampleRpc("initmasternode", "\"bls-sk1xye8es37kk7y2mz7mad6yz7fdygttexqwhypa0u86hzw2crqgxfqy29ajm\""));
    }

    std::string _strMasterNodePrivKey = request.params[0].get_str();
    if (_strMasterNodePrivKey.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "Masternode key cannot be empty.");

    if (!activeMasternodeManager) {
        activeMasternodeManager = new CActiveDeterministicMasternodeManager();
        RegisterValidationInterface(activeMasternodeManager);
    }
    auto res = activeMasternodeManager->SetOperatorKey(_strMasterNodePrivKey);
    if (!res) throw std::runtime_error(res.getError());
    const CBlockIndex* pindexTip = WITH_LOCK(cs_main, return chainActive.Tip(); );
    activeMasternodeManager->Init(pindexTip);
    if (activeMasternodeManager->GetState() == CActiveDeterministicMasternodeManager::MASTERNODE_ERROR) {
        throw std::runtime_error(activeMasternodeManager->GetStatus());
    }
    return "success";
}

static inline bool filter(const std::string& str, const std::string& strFilter)
{
    return str.find(strFilter) != std::string::npos;
}

static inline bool filterMasternode(const UniValue& dmno, const std::string& strFilter, bool fEnabled)
{
    return strFilter.empty() || (filter("ENABLED", strFilter) && fEnabled)
                             || (filter("POSE_BANNED", strFilter) && !fEnabled)
                             || (filter(dmno["proTxHash"].get_str(), strFilter))
                             || (filter(dmno["collateralHash"].get_str(), strFilter))
                             || (filter(dmno["collateralAddress"].get_str(), strFilter))
                             || (filter(dmno["dmnstate"]["ownerAddress"].get_str(), strFilter))
                             || (filter(dmno["dmnstate"]["operatorPubKey"].get_str(), strFilter))
                             || (filter(dmno["dmnstate"]["votingAddress"].get_str(), strFilter));
}

UniValue listmasternodes(const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() > 1))
        throw std::runtime_error(
            "listmasternodes ( \"filter\" )\n"
            "\nGet a ranked list of masternodes\n"

            "\nArguments:\n"
            "1. \"filter\"    (string, optional) Filter search text. Partial match by txhash, status, or addr.\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"proTxHash\": \"hash\",          (string) Deterministic masternode transaction hash\n"
            "    \"collateralHash\": \"hash\",     (string) Collateral transaction hash\n"
            "    \"collateralIndex\": n,          (numeric) Collateral transaction output index\n"
            "    \"operatorReward\": n,           (numeric) Operator reward percentage\n"
            "    \"dmnstate\": {                  (object) Deterministic masternode state\n"
            "      \"ownerAddress\": \"addr\",    (string) Owner address\n"
            "      \"operatorPubKey\": \"key\",   (string) Operator public key\n"
            "      \"votingAddress\": \"addr\",   (string) Voting address\n"
            "      \"payoutAddress\": \"addr\",   (string) Payout address\n"
            "      \"status\": s,                 (string) Status (ENABLED/POSE_BANNED/etc)\n"
            "      ...\n"
            "    },\n"
            "    \"collateralAddress\": \"addr\"   (string) Collateral address\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listmasternodes", "") + HelpExampleRpc("listmasternodes", ""));


    const std::string& strFilter = request.params.size() > 0 ? request.params[0].get_str() : "";
    UniValue ret(UniValue::VARR);

    auto mnList = deterministicMNManager->GetListAtChainTip();
    mnList.ForEachMN(false, [&](const CDeterministicMNCPtr& dmn) {
        UniValue obj = DmnToJson(dmn);
        if (filterMasternode(obj, strFilter, !dmn->IsPoSeBanned())) {
            ret.push_back(obj);
        }
    });
    return ret;
}

UniValue getmasternodecount (const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() > 0))
        throw std::runtime_error(
            "getmasternodecount\n"
            "\nGet masternode count values\n"

            "\nResult:\n"
            "{\n"
            "  \"total\": n,        (numeric) Total masternodes\n"
            "  \"enabled\": n       (numeric) Enabled masternodes\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getmasternodecount", "") + HelpExampleRpc("getmasternodecount", ""));

    UniValue obj(UniValue::VOBJ);
    auto mnList = deterministicMNManager->GetListAtChainTip();
    obj.pushKV("total", (int)mnList.GetAllMNsCount());
    obj.pushKV("enabled", (int)mnList.GetValidMNsCount());

    return obj;
}

// Returns the registered deterministic masternode matching this conf entry's
// service address, or nullptr if none is registered yet.
static CDeterministicMNCPtr GetRegisteredDMNForEntry(const CMasternodeConfig::CMasternodeEntry& mne)
{
    if (!mne.IsDeterministic()) return nullptr;
    CService service;
    if (!Lookup(mne.getIp(), service, Params().GetDefaultPort(), false)) return nullptr;
    return deterministicMNManager->GetListAtChainTip().GetMNByService(service);
}

// Starts a single masternode conf entry via the DMN registration helper.
static void StartMasternodeEntryByType(UniValue& statusObjRet,
                                       const CMasternodeConfig::CMasternodeEntry& mne,
                                       CWallet* pwallet,
                                       const std::string& strCommand)
{
    if (strCommand == "missing" && GetRegisteredDMNForEntry(mne)) return;   // already running
    if (strCommand == "disabled") return;   // no disabled state for DMNs; skip
    statusObjRet.pushKV("alias", mne.getAlias());
    std::string txid, errorMessage;
    if (StartDeterministicMasternode(*pwallet, mne, txid, errorMessage)) {
        statusObjRet.pushKV("result", "success");
        statusObjRet.pushKV("txid", txid);
        statusObjRet.pushKV("vpsConfig", strprintf("-mnoperatorprivatekey=%s", mne.getPrivKey()));
    } else {
        statusObjRet.pushKV("result", "failed");
        statusObjRet.pushKV("error", errorMessage);
    }
}

UniValue startmasternode(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    std::string strCommand;
    if (!request.params.empty()) {
        strCommand = request.params[0].get_str();
    }

    if (strCommand == "local")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Local start is deprecated. Start your masternode from the controller wallet instead.");
    if (strCommand == "many")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Many set is deprecated. Use either 'all', 'missing', or 'disabled'.");

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4 ||
        (strCommand == "alias" && request.params.size() < 3))
        throw std::runtime_error(
            "startmasternode \"all|missing|disabled|alias\" lock_wallet ( \"alias\" reload_conf )\n"
            "\nAttempts to start one or more masternode(s)\n"
            "\nDeterministic entries (bech32 BLS key in the config) are registered via protx;\n"
            "the registration tx embeds the 4000 OLC collateral and the collateral output is locked.\n" +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. set          (string, required) Specify which set of masternode(s) to start.\n"
            "2. lock_wallet  (boolean, required) Lock wallet after completion.\n"
            "3. alias        (string, optional) Masternode alias. Required if using 'alias' as the set.\n"
            "4. reload_conf  (boolean, optional, default=False) reload the masternodes.conf data from disk"

            "\nResult:\n"
            "{\n"
            "  \"overall\": \"xxxx\",     (string) Overall status message\n"
            "  \"detail\": [\n"
            "    {\n"
            "      \"alias\": \"xxxx\",   (string) Node alias\n"
            "      \"result\": \"xxxx\",  (string) 'success' or 'failed'\n"
            "      \"error\": \"xxxx\"    (string) Error message, if failed\n"
            "      \"txid\": \"xxxx\",       (string, deterministic) Registration transaction id\n"
            "      \"vpsConfig\": \"xxxx\"   (string, deterministic) Line to paste into the VPS config: -mnoperatorprivatekey=<key>\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("startmasternode", "\"alias\" false \"my_mn\"") + HelpExampleRpc("startmasternode", "\"alias\" false \"my_mn\""));

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VBOOL, UniValue::VSTR, UniValue::VBOOL}, true);

    EnsureWalletIsUnlocked(pwallet);

    bool fLock = request.params[1].get_bool();
    bool fReload = request.params.size() > 3 ? request.params[3].get_bool() : false;

    // Check reload param
    if (fReload) {
        masternodeConfig.clear();
        std::string error;
        if (!masternodeConfig.read(error)) {
            throw std::runtime_error("Error reloading masternode.conf, " + error);
        }
    }

    if (strCommand == "all" || strCommand == "missing" || strCommand == "disabled") {
        if ((strCommand == "missing" || strCommand == "disabled") &&
            !g_tiertwo_sync_state.IsMasternodeListSynced()) {
            throw std::runtime_error("You can't use this command until masternode list is synced\n");
        }

        int successful = 0;
        int failed = 0;

        UniValue resultsObj(UniValue::VARR);

        for (const CMasternodeConfig::CMasternodeEntry& mne : masternodeConfig.getEntries()) {
            UniValue statusObj(UniValue::VOBJ);
            StartMasternodeEntryByType(statusObj, mne, pwallet, strCommand);
            if (statusObj.empty()) continue;   // skipped by missing/disabled filter
            resultsObj.push_back(statusObj);
            if (statusObj.exists("result") && statusObj["result"].get_str() == "success") {
                successful++;
            } else {
                failed++;
            }
        }
        if (fLock)
            pwallet->Lock();

        UniValue returnObj(UniValue::VOBJ);
        returnObj.pushKV("overall", strprintf("Successfully started %d masternodes, failed to start %d, total %d", successful, failed, successful + failed));
        returnObj.pushKV("detail", resultsObj);

        return returnObj;
    }

    if (strCommand == "alias") {
        std::string alias = request.params[2].get_str();

        bool found = false;

        UniValue statusObj(UniValue::VOBJ);

        for (const CMasternodeConfig::CMasternodeEntry& mne : masternodeConfig.getEntries()) {
            if (mne.getAlias() == alias) {
                found = true;
                StartMasternodeEntryByType(statusObj, mne, pwallet, strCommand);
                break;
            }
        }

        if (fLock)
            pwallet->Lock();

        if(!found) {
            statusObj.pushKV("alias", alias);
            statusObj.pushKV("result", "failed");
            statusObj.pushKV("error", "Could not find alias in config. Verify with listmasternodeconf.");
        }

        return statusObj;
    }
    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid set name %s.", strCommand));
}

UniValue createmasternodekey(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "createmasternodekey \"dmn\" ( \"alias\" )\n"
            "\nCreate a new masternode key set for a deterministic masternode\n"

            "\nArguments:\n"
            "1. \"dmn\"   (string, required) Must be set to \"dmn\" to create a deterministic masternode key set.\n"
            "2. \"alias\" (string, required) Alias to label the owner/voting keys with.\n"

            "\nResult:\n"
            "{\n"
            "  \"ownerAddress\": \"xxxx\",     (string) Owner address (also voting and payout address), stored in this wallet\n"
            "  \"votingAddress\": \"xxxx\",    (string) Voting address (same as ownerAddress)\n"
            "  \"payoutAddress\": \"xxxx\",    (string) Payout address (distinct from owner; created in this wallet)\n"
            "  \"operatorPrivKey\": \"xxxx\",  (string) BLS operator private key. Paste this into the VPS config as -mnoperatorprivatekey=<key>\n"
            "  \"confLine\": \"xxxx\"          (string, if alias given) Ready-to-paste masternode.conf line\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("createmasternodekey", "dmn mn1") + HelpExampleRpc("createmasternodekey", "dmn mn1"));

    if (request.params[0].get_str() != "dmn") {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "only deterministic masternodes are supported: createmasternodekey dmn \"my_alias\"");
    }

    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;
    EnsureWalletIsUnlocked(pwallet);

    std::string alias = request.params.size() > 1 ? request.params[1].get_str() : "";
    if (alias.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "an alias is required in dmn mode: createmasternodekey dmn \"my_alias\"");
    }

    // owner/voting address (reuse if already generated for this alias)
    const CTxDestination& ownerDest = GetOrCreateOwnerAddress(pwallet, alias);

    // payout address: distinct from owner/voting per consensus (bad-protx-payee-reuse)
    CTxDestination payoutDest;
    {
        LOCK(pwallet->cs_wallet);
        bool found = false;
        for (auto it = pwallet->NewAddressBookIterator(); it.IsValid(); it.Next()) {
            const AddressBook::CAddressBookData& data = it.GetValue();
            if (data.name == alias + "-payout") {
                const CTxDestination* pDest = it.GetCTxDestKey();
                if (pDest && IsValidDestination(*pDest)) {
                    payoutDest = *pDest;
                    found = true;
                }
                break;
            }
        }
        if (!found) {
            const CallResult<CTxDestination>& payoutRes = pwallet->getNewAddress(
                alias + "-payout", AddressBook::AddressBookPurpose::RECEIVE,
                CChainParams::Base58Type::PUBKEY_ADDRESS);
            if (!payoutRes) {
                throw JSONRPCError(RPC_WALLET_ERROR, payoutRes.getError());
            }
            payoutDest = *payoutRes.getObjResult();
        }
    }

    // operator BLS key
    CBLSSecretKey operatorKey;
    operatorKey.MakeNewKey();
    const std::string& operatorPrivKey = bls::EncodeSecret(Params(), operatorKey);

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("ownerAddress", EncodeDestination(ownerDest));
    ret.pushKV("votingAddress", EncodeDestination(ownerDest));
    ret.pushKV("payoutAddress", EncodeDestination(payoutDest));
    ret.pushKV("operatorPrivKey", operatorPrivKey);
    ret.pushKV("confLine", strprintf("%s %s %s", alias, "YOUR_VPS_IP:PORT", operatorPrivKey));
    return ret;
}

UniValue listmasternodeconf(const JSONRPCRequest& request)
{
    std::string strFilter = "";

    if (request.params.size() == 1) strFilter = request.params[0].get_str();

    if (request.fHelp || (request.params.size() > 1))
        throw std::runtime_error(
            "listmasternodeconf ( \"filter\" )\n"
            "\nPrint masternode.conf in JSON format\n"

            "\nArguments:\n"
            "1. \"filter\"    (string, optional) Filter search text. Partial match on alias, address, txHash, or status.\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"alias\": \"xxxx\",        (string) masternode alias\n"
            "    \"address\": \"xxxx\",      (string) masternode IP address\n"
            "    \"privateKey\": \"xxxx\",   (string) masternode private key\n"
            "    \"type\": \"deterministic\",(string) \"deterministic\"\n"
            "    \"txHash\": \"xxxx\",       (string) transaction hash\n"
            "    \"outputIndex\": n,       (numeric) transaction output index\n"
            "    \"status\": \"xxxx\"        (string) masternode status\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listmasternodeconf", "") + HelpExampleRpc("listmasternodeconf", ""));

    UniValue ret(UniValue::VARR);

    for (const CMasternodeConfig::CMasternodeEntry& mne : masternodeConfig.getEntries()) {
        const CDeterministicMNCPtr& dmn = GetRegisteredDMNForEntry(mne);
        std::string strStatus = dmn ? (dmn->IsPoSeBanned() ? "POSE_BANNED" : "ENABLED") : "MISSING";

        if (strFilter != "" && mne.getAlias().find(strFilter) == std::string::npos &&
            mne.getIp().find(strFilter) == std::string::npos &&
            strStatus.find(strFilter) == std::string::npos) continue;

        UniValue mnObj(UniValue::VOBJ);
        mnObj.pushKV("alias", mne.getAlias());
        mnObj.pushKV("address", mne.getIp());
        mnObj.pushKV("privateKey", mne.getPrivKey());
        mnObj.pushKV("type", mne.IsDeterministic() ? "deterministic" : "legacy");
        if (!mne.IsDeterministic()) {
            mnObj.pushKV("txHash", mne.getTxHash());
            mnObj.pushKV("outputIndex", mne.getOutputIndex());
        }
        mnObj.pushKV("status", strStatus);
        ret.push_back(mnObj);
    }

    return ret;
}

UniValue getmasternodestatus(const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() != 0))
        throw std::runtime_error(
            "getmasternodestatus\n"
            "\nPrint masternode status\n"

            "\nResult:\n"
            "{\n"
            "... !TODO ...\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getmasternodestatus", "") + HelpExampleRpc("getmasternodestatus", ""));

    if (!fMasterNode)
        throw JSONRPCError(RPC_MISC_ERROR, _("This is not a masternode."));

    if (!activeMasternodeManager) {
        throw JSONRPCError(RPC_MISC_ERROR, _("Active Masternode not initialized."));
    }

    if (!deterministicMNManager->IsDIP3Enforced()) {
        // this should never happen as ProTx transactions are not accepted yet
        throw JSONRPCError(RPC_MISC_ERROR, _("Deterministic masternodes are not enforced yet"));
    }

    const CActiveMasternodeInfo* amninfo = activeMasternodeManager->GetInfo();
    UniValue mnObj(UniValue::VOBJ);
    auto dmn = deterministicMNManager->GetListAtChainTip().GetMNByOperatorKey(amninfo->pubKeyOperator);
    if (dmn) {
        dmn->ToJson(mnObj);
    }
    mnObj.pushKV("netaddr", amninfo->service.ToString());
    mnObj.pushKV("status", activeMasternodeManager->GetStatus());
    return mnObj;
}

UniValue getmasternodewinners(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "getmasternodewinners ( blocks \"filter\" )\n"
            "\nPrint the masternode winners for the last n blocks\n"

            "\nArguments:\n"
            "1. blocks      (numeric, optional) Number of previous blocks to show (default: 10)\n"
            "2. filter      (string, optional) Search filter matching MN address\n"

            "\nResult (single winner):\n"
            "[\n"
            "  {\n"
            "    \"nHeight\": n,           (numeric) block height\n"
            "    \"winner\": {\n"
            "      \"address\": \"xxxx\",    (string) OrganicLife MN Address\n"
            "      \"nVotes\": n,          (numeric) Number of votes for winner\n"
            "    }\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nResult (multiple winners):\n"
            "[\n"
            "  {\n"
            "    \"nHeight\": n,           (numeric) block height\n"
            "    \"winner\": [\n"
            "      {\n"
            "        \"address\": \"xxxx\",  (string) OrganicLife MN Address\n"
            "        \"nVotes\": n,        (numeric) Number of votes for winner\n"
            "      }\n"
            "      ,...\n"
            "    ]\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getmasternodewinners", "") + HelpExampleRpc("getmasternodewinners", ""));

    int nHeight = WITH_LOCK(cs_main, return chainActive.Height());
    if (nHeight < 0) return "[]";

    int nLast = 10;
    std::string strFilter = "";

    if (request.params.size() >= 1)
        nLast = atoi(request.params[0].get_str());

    if (request.params.size() == 2)
        strFilter = request.params[1].get_str();

    UniValue ret(UniValue::VARR);

    for (int i = nHeight - nLast; i < nHeight + 20; i++) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("nHeight", i);

        std::string strPayment = GetRequiredPaymentsString(i);
        if (strFilter != "" && strPayment.find(strFilter) == std::string::npos) continue;

        if (strPayment.find(',') != std::string::npos) {
            UniValue winner(UniValue::VARR);
            boost::char_separator<char> sep(",");
            boost::tokenizer< boost::char_separator<char> > tokens(strPayment, sep);
            for (const std::string& t : tokens) {
                UniValue addr(UniValue::VOBJ);
                std::size_t pos = t.find(":");
                std::string strAddress = t.substr(0,pos);
                uint64_t nVotes = atoi(t.substr(pos+1));
                addr.pushKV("address", strAddress);
                addr.pushKV("nVotes", nVotes);
                winner.push_back(addr);
            }
            obj.pushKV("winner", winner);
        } else if (strPayment.find("Unknown") == std::string::npos) {
            UniValue winner(UniValue::VOBJ);
            std::size_t pos = strPayment.find(":");
            std::string strAddress = strPayment.substr(0,pos);
            uint64_t nVotes = atoi(strPayment.substr(pos+1));
            winner.pushKV("address", strAddress);
            winner.pushKV("nVotes", nVotes);
            obj.pushKV("winner", winner);
        } else {
            UniValue winner(UniValue::VOBJ);
            winner.pushKV("address", strPayment);
            winner.pushKV("nVotes", 0);
            obj.pushKV("winner", winner);
        }

            ret.push_back(obj);
    }

    return ret;
}

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                         actor (function)            okSafe argNames
  //  --------------------- ---------------------------  --------------------------  ------ --------
    { "masternode",         "createmasternodekey",       &createmasternodekey,       true,  {"mode","alias"} },
    { "masternode",         "getmasternodecount",        &getmasternodecount,        true,  {} },
    { "masternode",         "getmasternodestatus",       &getmasternodestatus,       true,  {} },
    { "masternode",         "getmasternodewinners",      &getmasternodewinners,      true,  {"blocks","filter"} },
    { "masternode",         "initmasternode",            &initmasternode,            true,  {"privkey"} },
    { "masternode",         "listmasternodeconf",        &listmasternodeconf,        true,  {"filter"} },
    { "masternode",         "listmasternodes",           &listmasternodes,           true,  {"filter"} },
    { "masternode",         "startmasternode",           &startmasternode,           true,  {"set","lock_wallet","alias","reload_conf"} },
};
// clang-format on

void RegisterMasternodeRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
