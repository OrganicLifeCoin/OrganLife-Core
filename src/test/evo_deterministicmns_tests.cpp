// Copyright (c) 2018-2021 The Dash Core developers
// Copyright (c) 2021-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "consensus/params.h"
#include "consensus/tx_verify.h"
#include "evo/specialtx_validation.h"
#include "evo/deterministicmns.h"
#include "netbase.h"
#include "pqtransaction.h"
#include "primitives/transaction.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

static CKey GetRandomKey()
{
    CKey keyRet;
    keyRet.MakeNewKey(true);
    return keyRet;
}

static CBLSSecretKey GetRandomBLSKey()
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    return sk;
}

static CScript GenerateRandomAddress()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(key.GetPubKey().GetID());
}

BOOST_AUTO_TEST_SUITE(deterministicmns_tests)

BOOST_AUTO_TEST_CASE(legacy_deterministic_mn_and_qfc_transactions_rejected_under_pq)
{
    BasicTestingSetup setup(CBaseChainParams::REGTEST);
    // PQ activation rejects all legacy transaction types, including the old
    // deterministic-masternode and quorum-commitment authorities.
    const std::vector<CTransaction::TxType> legacyTypes{
        CTransaction::PROREG, CTransaction::PROUPSERV, CTransaction::PROUPREG,
        CTransaction::PROUPREV, CTransaction::LLMQCOMM};
    for (const int v6Height : {Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT,
                               Consensus::NetworkUpgrade::ALWAYS_ACTIVE}) {
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, v6Height);
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 0);
        for (const auto type : legacyTypes) {
            CMutableTransaction tx;
            tx.nVersion = CTransaction::SAPLING;
            tx.nType = type;
            tx.vin.emplace_back(COutPoint(UINT256_ONE, 0));
            tx.vout.emplace_back(COIN, pq::GetScript(pq::KeyID{}));
            CValidationState state;
            const bool accepted = ContextualCheckTransaction(
                MakeTransactionRef(tx), state, Params(), 1, true, false);
            BOOST_CHECK_MESSAGE(!accepted,
                                strprintf("legacy type %d accepted under PQ activation", static_cast<int>(type)));
            BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pq-only-transaction");
        }
    }
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0,
                                    Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
}

BOOST_AUTO_TEST_CASE(dmn_getlistforblock_genesis_anchor)
{
    // Mainnet and testnet activate V6_0 (DIP3) at genesis (activation height 0),
    // so there is no block "before the enforcement" to anchor the initial empty
    // masternode list. With an empty evo DB, GetListForBlock() must anchor the
    // empty list at the genesis block itself instead of throwing.
    for (const std::string& chainName : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET}) {
        BasicTestingSetup setup(chainName);

        CBlockIndex genesis;
        genesis.nHeight = 0;
        genesis.pprev = nullptr;
        genesis.phashBlock = &Params().GetConsensus().hashGenesisBlock;

        deterministicMNManager->SetTipIndex(&genesis);
        CDeterministicMNList list;
        BOOST_CHECK_NO_THROW(list = deterministicMNManager->GetListAtChainTip());
        BOOST_CHECK_EQUAL(list.GetAllMNsCount(), 0u);
        BOOST_CHECK_EQUAL(list.GetHeight(), -1);
    }
}

// Creates a ProRegTx carrying the given service address. It uses an internal
// collateral (first output) and carries no payload signature, which is enough
// for the non-contextual checks (CheckSpecialTxNoContext).
static CMutableTransaction CreateProRegTxWithService(const CService& service)
{
    ProRegPL pl;
    pl.collateralOutpoint = COutPoint(UINT256_ZERO, 0); // internal collateral
    pl.addr = service;
    CKey ownerKey = GetRandomKey();
    pl.keyIDOwner = ownerKey.GetPubKey().GetID();
    pl.pubKeyOperator = GetRandomBLSKey().GetPublicKey();
    pl.keyIDVoting = pl.keyIDOwner;
    pl.scriptPayout = GenerateRandomAddress();
    pl.nOperatorReward = 0;

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROREG;
    tx.vout.emplace_back(Params().GetConsensus().nMNCollateralAmt, GenerateRandomAddress());

    pl.inputsHash = CalcTxInputsHash(tx);
    SetTxPayload(tx, pl);
    return tx;
}

BOOST_AUTO_TEST_CASE(protx_service_accepts_ipv6)
{
    BasicTestingSetup setup(CBaseChainParams::REGTEST);

    // IPv4 service registrations keep being accepted (control).
    const CService ipv4Service = LookupNumeric("1.1.1.1", Params().GetDefaultPort());
    BOOST_CHECK(ipv4Service.IsValid());
    CValidationState state;
    BOOST_CHECK_MESSAGE(
        WITH_LOCK(cs_main, return CheckSpecialTxNoContext(CreateProRegTxWithService(ipv4Service), state);),
        state.GetRejectReason());

    // An IPv6 service must be accepted as well (masternodes on IPv6).
    const CService ipv6Service = LookupNumeric("2001:4860:4860::8888", Params().GetDefaultPort());
    BOOST_CHECK(ipv6Service.IsIPv6());
    state = CValidationState();
    BOOST_CHECK_MESSAGE(
        WITH_LOCK(cs_main, return CheckSpecialTxNoContext(CreateProRegTxWithService(ipv6Service), state);),
        state.GetRejectReason());
}

BOOST_AUTO_TEST_SUITE_END()
