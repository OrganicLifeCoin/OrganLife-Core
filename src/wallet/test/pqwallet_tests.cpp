// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <wallet/test/wallet_test_fixture.h>
#include <wallet/pqkey.h>
#include <wallet/rpcwallet.h>
#include <key_io.h>
#include <pqtransaction.h>
#include <policy/policy.h>
#include <wallet/fees.h>
#include <validation.h>
#include <rpc/server.h>
#include <script/sign.h>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <chrono>
#include <future>

extern UniValue CallRPC(std::string args);

namespace {
const SecureString PASSPHRASE = "pq-test-passphrase";
const SecureString NEW_PASSPHRASE = "pq-test-new-passphrase";

struct DiskWallet : CWallet {
    using CWallet::CWallet;
    ~DiskWallet() { GetDBHandle().Flush(true); }
};

void EncryptTestWallet(CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    CKey key;
    key.MakeNewKey(true);
    BOOST_REQUIRE(wallet.AddKeyPubKey(key, key.GetPubKey()));
    BOOST_REQUIRE(wallet.EncryptWallet(PASSPHRASE));
}

UniValue CallPQRPC(const std::string& args)
{
    static unsigned int sequence = 0;
    return CallRPC(args + " " + (GetDataDir() / ("pq-rpc-backup-" + std::to_string(++sequence) + ".dat")).string());
}

struct RegisteredWallet : SaplingTestingSetup {
    DiskWallet m_wallet;
    explicit RegisteredWallet(const std::string& network = CBaseChainParams::REGTEST) : SaplingTestingSetup(network),
        m_wallet("pq-fixture", WalletDatabase::Create(GetDataDir() / "pq-fixture"))
    {
        bool first_run;
        BOOST_REQUIRE_EQUAL(m_wallet.LoadWallet(first_run), DB_LOAD_OK);
        RegisterWalletRPCCommands(tableRPC);
        vpwallets.push_back(&m_wallet);
    }
    ~RegisteredWallet()
    {
        vpwallets.erase(std::find(vpwallets.begin(), vpwallets.end(), &m_wallet));
    }
};

struct RegisteredTestnetWallet : RegisteredWallet {
    RegisteredTestnetWallet() : RegisteredWallet(CBaseChainParams::TESTNET) {}
};

void CheckWrongNetworkLoad(CWallet& wallet, const std::string& other_network)
{
    const auto network = Params().NetworkIDString();
    SelectParams(other_network);
    CWallet loaded("wrong-network", WalletDatabase::CreateDummy());
    BOOST_CHECK_EQUAL(WalletBatch(wallet.GetDBHandle()).LoadWallet(&loaded), DB_CORRUPT);
    SelectParams(network);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(pqwallet_tests, RegisteredWallet)

BOOST_AUTO_TEST_CASE(empty_wallet_passphrase_authentication)
{
    const SecureString wrong_passphrase = "wrong-pq-test-passphrase";
    BOOST_REQUIRE(m_wallet.EncryptWallet(PASSPHRASE));
    BOOST_CHECK(m_wallet.IsLocked());
    BOOST_CHECK(!m_wallet.Unlock(wrong_passphrase));
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
}

BOOST_AUTO_TEST_CASE(encryption_lock_staking_and_network_gates)
{
    std::string address = "stale";
    BOOST_CHECK(!m_wallet.GeneratePQAddress(address));
    BOOST_CHECK(address.empty());
    EncryptTestWallet(m_wallet);
    BOOST_CHECK(!m_wallet.GeneratePQAddress(address));
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, true));
    BOOST_CHECK(!m_wallet.GeneratePQAddress(address));
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, false));
    BOOST_REQUIRE(m_wallet.GeneratePQAddress(address));
    mldsa44::Key key;
    BOOST_REQUIRE(m_wallet.GetPQKey(address, key));
    const auto pub = key.GetPublicKey();
    BOOST_REQUIRE(m_wallet.Lock());
    BOOST_CHECK(!m_wallet.GetPQKey(address, key));
    BOOST_CHECK(!key.IsValid());
    BOOST_CHECK_EQUAL(m_wallet.GetPQAddresses().size(), 1U);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, true));
    BOOST_CHECK(!m_wallet.GetPQKey(address, key));
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, false));
    BOOST_REQUIRE(m_wallet.GetPQKey(address, key));
    BOOST_CHECK(key.GetPublicKey() == pub);
    CheckWrongNetworkLoad(m_wallet, CBaseChainParams::TESTNET);
    SelectParams(CBaseChainParams::MAIN);
    BOOST_CHECK(!m_wallet.GetPQKey(address, key));
    BOOST_CHECK(!key.IsValid());
    std::string rejected;
    BOOST_CHECK(!m_wallet.GeneratePQAddress(rejected));
    BOOST_CHECK(m_wallet.GetPQAddresses().empty());
    SelectParams(CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_CASE(binary_backup_restore_and_passphrase_change)
{
    const auto original_path = GetDataDir() / "pq-original";
    const auto backup_path = GetDataDir() / "pq-backup";
    fs::create_directories(backup_path);
    std::vector<std::string> addresses;
    mldsa44::PublicKey public_key;
    {
        DiskWallet original("pq-original", WalletDatabase::Create(original_path));
        bool first_run;
        BOOST_REQUIRE_EQUAL(original.LoadWallet(first_run), DB_LOAD_OK);
        EncryptTestWallet(original);
        BOOST_REQUIRE(original.Unlock(PASSPHRASE));
        std::string address;
        BOOST_REQUIRE(original.GeneratePQAddress(address));
        std::string second;
        BOOST_REQUIRE(original.GeneratePQAddress(second));
        BOOST_CHECK(address != second);
        addresses = original.GetPQAddresses();
        BOOST_REQUIRE_EQUAL(addresses.size(), 2U);
        mldsa44::Key key;
        BOOST_REQUIRE(original.GetPQKey(addresses[0], key));
        public_key = key.GetPublicKey();
        BOOST_REQUIRE(original.ChangeWalletPassphrase(PASSPHRASE, NEW_PASSPHRASE));
        BOOST_REQUIRE(original.Lock());
        BOOST_REQUIRE(original.BackupWallet(backup_path.string()));
    }
    for (const auto& path : {original_path, backup_path}) {
        DiskWallet restored(path.filename().string(), WalletDatabase::Create(path));
        bool first_run;
        BOOST_REQUIRE_EQUAL(restored.LoadWallet(first_run), DB_LOAD_OK);
        BOOST_CHECK(restored.IsLocked());
        BOOST_CHECK(restored.GetPQAddresses() == addresses);
        mldsa44::Key key;
        BOOST_CHECK(!restored.GetPQKey(addresses[0], key));
        BOOST_CHECK(!restored.Unlock(PASSPHRASE));
        BOOST_REQUIRE(restored.Unlock(NEW_PASSPHRASE));
        BOOST_REQUIRE(restored.GetPQKey(addresses[0], key));
        BOOST_CHECK(key.GetPublicKey() == public_key);
        const std::vector<unsigned char> message{0, 1, 255};
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(key.Sign(message, {}, signature));
        BOOST_CHECK(mldsa44::Verify(public_key, message, {}, signature));
        BOOST_REQUIRE(restored.Lock());
    }
}

BOOST_AUTO_TEST_CASE(corrupt_records_are_not_silently_ignored)
{
    EncryptTestWallet(m_wallet);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
    std::string address;
    BOOST_REQUIRE(m_wallet.GeneratePQAddress(address));
    pq::KeyID id;
    BOOST_REQUIRE(pq::DecodeAddress(address, "regtest", id));
    pqwallet::Record record;
    {
        BerkeleyBatch batch(m_wallet.GetDBHandle());
        BOOST_REQUIRE(batch.Read(std::make_pair(std::string("pqkey44"), id), record));
    }
    BOOST_CHECK(!m_wallet.LoadPQKey(id, record));
    auto wrong_id = id;
    wrong_id[0] ^= 1;
    BOOST_CHECK(!m_wallet.LoadPQKey(wrong_id, record));
    BOOST_CHECK(WalletBatch::IsKeyType("pqkey44"));
    for (bool valid : {true, false}) {
        auto recover_record = record;
        if (!valid) recover_record.version = 2;
        CDataStream key_bytes(SER_DISK, CLIENT_VERSION), value_bytes(SER_DISK, CLIENT_VERSION);
        key_bytes << std::make_pair(std::string("pqkey44"), id);
        value_bytes << recover_record;
        CWallet recovery("recovery", WalletDatabase::CreateDummy());
        BOOST_CHECK_EQUAL(WalletBatch::RecoverKeysOnlyFilter(&recovery, key_bytes, value_bytes), valid);
    }
    for (int corruption = 0; corruption < 3; ++corruption) {
        auto bad = record;
        if (corruption == 0) bad.version = 2;
        if (corruption == 1) bad.public_key[0] ^= 1;
        if (corruption == 2) bad.encrypted_seed[0] ^= 1;
        {
            BerkeleyBatch batch(m_wallet.GetDBHandle());
            BOOST_REQUIRE(batch.Write(std::make_pair(std::string("pqkey44"), id), bad));
        }
        CWallet loaded("corrupt", WalletDatabase::CreateDummy());
        const auto result = WalletBatch(m_wallet.GetDBHandle()).LoadWallet(&loaded);
        if (corruption < 2) {
            BOOST_CHECK_EQUAL(result, DB_CORRUPT);
        } else {
            BOOST_REQUIRE_EQUAL(result, DB_LOAD_OK);
            BOOST_CHECK(!loaded.Unlock(PASSPHRASE));
            BOOST_CHECK(loaded.IsLocked());
        }
    }
    // Exact record sizes, including key/value trailing bytes, are mandatory.
    for (int malformed = 0; malformed < 3; ++malformed) {
        {
            BerkeleyBatch batch(m_wallet.GetDBHandle());
            const auto dbkey = std::make_pair(std::string("pqkey44"), id);
            if (malformed == 0) BOOST_REQUIRE(batch.Write(dbkey, std::make_pair(record, uint8_t{0})));
            if (malformed == 1) BOOST_REQUIRE(batch.Write(dbkey, std::array<unsigned char, 1384>{}));
            if (malformed == 2) {
                BOOST_REQUIRE(batch.Erase(dbkey));
                BOOST_REQUIRE(batch.Write(std::make_pair(dbkey, uint8_t{0}), record));
            }
        }
        CWallet loaded("malformed", WalletDatabase::CreateDummy());
        BOOST_CHECK_EQUAL(WalletBatch(m_wallet.GetDBHandle()).LoadWallet(&loaded), DB_CORRUPT);
    }
    {
        BerkeleyBatch batch(m_wallet.GetDBHandle());
        BOOST_REQUIRE(batch.Erase(std::make_pair(std::make_pair(std::string("pqkey44"), id), uint8_t{0})));
        BOOST_REQUIRE(batch.Write(std::make_pair(std::string("pqkey44"), id), record));
    }
    SelectParams(CBaseChainParams::MAIN);
    CWallet wrong_network("wrong-network", WalletDatabase::CreateDummy());
    BOOST_CHECK_EQUAL(WalletBatch(m_wallet.GetDBHandle()).LoadWallet(&wrong_network), DB_CORRUPT);
    SelectParams(CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_CASE(missing_master_key_is_corrupt)
{
    EncryptTestWallet(m_wallet);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
    std::string address;
    BOOST_REQUIRE(m_wallet.GeneratePQAddress(address));
    {
        BerkeleyBatch batch(m_wallet.GetDBHandle());
        for (const auto& master : m_wallet.mapMasterKeys)
            BOOST_REQUIRE(batch.Erase(std::make_pair(std::string("mkey"), master.first)));
    }
    CWallet loaded("missing-master", WalletDatabase::CreateDummy());
    BOOST_CHECK_EQUAL(WalletBatch(m_wallet.GetDBHandle()).LoadWallet(&loaded), DB_CORRUPT);
}

BOOST_AUTO_TEST_CASE(rpc_addresses_use_only_the_pq_payment_path)
{
    BOOST_CHECK_THROW(CallPQRPC("getnewpqaddress"), std::runtime_error);
    EncryptTestWallet(m_wallet);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
    const auto created = CallPQRPC("getnewpqaddress");
    const auto address = created["address"].get_str();
    BOOST_CHECK(created["experimental"].get_bool());
    BOOST_CHECK(created["payable"].get_bool());
    BOOST_CHECK(!IsValidDestination(DecodeDestination(address)));
    BOOST_CHECK_THROW(CallRPC("sendtoaddress " + address + " 1"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("getnewpqaddress unexpected extra"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("listpqaddresses unexpected"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("dumpwallet unused"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("dumpprivkey " + address), std::runtime_error);
    BOOST_REQUIRE(m_wallet.Lock());
    const auto listed = CallRPC("listpqaddresses");
    BOOST_CHECK_EQUAL(listed["addresses"].size(), 1U);
    BOOST_CHECK_EQUAL(listed["addresses"][0].get_str(), address);
    BOOST_CHECK(listed["payable"].get_bool());
    BOOST_CHECK_THROW(CallPQRPC("getnewpqaddress"), std::runtime_error);
    SelectParams(CBaseChainParams::MAIN);
    BOOST_CHECK_THROW(CallPQRPC("getnewpqaddress"), std::runtime_error);
    BOOST_CHECK_THROW(CallRPC("listpqaddresses"), std::runtime_error);
    SelectParams(CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_CASE(rpc_address_creation_requires_a_successful_backup)
{
    EncryptTestWallet(m_wallet);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
    BOOST_CHECK_THROW(CallRPC("getnewpqaddress"), std::runtime_error);
    const auto key_count = m_wallet.GetPQAddresses().size();
    const auto missing_parent = GetDataDir() / "missing-pq-backup-dir" / "wallet.dat";
    BOOST_CHECK_EXCEPTION(CallRPC("getnewpqaddress " + missing_parent.string()), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("backup failed") != std::string::npos; });
    BOOST_CHECK_EQUAL(m_wallet.GetPQAddresses().size(), key_count);
    CWallet reloaded("pq-rpc-cleanup", WalletDatabase::CreateDummy());
    BOOST_REQUIRE_EQUAL(WalletBatch(m_wallet.GetDBHandle()).LoadWallet(&reloaded), DB_LOAD_OK);
    BOOST_CHECK_EQUAL(reloaded.GetPQAddresses().size(), key_count);
    const auto backup = GetDataDir() / "pq-rpc-address-backup.dat";
    const auto created = CallRPC("getnewpqaddress " + backup.string());
    BOOST_CHECK(fs::exists(backup));
    const auto addresses = m_wallet.GetPQAddresses();
    BOOST_CHECK(addresses.end() != std::find(addresses.begin(), addresses.end(), created["address"].get_str()));
}

BOOST_AUTO_TEST_CASE(payment_rpc_activation_and_unlock_gates)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
    EncryptTestWallet(m_wallet);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
    const auto address = CallPQRPC("getnewpqaddress")["address"].get_str();
    BOOST_CHECK_THROW(CallRPC("fundpqaddress"), std::runtime_error);
    BOOST_CHECK_EXCEPTION(CallPQRPC("sendpqtoaddress " + address + " 1"), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("active") != std::string::npos; });
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 0);
    BOOST_CHECK(CallRPC("listpqaddresses")["payable"].get_bool());
    BOOST_CHECK(CallPQRPC("getnewpqaddress")["payable"].get_bool());
    BOOST_CHECK_EQUAL(CallRPC("listpqunspent").size(), 0U);
    const std::string call = "sendpqtoaddress " + address + " 1";
    BOOST_CHECK_EXCEPTION(CallPQRPC(call), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("Insufficient") != std::string::npos; });
    BOOST_CHECK_THROW(CallPQRPC("sendpqtoaddress invalid 1"), std::runtime_error);
    BOOST_CHECK_THROW(CallPQRPC("sendpqtoaddress " + address + " -1"), std::runtime_error);
    BOOST_REQUIRE(m_wallet.Lock());
    BOOST_CHECK_THROW(CallPQRPC(call), std::runtime_error);
    BOOST_CHECK_EQUAL(CallRPC("listpqunspent").size(), 0U);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, true));
    BOOST_CHECK_THROW(CallPQRPC(call), std::runtime_error);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, false));
    BOOST_CHECK_THROW(CallRPC("listpqunspent unexpected"), std::runtime_error);
    SelectParams(CBaseChainParams::MAIN);
    BOOST_CHECK_THROW(CallRPC("listpqunspent"), std::runtime_error);
    BOOST_CHECK_THROW(CallPQRPC("sendpqtoaddress " + address + " 1"), std::runtime_error);
    SelectParams(CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_CASE(pq_relevance_is_separate_from_ordinary_coin_selection)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V5_0, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    EncryptTestWallet(m_wallet);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
    std::string address;
    BOOST_REQUIRE(m_wallet.GeneratePQAddress(address));
    pq::KeyID id;
    BOOST_REQUIRE(pq::DecodeAddress(address, "regtest", id));
    CMutableTransaction incoming;
    incoming.vin.emplace_back(uint256S("1234"), 0);
    incoming.vout.emplace_back(10 * COIN, pq::GetScript(id));
    const auto incoming_ref = MakeTransactionRef(incoming);
    const auto* tip = WITH_LOCK(cs_main, return chainActive.Tip());
    const CWalletTx::Confirmation confirmed(CWalletTx::Status::CONFIRMED,
        tip->nHeight, tip->GetBlockHash(), 0);
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(tip);
        BOOST_REQUIRE(m_wallet.AddToWalletIfInvolvingMe(incoming_ref, confirmed, true));
        pcoinsTip->AddCoin(COutPoint(incoming_ref->GetHash(), 0), Coin(incoming.vout[0], tip->nHeight, false, false), false);
        BOOST_CHECK_EQUAL(m_wallet.IsMine(incoming.vout[0]), ISMINE_NO);
        std::vector<COutput> ordinary_coins;
        m_wallet.AvailableCoins(&ordinary_coins);
        BOOST_CHECK(ordinary_coins.empty());
        BOOST_CHECK_EQUAL(m_wallet.GetAvailableBalance(), 0);
    }
    const auto listed = CallRPC("listpqunspent");
    BOOST_REQUIRE_EQUAL(listed.size(), 1U);
    BOOST_CHECK_EQUAL(listed[0]["address"].get_str(), address);
    BOOST_CHECK_EQUAL(listed[0]["confirmations"].get_int(), 1);
    BOOST_CHECK(!listed[0]["locked"].get_bool());
    WITH_LOCK(m_wallet.cs_wallet, m_wallet.LockCoin(COutPoint(incoming_ref->GetHash(), 0)));
    BOOST_CHECK(CallRPC("listpqunspent")[0]["locked"].get_bool());
    WITH_LOCK(m_wallet.cs_wallet, m_wallet.UnlockCoin(COutPoint(incoming_ref->GetHash(), 0)));
    CMutableTransaction outgoing;
    outgoing.vin.emplace_back(incoming_ref->GetHash(), 0);
    outgoing.vout.emplace_back(9 * COIN, CScript() << OP_RETURN);
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        BOOST_REQUIRE(m_wallet.AddToWalletIfInvolvingMe(MakeTransactionRef(outgoing),
            CWalletTx::Confirmation{}, true));
        BOOST_CHECK(m_wallet.IsSpent(incoming_ref->GetHash(), 0));
    }
    BOOST_CHECK_EQUAL(CallRPC("listpqunspent").size(), 0U);
    BOOST_REQUIRE(m_wallet.AbandonTransaction(outgoing.GetHash()));
    BOOST_CHECK_EQUAL(CallRPC("listpqunspent").size(), 1U);
}

