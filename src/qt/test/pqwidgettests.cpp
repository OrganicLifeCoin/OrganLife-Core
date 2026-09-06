// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include "pqwidgettests.h"

#include "askpassphrasedialog.h"
#include "defaultdialog.h"
#include "dashboardwidget.h"
#include "coincontroldialog.h"
#include "requestdialog.h"
#include "sendconfirmdialog.h"
#include "networkstyle.h"
#include "optionsmodel.h"
#include "organiclifegui.h"
#include "pqwalletui.h"
#include "receivewidget.h"
#include "send.h"
#include "transactionrecord.h"
#include "transactiontablemodel.h"
#include "walletmodeltransaction.h"

#include <chainparams.h>
#include <coincontrol.h>
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
#include <QGridLayout>
#include <QElapsedTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QListView>
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

    // Previously the PQ screen hid every owned address and the payment-request action.
    auto* addresses = page.findChild<QListView*>("listViewAddress");
    QVERIFY(addresses && addresses->model());
    QCOMPARE(addresses->model()->rowCount(), 1);
    QVERIFY(!page.findChild<QWidget*>("btnRequest")->isHidden());
    const QString savedAddress = QString::fromStdString(wallet.GetPQAddresses().front());
    QVERIFY(addressLabel->toolTip().contains(savedAddress));

    RequestDialog request(&mainWindow);
    request.setWalletModel(&model);
    request.setReceiveAddress(savedAddress);
    request.findChild<QLineEdit*>("lineEditAmount")->setText("1.25");
    request.findChild<QLineEdit*>("lineEditDescription")->setText("Test payment");
    request.findChild<QPushButton*>("btnSave")->click();
    QCOMPARE(request.findChild<QLabel*>("labelAddress")->text(), savedAddress);
    QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
    QCOMPARE(QDir(backupPath).entryList({"*.dat"}, QDir::Files).size(), 1);

    // Switching away without destroying the old model closes its open request.
    QTimer::singleShot(0, &page, [&] {
        QVERIFY(page.activeRequestDialog);
        page.setWalletModel(nullptr);
    });
    page.onRequestClicked();
    QVERIFY(!page.isShowingDialog);
    QCOMPARE(addresses->model()->rowCount(), 0);
    QVERIFY(!addressLabel->text().contains(savedAddress));
    QSettings().remove(PQWalletUI::backupSettingsKey(&model));
}

void PQWidgetTests::chartPeriodsExistBeforeFirstReward()
{
#ifdef USE_QTCHARTS
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("test"));
    OrganicLifeGUI mainWindow(networkStyle.get());
    DashboardWidget page(&mainWindow);
    auto* months = page.findChild<QComboBox*>("comboBoxMonths");
    auto* years = page.findChild<QComboBox*>("comboBoxYears");
    QVERIFY(months && years);
    QCOMPARE(months->count(), 12);
    QCOMPARE(years->currentText(), QString::number(QDate::currentDate().year()));
    QVERIFY(!years->isHidden());
    QVERIFY(months->isHidden());
    page.findChild<QPushButton*>("pushButtonMonth")->click();
    QVERIFY(!months->isHidden());
    page.findChild<QPushButton*>("pushButtonAll")->click();
    QVERIFY(years->parentWidget()->isHidden());

    std::vector<ChartStakeSample> samples(100000, {2026, 9, 6, COIN, TransactionRecord::StakeMint});
    QElapsedTimer timer;
    timer.start();
    const auto rewards = AggregateChartRewards(samples, ChartBucketMode::Month);
    qInfo() << "100,000 chart samples aggregated in" << timer.nsecsElapsed() / 1000 << "us";
    QVERIFY(!rewards.amountsBy.empty());
