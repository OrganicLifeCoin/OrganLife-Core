// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include "pqwidgettests.h"

#include "askpassphrasedialog.h"
#include "defaultdialog.h"
#include "networkstyle.h"
#include "optionsmodel.h"
#include "organiclifegui.h"
#include "pqwalletui.h"
#include "receivewidget.h"
#include "send.h"
#include "transactionrecord.h"
#include "walletmodeltransaction.h"

#include <chainparams.h>
#include <crypto/mldsa44.h>
#include <guiinterface.h>
#include <guiutil.h>
#include <interfaces/handler.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <pqtransaction.h>
#include <scheduler.h>
#include <script/sigcache.h>
#include <validation.h>
#include <validationinterface.h>
#include <util/system.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSemaphore>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <thread>

void PQWidgetTests::seamlessWalletUsesStandardScreens()
{
    const auto oldNetwork = Params().NetworkIDString();
    SelectParams(CBaseChainParams::TESTNET);
    {
        std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("test"));
        QVERIFY(networkStyle);
        OrganicLifeGUI mainWindow(networkStyle.get());
        QVERIFY(mainWindow.findChild<SendWidget*>());
        QVERIFY(mainWindow.findChild<ReceiveWidget*>());
        QVERIFY(!mainWindow.findChild<QWidget*>("pqWalletPage"));
        QVERIFY(mainWindow.findChild<QWidget*>("btnSend"));
        QVERIFY(mainWindow.findChild<QWidget*>("btnReceive"));
        QVERIFY(!mainWindow.findChild<QWidget*>("btnPQ"));
    }
    SelectParams(oldNetwork);
}

void PQWidgetTests::actualWalletUnloadQuiescesModel()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto oldDataDir = gArgs.GetArg("-datadir", "");
    const auto oldNetwork = Params().NetworkIDString();
    struct RestoreEnvironment {
        std::string datadir, network;
        ~RestoreEnvironment()
        {
            gArgs.ForceSetArg("-datadir", datadir);
            ClearDatadirCache();
            SelectParams(network);
        }
    } restore{oldDataDir, oldNetwork};
    SelectParams(CBaseChainParams::TESTNET);
    gArgs.ForceSetArg("-datadir", directory.path().toStdString());
    ClearDatadirCache();

    auto* wallet = new CWallet("pq-ui-unload",
        WalletDatabase::Create(fs::path(directory.path().toStdString()) / "wallet"));
    bool firstRun;
    QCOMPARE(wallet->LoadWallet(firstRun), DB_LOAD_OK);
    vpwallets.push_back(wallet);
    auto* model = new WalletModel(wallet, new OptionsModel());
    model->getOptionsModel()->setParent(model);
    model->init();
    QPointer<WalletModel> guardedModel(model);
    auto* pollTimer = model->findChild<QTimer*>();
    QVERIFY(pollTimer && pollTimer->isActive());

    QSemaphore futureStarted, releaseFuture;
    std::atomic<bool> futureFinished{false};
    model->pollFuture = QtConcurrent::run([&] {
        futureStarted.release();
        releaseFuture.acquire();
        futureFinished.store(true);
    });
    QVERIFY(futureStarted.tryAcquire(1, 1000));
    std::thread releaseThread([&] {
        QThread::msleep(100);
        releaseFuture.release();
    });

    bool cleanupRan = false;
    bool stoppedBeforeDelete = false;
    auto unloadHandler = interfaces::MakeHandler(uiInterface.UnloadWallet.connect([&](CWallet* unloading) {
        if (unloading != wallet) return;
        QMetaObject::invokeMethod(qApp, [&] {
            model->stop();
            stoppedBeforeDelete = !pollTimer->isActive() && futureFinished.load();
            delete model;
            model = nullptr;
            cleanupRan = true;
        }, GUIUtil::blockingGUIThreadConnection());
    }));

    std::string unloadError;
    QFuture<bool> unloadFuture = QtConcurrent::run([&] {
        const OperationResult result = UnloadWalletByName("pq-ui-unload");
        unloadError = result.getError();
        return bool(result);
    });
    QTRY_VERIFY_WITH_TIMEOUT(unloadFuture.isFinished(), 10000);
    const bool unloaded = unloadFuture.result();
    releaseThread.join();
    QVERIFY2(unloaded, unloadError.c_str());
    wallet = nullptr;
    QVERIFY(cleanupRan);
    QVERIFY(stoppedBeforeDelete);
    QVERIFY(guardedModel.isNull());
}