BOOST_AUTO_TEST_CASE(pq_listing_waits_for_pending_wallet_notifications)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V5_0, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, Consensus::NetworkUpgrade::ALWAYS_ACTIVE);
    EncryptTestWallet(m_wallet);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
    std::string address;
    BOOST_REQUIRE(m_wallet.GeneratePQAddress(address));
    pq::KeyID id;
    BOOST_REQUIRE(pq::DecodeAddress(address, "regtest", id));
    CMutableTransaction incoming;
    incoming.vin.emplace_back(uint256S("9abc"), 0);
    incoming.vout.emplace_back(COIN, pq::GetScript(id));
    const auto tx = MakeTransactionRef(incoming);
    std::promise<void> release_notification, notification_started, notification_finished;
    auto gate = release_notification.get_future().share();
    auto started = notification_started.get_future();
    auto finished = notification_finished.get_future();
    bool recorded = false;
    CallFunctionInValidationInterfaceQueue([&] {
        notification_started.set_value();
        gate.wait();
        LOCK2(cs_main, m_wallet.cs_wallet);
        const auto* tip = chainActive.Tip();
        m_wallet.SetLastBlockProcessed(tip);
        recorded = m_wallet.AddToWalletIfInvolvingMe(tx,
            {CWalletTx::Status::CONFIRMED, tip->nHeight, tip->GetBlockHash(), 0}, true);
        pcoinsTip->AddCoin(COutPoint(tx->GetHash(), 0), Coin(tx->vout[0], tip->nHeight, false, false), false);
        notification_finished.set_value();
    });
    started.wait();
    auto listing = std::async(std::launch::async, [] { return CallRPC("listpqunspent"); });
    const bool waited = listing.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
    release_notification.set_value();
    const auto result = listing.get();
    finished.wait();
    BOOST_CHECK(waited);
    BOOST_CHECK(recorded);
    BOOST_REQUIRE_EQUAL(result.size(), 1U);
    BOOST_CHECK_EQUAL(result[0]["address"].get_str(), address);
}