#endif
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
        tip.nTime = QDateTime::currentSecsSinceEpoch();
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
        TxDetailDialog detail(&window, false);
        detail.setData(&model, model.getTransactionTableModel()->index(0, 0));
        detail.onOutputsClicked();
        auto* outputContainer = detail.findChild<QWidget*>("container_outputs_base");
        auto* outputGrid = qobject_cast<QGridLayout*>(outputContainer->layout());
        QVERIFY(outputGrid && outputGrid->itemAtPosition(0, 1));
        auto* outputAddress = qobject_cast<QLabel*>(outputGrid->itemAtPosition(0, 0)->widget());
        QVERIFY(outputAddress->text().startsWith("olcpqtest"));
        QCOMPARE(wallet.GetStakingBalance(), CAmount(5 * COIN));
        wallet.LockCoin(coin);
        QCOMPARE(wallet.GetStakingBalance(), CAmount(0));
        wallet.UnlockCoin(coin);
        auto* dashboardBalance = dashboard->findChild<QLabel*>("headerAvailableBalance");
        QVERIFY(dashboardBalance);
        QCOMPARE(dashboardBalance->text(), GUIUtil::formatBalance(5 * COIN, options.getDisplayUnit()));
        SendWidget send(&window);
        send.setWalletModel(&model);
        send.hide();
        const QString previewDirectory = qEnvironmentVariable("OLC_WALLET_PREVIEW_DIR");
        struct RestoreTheme {
            QVariant theme{QSettings().value("theme")};
            ~RestoreTheme() {
                if (theme.isValid()) QSettings().setValue("theme", theme);
                else QSettings().remove("theme");
            }
        } restoreTheme;
        if (!previewDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(previewDirectory));
            window.resize(1280, 800);
            for (auto* sendPage : window.findChildren<SendWidget*>()) sendPage->setWalletModel(&model);
            auto* receive = window.findChild<ReceiveWidget*>();
            receive->setWalletModel(&model);
            window.show();
            for (const QString& theme : {QStringLiteral("default"), QStringLiteral("default-dark")}) {
                QSettings().setValue("theme", theme);
                QString css = GUIUtil::loadStyleSheet();
                window.setStyleSheet(css);
                Q_EMIT window.themeChanged(theme == "default", css);
                const auto capture = [&](const QString& name) {
                    QCoreApplication::processEvents();
                    return window.grab().save(previewDirectory + "/" + theme + "-" + name + ".png");
                };
                window.goToSend();
                QVERIFY(capture("send"));
                window.goToReceive();
                QVERIFY(capture("receive"));
                window.goToSettings();
                QVERIFY(capture("settings"));
                window.goToDashboard();
                QVERIFY(capture("dashboard"));
                DefaultDialog dialog(&window);
                dialog.setText(tr("Confirm payment"), tr("Review the recipient, amount and fee before sending."), tr("Send payment"), tr("Cancel"));
                dialog.show();
                QCoreApplication::processEvents();
                QVERIFY(dialog.grab().save(previewDirectory + "/" + theme + "-dialog.png"));
                dialog.hide();
                AskPassphraseDialog unlock(AskPassphraseDialog::Mode::UnlockAnonymize, &window, &model, AskPassphraseDialog::Context::Unlock_Menu);
                unlock.show();
                QCoreApplication::processEvents();
                QVERIFY(unlock.grab().save(previewDirectory + "/" + theme + "-unlock.png"));
            }
            window.hide();
        }
        auto* entry = send.entries.front();
        entry->setAddress(externalAddress);
        entry->setAmount("1.00000000");
        auto* subtractFee = entry->findChild<QCheckBox*>("checkboxSubtractFeeFromAmount");
        QVERIFY(subtractFee);
        subtractFee->setChecked(true);
        QVERIFY(entry->getValue().fSubtractFee);
        subtractFee->setChecked(false);
        QVERIFY(entry->validate());
        QVERIFY(send.findChild<QWidget*>("pushButtonAddRecipient")->isHidden());
        QVERIFY(send.findChild<QWidget*>("pushLeft")->isHidden());
        QVERIFY(send.findChild<QWidget*>("pushRight")->isHidden());

        SendCoinsRecipient recipient;
        recipient.address = externalAddress;
        recipient.amount = COIN;

        std::map<WalletModel::ListCoinsKey, std::vector<WalletModel::ListCoinsValue>> available;
        model.listCoins(available);
        QVERIFY(!available.empty());
        QVERIFY(!send.findChild<QWidget*>("btnCoinControl")->isHidden());
        QVERIFY(!send.findChild<QWidget*>("btnChangeAddress")->isHidden());
        QVERIFY(!send.findChild<QWidget*>("pushButtonFee")->isHidden());
        CCoinControl selection;
        selection.Select(COutPoint(uint256S("deadbeef"), 0), 5 * COIN);
        WalletModelTransaction unavailableSelection({recipient});
        QVERIFY(model.prepareTransaction(&unavailableSelection, &selection).status != WalletModel::OK);
        QVERIFY(!unavailableSelection.getTransaction());
        QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));

        selection.UnSelectAll();
        selection.Select(coin, 5 * COIN);
        send.beginPQPreparation();
        WalletModelTransaction selectedPayment({recipient});
        QCOMPARE(model.prepareTransaction(&selectedPayment, &selection).status, WalletModel::OK);
        QCOMPARE(selectedPayment.getTransaction()->vin.size(), size_t(1));
        QVERIFY(selectedPayment.getTransaction()->vin.front().prevout == coin);
        QVERIFY(send.cleanupNewPQKeys());

        selection.destPQChange = ownedAddress;
        selection.fOverrideFeeRate = true;
        selection.nFeeRate = CFeeRate(2 * CENT);
        CoinControlDialog control(&window);
        control.setModel(&model);
        *control.coinControl = selection;
        control.addPayAmount(COIN, false);
        const auto totals = control.getTotals();
        QCOMPARE(totals.nPayFee, selection.nFeeRate.GetFee(totals.nBytes));
        QCOMPARE(totals.nChange, CAmount(4 * COIN - totals.nPayFee));
        control.clearPayAmounts();
        control.addPayAmount(COIN, false, true);
        QCOMPARE(control.getTotals().nChange, CAmount(4 * COIN));
        if (!previewDirectory.isEmpty()) {
            control.refreshDialog();
            control.show();
            QCoreApplication::processEvents();
            QVERIFY(control.grab().save(previewDirectory + "/coin-control.png"));
            control.hide();
        }
        WalletModelTransaction customChange({recipient});
        QCOMPARE(model.prepareTransaction(&customChange, &selection).status, WalletModel::OK);
        QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
        QVERIFY(customChange.getTransaction()->vout.back().scriptPubKey == pq::GetScript(ownedId));
        QVERIFY(customChange.getTransactionFee() >= selection.nFeeRate.GetFee(customChange.getTransactionSize()));
        selection.destPQChange = "not-an-address";
        WalletModelTransaction invalidChange({recipient});
        QVERIFY(model.prepareTransaction(&invalidChange, &selection).status != WalletModel::OK);
        selection.destPQChange.clear();
        wallet.LockCoin(coin);
        WalletModelTransaction lockedSelection({recipient});
        QVERIFY(model.prepareTransaction(&lockedSelection, &selection).status != WalletModel::OK);
        wallet.UnlockCoin(coin);

        SendCoinsRecipient subtractRecipient = recipient;
        subtractRecipient.amount = 5 * COIN;
        subtractRecipient.fSubtractFee = true;
        WalletModelTransaction spendAll({subtractRecipient});
        QCOMPARE(model.prepareTransaction(&spendAll, &selection).status, WalletModel::OK);
        QCOMPARE(spendAll.getTransaction()->vout.size(), size_t(1));
        QCOMPARE(spendAll.getTransaction()->vout.front().nValue + spendAll.getTransactionFee(), CAmount(5 * COIN));

        CMutableTransaction extraFunding = incoming;
        extraFunding.vin.front().prevout.hash = uint256S("9876");
        extraFunding.vout.front().nValue = COIN;
        const auto extraTx = MakeTransactionRef(extraFunding);
        const COutPoint extraCoin(extraTx->GetHash(), 0);
        {
            LOCK2(cs_main, wallet.cs_wallet);
            QVERIFY(wallet.AddToWalletIfInvolvingMe(extraTx,
                {CWalletTx::Status::CONFIRMED, 1, tipHash, 2}, true));
            pcoinsTip->AddCoin(extraCoin, Coin(extraTx->vout.front(), 1, false, false), false);
        }
        selection.Select(extraCoin, COIN);
        selection.destPQChange = ownedAddress;
        WalletModelTransaction twoSelected({recipient});
        QCOMPARE(model.prepareTransaction(&twoSelected, &selection).status, WalletModel::OK);
        QCOMPARE(twoSelected.getTransaction()->vin.size(), size_t(2));
        selection.UnSelect(coin);
        SendCoinsRecipient largerRecipient = recipient;
        largerRecipient.amount = 2 * COIN;
        WalletModelTransaction insufficientSelected({largerRecipient});
        QVERIFY(model.prepareTransaction(&insufficientSelected, &selection).status != WalletModel::OK);
        selection.fAllowOtherInputs = true;
        WalletModelTransaction supplemented({largerRecipient});
        QCOMPARE(model.prepareTransaction(&supplemented, &selection).status, WalletModel::OK);
        QCOMPARE(supplemented.getTransaction()->vin.size(), size_t(2));
        QVERIFY(supplemented.getTransaction()->vin.front().prevout == extraCoin);
        selection.Select(coin, 5 * COIN);
        selection.Select(COutPoint(uint256S("abcde"), 0), COIN);
        WalletModelTransaction tooManyInputs({recipient});
        QVERIFY(model.prepareTransaction(&tooManyInputs, &selection).status != WalletModel::OK);

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

