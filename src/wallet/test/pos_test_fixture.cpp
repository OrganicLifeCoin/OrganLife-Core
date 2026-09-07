// Copyright (c) 2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/pos_test_fixture.h"
#include "wallet/wallet.h"
#include "pqtransaction.h"

#include <boost/test/unit_test.hpp>

TestPoSChainSetup::TestPoSChainSetup() : TestChainSetup(0)
{
    initZKSNARKS(); // init zk-snarks lib

    bool fFirstRun;
    pwalletMain = std::make_unique<CWallet>("pos-wallet", WalletDatabase::Create(GetDataDir() / "pos-wallet"));
    BOOST_REQUIRE_EQUAL(pwalletMain->LoadWallet(fFirstRun), DB_LOAD_OK);
    const SecureString passphrase = "pq-pos-fixture";
    BOOST_REQUIRE(pwalletMain->EncryptWallet(passphrase));
    BOOST_REQUIRE(pwalletMain->Unlock(passphrase));
    std::string address;
    BOOST_REQUIRE(pwalletMain->GeneratePQAddress(address));
    pq::KeyID id;
    BOOST_REQUIRE(pq::DecodeAddress(address, Params().NetworkIDString(), id));
    RegisterValidationInterface(pwalletMain.get());

    int posActivation = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight - 1;
    for (int i = 0; i < posActivation; i++) {
        CBlock b = CreateAndProcessBlock({}, pq::GetScript(id));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), b.GetHash());
        coinbaseTxns.emplace_back(*b.vtx[0]);
    }

    // Avoid spending the premine output in PoS fork unit tests. With the custom monetary policy,
    // the premine at height 1 can dominate wallet coin selection and make tests flaky by causing
    // unrelated transactions to spend the same large UTXO across competing chains.
    SyncWithValidationInterfaceQueue();
    if (!coinbaseTxns.empty() && !coinbaseTxns[0].vout.empty()) {
        LOCK(pwalletMain->cs_wallet);
        pwalletMain->LockCoin(COutPoint(coinbaseTxns[0].GetHash(), 0));
    }
}

TestPoSChainSetup::~TestPoSChainSetup()
{
    SyncWithValidationInterfaceQueue();
    UnregisterValidationInterface(pwalletMain.get());
    pwalletMain->Flush(true);
}
