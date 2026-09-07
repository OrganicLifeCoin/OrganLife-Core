// Copyright (c) 2012-2014 The Bitcoin Core developers
// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/wallet_test_fixture.h"

#include "blockassembler.h"
#include "consensus/merkle.h"
#include "pqtransaction.h"
#include "rpc/server.h"
#include "util/system.h"
#include "txmempool.h"
#include "validation.h"
#include "wallet/db.h"
#include "wallet/rpcwallet.h"
#include "wallet/wallet.h"
#include "wallet/walletutil.h"

#include <fstream>
#include <set>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>
#include <univalue.h>

// how many times to run all the tests to have a chance to catch errors that only show up with particular random shuffles
#define RUN_TESTS 100

// some tests fail 1% of the time due to bad luck.
// we repeat those tests this many times and only complain if all iterations of the test fail
#define RANDOM_REPEATS 5

std::vector<std::unique_ptr<CWalletTx>> wtxn;

typedef std::set<std::pair<const CWalletTx*,unsigned int> > CoinSet;

BOOST_FIXTURE_TEST_SUITE(wallet_tests, WalletTestingSetup)

static const CWallet testWallet("dummy", WalletDatabase::CreateDummy());
static std::vector<COutput> vCoins;

static void add_coin(const CAmount& nValue, int nAge = 6*24, bool fIsFromMe = false, int nInput=0)
{
    static int nextLockTime = 0;
    CMutableTransaction tx;
    tx.nLockTime = nextLockTime++;        // so all transactions get different hashes
    tx.vout.resize(nInput+1);
    tx.vout[nInput].nValue = nValue;
    if (fIsFromMe) {
        // IsFromMe() returns (GetDebit() > 0), and GetDebit() is 0 if vin.empty(),
        // so stop vin being empty, and cache a non-zero Debit to fake out IsFromMe()
        tx.vin.resize(1);
    }
    std::unique_ptr<CWalletTx> wtx(new CWalletTx(&testWallet, MakeTransactionRef(std::move(tx))));
    if (fIsFromMe) {
        wtx->m_amounts[CWalletTx::DEBIT].Set(ISMINE_SPENDABLE, 1);
    }
    COutput output(wtx.get(), nInput, nAge, true /* spendable */, true /* solvable */, true /* safe */);
    vCoins.push_back(output);
    wtxn.emplace_back(std::move(wtx));
}

static void empty_wallet(void)
{
    vCoins.clear();
    wtxn.clear();
}

static bool equal_sets(CoinSet a, CoinSet b)
{
    std::pair<CoinSet::iterator, CoinSet::iterator> ret = mismatch(a.begin(), a.end(), b.begin());
    return ret.first == a.end() && ret.second == b.end();
}

static fs::path FindLegacyWalletBackupDir(const fs::path& dataDir)
{
    for (const fs::directory_entry& entry : fs::directory_iterator(dataDir)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.find(".legacy-wallet-layout-backup") == 0) {
            return entry.path();
        }
    }
    return fs::path();
}

