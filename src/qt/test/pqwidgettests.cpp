// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include "pqwidgettests.h"
#include "pqwidget.h"
#include "bitcoinunits.h"
#include "defaultdialog.h"
#include "networkstyle.h"
#include "optionsmodel.h"
#include "organiclifegui.h"
#include "askpassphrasedialog.h"
#include "sendconfirmdialog.h"
#include "transactionrecord.h"
#include "ui_sendconfirmdialog.h"
#include <chainparams.h>
#include <crypto/mldsa44.h>
#include <guiinterface.h>
#include <guiutil.h>
#include <interfaces/handler.h>
#include <key.h>
#include <pqtransaction.h>
#include <script/sigcache.h>
#include <scheduler.h>
#include <validation.h>
#include <validationinterface.h>
#include <util/system.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSemaphore>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <thread>

void PQWidgetTests::noWalletIsReadOnly()
{
    PQWidget page(nullptr);
    QVERIFY(!page.createAddress->isEnabled());
    QVERIFY(!page.send->isEnabled());
    QVERIFY(page.receiveAddress->isReadOnly());
    QCOMPARE(page.history->columnCount(), 5);
    page.receive();
    page.pay();
    QVERIFY(page.receiveAddress->text().isEmpty());

    const QDir appDir(QCoreApplication::applicationDirPath());
    QString navSourcePath = appDir.absoluteFilePath("../navmenuwidget.cpp");
    if (!QFile::exists(navSourcePath)) {
        navSourcePath = appDir.absoluteFilePath("../../../../src/qt/navmenuwidget.cpp");
    }
    QFile navSource(navSourcePath);
    QVERIFY2(navSource.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(navSourcePath));
    const QString navigation = QString::fromUtf8(navSource.readAll());
    const qsizetype refreshStart = navigation.indexOf("void NavMenuWidget::updateButtonStyles()");
    const qsizetype refreshEnd = navigation.indexOf("NavMenuWidget::~NavMenuWidget", refreshStart);
    QVERIFY(refreshStart >= 0 && refreshEnd > refreshStart);
    const QString refresh = navigation.mid(refreshStart, refreshEnd - refreshStart);
    QVERIFY(refresh.contains("for (QWidget* button : btns)"));
    QVERIFY(refresh.contains("forceUpdateStyle(button, true)"));
}