void CheckPaymentTransactions(CWallet& m_wallet)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 0);
    EncryptTestWallet(m_wallet);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
    std::string address;
    BOOST_REQUIRE(m_wallet.GeneratePQAddress(address));
    pq::KeyID id;
    BOOST_REQUIRE(pq::DecodeAddress(address, Params().NetworkIDString(), id));
    const auto other_address = pq::EncodeAddress(id, Params().IsRegTestNet() ? "test" : "regtest");
    BOOST_CHECK_EXCEPTION(CallPQRPC("sendpqtoaddress " + other_address + " 1"), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("Invalid") != std::string::npos; });
    const auto* tip = WITH_LOCK(cs_main, return chainActive.Tip());
    const CWalletTx::Confirmation confirmed(CWalletTx::Status::CONFIRMED,
        tip->nHeight, tip->GetBlockHash(), 0);
    CMutableTransaction source;
    source.vin.emplace_back(uint256S("5678"), 0);
    source.vout.emplace_back(30 * COIN, pq::GetScript(id));
    const auto source_ref = MakeTransactionRef(source);
    const COutPoint pq_coin(source_ref->GetHash(), 0);
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        m_wallet.SetLastBlockProcessed(tip);
        BOOST_REQUIRE(m_wallet.AddToWalletIfInvolvingMe(source_ref, confirmed, true));
        pcoinsTip->AddCoin(COutPoint(source_ref->GetHash(), 0), Coin(source.vout[0], tip->nHeight, false, false), false);
        std::vector<COutput> ordinary_coins;
        m_wallet.AvailableCoins(&ordinary_coins);
        BOOST_CHECK(ordinary_coins.empty());
    }
    BOOST_REQUIRE_EQUAL(CallRPC("listpqunspent").size(), 1U);
    CMutableTransaction stake;
    stake.vin.emplace_back(pq_coin);
    stake.vout.emplace_back(0, CScript());
    stake.vout.emplace_back(30 * COIN, pq::GetScript(id));
    BOOST_REQUIRE(m_wallet.SignCoinStake(stake));
    BOOST_CHECK(stake.vin[0].scriptSig.empty());
    pq::Payload stake_payload;
    BOOST_REQUIRE(pq::DecodePayload(stake, stake_payload));
    BOOST_CHECK_EQUAL(stake_payload.mode, pq::STAKE);
    std::string stake_reason;
    BOOST_CHECK(pq::VerifyInputs(stake, {source.vout[0]}, Params(), stake_reason));
    WITH_LOCK(m_wallet.cs_wallet, m_wallet.LockCoin(pq_coin));
    BOOST_CHECK_THROW(CallPQRPC("sendpqtoaddress " + address + " 1"), std::runtime_error);
    WITH_LOCK(m_wallet.cs_wallet, m_wallet.UnlockCoin(pq_coin));
    const auto transaction_count = WITH_LOCK(m_wallet.cs_wallet, return m_wallet.mapWallet.size());
    const auto key_count = m_wallet.GetPQAddresses().size();
    const auto missing_backup = GetDataDir() / "missing-pq-payment-backup-dir" / "wallet.dat";
    BOOST_CHECK_EXCEPTION(CallRPC("sendpqtoaddress " + address + " 1 " + missing_backup.string()), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("backup failed") != std::string::npos; });
    BOOST_CHECK_EQUAL(WITH_LOCK(m_wallet.cs_wallet, return m_wallet.mapWallet.size()), transaction_count);
    BOOST_CHECK_EQUAL(m_wallet.GetPQAddresses().size(), key_count);
    BOOST_CHECK(!WITH_LOCK(m_wallet.cs_wallet, return m_wallet.IsSpent(pq_coin)));
    const auto old_key_count = m_wallet.GetPQAddresses().size();
    const auto sent = CallPQRPC("sendpqtoaddress " + address + " 1");
    const auto transfer = WITH_LOCK(m_wallet.cs_wallet, return m_wallet.mapWallet.at(uint256S(sent["txid"].get_str())).tx);
    BOOST_REQUIRE_EQUAL(transfer->vin.size(), 1U);
    BOOST_CHECK(transfer->vin[0].scriptSig.empty());
    BOOST_REQUIRE_EQUAL(transfer->vout.size(), 2U);
    for (const auto& out : transfer->vout) {
        pq::KeyID output_id;
        BOOST_CHECK(pq::ExtractID(out.scriptPubKey, output_id));
        BOOST_CHECK(!IsDust(out, dustRelayFee));
        BOOST_CHECK_EQUAL(GetDustThreshold(out, dustRelayFee),
            dustRelayFee.GetFee(GetSerializeSize(out, 0) + 32 + 4 + 1 + 4 + pq::AUTH_SIZE + 3));
    }
    pq::Payload payload;
    BOOST_REQUIRE(pq::DecodePayload(*transfer, payload));
    BOOST_CHECK_EQUAL(payload.mode, pq::TRANSFER);
    BOOST_REQUIRE_EQUAL(payload.authorizations.size(), 1U);
    std::string reason;
    BOOST_CHECK(pq::VerifyInputs(*transfer, {source.vout[0]}, Params(), reason));
    {
        LOCK(cs_main);
        BOOST_CHECK_MESSAGE(IsStandardTx(transfer, chainActive.Height() + 1, reason), reason);
        BOOST_CHECK(AreInputsStandard(*transfer, *pcoinsTip));
    }
    BOOST_CHECK(AmountFromValue(sent["fee"]) >= GetMinimumFee(GetSerializeSize(*transfer, PROTOCOL_VERSION), nTxConfirmTarget, mempool));
    BOOST_CHECK_EQUAL(m_wallet.GetPQAddresses().size(), old_key_count + 1);
    BOOST_CHECK(WITH_LOCK(m_wallet.cs_wallet, return m_wallet.IsSpent(pq_coin)));
    // Both outputs belong to this test wallet. Spending almost their full sum requires both authorizations.
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        pcoinsTip->SpendCoin(pq_coin);
        mempool.clear();
        BOOST_REQUIRE(m_wallet.AddToWalletIfInvolvingMe(transfer, confirmed, true));
        m_wallet.mapWallet.at(transfer->GetHash()).fInMempool = false;
        for (size_t i = 0; i < transfer->vout.size(); ++i)
            pcoinsTip->AddCoin(COutPoint(transfer->GetHash(), i), Coin(transfer->vout[i], tip->nHeight, false, false), false);
    }
    const auto combined = CallPQRPC("sendpqtoaddress " + address + " 29");
    const auto two_input_tx = WITH_LOCK(m_wallet.cs_wallet, return m_wallet.mapWallet.at(uint256S(combined["txid"].get_str())).tx);
    BOOST_REQUIRE_EQUAL(two_input_tx->vin.size(), 2U);
    std::vector<CTxOut> combined_prevouts;
    for (const auto& input : two_input_tx->vin) {
        BOOST_CHECK(input.scriptSig.empty());
        combined_prevouts.push_back(transfer->vout[input.prevout.n]);
    }
    BOOST_REQUIRE(pq::DecodePayload(*two_input_tx, payload));
    BOOST_CHECK_EQUAL(payload.authorizations.size(), 2U);
    BOOST_CHECK(pq::VerifyInputs(*two_input_tx, combined_prevouts, Params(), reason));
    BOOST_CHECK(AmountFromValue(combined["fee"]) >= GetMinimumFee(GetSerializeSize(*two_input_tx, PROTOCOL_VERSION), nTxConfirmTarget, mempool));
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        for (const auto& input : two_input_tx->vin) pcoinsTip->SpendCoin(input.prevout);
        mempool.clear();
        BOOST_REQUIRE(m_wallet.AddToWalletIfInvolvingMe(two_input_tx, confirmed, true));
        m_wallet.mapWallet.at(two_input_tx->GetHash()).fInMempool = false;
        for (size_t i = 0; i < two_input_tx->vout.size(); ++i)
            pcoinsTip->AddCoin(COutPoint(two_input_tx->GetHash(), i), Coin(two_input_tx->vout[i], tip->nHeight, false, false), false);
    }
    BOOST_REQUIRE(m_wallet.Lock());
    BOOST_CHECK_EQUAL(CallRPC("listpqunspent").size(), 2U);
    const auto backup_path = GetDataDir() / "pq-payment-backup";
    fs::create_directories(backup_path);
    BOOST_REQUIRE(m_wallet.BackupWallet(backup_path.string()));
    m_wallet.GetDBHandle().Flush(true); // Close the original BDB handle before opening its same-file-id backup.
    DiskWallet restored("pq-payment-backup", WalletDatabase::Create(backup_path));
    bool first_run;
    BOOST_REQUIRE_EQUAL(restored.LoadWallet(first_run), DB_LOAD_OK);
    BOOST_CHECK(restored.GetPQAddresses() == m_wallet.GetPQAddresses());
    BOOST_CHECK_EQUAL(restored.mapWallet.count(source_ref->GetHash()), 1U);
    BOOST_CHECK_EQUAL(restored.mapWallet.count(transfer->GetHash()), 1U);
    BOOST_CHECK_EQUAL(restored.mapWallet.count(two_input_tx->GetHash()), 1U);
    BOOST_REQUIRE(restored.Unlock(PASSPHRASE));
    for (const auto& restored_address : restored.GetPQAddresses()) {
        pq::KeyID restored_id;
        mldsa44::Key key;
        BOOST_CHECK(pq::DecodeAddress(restored_address, Params().NetworkIDString(), restored_id));
        BOOST_CHECK(restored.GetPQKey(restored_address, key));
    }
}