void PQWidgetTests::standardReceiveUsesPQAddressAndBackup()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto oldDataDir = gArgs.GetArg("-datadir", "");
    const auto oldNetwork = Params().NetworkIDString();
    struct RestoreEnvironment {
        std::string datadir, network;
        ~RestoreEnvironment()
        {
            gArgs.ForceSetArg("-datadir", datadir);
            ClearDatadirCache();
            SelectParams(network);
            ECC_Stop();
        }
    } restore{oldDataDir, oldNetwork};
    ECC_Start();
    InitSignatureCache();
    ECCVerifyHandle verify;
    SelectParams(CBaseChainParams::TESTNET);
    gArgs.ForceSetArg("-datadir", directory.path().toStdString());
    ClearDatadirCache();

    CWallet wallet("pq-standard-receive",
        WalletDatabase::Create(fs::path(directory.path().toStdString()) / "wallet"));
    bool firstRun;
    QCOMPARE(wallet.LoadWallet(firstRun), DB_LOAD_OK);
    CKey key;
    key.MakeNewKey(true);
    QVERIFY(wallet.AddKeyPubKey(key, key.GetPubKey()));
    QVERIFY(wallet.EncryptWallet(SecureString("pq-standard-receive-passphrase")));
    QVERIFY(wallet.Unlock(SecureString("pq-standard-receive-passphrase")));
    OptionsModel options;
    WalletModel model(&wallet, &options);
    model.init();

    const QString backupPath = directory.path() + "/backups";
    QVERIFY(QDir().mkdir(backupPath));
    QSettings().setValue(PQWalletUI::backupSettingsKey(&model), backupPath);

    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("test"));
    QVERIFY(networkStyle);
    OrganicLifeGUI mainWindow(networkStyle.get());
    ReceiveWidget page(&mainWindow);
    page.setWalletModel(&model);
    page.onNewAddressClicked();

    auto* addressLabel = page.findChild<QLabel*>("labelAddress");
    QVERIFY(addressLabel);
    QVERIFY(addressLabel->text().startsWith("olcpqtest1"));
    QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
    QCOMPARE(QDir(backupPath).entryList({"*.dat"}, QDir::Files).size(), 1);
    QSettings().remove(PQWalletUI::backupSettingsKey(&model));
}