void PQWidgetTests::actualWalletUnloadQuiescesModel()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto oldDataDir = gArgs.GetArg("-datadir", "");
    const auto oldNetwork = Params().NetworkIDString();
    struct RestoreEnvironment {
        std::string datadir, network;
        ~RestoreEnvironment() {
            gArgs.ForceSetArg("-datadir", datadir);
            ClearDatadirCache();
            SelectParams(network);
        }
    } restore{oldDataDir, oldNetwork};
    SelectParams(CBaseChainParams::TESTNET);
    gArgs.ForceSetArg("-datadir", directory.path().toStdString());
    ClearDatadirCache();

    const QDir appDir(QCoreApplication::applicationDirPath());
    QString appSourcePath = appDir.absoluteFilePath("../organiclife.cpp");
    if (!QFile::exists(appSourcePath)) {
        appSourcePath = appDir.absoluteFilePath("../../../../src/qt/organiclife.cpp");
    }
    QFile appSource(appSourcePath);
    QVERIFY2(appSource.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(appSourcePath));
    const QString lifecycleSource = QString::fromUtf8(appSource.readAll());
    const qsizetype unloadStart = lifecycleSource.indexOf("walletUnloadHandler =");
    const qsizetype unloadEnd = lifecycleSource.indexOf("#endif", unloadStart);
    QVERIFY(unloadStart >= 0 && unloadEnd > unloadStart);
    QVERIFY2(lifecycleSource.mid(unloadStart, unloadEnd - unloadStart)
                 .contains("GUIUtil::blockingGUIThreadConnection()"),
             "The production unload callback must finish deleting the Qt model before CWallet deletion");

    auto* unloadingWallet = new CWallet("pq-ui-unload",
        WalletDatabase::Create(fs::path(directory.path().toStdString()) / "unload-wallet"));
    bool firstRun;
    QVERIFY(unloadingWallet->LoadWallet(firstRun) == DB_LOAD_OK);
    vpwallets.push_back(unloadingWallet);
    struct WalletCleanup {
        CWallet*& wallet;
        ~WalletCleanup() {
            if (!wallet) return;
            const auto it = std::find(vpwallets.begin(), vpwallets.end(), wallet);
            if (it != vpwallets.end()) vpwallets.erase(it);
            delete wallet;
        }
    } cleanup{unloadingWallet};

    OptionsModel options;
    auto* unloadingModel = new WalletModel(unloadingWallet, &options);
    unloadingModel->init();
    QPointer<WalletModel> guardedModel(unloadingModel);
    auto* pollTimer = unloadingModel->findChild<QTimer*>();
    QVERIFY(pollTimer && pollTimer->isActive());
    QSemaphore futureStarted, releaseFuture;
    std::atomic<bool> futureFinished{false};
    unloadingModel->pollFuture = QtConcurrent::run([&] {
        futureStarted.release();
        releaseFuture.acquire();
        futureFinished.store(true);
    });
    QVERIFY(futureStarted.tryAcquire(1, 1000));
    QVERIFY(unloadingModel->pollFuture.isRunning());
    std::thread releaseThread([&] {
        QThread::msleep(100);
        releaseFuture.release();
    });

    bool cleanupRan = false;
    bool stoppedBeforeDelete = false;
    bool coreAliveDuringCleanup = false;
    auto unloadHandler = interfaces::MakeHandler(uiInterface.UnloadWallet.connect([&](CWallet* wallet) {
        if (wallet != unloadingWallet) return;
        QMetaObject::invokeMethod(qApp, [&] {
            unloadingModel->stop();
            stoppedBeforeDelete = !pollTimer->isActive() && futureFinished.load();
            delete unloadingModel;
            unloadingModel = nullptr;
            coreAliveDuringCleanup = wallet->GetName() == "pq-ui-unload";
            cleanupRan = true;
        }, GUIUtil::blockingGUIThreadConnection());
    }));
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("test"));
    QVERIFY(networkStyle);
    OrganicLifeGUI mainWindow(networkStyle.get());
    AskPassphraseDialog encryptDialog(AskPassphraseDialog::Mode::Encrypt, &mainWindow,
        unloadingModel, AskPassphraseDialog::Context::Encrypt);
    auto* passphrase = encryptDialog.findChild<QLineEdit*>("passEdit2");
    auto* repeatedPassphrase = encryptDialog.findChild<QLineEdit*>("passEdit3");
    QVERIFY(passphrase && repeatedPassphrase);
    passphrase->setText("pq-ui-unload-passphrase");
    repeatedPassphrase->setText("pq-ui-unload-passphrase");
    QFuture<bool> unloadFuture;
    std::string unloadError;
    bool unloadStarted = false;
    bool confirmationAccepted = false;
    bool encryptionCompleted = false;
    QTimer closeEncryptionDialogs;
    connect(&closeEncryptionDialogs, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<DefaultDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (!confirmationAccepted) {
            confirmationAccepted = true;
            dialog->accept();
            return;
        }
        if (!unloadStarted) {
            encryptionCompleted = unloadingWallet->IsCrypted();
            unloadStarted = true;
            unloadFuture = QtConcurrent::run([&] {
                const auto result = UnloadWalletByName("pq-ui-unload");
                unloadError = result.getError();
                return bool(result);
            });
        }
        dialog->accept();
    });
    closeEncryptionDialogs.start(10);
    encryptDialog.accept();
    closeEncryptionDialogs.stop();
    QVERIFY(unloadStarted);
    QVERIFY(encryptionCompleted);
    QTRY_VERIFY_WITH_TIMEOUT(unloadFuture.isFinished(), 10000);
    const bool unloaded = unloadFuture.result();
    if (unloaded) unloadingWallet = nullptr;
    releaseThread.join();
    QVERIFY2(unloaded, unloadError.c_str());
    QVERIFY(cleanupRan);
    QVERIFY(stoppedBeforeDelete);
    QVERIFY(coreAliveDuringCleanup);
    QVERIFY(guardedModel.isNull());
    QVERIFY(std::none_of(vpwallets.begin(), vpwallets.end(), [](const CWalletRef wallet) {
        return wallet && wallet->GetName() == "pq-ui-unload";
    }));

    // Keep a direct GUI-thread unload covered as well as the worker-thread path above.
    auto* directWallet = new CWallet("pq-ui-unload-direct",
        WalletDatabase::Create(fs::path(directory.path().toStdString()) / "unload-wallet-direct"));
    QVERIFY(directWallet->LoadWallet(firstRun) == DB_LOAD_OK);
    vpwallets.push_back(directWallet);
    struct DirectWalletCleanup {
        CWallet*& wallet;
        ~DirectWalletCleanup() {
            if (!wallet) return;
            const auto it = std::find(vpwallets.begin(), vpwallets.end(), wallet);
            if (it != vpwallets.end()) vpwallets.erase(it);
            delete wallet;
        }
    } directCleanup{directWallet};
    auto* directModel = new WalletModel(directWallet, &options);
    directModel->init();
    QPointer<WalletModel> guardedDirectModel(directModel);
    bool directCleanupRan = false;
    auto directUnloadHandler = interfaces::MakeHandler(uiInterface.UnloadWallet.connect([&](CWallet* wallet) {
        if (wallet != directWallet) return;
        QMetaObject::invokeMethod(qApp, [&] {
            directModel->stop();
            delete directModel;
            directModel = nullptr;
            directCleanupRan = true;
        }, GUIUtil::blockingGUIThreadConnection());
    }));
    const OperationResult directUnloaded = UnloadWalletByName("pq-ui-unload-direct");
    QVERIFY2(directUnloaded, directUnloaded.getError().c_str());
    directWallet = nullptr;
    QVERIFY(directCleanupRan);
    QVERIFY(guardedDirectModel.isNull());
}