BOOST_AUTO_TEST_CASE(pq_transfer_uses_finalized_payload_and_full_size_fees)
{
    CheckPaymentTransactions(m_wallet);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(pqwallet_testnet_tests, RegisteredTestnetWallet)

BOOST_AUTO_TEST_CASE(receiving_before_activation_preserves_security_and_network_gates)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
    BOOST_CHECK(!CWallet::PQPaymentsActive());
    BOOST_CHECK_EQUAL(CallRPC("listpqaddresses")["addresses"].size(), 0U);
    BOOST_CHECK_EXCEPTION(CallPQRPC("getnewpqaddress"), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("Encrypt") != std::string::npos; });
    EncryptTestWallet(m_wallet);
    BOOST_CHECK_THROW(CallPQRPC("getnewpqaddress"), std::runtime_error);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, true));
    BOOST_CHECK_THROW(CallPQRPC("getnewpqaddress"), std::runtime_error);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, false));
    const auto created = CallPQRPC("getnewpqaddress");
    const auto address = created["address"].get_str();
    BOOST_CHECK_EQUAL(address.size(), 69U);
    BOOST_CHECK_EQUAL(address.substr(0, 10), "olcpqtest1");
    BOOST_CHECK(!created["payable"].get_bool());
    BOOST_CHECK(!IsValidDestination(DecodeDestination(address)));
    BOOST_CHECK_THROW(CallRPC("sendtoaddress " + address + " 1"), std::runtime_error);
    mldsa44::Key key;
    BOOST_REQUIRE(m_wallet.GetPQKey(address, key));
    const auto public_key = key.GetPublicKey();
    {
        CWallet loaded("testnet-reloaded", WalletDatabase::CreateDummy());
        BOOST_REQUIRE_EQUAL(WalletBatch(m_wallet.GetDBHandle()).LoadWallet(&loaded), DB_LOAD_OK);
        BOOST_CHECK(loaded.IsLocked());
        BOOST_CHECK(loaded.GetPQAddresses() == m_wallet.GetPQAddresses());
        BOOST_REQUIRE(loaded.Unlock(PASSPHRASE));
        BOOST_REQUIRE(loaded.GetPQKey(address, key));
        BOOST_CHECK(key.GetPublicKey() == public_key);
    }
    BOOST_CHECK_THROW(CallRPC("fundpqaddress"), std::runtime_error);
    BOOST_CHECK_EXCEPTION(CallPQRPC("sendpqtoaddress " + address + " 1"), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("active") != std::string::npos; });
    BOOST_CHECK_EQUAL(CallRPC("listpqunspent").size(), 0U);
    CheckWrongNetworkLoad(m_wallet, CBaseChainParams::REGTEST);
    // Switching back restores the network defaults, not this test's delayed activation.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
    BOOST_REQUIRE(m_wallet.Lock());
    BOOST_CHECK_EQUAL(CallRPC("listpqaddresses")["addresses"][0].get_str(), address);
    BOOST_CHECK(!CallRPC("listpqaddresses")["payable"].get_bool());
    BOOST_CHECK(!m_wallet.GetPQKey(address, key));
    BOOST_CHECK(!key.IsValid());
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, true));
    BOOST_CHECK(!m_wallet.GetPQKey(address, key));
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, false));
    BOOST_REQUIRE(m_wallet.GetPQKey(address, key));
    BOOST_CHECK(key.GetPublicKey() == public_key);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 0);
    const auto call = "sendpqtoaddress " + address + " 1";
    BOOST_CHECK_EXCEPTION(CallPQRPC(call), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("Insufficient") != std::string::npos; });
    BOOST_REQUIRE(m_wallet.Lock());
    BOOST_CHECK_EXCEPTION(CallPQRPC(call), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("walletpassphrase") != std::string::npos; });
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, true));
    BOOST_CHECK_EXCEPTION(CallPQRPC(call), std::runtime_error,
        [](const std::runtime_error& e) { return std::string(e.what()).find("walletpassphrase") != std::string::npos; });
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE, false));
}