#ifdef USE_QTCHARTS
        // Feed a real PQ stake through the wallet transaction model, then remove
        // it from visible history: the chart must update without reopening it.
        CMutableTransaction stake;
        stake.nType = CTransaction::PQ;
        stake.vin.emplace_back(coin);
        stake.vout.emplace_back(0, CScript());
        stake.vout.emplace_back(6 * COIN, pq::GetScript(ownedId));
        const auto stakeTx = MakeTransactionRef(stake);
        {
            LOCK2(cs_main, wallet.cs_wallet);
            QVERIFY(wallet.AddToWalletIfInvolvingMe(stakeTx,
                {CWalletTx::Status::CONFIRMED, 1, tipHash, 1}, true));
        }
        const auto stakeHash = QString::fromStdString(stakeTx->GetHash().ToString());
        model.getTransactionTableModel()->updateTransaction(stakeHash, CT_NEW, true);
        QTRY_VERIFY(dashboard->chartData && dashboard->chartData->totalPiv == COIN);
        QVERIFY(dashboard->chartHasRenderedData);
        window.goToDashboard();
        window.show();
        QCoreApplication::processEvents();
        QTRY_VERIFY(dashboard->chartTimeline->width() >= dashboard->chartView->viewport()->width() - 20);
        if (!previewDirectory.isEmpty()) {
            QVERIFY(window.grab().save(previewDirectory + "/dashboard-reward.png"));
        }
        window.hide();
        model.getTransactionTableModel()->hideTransaction(stakeHash);
        QTRY_VERIFY(dashboard->chartData && dashboard->chartData->totalPiv == 0);
        dashboard->setWalletModel(nullptr);
        QVERIFY(dashboard->chartStakeRowsSnapshot.empty());
#endif
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