static std::string ReadFileContents(const fs::path& path)
{
    std::ifstream stream(path.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

namespace {
// Exercise the real Berkeley write-error path without a production test hook.
decltype(DB::put) saved_wallet_put;
unsigned wallet_writes_before_failure;
struct FailWalletTxWrites {
    DB* db;
    explicit FailWalletTxWrites(CWallet& wallet, unsigned allow_writes = 0)
    {
        std::string file;
        auto* env = GetWalletEnv(wallet.GetDBHandle().GetPathToFile(), file);
        db = env->mapDb.at(file)->get_DB();
        saved_wallet_put = db->put;
        wallet_writes_before_failure = allow_writes;
        db->put = [](DB* handle, DB_TXN* txn, DBT* key, DBT* value, u_int32_t flags) {
            const auto* data = static_cast<const char*>(key->data);
            CDataStream stream(data, data + key->size, SER_DISK, CLIENT_VERSION);
            std::string type;
            stream >> type;
            if (type == "tx") {
                if (!wallet_writes_before_failure) return EIO;
                --wallet_writes_before_failure;
            }
            return saved_wallet_put(handle, txn, key, value, flags);
        };
    }
    ~FailWalletTxWrites() { db->put = saved_wallet_put; }
};

decltype(DB_ENV::txn_begin) saved_wallet_txn_begin;
bool fail_wallet_txn_begin;
unsigned failed_wallet_commits;
struct FailWalletTransaction {
    DB_ENV* env;
    explicit FailWalletTransaction(CWallet& wallet, bool fail_begin)
    {
        std::string file;
        env = GetWalletEnv(wallet.GetDBHandle().GetPathToFile(), file)->dbenv->get_DB_ENV();
        saved_wallet_txn_begin = env->txn_begin;
        fail_wallet_txn_begin = fail_begin;
        failed_wallet_commits = 0;
        env->txn_begin = [](DB_ENV* handle, DB_TXN* parent, DB_TXN** txn, u_int32_t flags) {
            if (fail_wallet_txn_begin) return EIO;
            const int result = saved_wallet_txn_begin(handle, parent, txn, flags);
            if (!result) {
                (*txn)->commit = [](DB_TXN* transaction, u_int32_t) {
                    // Berkeley commit failure consumes and aborts its handle.
                    ++failed_wallet_commits;
                    const int aborted = transaction->abort(transaction);
                    return aborted ? aborted : EIO;
                };
            }
            return result;
        };
    }
    ~FailWalletTransaction() { env->txn_begin = saved_wallet_txn_begin; }
};

void CheckMetadata(const CWalletTx& actual, const CWalletTx& expected)
{
    BOOST_CHECK(actual.mapValue == expected.mapValue);
    BOOST_CHECK(actual.vOrderForm == expected.vOrderForm);
    BOOST_CHECK_EQUAL(actual.nTimeSmart, expected.nTimeSmart);
    BOOST_CHECK_EQUAL(actual.fFromMe, expected.fFromMe);
}

void CheckConflictMetadataPersistence(CWallet& wallet, bool sapling)
{
    LOCK2(cs_main, wallet.cs_wallet);
    if (sapling) {
        wallet.SetMinVersion(FEATURE_SAPLING);
        wallet.SetupSPKM(false);
    }
    auto make_tx = [&](unsigned id, std::initializer_list<unsigned> inputs) {
        CMutableTransaction tx;
        tx.nLockTime = id;
        tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
        for (const auto input : inputs) {
            const auto hash = ArithToUint256(arith_uint256(input));
            if (sapling) {
                tx.nVersion = CTransaction::TxVersion::SAPLING;
                SpendDescription spend;
                spend.nullifier = hash;
                tx.sapData->vShieldedSpend.push_back(spend);
            } else {
                tx.vin.emplace_back(hash, 0);
            }
        }
        CWalletTx wtx(&wallet, MakeTransactionRef(tx));
        wtx.mapValue["comment"] = std::to_string(id);
        wtx.vOrderForm.emplace_back("purpose", std::to_string(id));
        wtx.fFromMe = id % 2;
        return wtx;
    };
    auto read_record = [&](const CWalletTx& wtx) {
        CWalletTx recorded(&wallet, nullptr);
        BerkeleyBatch batch(wallet.GetDBHandle(), "r");
        BOOST_REQUIRE(batch.Read(std::make_pair(std::string("tx"), wtx.GetHash()), recorded));
        return recorded;
    };

    // A simple conflicting pair must have identical live, stored and restored metadata.
    const auto first = make_tx(1, {10});
    const auto second = make_tx(2, {10});
    SetMockTime(1700000000);
    const bool first_added = wallet.AddToWallet(first);
    SetMockTime(0);
    BOOST_REQUIRE(first_added);
    BOOST_REQUIRE(wallet.AddToWallet(second));
    const auto expected = wallet.mapWallet.at(first.GetHash());
    CheckMetadata(wallet.mapWallet.at(second.GetHash()), expected);
    CheckMetadata(read_record(second), expected);
    {
        CWallet restored("conflict-reload", WalletDatabase::CreateDummy());
        LOCK(restored.cs_wallet);
        BOOST_REQUIRE_EQUAL(WalletBatch(wallet.GetDBHandle()).LoadWallet(&restored), DB_LOAD_OK);
        CheckMetadata(restored.mapWallet.at(second.GetHash()), expected);
    }

    // Input order matters: the first range changes the oldest member of the
    // second range. Staging only the new transaction would get this wrong.
    const auto a = make_tx(3, {20});
    const auto d = make_tx(4, {30});
    const auto b = make_tx(5, {20, 30, 40});
    const auto c = make_tx(6, {20, 40});
    BOOST_REQUIRE(wallet.AddToWallet(a));
    BOOST_REQUIRE(wallet.AddToWallet(d));
    BOOST_REQUIRE(wallet.AddToWallet(b));
    const auto before_b = wallet.mapWallet.at(b.GetHash());
    const auto disk_b = read_record(b);
    const auto order = wallet.nOrderPosNext;
    {
        FailWalletTxWrites fail(wallet);
        BOOST_CHECK(!wallet.AddToWallet(c));
        BOOST_CHECK(!wallet.mapWallet.count(c.GetHash()));
        CheckMetadata(wallet.mapWallet.at(b.GetHash()), before_b);
        CheckMetadata(read_record(b), disk_b);
    }
    {
        // The changed peer is written first, then the incoming record fails.
        FailWalletTxWrites fail(wallet, 1);
        BOOST_CHECK(!wallet.AddToWallet(c));
        BOOST_CHECK(!wallet.mapWallet.count(c.GetHash()));
        CheckMetadata(wallet.mapWallet.at(b.GetHash()), before_b);
        CheckMetadata(read_record(b), disk_b);
        BerkeleyBatch batch(wallet.GetDBHandle(), "r");
        int64_t stored_order = -1;
        BOOST_REQUIRE(batch.Read(std::string("orderposnext"), stored_order));
        BOOST_CHECK_EQUAL(stored_order, order);
        CWalletTx recorded(&wallet, nullptr);
        BOOST_CHECK(!batch.Read(std::make_pair(std::string("tx"), c.GetHash()), recorded));
    }
    BOOST_REQUIRE(wallet.AddToWallet(c));
    CheckMetadata(wallet.mapWallet.at(c.GetHash()), wallet.mapWallet.at(a.GetHash()));
    CheckMetadata(read_record(c), wallet.mapWallet.at(c.GetHash()));
    CheckMetadata(read_record(b), wallet.mapWallet.at(b.GetHash()));
    {
        CWallet restored("overlapping-conflict-reload", WalletDatabase::CreateDummy());
        LOCK(restored.cs_wallet);
        BOOST_REQUIRE_EQUAL(WalletBatch(wallet.GetDBHandle()).LoadWallet(&restored), DB_LOAD_OK);
        for (const auto* tx : {&a, &b, &c, &d})
            CheckMetadata(restored.mapWallet.at(tx->GetHash()), wallet.mapWallet.at(tx->GetHash()));
        BOOST_CHECK(restored.GetConflicts(b.GetHash()).count(c.GetHash()));
        const auto spent = ArithToUint256(arith_uint256(40));
        if (sapling)
            BOOST_CHECK(restored.GetSaplingScriptPubKeyMan()->IsSaplingSpent(spent));
        else
            BOOST_CHECK(restored.IsSpent(spent, 0));
    }
}
}

BOOST_AUTO_TEST_CASE(dummy_wallet_transactions_are_successful_noops)
{
    auto database = WalletDatabase::CreateDummy();
    WalletBatch batch(*database);
    BOOST_CHECK(batch.TxnBegin());
    BOOST_CHECK(batch.WriteOrderPosNext(1));
    BOOST_CHECK(batch.TxnCommit());
    BOOST_CHECK(batch.TxnBegin());
    BOOST_CHECK(batch.TxnAbort());
}

BOOST_AUTO_TEST_CASE(wallet_transaction_begin_and_commit_failure_are_atomic)
{
    LOCK2(cs_main, m_wallet.cs_wallet);
    CMutableTransaction funding;
    funding.vout.emplace_back(2 * COIN, CScript() << OP_TRUE);
    const auto funding_ref = MakeTransactionRef(funding);
    BOOST_REQUIRE(m_wallet.AddToWallet(CWalletTx(&m_wallet, funding_ref)));
    CMutableTransaction payment;
    payment.vin.emplace_back(funding_ref->GetHash(), 0);
    payment.vout.emplace_back(COIN, CScript() << OP_TRUE);
    const auto payment_ref = MakeTransactionRef(payment);
    const auto order = m_wallet.nOrderPosNext;
    for (const bool fail_begin : {true, false}) {
        FailWalletTransaction fail(m_wallet, fail_begin);
        const auto result = m_wallet.CommitTransaction(payment_ref, nullptr, nullptr);
        BOOST_CHECK_EQUAL(result.status, CWallet::CommitStatus::NotRecorded);
        BOOST_CHECK(!m_wallet.mapWallet.count(payment_ref->GetHash()));
        BOOST_CHECK_EQUAL(m_wallet.nOrderPosNext, order);
        BOOST_CHECK(!m_wallet.IsSpent(funding_ref->GetHash(), 0));
        BOOST_CHECK_EQUAL(failed_wallet_commits, fail_begin ? 0U : 1U);
        BerkeleyBatch batch(m_wallet.GetDBHandle(), "r");
        CWalletTx recorded(&m_wallet, nullptr);
        BOOST_CHECK(!batch.Read(std::make_pair(std::string("tx"), payment_ref->GetHash()), recorded));
        int64_t stored_order = -1;
        BOOST_REQUIRE(batch.Read(std::string("orderposnext"), stored_order));
        BOOST_CHECK_EQUAL(stored_order, order);
    }
    BOOST_REQUIRE(m_wallet.AddToWallet(CWalletTx(&m_wallet, payment_ref)));
}

BOOST_AUTO_TEST_CASE(ordinary_conflict_metadata_is_persisted_before_publication)
{
    CheckConflictMetadataPersistence(m_wallet, false);
}

BOOST_AUTO_TEST_CASE(sapling_conflict_metadata_is_persisted_before_publication)
{
    CheckConflictMetadataPersistence(m_wallet, true);
}

BOOST_AUTO_TEST_CASE(transaction_record_failure_does_not_publish_or_commit)
{
    LOCK2(cs_main, m_wallet.cs_wallet);
    CMutableTransaction funding;
    funding.vout.emplace_back(2 * COIN, CScript() << OP_TRUE);
    const auto funding_ref = MakeTransactionRef(funding);
    BOOST_REQUIRE(m_wallet.AddToWallet(CWalletTx(&m_wallet, funding_ref)));

    CMutableTransaction payment;
    payment.vin.emplace_back(funding_ref->GetHash(), 0);
    payment.vout.emplace_back(COIN, CScript() << OP_TRUE);
    const auto payment_ref = MakeTransactionRef(payment);
    const COutPoint prevout(funding_ref->GetHash(), 0);
    const auto order = m_wallet.nOrderPosNext;
    const auto ordered_size = m_wallet.wtxOrdered.size();
    m_wallet.LockCoin(prevout);
    unsigned notifications = 0;
    boost::signals2::scoped_connection connection = m_wallet.NotifyTransactionChanged.connect(
        [&](CWallet*, const uint256&, ChangeType) { ++notifications; });

    {
        FailWalletTxWrites fail(m_wallet);
        for (int retry = 0; retry != 2; ++retry) {
            BOOST_CHECK(!m_wallet.AddToWallet(CWalletTx(&m_wallet, payment_ref)));
            BOOST_CHECK(!m_wallet.mapWallet.count(payment_ref->GetHash()));
            BOOST_CHECK_EQUAL(m_wallet.wtxOrdered.size(), ordered_size);
            BOOST_CHECK_EQUAL(m_wallet.nOrderPosNext, order);
            BOOST_CHECK(!m_wallet.IsSpent(prevout));
            BOOST_CHECK(m_wallet.IsLockedCoin(prevout.hash, prevout.n));
        }
        const auto result = m_wallet.CommitTransaction(payment_ref, nullptr, nullptr);
        BOOST_CHECK_EQUAL(result.status, CWallet::CommitStatus::NotRecorded);
        BOOST_CHECK(result.hashTx.IsNull());
        BOOST_CHECK(!m_wallet.mapWallet.count(payment_ref->GetHash()));
        BOOST_CHECK(!mempool.exists(payment_ref->GetHash()));
        BOOST_CHECK(!m_wallet.IsSpent(prevout));
        BOOST_CHECK(m_wallet.IsLockedCoin(prevout.hash, prevout.n));

        auto update = m_wallet.mapWallet.at(funding_ref->GetHash());
        update.fFromMe = true;
        BOOST_CHECK(!m_wallet.AddToWallet(update));
        BOOST_CHECK(!m_wallet.mapWallet.at(funding_ref->GetHash()).fFromMe);
        BOOST_CHECK_EQUAL(notifications, 0U);
        CWalletTx recorded(&m_wallet, nullptr);
        BerkeleyBatch batch(m_wallet.GetDBHandle(), "r");
        BOOST_CHECK(!batch.Read(std::make_pair(std::string("tx"), payment_ref->GetHash()), recorded));
        BOOST_REQUIRE(batch.Read(std::make_pair(std::string("tx"), funding_ref->GetHash()), recorded));
        BOOST_CHECK(!recorded.fFromMe);
    }

    BOOST_REQUIRE(m_wallet.AddToWallet(CWalletTx(&m_wallet, payment_ref)));
    BOOST_CHECK(m_wallet.IsSpent(prevout));
    BOOST_CHECK(!m_wallet.IsLockedCoin(prevout.hash, prevout.n));
    BOOST_CHECK_EQUAL(m_wallet.nOrderPosNext, order + 1);
    BOOST_CHECK_EQUAL(m_wallet.wtxOrdered.size(), ordered_size + 1);
    BOOST_CHECK_EQUAL(notifications, 1U);
    connection.disconnect();
    CWallet restored("persistence-reload", WalletDatabase::CreateDummy());
    LOCK(restored.cs_wallet);
    BOOST_REQUIRE_EQUAL(WalletBatch(m_wallet.GetDBHandle()).LoadWallet(&restored), DB_LOAD_OK);
    BOOST_CHECK(restored.mapWallet.count(payment_ref->GetHash()));
    BOOST_CHECK(restored.IsSpent(prevout));
}

BOOST_AUTO_TEST_CASE(coin_selection_tests)
{
    CoinSet setCoinsRet, setCoinsRet2;
    CAmount nValueRet;

    LOCK(testWallet.cs_wallet);

    // test multiple times to allow for differences in the shuffle order
    for (int i = 0; i < RUN_TESTS; i++)
    {
        empty_wallet();

        // with an empty wallet we can't even pay one cent
        BOOST_CHECK(!testWallet.SelectCoinsMinConf( 1 * CENT, 1, 6, 0, vCoins, setCoinsRet, nValueRet));

        add_coin(1*CENT, 4);        // add a new 1 cent coin

        // with a new 1 cent coin, we still can't find a mature 1 cent
        BOOST_CHECK(!testWallet.SelectCoinsMinConf( 1 * CENT, 1, 6, 0, vCoins, setCoinsRet, nValueRet));

        // but we can find a new 1 cent
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 1 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * CENT);

        add_coin(2*CENT);           // add a mature 2 cent coin

        // we can't make 3 cents of mature coins
        BOOST_CHECK(!testWallet.SelectCoinsMinConf( 3 * CENT, 1, 6, 0, vCoins, setCoinsRet, nValueRet));

        // we can make 3 cents of new  coins
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 3 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 3 * CENT);

        add_coin(5*CENT);           // add a mature 5 cent coin,
        add_coin(10*CENT, 3, true); // a new 10 cent coin sent from one of our own addresses
        add_coin(20*CENT);          // and a mature 20 cent coin

        // now we have new: 1+10=11 (of which 10 was self-sent), and mature: 2+5+20=27.  total = 38

        // we can't make 38 cents only if we disallow new coins:
        BOOST_CHECK(!testWallet.SelectCoinsMinConf(38 * CENT, 1, 6, 0, vCoins, setCoinsRet, nValueRet));
        // we can't even make 37 cents if we don't allow new coins even if they're from us
        BOOST_CHECK(!testWallet.SelectCoinsMinConf(38 * CENT, 6, 6, 0, vCoins, setCoinsRet, nValueRet));
        // but we can make 37 cents if we accept new coins from ourself
        BOOST_CHECK(testWallet.SelectCoinsMinConf(37 * CENT, 1, 6, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 37 * CENT);
        // and we can make 38 cents if we accept all new coins
        BOOST_CHECK(testWallet.SelectCoinsMinConf(38 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 38 * CENT);

        // try making 34 cents from 1,2,5,10,20 - we can't do it exactly
        BOOST_CHECK(testWallet.SelectCoinsMinConf(34 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 35 * CENT);       // but 35 cents is closest
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);     // the best should be 20+10+5.  it's incredibly unlikely the 1 or 2 got included (but possible)

        // when we try making 7 cents, the smaller coins (1,2,5) are enough.  We should see just 2+5
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 7 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 7 * CENT);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

        // when we try making 8 cents, the smaller coins (1,2,5) are exactly enough.
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 8 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK(nValueRet == 8 * CENT);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);

        // when we try making 9 cents, no subset of smaller coins is enough, and we get the next bigger coin (10)
        BOOST_CHECK(testWallet.SelectCoinsMinConf( 9 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 10 * CENT);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        // now clear out the wallet and start again to test choosing between subsets of smaller coins and the next biggest coin
        empty_wallet();

        add_coin(6*CENT);
        add_coin(7*CENT);
        add_coin(8*CENT);
        add_coin(20*CENT);
        add_coin(30*CENT); // now we have 6+7+8+20+30 = 71 cents total

        // check that we have 71 and not 72
        BOOST_CHECK(testWallet.SelectCoinsMinConf(71 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK(!testWallet.SelectCoinsMinConf(72 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));

        // now try making 16 cents.  the best smaller coins can do is 6+7+8 = 21; not as good at the next biggest coin, 20
        BOOST_CHECK(testWallet.SelectCoinsMinConf(16 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 20 * CENT); // we should get 20 in one coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        add_coin(5*CENT); // now we have 5+6+7+8+20+30 = 75 cents total

        // now if we try making 16 cents again, the smaller coins can make 5+6+7 = 18 cents, better than the next biggest coin, 20
        BOOST_CHECK(testWallet.SelectCoinsMinConf(16 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 18 * CENT); // we should get 18 in 3 coins
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);

        add_coin(18*CENT); // now we have 5+6+7+8+18+20+30

        // and now if we try making 16 cents again, the smaller coins can make 5+6+7 = 18 cents, the same as the next biggest coin, 18
        BOOST_CHECK(testWallet.SelectCoinsMinConf(16 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 18 * CENT);  // we should get 18 in 1 coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U); // because in the event of a tie, the biggest coin wins

        // now try making 11 cents.  we should get 5+6
        BOOST_CHECK(testWallet.SelectCoinsMinConf(11 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 11 * CENT);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

        // check that the smallest bigger coin is used
        add_coin(1*COIN);
        add_coin(2*COIN);
        add_coin(3*COIN);
        add_coin(4*COIN); // now we have 5+6+7+8+18+20+30+100+200+300+400 = 1094 cents
        BOOST_CHECK(testWallet.SelectCoinsMinConf(95 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * COIN);  // we should get 1 BTC in 1 coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        BOOST_CHECK(testWallet.SelectCoinsMinConf(195 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 2 * COIN);  // we should get 2 BTC in 1 coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        // empty the wallet and start again, now with fractions of a cent, to test small change avoidance

        empty_wallet();
        add_coin(0.1*MIN_CHANGE);
        add_coin(0.2*MIN_CHANGE);
        add_coin(0.3*MIN_CHANGE);
        add_coin(0.4*MIN_CHANGE);
        add_coin(0.5*MIN_CHANGE);

        // try making 1 * MIN_CHANGE from the 1.5 * MIN_CHANGE
        // we'll get change smaller than MIN_CHANGE whatever happens, so can expect MIN_CHANGE exactly
        BOOST_CHECK(testWallet.SelectCoinsMinConf(MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, MIN_CHANGE);

        // but if we add a bigger coin, small change is avoided
        add_coin(1111*MIN_CHANGE);

        // try making 1 from 0.1 + 0.2 + 0.3 + 0.4 + 0.5 + 1111 = 1112.5
        BOOST_CHECK(testWallet.SelectCoinsMinConf(1 * MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * MIN_CHANGE); // we should get the exact amount

        // if we add more small coins:
        add_coin(0.6*MIN_CHANGE);
        add_coin(0.7*MIN_CHANGE);

        // and try again to make 1.0 * MIN_CHANGE
        BOOST_CHECK(testWallet.SelectCoinsMinConf(1 * MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * MIN_CHANGE); // we should get the exact amount

        // run the 'mtgox' test (see http://blockexplorer.com/tx/29a3efd3ef04f9153d47a990bd7b048a4b2d213daaa5fb8ed670fb85f13bdbcf)
        // they tried to consolidate 10 50k coins into one 500k coin, and ended up with 50k in change
        empty_wallet();
        for (int j = 0; j < 20; j++)
            add_coin(50000 * COIN);

        BOOST_CHECK(testWallet.SelectCoinsMinConf(500000 * COIN, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 500000 * COIN); // we should get the exact amount
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 10U); // in ten coins

        // if there's not enough in the smaller coins to make at least 1 * MIN_CHANGE change (0.5+0.6+0.7 < 1.0+1.0),
        // we need to try finding an exact subset anyway

        // sometimes it will fail, and so we use the next biggest coin:
        empty_wallet();
        add_coin(0.5 * CENT);
        add_coin(0.6 * CENT);
        add_coin(0.7 * CENT);
        add_coin(1111 * CENT);
        BOOST_CHECK(testWallet.SelectCoinsMinConf(1 * CENT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1111 * CENT); // we get the bigger coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        // but sometimes it's possible, and we use an exact subset (0.4 + 0.6 = 1.0)
        empty_wallet();
        add_coin(0.4 * MIN_CHANGE);
        add_coin(0.6 * MIN_CHANGE);
        add_coin(0.8 * MIN_CHANGE);
        add_coin(1111 * MIN_CHANGE);
        BOOST_CHECK(testWallet.SelectCoinsMinConf(MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, MIN_CHANGE);   // we should get the exact amount
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U); // in two coins 0.4+0.6

        // test avoiding small change
        empty_wallet();
        add_coin(0.05 * MIN_CHANGE);
        add_coin(1    * MIN_CHANGE);
        add_coin(100  * MIN_CHANGE);

        // trying to make 100.01 from these three coins
        BOOST_CHECK(testWallet.SelectCoinsMinConf(100.01 * MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 101.05 * MIN_CHANGE); // we should get all coins
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);

        // but if we try to make 99.9, we should take the bigger of the two small coins to avoid small change
        BOOST_CHECK(testWallet.SelectCoinsMinConf(99.9 * MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 101 * MIN_CHANGE);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

        // test with many inputs
        for (CAmount amt=1500; amt < COIN; amt*=10) {
            empty_wallet();
            // Create 676 inputs (= MAX_STANDARD_TX_SIZE / 148 bytes per input)
            for (uint16_t j = 0; j < 676; j++)
                add_coin(amt);
            BOOST_CHECK(testWallet.SelectCoinsMinConf(2000, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
            if (amt - 2000 < MIN_CHANGE) {
                // needs more than one input:
                uint16_t returnSize = std::ceil((2000.0 + MIN_CHANGE)/amt);
                CAmount returnValue = amt * returnSize;
                BOOST_CHECK_EQUAL(nValueRet, returnValue);
                BOOST_CHECK_EQUAL(setCoinsRet.size(), returnSize);
            } else {
                // one input is sufficient:
                BOOST_CHECK_EQUAL(nValueRet, amt);
                BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);
            }
        }

        // test randomness
        {
            empty_wallet();
            for (int i2 = 0; i2 < 100; i2++)
                add_coin(COIN);

            // picking 50 from 100 coins doesn't depend on the shuffle,
            // but does depend on randomness in the stochastic approximation code
            BOOST_CHECK(testWallet.SelectCoinsMinConf(50 * COIN, 1, 6, 0, vCoins, setCoinsRet , nValueRet));
            BOOST_CHECK(testWallet.SelectCoinsMinConf(50 * COIN, 1, 6, 0, vCoins, setCoinsRet2, nValueRet));
            BOOST_CHECK(!equal_sets(setCoinsRet, setCoinsRet2));

            int fails = 0;
            for (int j = 0; j < RANDOM_REPEATS; j++)
            {
                // selecting 1 from 100 identical coins depends on the shuffle; this test will fail 1% of the time
                // run the test RANDOM_REPEATS times and only complain if all of them fail
                BOOST_CHECK(testWallet.SelectCoinsMinConf(COIN, 1, 6, 0, vCoins, setCoinsRet , nValueRet));
                BOOST_CHECK(testWallet.SelectCoinsMinConf(COIN, 1, 6, 0, vCoins, setCoinsRet2, nValueRet));
                if (equal_sets(setCoinsRet, setCoinsRet2))
                    fails++;
            }
            BOOST_CHECK_NE(fails, RANDOM_REPEATS);

            // add 75 cents in small change.  not enough to make 90 cents,
            // then try making 90 cents.  there are multiple competing "smallest bigger" coins,
            // one of which should be picked at random
            add_coin(5 * CENT);
            add_coin(10 * CENT);
            add_coin(15 * CENT);
            add_coin(20 * CENT);
            add_coin(25 * CENT);

            fails = 0;
            for (int j = 0; j < RANDOM_REPEATS; j++)
            {
                // selecting 1 from 100 identical coins depends on the shuffle; this test will fail 1% of the time
                // run the test RANDOM_REPEATS times and only complain if all of them fail
                BOOST_CHECK(testWallet.SelectCoinsMinConf(90*CENT, 1, 6, 0, vCoins, setCoinsRet , nValueRet));
                BOOST_CHECK(testWallet.SelectCoinsMinConf(90*CENT, 1, 6, 0, vCoins, setCoinsRet2, nValueRet));
                if (equal_sets(setCoinsRet, setCoinsRet2))
                    fails++;
            }
            BOOST_CHECK_NE(fails, RANDOM_REPEATS);
        }
    }
    empty_wallet();
}

BOOST_AUTO_TEST_CASE(managed_wallet_dir_defaults_to_datadir_wallets_subdirectory)
{
    const fs::path dataDir = SetDataDir("managed_wallet_dir_defaults");
    ClearDatadirCache();

    BOOST_CHECK_EQUAL(GetWalletDir(), dataDir / "wallets");
}

BOOST_AUTO_TEST_CASE(legacy_migration_allows_preexisting_wallets_directory)
{
    const fs::path dataDir = SetDataDir("legacy_migration_preexisting_wallets_dir");
    ClearDatadirCache();

    fs::create_directories(dataDir / "wallets");

    std::vector<std::string> warnings;
    const OperationResult result = MigrateLegacyManagedWalletLayout(warnings);

    BOOST_REQUIRE(result);
    BOOST_CHECK(warnings.empty());
    BOOST_CHECK(fs::is_directory(dataDir / "wallets"));
}

BOOST_AUTO_TEST_CASE(managed_wallet_listing_uses_named_dat_files_inside_wallet_dir)
{
    const fs::path dataDir = SetDataDir("managed_wallet_listing");
    ClearDatadirCache();

    const fs::path walletDir = dataDir / "wallets";
    fs::create_directories(walletDir);
    std::ofstream((walletDir / "wallet.dat").string()).put('\n');
    std::ofstream((walletDir / "DeveloperWallet.dat").string()).put('\n');
    std::ofstream((walletDir / "Trading.dat").string()).put('\n');
    fs::create_directories(walletDir / "LegacyDirectoryWallet");

    const std::vector<std::string> wallets = ListWalletDir();
    const std::vector<std::string> expectedWallets{"DeveloperWallet", "Trading"};
    BOOST_CHECK(wallets == expectedWallets);
}

BOOST_AUTO_TEST_CASE(managed_wallet_dat_paths_use_shared_wallet_directory_environment)
{
    const fs::path dataDir = SetDataDir("managed_wallet_env_mapping");
    ClearDatadirCache();

    const fs::path walletDir = dataDir / "wallets";
    fs::create_directories(walletDir);

    std::string databaseFilename;
    BerkeleyEnvironment* env = GetWalletEnv(walletDir / "DeveloperWallet.dat", databaseFilename);

    BOOST_REQUIRE(env != nullptr);
    BOOST_CHECK_EQUAL(env->Directory(), walletDir);
    BOOST_CHECK_EQUAL(databaseFilename, "DeveloperWallet.dat");
}

BOOST_AUTO_TEST_CASE(legacy_primary_wallet_migration_moves_root_wallet_file_into_wallets_directory)
{
    const fs::path dataDir = SetDataDir("legacy_primary_wallet_migration");
    ClearDatadirCache();

    const fs::path legacyPrimaryWallet = dataDir / "wallet.dat";
    std::ofstream(legacyPrimaryWallet.string()) << "primary";
    fs::create_directories(dataDir / "database");

    std::vector<std::string> warnings;
    const OperationResult result = MigrateLegacyManagedWalletLayout(warnings);

    BOOST_REQUIRE(result);
    BOOST_CHECK(warnings.empty());
    BOOST_CHECK(!fs::exists(legacyPrimaryWallet));
    BOOST_CHECK(fs::is_regular_file(dataDir / "wallets" / "wallet.dat"));
    const fs::path backupDir = FindLegacyWalletBackupDir(dataDir);
    BOOST_REQUIRE(fs::is_directory(backupDir));
    BOOST_CHECK(fs::is_regular_file(backupDir / "wallet.dat"));
    BOOST_CHECK(fs::is_directory(backupDir / "database"));
    BOOST_CHECK_EQUAL(ReadFileContents(dataDir / "wallets" / "wallet.dat"), "primary");
    BOOST_CHECK_EQUAL(ReadFileContents(backupDir / "wallet.dat"), "primary");
}

BOOST_AUTO_TEST_CASE(legacy_named_wallet_migration_moves_directory_wallet_into_named_dat_file)
{
    const fs::path dataDir = SetDataDir("legacy_named_wallet_migration");
    ClearDatadirCache();

    const fs::path legacyWalletDir = dataDir / "DeveloperWallet";
    fs::create_directories(legacyWalletDir);
    std::ofstream((legacyWalletDir / "wallet.dat").string()) << "developer";
    fs::create_directories(legacyWalletDir / "database");

    std::vector<std::string> warnings;
    const OperationResult result = MigrateLegacyManagedWalletLayout(warnings);

    BOOST_REQUIRE(result);
    BOOST_CHECK(warnings.empty());
    BOOST_CHECK(fs::is_regular_file(dataDir / "wallets" / "DeveloperWallet.dat"));
    BOOST_CHECK(!fs::exists(legacyWalletDir));
    const fs::path backupDir = FindLegacyWalletBackupDir(dataDir);
    BOOST_REQUIRE(fs::is_directory(backupDir));
    BOOST_CHECK(fs::is_regular_file(backupDir / "DeveloperWallet" / "wallet.dat"));
    BOOST_CHECK(fs::is_directory(backupDir / "DeveloperWallet" / "database"));
    BOOST_CHECK_EQUAL(ReadFileContents(dataDir / "wallets" / "DeveloperWallet.dat"), "developer");
    BOOST_CHECK_EQUAL(ReadFileContents(backupDir / "DeveloperWallet" / "wallet.dat"), "developer");
}

struct PQRescanSetup : TestnetSetup {
    struct RescanWallet : CWallet {
        using CWallet::CWallet;
        ~RescanWallet() { GetDBHandle().Flush(true); }
    } wallet{"rescan", WalletDatabase::Create(GetDataDir() / "rescan-wallet")};
    CScript scriptPubKey;
    PQRescanSetup()
    {
        bool firstRun;
        BOOST_REQUIRE_EQUAL(wallet.LoadWallet(firstRun), DB_LOAD_OK);
        const SecureString passphrase = "public-rescan-test-passphrase";
        BOOST_REQUIRE(wallet.EncryptWallet(passphrase));
        BOOST_REQUIRE(wallet.Unlock(passphrase));
        std::string address;
        BOOST_REQUIRE(wallet.GeneratePQAddress(address));
        pq::KeyID id;
        BOOST_REQUIRE(pq::DecodeAddress(address, Params().NetworkIDString(), id));
        BOOST_REQUIRE(wallet.Lock());
        scriptPubKey = pq::GetScript(id);
    }
    ~PQRescanSetup() { SetMockTime(0); }

    std::shared_ptr<CBlock> minePQBlock() {
        std::unique_ptr<CBlockTemplate> blockTemplate = BlockAssembler(
                Params(), false).CreateNewBlock(scriptPubKey);
        BOOST_REQUIRE(blockTemplate);
        std::shared_ptr<CBlock> block = std::make_shared<CBlock>(blockTemplate->block);
        const int height = WITH_LOCK(cs_main, return chainActive.Height() + 1);
        BOOST_REQUIRE(SolveBlock(block, height));
        BOOST_REQUIRE(ProcessNewBlock(block, nullptr));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), block->GetHash());
        return block;
    }
};

BOOST_FIXTURE_TEST_CASE(rescan, PQRescanSetup)
{
    const auto firstBlock = minePQBlock();
    // Cap last block file size, and mine new block in a new block file.
    CBlockIndex* oldTip = chainActive.Tip();
    GetBlockFileInfo(oldTip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE;
    const auto secondBlock = minePQBlock();
    CBlockIndex* newTip = chainActive.Tip();
    BOOST_REQUIRE(oldTip->GetBlockPos().nFile != newTip->GetBlockPos().nFile);
    WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(newTip));
    BOOST_REQUIRE(wallet.mapWallet.empty());
    // Verify ScanForWalletTransactions picks up transactions in both the old
    // and new block files.
    WalletRescanReserver reserver(&wallet);
    BOOST_REQUIRE(reserver.reserve());
    BOOST_CHECK_EQUAL(nullptr, wallet.ScanForWalletTransactions(oldTip, nullptr, reserver));
    BOOST_REQUIRE_EQUAL(wallet.mapWallet.size(), 2U);
    for (const auto& block : {firstBlock, secondBlock}) {
        const auto* recovered = wallet.GetWalletTx(block->vtx[0]->GetHash());
        BOOST_REQUIRE(recovered);
        BOOST_CHECK(wallet.IsPQMine(recovered->tx->vout[0]));
        BOOST_CHECK_EQUAL(recovered->tx->vout[0].nValue, block->vtx[0]->GetValueOut());
    }
    BOOST_CHECK_EQUAL(wallet.GetPQBalance().m_mine_trusted, firstBlock->vtx[0]->GetValueOut());
    BOOST_CHECK_EQUAL(wallet.GetPQBalance().m_mine_immature, secondBlock->vtx[0]->GetValueOut());
    BOOST_CHECK(wallet.IsLocked());
}

BOOST_FIXTURE_TEST_CASE(rescan_from_time_boundary, PQRescanSetup)
{
    // PQ keys have no legacy birthday metadata; exercise the same timestamp
    // boundary on a wallet that has not received any block notifications.
    const int64_t start = Params().GenesisBlock().GetBlockTime() + 10000;
    SetMockTime(start);
    const auto before = minePQBlock();
    SetMockTime(start + 15);
    const auto boundary = minePQBlock();
    const int64_t keyTime = boundary->GetBlockTime() + TIMESTAMP_WINDOW;
    SetMockTime(keyTime);
    const auto after = minePQBlock();
    WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(chainActive.Tip()));
    BOOST_REQUIRE(wallet.mapWallet.empty());
    WalletRescanReserver reserver(&wallet);
    BOOST_REQUIRE(reserver.reserve());
    BOOST_CHECK_EQUAL(wallet.RescanFromTime(keyTime, reserver, true), keyTime);
    BOOST_CHECK_EQUAL(wallet.mapWallet.size(), 2U);
    BOOST_CHECK(!wallet.GetWalletTx(before->vtx[0]->GetHash()));
    BOOST_CHECK(wallet.GetWalletTx(boundary->vtx[0]->GetHash()));
    BOOST_CHECK(wallet.GetWalletTx(after->vtx[0]->GetHash()));
    BOOST_CHECK(wallet.IsLocked());
}

BOOST_AUTO_TEST_CASE(legacy_wallet_text_import_export_is_not_exposed)
{
    CRPCTable commands;
    RegisterWalletRPCCommands(commands);
    for (const auto* method : {"dumpwallet", "importwallet", "importmulti"}) {
        BOOST_CHECK(commands[method] == nullptr);
        JSONRPCRequest request;
        request.strMethod = method;
        request.params = UniValue(UniValue::VARR);
        BOOST_CHECK_EXCEPTION(commands.execute(request), UniValue,
            [](const UniValue& error) { return error["code"].get_int() == -32601; });
    }
}

void removeTxFromMempool(CWalletTx& wtx)
{
    LOCK(mempool.cs);
    if (mempool.exists(wtx.GetHash())) {
        auto it = mempool.mapTx.find(wtx.GetHash());
        if (it != mempool.mapTx.end()) {
            mempool.mapTx.erase(it);
        }
    }
}

/**
 * Mimic block creation.
 */
CBlockIndex* SimpleFakeMine(CWalletTx& wtx, CWallet &wallet, CBlockIndex* pprev = nullptr)
{
    CBlock block;
    block.vtx.emplace_back(wtx.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    if (pprev) block.hashPrevBlock = pprev->GetBlockHash();
    CBlockIndex* fakeIndex = new CBlockIndex(block);
    fakeIndex->pprev = pprev;
    mapBlockIndex.emplace(block.GetHash(), fakeIndex);
    fakeIndex->phashBlock = &mapBlockIndex.find(block.GetHash())->first;
    chainActive.SetTip(fakeIndex);
    BOOST_CHECK(chainActive.Contains(fakeIndex));
    WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(fakeIndex));
    wtx.m_confirm = CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, fakeIndex->nHeight, fakeIndex->GetBlockHash(), 0);
    removeTxFromMempool(wtx);
    wtx.fInMempool = false;
    return fakeIndex;
}

void fakeMempoolInsertion(const CTransactionRef& wtxCredit)
{
    CTxMemPoolEntry entry(wtxCredit, 0, 0, 0, false, 0);
    LOCK(mempool.cs);
    mempool.mapTx.insert(entry);
}

CWalletTx& BuildAndLoadTxToWallet(const std::vector<CTxIn>& vin,
                                  const std::vector<CTxOut>& vout,
                                  CWallet& wallet)
{
    CMutableTransaction mTx;
    mTx.vin = vin;
    mTx.vout = vout;
    CTransaction tx(mTx);
    CWalletTx wtx(&wallet, MakeTransactionRef(tx));
    wallet.LoadToWallet(wtx);
    return wallet.mapWallet.at(tx.GetHash());
}

CWalletTx& ReceiveBalanceWith(const std::vector<CTxOut>& vout,
                          CWallet& wallet)
{
    std::vector<CTxIn> vin;
    vin.emplace_back(CTxIn(COutPoint(uint256(), 999)));
    return BuildAndLoadTxToWallet(vin, vout, wallet);
}

class PubKeyCountingWallet : public CWallet
{
public:
    using CWallet::CWallet;

    bool GetPubKey(const CKeyID& address, CPubKey& pubkey) const override
    {
        ++getPubKeyCalls;
        return CWallet::GetPubKey(address, pubkey);
    }

    mutable int getPubKeyCalls{0};
};

BOOST_AUTO_TEST_CASE(directly_spendable_output_skips_solvability_check)
{
    PubKeyCountingWallet wallet("availability", WalletDatabase::CreateMock());
    bool firstRun;
    BOOST_REQUIRE_EQUAL(wallet.LoadWallet(firstRun), DB_LOAD_OK);
    wallet.SetMinVersion(FEATURE_PRE_SPLIT_KEYPOOL);
    BOOST_REQUIRE(wallet.SetupSPKM(false));
    WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(chainActive.Tip()));

    auto address = wallet.getNewAddress("availability");
    BOOST_REQUIRE(address);
    CWalletTx& wtx = ReceiveBalanceWith(
            {CTxOut(COIN, GetScriptForDestination(*address.getObjResult()))},
            wallet);
    fakeMempoolInsertion(wtx.tx);
    wtx.fInMempool = true;

    wallet.getPubKeyCalls = 0;
    CWallet::AvailableCoinsFilter filter;
    filter.fOnlySafe = false;
    filter.fOnlySpendable = true;
    std::vector<COutput> coins;
    BOOST_REQUIRE(wallet.AvailableCoins(&coins, nullptr, filter));
    BOOST_REQUIRE_EQUAL(coins.size(), 1U);
    BOOST_CHECK(coins[0].fSpendable);
    BOOST_CHECK(coins[0].fSolvable);
    BOOST_CHECK_EQUAL(wallet.getPubKeyCalls, 0);

    removeTxFromMempool(wtx);
}

void CheckBalances(const CWalletTx& tx,
                   const CAmount& nCreditAll,
                   const CAmount& nCreditSpendable,
                   const CAmount& nAvailableCredit,
                   const CAmount& nDebitAll,
                   const CAmount& nDebitSpendable)
{
    BOOST_CHECK_EQUAL(tx.GetCredit(ISMINE_ALL), nCreditAll);
    BOOST_CHECK_EQUAL(tx.GetCredit(ISMINE_SPENDABLE), nCreditSpendable);
    BOOST_CHECK(tx.IsAmountCached(CWalletTx::CREDIT, ISMINE_SPENDABLE));
    BOOST_CHECK_EQUAL(tx.GetAvailableCredit(), nAvailableCredit);
    BOOST_CHECK(tx.IsAmountCached(CWalletTx::AVAILABLE_CREDIT, ISMINE_SPENDABLE));
    BOOST_CHECK_EQUAL(tx.GetDebit(ISMINE_ALL), nDebitAll);
    BOOST_CHECK_EQUAL(tx.GetDebit(ISMINE_SPENDABLE), nDebitSpendable);
    BOOST_CHECK(tx.IsAmountCached(CWalletTx::DEBIT, ISMINE_SPENDABLE));
}

/**
 * Validates the correct behaviour of the CWalletTx "standard" balance methods.
 * (where "standard" is defined by direct P2PKH scripts, no P2CS contracts nor other types)
 *
 * 1) CWalletTx::GetCredit.
 * 2) CWalletTx::GetDebit.
 * 4) CWalletTx::GetAvailableCredit
 * 3) CWallet::GetUnconfirmedBalance.
 */
BOOST_AUTO_TEST_CASE(cached_balances_tests)
{
    // 1) Receive balance from an external source and verify:
    // * GetCredit(ISMINE_ALL) correctness (must be equal to 'nCredit' amount)
    // * GetCredit(ISMINE_SPENDABLE) correctness (must be equal to ISMINE_ALL) + must be cached.
    // * GetAvailableCredit() correctness (must be equal to ISMINE_ALL)
    // * GetDebit(ISMINE_ALL) correctness (must be 0)
    // * wallet.GetUnconfirmedBalance() correctness (must be equal 'nCredit')

    // 2) Confirm the tx and verify:
    // * wallet.GetUnconfirmedBalance() correctness (must be 0)
    // * GetAvailableCredit() correctness (must be equal to (1) ISMINE_ALL)

    // 3) Spend one of the two outputs of the receiving tx to an external source
    // and verify:
    // * creditTx.GetAvailableCredit() correctness (must be equal to 'nCredit' / 2) + must be cached.
    // * debitTx.GetDebit(ISMINE_ALL) correctness (must be equal to 'nCredit' / 2)
    // * debitTx.GetDebit(ISMINE_SPENDABLE) correctness (must be equal to 'nCredit' / 2) + must be cached.
    // * debitTx.GetAvailableCredit() correctness (must be 0).

    CAmount nCredit = 20 * COIN;

    // Setup wallet
    CWallet wallet("testWallet1", WalletDatabase::CreateMock());
    bool fFirstRun;
    BOOST_CHECK_EQUAL(wallet.LoadWallet(fFirstRun), DB_LOAD_OK);
    LOCK2(cs_main, wallet.cs_wallet);
    wallet.SetMinVersion(FEATURE_PRE_SPLIT_KEYPOOL);
    wallet.SetupSPKM(false);
    wallet.SetLastBlockProcessed(chainActive.Tip());

    // Receive balance from an external source
    auto res = wallet.getNewAddress("receiving_address");
    BOOST_ASSERT(res);
    CTxDestination receivingAddr = *res.getObjResult();
    CTxOut creditOut(nCredit/2, GetScriptForDestination(receivingAddr));
    CWalletTx& wtxCredit = ReceiveBalanceWith({creditOut, creditOut},wallet);

    // Validates (1)
    CheckBalances(
            wtxCredit,
            nCredit,            // CREDIT-ISMINE_ALL
            nCredit,            // CREDIT-ISMINE_SPENDABLE
            nCredit,            // AVAILABLE_CREDIT
            0,                  // DEBIT-ISMINE_ALL
            0                   // DEBIT-ISMINE_SPENDABLE
    );

    // GetUnconfirmedBalance requires tx in mempool.
    fakeMempoolInsertion(wtxCredit.tx);
    wtxCredit.fInMempool = true;
    BOOST_CHECK_EQUAL(wallet.GetUnconfirmedBalance(), nCredit);

    // 2) Confirm tx and verify
    SimpleFakeMine(wtxCredit, wallet);
    BOOST_CHECK_EQUAL(wallet.GetUnconfirmedBalance(), 0);
    BOOST_CHECK_EQUAL(wtxCredit.GetAvailableCredit(), nCredit);

    // 3) Spend one of the two outputs of the receiving tx to an external source and verify.
    // Create debit transaction.
    CAmount nDebit = nCredit / 2;
    std::vector<CTxIn> vinDebit = {CTxIn(COutPoint(wtxCredit.GetHash(), 0))};
    CKey key;
    key.MakeNewKey(true);
    std::vector<CTxOut> voutDebit = {CTxOut(nDebit, GetScriptForDestination(key.GetPubKey().GetID()))};
    CWalletTx& wtxDebit = BuildAndLoadTxToWallet(vinDebit, voutDebit, wallet);

    // Validates (3)

    // First the debit tx
    CheckBalances(
            wtxDebit,
            0,                   // CREDIT-ISMINE_ALL
            0,                   // CREDIT-ISMINE_SPENDABLE
            0,                   // AVAILABLE_CREDIT
            nDebit,              // DEBIT-ISMINE_ALL
            nDebit               // DEBIT-ISMINE_SPENDABLE
    );

    // Secondly the prev credit tx update

    // One output spent, the other one not. Force available credit recalculation.
    // If we don't request it, it will not happen.
    BOOST_CHECK_EQUAL(wtxCredit.GetAvailableCredit(false), nCredit - nDebit);
    BOOST_CHECK(wtxCredit.IsAmountCached(CWalletTx::AVAILABLE_CREDIT, ISMINE_SPENDABLE));

}

BOOST_AUTO_TEST_CASE(abandoned_tx_clears_stale_pending_balance)
{
    CAmount nCredit = 20 * COIN;

    CWallet wallet("testWallet_abandon_pending", WalletDatabase::CreateMock());
    bool fFirstRun;
    BOOST_CHECK_EQUAL(wallet.LoadWallet(fFirstRun), DB_LOAD_OK);
    LOCK2(cs_main, wallet.cs_wallet);
    wallet.SetMinVersion(FEATURE_PRE_SPLIT_KEYPOOL);
    wallet.SetupSPKM(false);
    wallet.SetLastBlockProcessed(chainActive.Tip());

    auto res = wallet.getNewAddress("receiving_address");
    BOOST_ASSERT(res);
    CTxDestination receivingAddr = *res.getObjResult();
    CTxOut creditOut(nCredit, GetScriptForDestination(receivingAddr));
    CWalletTx& wtxCredit = ReceiveBalanceWith({creditOut}, wallet);

    fakeMempoolInsertion(wtxCredit.tx);
    wtxCredit.fInMempool = true;
    BOOST_CHECK_EQUAL(wallet.GetUnconfirmedBalance(), nCredit);

    // Simulate stale mempool flag state: once abandoned, tx must never count as pending.
    wtxCredit.setAbandoned();
    wtxCredit.MarkDirty();
    BOOST_CHECK(!wtxCredit.InMempool());
    BOOST_CHECK_EQUAL(wallet.GetUnconfirmedBalance(), 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_CASE(testnet_rescan_handles_null_genesis_sapling_root, TestnetSetup)
{
    struct RescanWallet : CWallet {
        using CWallet::CWallet;
        ~RescanWallet() { GetDBHandle().Flush(true); }
    } wallet("testnet-rescan", WalletDatabase::Create(GetDataDir() / "testnet-rescan"));
    bool firstRun;
    BOOST_REQUIRE_EQUAL(wallet.LoadWallet(firstRun), DB_LOAD_OK);
    const SecureString passphrase = "public-testnet-rescan-fixture";
    BOOST_REQUIRE(wallet.EncryptWallet(passphrase));
    BOOST_REQUIRE(wallet.Unlock(passphrase));
    std::string address;
    BOOST_REQUIRE(wallet.GeneratePQAddress(address));
    pq::KeyID id;
    BOOST_REQUIRE(pq::DecodeAddress(address, Params().NetworkIDString(), id));
    BOOST_REQUIRE(wallet.Lock());
    const CScript scriptPubKey = pq::GetScript(id);

    std::unique_ptr<CBlockTemplate> blockTemplate = BlockAssembler(
            Params(), false).CreateNewBlock(scriptPubKey);
    BOOST_REQUIRE(blockTemplate);

    std::shared_ptr<CBlock> block = std::make_shared<CBlock>(blockTemplate->block);
    BOOST_REQUIRE(SolveBlock(block, 1));
    BOOST_REQUIRE(ProcessNewBlock(block, nullptr));

    CBlockIndex* tip = WITH_LOCK(cs_main, return chainActive.Tip());
    BOOST_REQUIRE_EQUAL(tip->GetBlockHash(), block->GetHash());
    WITH_LOCK(wallet.cs_wallet, wallet.SetLastBlockProcessed(tip));
    BOOST_REQUIRE(wallet.mapWallet.empty());

    WalletRescanReserver reserver(&wallet);
    BOOST_REQUIRE(reserver.reserve());
    CBlockIndex* genesis = WITH_LOCK(cs_main, return chainActive.Genesis());
    BOOST_REQUIRE(genesis);
    BOOST_REQUIRE(genesis->hashFinalSaplingRoot.IsNull());
    BOOST_REQUIRE(Params().GetConsensus().NetworkUpgradeActive(genesis->nHeight, Consensus::UPGRADE_V5_0));
    BOOST_CHECK_EQUAL(nullptr, wallet.ScanForWalletTransactions(genesis, nullptr, reserver));
    LOCK(wallet.cs_wallet);
    const auto* recovered = wallet.GetWalletTx(block->vtx[0]->GetHash());
    BOOST_REQUIRE(recovered);
    BOOST_CHECK_EQUAL(recovered->GetDepthInMainChain(), 1);
    BOOST_CHECK(recovered->IsInMainChainImmature());
    BOOST_REQUIRE_EQUAL(recovered->tx->vout.size(), 1U);
    BOOST_CHECK(wallet.IsPQMine(recovered->tx->vout[0]));
    BOOST_CHECK_EQUAL(recovered->tx->vout[0].nValue, block->vtx[0]->GetValueOut());
    BOOST_CHECK(wallet.IsLocked());
}