void PQWidgetTests::standardSendUsesPQBackupAndHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto oldDataDir = gArgs.GetArg("-datadir", "");
    const auto oldNetwork = Params().NetworkIDString();
    struct RestoreEnvironment {
        std::string datadir, network;
        ~RestoreEnvironment()
        {
            gArgs.ForceSetArg("-datadir", datadir);
            ClearDatadirCache();
            SelectParams(network);
            ECC_Stop();
        }
    } restore{oldDataDir, oldNetwork};
    ECC_Start();
    InitSignatureCache();
    ECCVerifyHandle verify;
    SelectParams(CBaseChainParams::TESTNET);
    gArgs.ForceSetArg("-datadir", directory.path().toStdString());
    ClearDatadirCache();

    struct DiskWallet : CWallet {
        using CWallet::CWallet;
        ~DiskWallet() { GetDBHandle().Flush(true); }
    };
    QString paymentBackupFile;
    std::vector<std::string> paymentBackupAddresses;
    {
        DiskWallet wallet("pq-standard-send",
            WalletDatabase::Create(fs::path(directory.path().toStdString()) / "wallet"));
        bool firstRun;
        QCOMPARE(wallet.LoadWallet(firstRun), DB_LOAD_OK);
        CKey key;
        key.MakeNewKey(true);
        QVERIFY(wallet.AddKeyPubKey(key, key.GetPubKey()));
        QVERIFY(wallet.EncryptWallet(SecureString("pq-standard-send-passphrase")));
        QVERIFY(wallet.Unlock(SecureString("pq-standard-send-passphrase")));
        std::string ownedAddress;
        QVERIFY(wallet.GeneratePQAddress(ownedAddress));

        OptionsModel options;
        WalletModel model(&wallet, &options);
        model.init();
        const QString backupPath = directory.path() + "/backups";
        QVERIFY(QDir().mkdir(backupPath));

        CCoinsView view;
        CBlockIndex genesis(Params().GenesisBlock()), tip(Params().GenesisBlock());
        const auto genesisHash = Params().GetConsensus().hashGenesisBlock;
        const auto tipHash = uint256S("1234");
        genesis.phashBlock = &genesisHash;
        tip.phashBlock = &tipHash;
        tip.pprev = &genesis;
        tip.nHeight = 1;
        struct ClearChain {
            CBlockIndex* oldBestHeader;
            ~ClearChain()
            {
                LOCK(cs_main);
                mempool.clear();
                chainActive.SetTip(nullptr);
                mapBlockIndex.clear();
                pindexBestHeader = oldBestHeader;
                pcoinsTip.reset();
            }
        } clearChain{pindexBestHeader};

        pq::KeyID ownedId;
        QVERIFY(pq::DecodeAddress(ownedAddress, "test", ownedId));
        CMutableTransaction incoming;
        incoming.nType = CTransaction::PQ;
        incoming.vin.emplace_back(uint256S("5678"), 0);
        incoming.vout.emplace_back(5 * COIN, pq::GetScript(ownedId));
        const auto funding = MakeTransactionRef(incoming);
        const COutPoint coin(funding->GetHash(), 0);
        {
            LOCK2(cs_main, wallet.cs_wallet);
            mapBlockIndex.emplace(genesisHash, &genesis);
            mapBlockIndex.emplace(tipHash, &tip);
            chainActive.SetTip(&tip);
            pindexBestHeader = &tip;
            pcoinsTip = std::make_unique<CCoinsViewCache>(&view);
            pcoinsTip->SetBestBlock(tipHash);
            wallet.SetLastBlockProcessed(&tip);
            QVERIFY(wallet.AddToWalletIfInvolvingMe(funding,
                {CWalletTx::Status::CONFIRMED, 1, tipHash, 0}, true));
            pcoinsTip->AddCoin(coin, Coin(funding->vout[0], 1, false, false), false);
        }

        interfaces::Wallet walletInterface(wallet);
        QCOMPARE(walletInterface.getBalances().balance, CAmount(5 * COIN));
        const auto incomingRows = TransactionRecord::decomposeTransaction(
            &wallet, *wallet.GetWalletTx(funding->GetHash()));
        QCOMPARE(incomingRows.size(), size_t(1));
        QCOMPARE(incomingRows.front().credit, CAmount(5 * COIN));

        mldsa44::Key externalKey;
        QVERIFY(externalKey.Generate());
        const auto externalId = pq::GetID(externalKey.GetPublicKey(), "test");
        QVERIFY(externalId);
        const QString externalAddress = QString::fromStdString(pq::EncodeAddress(*externalId, "test"));
        std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("test"));
        QVERIFY(networkStyle);
        OrganicLifeGUI window(networkStyle.get());
        auto* dashboard = window.findChild<DashboardWidget*>();
        QVERIFY(dashboard);
        dashboard->setWalletModel(&model);
        model.emitBalanceChanged();
        QCoreApplication::processEvents();
        auto* dashboardBalance = dashboard->findChild<QLabel*>("headerAvailableBalance");
        QVERIFY(dashboardBalance);
        QCOMPARE(dashboardBalance->text(), GUIUtil::formatBalance(5 * COIN, options.getDisplayUnit()));
        SendWidget send(&window);
        send.setWalletModel(&model);
        auto* entry = send.entries.front();
        entry->setAddress(externalAddress);
        entry->setAmount("1.00000000");
        QVERIFY(entry->validate());
        QVERIFY(send.findChild<QWidget*>("pushButtonAddRecipient")->isHidden());
        QVERIFY(send.findChild<QWidget*>("pushLeft")->isHidden());
        QVERIFY(send.findChild<QWidget*>("pushRight")->isHidden());

        SendCoinsRecipient recipient;
        recipient.address = externalAddress;
        recipient.amount = COIN;

        // A prepared payment that is cancelled must discard its unused change key.
        send.beginPQPreparation();
        QCOMPARE(send.pqPreparationWallet.data(), &model);
        WalletModelTransaction cancelled({recipient});
        QCOMPARE(model.prepareTransaction(&cancelled).status, WalletModel::OK);
        QVERIFY(wallet.GetPQAddresses().size() > size_t(1));
        QVERIFY(send.cleanupNewPQKeys());
        QVERIFY(send.pqPreparationWallet.isNull());
        QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
        QVERIFY(!wallet.IsSpent(coin));

        // Unloading the active wallet must clean the key from that exact wallet.
        send.beginPQPreparation();
        WalletModelTransaction interrupted({recipient});
        QCOMPARE(model.prepareTransaction(&interrupted).status, WalletModel::OK);
        QVERIFY(wallet.GetPQAddresses().size() > size_t(1));
        send.clearWalletModel();
        QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
        QVERIFY(send.pqPreparationWallet.isNull());
        QVERIFY(!entry->validate());
        send.setWalletModel(&model);

        // A failed snapshot must also leave the coin unspent and discard change.
        send.beginPQPreparation();
        WalletModelTransaction failedBackup({recipient});
        QCOMPARE(model.prepareTransaction(&failedBackup).status, WalletModel::OK);
        QString invalidBackupPath = directory.path() + "/missing/subdirectory";
        QVERIFY(!PQWalletUI::backupSnapshot(&model, invalidBackupPath));
        QVERIFY(send.cleanupNewPQKeys());
        QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
        QVERIFY(!wallet.IsSpent(coin));

        // The successful path snapshots the generated change key before commit.
        send.beginPQPreparation();
        WalletModelTransaction payment({recipient});
        QCOMPARE(model.prepareTransaction(&payment).status, WalletModel::OK);
        QString validBackupPath = backupPath;
        QVERIFY(PQWalletUI::backupSnapshot(&model, validBackupPath));
        send.pqPreparationTracked = false;
        send.pqPreparationWallet.clear();
        send.pqAddressesBeforePrepare.clear();
        paymentBackupAddresses = wallet.GetPQAddresses();

        CScheduler scheduler;
        std::thread schedulerThread([&] { scheduler.serviceQueue(); });
        GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);
        const auto committed = model.sendCoins(payment);
        scheduler.stop();
        schedulerThread.join();
        GetMainSignals().FlushBackgroundCallbacks();
        GetMainSignals().UnregisterBackgroundSignalScheduler();
        QCOMPARE(committed.status, WalletModel::OK);
        QVERIFY(wallet.IsSpent(coin));
        QVERIFY(mempool.exists(payment.getTransaction()->GetHash()));
        QCOMPARE(payment.getTransaction()->nType, static_cast<uint16_t>(CTransaction::PQ));

        const CWalletTx* paymentWalletTx = wallet.GetWalletTx(payment.getTransaction()->GetHash());
        QVERIFY(paymentWalletTx);
        const auto outgoingRows = TransactionRecord::decomposeTransaction(&wallet, *paymentWalletTx);
        QCOMPARE(outgoingRows.size(), size_t(1));
        QVERIFY(outgoingRows.front().debit < 0);
        QCOMPARE(QString::fromStdString(outgoingRows.front().address), externalAddress);

        const auto backups = QDir(backupPath).entryList({"*.dat"}, QDir::Files);
        QCOMPARE(backups.size(), 1);
        paymentBackupFile = backupPath + "/" + backups.front();
    }

    DiskWallet restored("pq-standard-send-restored",
        WalletDatabase::Create(fs::path(paymentBackupFile.toStdString())));
    bool firstRun;
    QCOMPARE(restored.LoadWallet(firstRun), DB_LOAD_OK);
    QCOMPARE(restored.GetPQAddresses(), paymentBackupAddresses);
    QVERIFY(restored.Unlock(SecureString("pq-standard-send-passphrase")));
    for (const auto& address : paymentBackupAddresses) {
        mldsa44::Key key;
        QVERIFY(restored.GetPQKey(address, key));
    }
}