BOOST_AUTO_TEST_CASE(transfer_change_and_backup_use_testnet_keys)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 0);
    pqwallet_tests::CheckPaymentTransactions(m_wallet);
}

BOOST_AUTO_TEST_CASE(key_and_coin_listing_resource_samples)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 0);
    EncryptTestWallet(m_wallet);
    BOOST_REQUIRE(m_wallet.Unlock(PASSPHRASE));
    const auto* tip = WITH_LOCK(cs_main, return chainActive.Tip());
    WITH_LOCK(m_wallet.cs_wallet, m_wallet.SetLastBlockProcessed(tip));
    size_t created = 0;
    for (size_t target : {size_t{1}, size_t{100}, size_t{1000}}) {
        for (; created < target; ++created) {
            std::string address;
            BOOST_REQUIRE(m_wallet.GeneratePQAddress(address));
            pq::KeyID id;
            BOOST_REQUIRE(pq::DecodeAddress(address, "test", id));
            CMutableTransaction incoming;
            incoming.vin.emplace_back(uint256S("beef"), created);
            incoming.vout.emplace_back(COIN, pq::GetScript(id));
            const auto tx = MakeTransactionRef(incoming);
            LOCK2(cs_main, m_wallet.cs_wallet);
            BOOST_REQUIRE(m_wallet.AddToWalletIfInvolvingMe(tx,
                {CWalletTx::Status::CONFIRMED, tip->nHeight, tip->GetBlockHash(), 0}, true));
            for (uint32_t i = 0; i < tx->vout.size(); ++i)
                pcoinsTip->AddCoin(COutPoint(tx->GetHash(), i), Coin(tx->vout[i], tip->nHeight, false, false), false);
        }
        BOOST_REQUIRE_EQUAL(m_wallet.GetPQAddresses().size(), target);
        BOOST_REQUIRE_EQUAL(m_wallet.GetPQUnspent().size(), target);
        BOOST_CHECK_EQUAL(m_wallet.GetAvailableBalance(), 0);
        for (int sample = 0; sample < 3; ++sample) {
            BOOST_REQUIRE(m_wallet.Lock());
            const auto list_start = std::chrono::steady_clock::now();
            const auto addresses = CallRPC("listpqaddresses");
            const auto coins = CallRPC("listpqunspent");
            const auto list_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - list_start).count();
            BOOST_CHECK_EQUAL(addresses["addresses"].size(), target);
            BOOST_CHECK_EQUAL(coins.size(), target);
            BOOST_CHECK(m_wallet.IsLocked());
            const auto unlock_start = std::chrono::steady_clock::now();
            const bool unlocked = m_wallet.Unlock(PASSPHRASE);
            const auto unlock_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - unlock_start).count();
            BOOST_REQUIRE(unlocked);
            BOOST_TEST_MESSAGE("PQ wallet keys=" << target << " wallet_entries=" << m_wallet.mapWallet.size()
                << " trusted_utxos=" << target << " sample=" << sample
                << " locked_address_plus_coin_RPC_us=" << list_us << " passphrase_unlock_us=" << unlock_us
                << " tip_cache_bytes=" << WITH_LOCK(cs_main, return pcoinsTip->DynamicMemoryUsage()));
        }
    }
    // Real encrypted records and wallet/RPC scans, with synthetic confirmed
    // transactions and trusted UTXOs. Setup/persistence is outside these timings;
    // unlock includes the existing calibrated passphrase KDF. RSS is external.
}

BOOST_AUTO_TEST_SUITE_END()