void PQWidgetTests::encryptedReceiveBackupAndFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto oldDataDir = gArgs.GetArg("-datadir", "");
    const auto oldNetwork = Params().NetworkIDString();
    struct RestoreEnvironment {
        std::string datadir, network;
        ~RestoreEnvironment() {
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
    QString address, backupFile, paymentBackupFile;
    std::vector<std::string> paymentBackupAddresses;
    {
    DiskWallet wallet("pq-ui", WalletDatabase::Create(fs::path(directory.path().toStdString()) / "wallet"));
    bool firstRun;
    QCOMPARE(wallet.LoadWallet(firstRun), DB_LOAD_OK);
    CKey key;
    key.MakeNewKey(true);
    QVERIFY(wallet.AddKeyPubKey(key, key.GetPubKey()));
    QVERIFY(wallet.EncryptWallet(SecureString("pq-ui-test-passphrase")));
    QVERIFY(wallet.Unlock(SecureString("pq-ui-test-passphrase")));
    OptionsModel options;
    WalletModel model(&wallet, &options);
    model.init();
    QVERIFY(QMetaObject::invokeMethod(&model, "pollBalanceChanged", Qt::DirectConnection));
    PQWidget page(nullptr);
    page.setWalletModel(&model);
    const QString backupPath = directory.path() + "/backups";
    QVERIFY(QDir().mkdir(backupPath));
    page.backupDirectory = backupPath;
    page.receive();
    address = page.receiveAddress->text();
    QVERIFY(address.startsWith("olcpqtest1"));
    const auto backups = QDir(backupPath).entryList({"*.dat"}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    backupFile = backupPath + "/" + backups.first();
    // An actual invalid backup path must leave the previously exposed address unchanged.
    page.backupDirectory = directory.path() + "/missing/subdirectory";
    QTimer closeMessage;
    connect(&closeMessage, &QTimer::timeout, [] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->accept();
    });
    closeMessage.start(10);
    page.receive();
    QCOMPARE(page.receiveAddress->text(), address);
    QCOMPARE(QDir(backupPath).entryList({"*.dat"}, QDir::Files).size(), 1);
    QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
    QVERIFY(!page.backupSnapshot());
    page.amount->setText("0.000000001");
    page.pay();
    QVERIFY(wallet.mapWallet.empty());

    // Exercise actual signed payment preparation against a disposable confirmed
    // coin. Cancellation and backup failure must never reach CommitTransaction.
    {
        QVERIFY(mapBlockIndex.empty());
        QVERIFY(!pcoinsTip);
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
            ~ClearChain() {
                LOCK(cs_main);
                mempool.clear();
                chainActive.SetTip(nullptr);
                mapBlockIndex.clear();
                pindexBestHeader = oldBestHeader;
                pcoinsTip.reset();
            }
        } clearChain{pindexBestHeader};
        pq::KeyID id;
        QVERIFY(pq::DecodeAddress(address.toStdString(), "test", id));
        CMutableTransaction incoming;
        incoming.vin.emplace_back(uint256S("5678"), 0);
        incoming.vout.emplace_back(5 * COIN, pq::GetScript(id));
        const auto tx = MakeTransactionRef(incoming);
        const COutPoint coin(tx->GetHash(), 0);
        {
            LOCK2(cs_main, wallet.cs_wallet);
            mempool.clear();
            mapBlockIndex.emplace(genesisHash, &genesis);
            mapBlockIndex.emplace(tipHash, &tip);
            chainActive.SetTip(&tip);
            pindexBestHeader = &tip;
            pcoinsTip = std::make_unique<CCoinsViewCache>(&view);
            pcoinsTip->SetBestBlock(tipHash);
            wallet.SetLastBlockProcessed(&tip);
            QVERIFY(wallet.AddToWalletIfInvolvingMe(tx,
                {CWalletTx::Status::CONFIRMED, 1, tipHash, 0}, true));
            pcoinsTip->AddCoin(coin, Coin(tx->vout[0], 1, false, false), false);
        }
        TxDetailDialog detail(nullptr, false);
        detail.model = &model;
        detail.txHash = tx->GetHash();
        detail.setInputsType(tx);
        QCOMPARE(detail.ui->labelTitlePrevTx->text(), QString("Previous Transaction"));
        mldsa44::Key externalKey;
        QVERIFY(externalKey.Generate());
        const auto externalId = pq::GetID(externalKey.GetPublicKey(), "test");
        QVERIFY(externalId);
        const QString externalAddress = QString::fromStdString(pq::EncodeAddress(*externalId, "test"));
        page.recipient->setText(address);
        page.amount->setText("1.00000000");
        page.backupDirectory = backupPath;
        closeMessage.stop();
        bool confirmed = false;
        QTimer cancelPayment;
        connect(&cancelPayment, &QTimer::timeout, [&] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                dialog->done(QMessageBox::Cancel);
                QVERIFY(dialog->text().contains(address));
                QVERIFY(dialog->text().contains("Fee:"));
                QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
                confirmed = true;
            }
        });
        cancelPayment.start(10);
        page.pay();
        cancelPayment.stop();
        QVERIFY(confirmed);
        QCOMPARE(wallet.mapWallet.size(), size_t(1));
        QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
        QVERIFY(!wallet.IsSpent(coin));
        QCOMPARE(QDir(backupPath).entryList({"*.dat"}, QDir::Files).size(), 1);

        bool backupFailed = false;
        QTimer acceptPayment;
        connect(&acceptPayment, &QTimer::timeout, [&] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if (dialog->text().startsWith("Recipient:")) dialog->done(QMessageBox::Yes);
                else { backupFailed = dialog->text().contains("wallet backup could not be saved"); dialog->accept(); }
            }
        });
        page.backupDirectory = directory.path() + "/missing/subdirectory";
        acceptPayment.start(10);
        page.pay();
        acceptPayment.stop();
        QVERIFY(backupFailed);
        QCOMPARE(wallet.mapWallet.size(), size_t(1));
        QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
        QVERIFY(!wallet.IsSpent(coin));

        CScheduler scheduler;
        std::thread schedulerThread([&] { scheduler.serviceQueue(); });
        GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);
        struct ClearSignals {
            CScheduler& scheduler;
            std::thread& thread;
            ~ClearSignals() {
                scheduler.stop();
                if (thread.joinable()) thread.join();
                GetMainSignals().FlushBackgroundCallbacks();
                GetMainSignals().UnregisterBackgroundSignalScheduler();
            }
        } clearSignals{scheduler, schedulerThread};
        page.recipient->setText(externalAddress);
        page.amount->setText("1.00000000");
        page.backupDirectory = backupPath;
        QString paymentReview, sentHash;
        bool acceptedReview = false;
        QTimer completePayment;
        connect(&completePayment, &QTimer::timeout, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (dialog->standardButtons().testFlag(QMessageBox::Yes)) {
                paymentReview = dialog->text();
                QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
                acceptedReview = true;
                dialog->done(QMessageBox::Yes);
            } else {
                sentHash = dialog->text();
                dialog->accept();
            }
        });
        completePayment.start(10);
        page.pay();
        completePayment.stop();
        QVERIFY(acceptedReview);
        QVERIFY(!sentHash.isEmpty());
        QCOMPARE(wallet.mapWallet.size(), size_t(2));
        QVERIFY(wallet.IsSpent(coin));
        CTransactionRef payment;
        for (const auto& item : wallet.mapWallet) {
            if (item.first != tx->GetHash()) payment = item.second.tx;
        }
        QVERIFY(payment);
        const CWalletTx* paymentWalletTx = wallet.GetWalletTx(payment->GetHash());
        QVERIFY(paymentWalletTx);
        QVERIFY(TransactionRecord::decomposeTransaction(&wallet, *paymentWalletTx).empty());
        QCOMPARE(sentHash, QString::fromStdString(payment->GetHash().ToString()));
        QVERIFY(mempool.exists(payment->GetHash()));
        pq::Payload paymentPayload;
        QVERIFY(pq::DecodePayload(*payment, paymentPayload));
        QCOMPARE(paymentPayload.mode, uint8_t(pq::TRANSFER));
        const CAmount fee = 5 * COIN - payment->GetValueOut();
        QVERIFY(fee > 0);
        const auto formatted = [](CAmount value) {
            return BitcoinUnits::format(BitcoinUnits::PIV, value, false, BitcoinUnits::separatorStandard, false) +
                   " " + BitcoinUnits::name(BitcoinUnits::PIV);
        };
        QVERIFY(paymentReview.contains("Recipient: " + externalAddress));
        QVERIFY(paymentReview.contains("Amount: " + formatted(COIN)));
        QVERIFY(paymentReview.contains("Fee: " + formatted(fee)));
        QVERIFY(paymentReview.contains("Total debit: " + formatted(COIN + fee)));
        QCOMPARE(page.amount->text(), QString());
        const auto paymentBackups = QDir(backupPath).entryList({"*.dat"}, QDir::Files);
        QCOMPARE(paymentBackups.size(), 2);
        for (const QString& file : paymentBackups) {
            const QString fullPath = backupPath + "/" + file;
            if (fullPath != backupFile) paymentBackupFile = fullPath;
        }
        QVERIFY(!paymentBackupFile.isEmpty());
        paymentBackupAddresses = wallet.GetPQAddresses();

        CAmount pending = 0;
        for (const auto& output : payment->vout) if (wallet.IsPQMine(output)) pending += output.nValue;
        QCOMPARE(page.balance->text(), QString("Confirmed PQ: %1     Pending PQ: %2").arg(formatted(0), formatted(pending)));
        const auto rowFor = [&](const uint256& hash) {
            const QString txid = QString::fromStdString(hash.ToString());
            for (int row = 0; row < page.history->rowCount(); ++row)
                if (page.history->item(row, 4)->text() == txid) return row;
            return -1;
        };
        const int rewardRow = rowFor(tx->GetHash());
        const int paymentRow = rowFor(payment->GetHash());
        QVERIFY(rewardRow >= 0);
        QVERIFY(paymentRow >= 0);
        QCOMPARE(page.history->item(rewardRow, 1)->text(), QString("PQ reward"));
        QCOMPARE(page.history->item(rewardRow, 2)->text(), formatted(5 * COIN));
        QCOMPARE(page.history->item(rewardRow, 3)->text(), QString("1 confirmations"));
        QCOMPARE(page.history->item(paymentRow, 1)->text(), QString("PQ payment"));
        QCOMPARE(page.history->item(paymentRow, 2)->text(), formatted(-(COIN + fee)));
        QCOMPARE(page.history->item(paymentRow, 3)->text(), QString("Pending"));
    }
    page.clearWalletModel();
    QVERIFY(page.receiveAddress->text().isEmpty());
    QVERIFY(!page.createAddress->isEnabled());
    QVERIFY(!page.send->isEnabled());
    QCOMPARE(page.history->rowCount(), 0);

    // Even a direct call (rather than hidden navigation) cannot expose PQ on mainnet.
    SelectParams(CBaseChainParams::MAIN);
    page.setWalletModel(&model);
    page.receive();
    page.pay();
    QVERIFY(page.receiveAddress->text().isEmpty());
    QVERIFY(!page.createAddress->isEnabled());
    QVERIFY(!page.send->isEnabled());
    page.clearWalletModel();
    SelectParams(CBaseChainParams::TESTNET);

    // Unloading the selected model while its unlock UI is open must safely cancel.
    QVERIFY(wallet.Lock());
    auto* unloadingModel = new WalletModel(&wallet, &options);
    unloadingModel->init();
    page.setWalletModel(unloadingModel);
    page.backupDirectory = backupPath;
    connect(unloadingModel, &WalletModel::requireUnlock, &page, [&] {
        AskPassphraseDialog dialog(AskPassphraseDialog::Mode::Unlock, &page,
            unloadingModel, AskPassphraseDialog::Context::Unlock_Full);
        QTimer::singleShot(0, &dialog, [&] {
            page.clearWalletModel();
            delete unloadingModel;
            unloadingModel = nullptr;
        });
        QCOMPARE(dialog.exec(), int(QDialog::Rejected));
    });
    page.receive();
    QVERIFY(!unloadingModel);
    QVERIFY(wallet.IsLocked());
    QVERIFY(page.receiveAddress->text().isEmpty());
    QCOMPARE(QDir(backupPath).entryList({"*.dat"}, QDir::Files).size(), 2);
    }
    // Berkeley DB forbids opening a byte-identical backup while its original
    // file ID is still loaded. Restore after closing the original wallet.
    DiskWallet restored("pq-restored", WalletDatabase::Create(fs::path(backupFile.toStdString())));
    bool firstRun;
    QCOMPARE(restored.LoadWallet(firstRun), DB_LOAD_OK);
    QVERIFY(restored.IsLocked());
    QCOMPARE(restored.GetPQAddresses().size(), size_t(1));
    QCOMPARE(QString::fromStdString(restored.GetPQAddresses().front()), address);
    QVERIFY(restored.Unlock(SecureString("pq-ui-test-passphrase")));
    mldsa44::Key restoredKey;
    QVERIFY(restored.GetPQKey(address.toStdString(), restoredKey));
    restored.GetDBHandle().Flush(true);
    {
        DiskWallet paymentRestored("pq-payment-restored", WalletDatabase::Create(fs::path(paymentBackupFile.toStdString())));
        QCOMPARE(paymentRestored.LoadWallet(firstRun), DB_LOAD_OK);
        QVERIFY(paymentRestored.IsLocked());
        QCOMPARE(paymentRestored.GetPQAddresses(), paymentBackupAddresses);
        QVERIFY(paymentRestored.Unlock(SecureString("pq-ui-test-passphrase")));
        for (const auto& restoredAddress : paymentBackupAddresses) {
            mldsa44::Key key;
            QVERIFY(paymentRestored.GetPQKey(restoredAddress, key));
        }
    }
}
