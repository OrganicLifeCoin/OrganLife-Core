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
#include "topbar.h"
#include "lockunlock.h"
#include "transactionrecord.h"
#include "transactiontablemodel.h"
#include "walletmodeltransaction.h"
#include "masternodeswidget.h"
#include "masternodewizarddialog.h"
#include "mninfodialog.h"
#include "mnmodel.h"

#include <chainparams.h>
#include <coincontrol.h>
#include <crypto/mldsa44.h>
#include <guiinterface.h>
#include <guiutil.h>
#include <interfaces/handler.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <netbase.h>
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
#include <QDialogButtonBox>
#include <QLabel>
#include <QGridLayout>
#include <QElapsedTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QFileDialog>
#include <QListView>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSemaphore>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QtTest/qtest_widgets.h>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <thread>
#include <sstream>
#ifdef WIN32
#include <aclapi.h>
#endif

void PQWidgetTests::masternodeControllerNavigation_data()
{
    QTest::addColumn<QString>("network");
    QTest::newRow("mainnet") << QString::fromStdString(CBaseChainParams::MAIN);
    QTest::newRow("regtest") << QString::fromStdString(CBaseChainParams::REGTEST);
    QTest::newRow("testnet") << QString::fromStdString(CBaseChainParams::TESTNET);
}

void PQWidgetTests::masternodeControllerNavigation()
{
    QFETCH(QString, network);
    const auto oldNetwork = Params().NetworkIDString();
    struct Restore { std::string network; ~Restore() { SelectParams(network); } } restore{oldNetwork};
    SelectParams(network.toStdString());
    auto style = std::unique_ptr<const NetworkStyle>(NetworkStyle::instantiate(network));
    OrganicLifeGUI window(style.get());
    auto* page = window.findChild<MasterNodesWidget*>();
    QVERIFY(page);
    auto* button = window.findChild<QToolButton*>("btnMaster");
    QVERIFY(button);
    QVERIFY(!button->isHidden());
    button->click();
    QCOMPARE(window.findChild<QStackedWidget*>()->currentWidget(), page);
    QVERIFY(page->findChild<QPushButton*>("pushButtonStartAll")->isHidden());
    QVERIFY(!page->findChild<QPushButton*>("pushButtonStartMissing")->isHidden());
    QVERIFY(!page->findChild<QPushButton*>("pushButtonStartMissing")->isEnabled());
    QVERIFY(!page->findChild<QPushButton*>("pushButtonSave")->isEnabled());
    QCOMPARE(page->findChild<QPushButton*>("pushButtonSave")->text(), QString("Create Masternode"));
    QVERIFY(!page->findChild<QWidget*>("btnCoinControl")->isHidden());
    page->clearWalletModel();
    QVERIFY(!page->findChild<QPushButton*>("pushButtonSave")->isEnabled());
}

void PQWidgetTests::masternodeRegistryFailsClosed()
{
    const auto oldNetwork = Params().NetworkIDString();
    struct Restore { std::string network; ~Restore() { masternodeConfig.clear(); SelectParams(network); } } restore{oldNetwork};
    masternodeConfig.add("obsolete", "127.0.0.1:51476", "legacy-secret", "aa", "0");
    for (const auto& network : {CBaseChainParams::MAIN, CBaseChainParams::REGTEST, CBaseChainParams::TESTNET}) {
        SelectParams(network);
        MNModel model(nullptr);
        model.updateMNList();
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(!model.registryError().isEmpty());
        QVERIFY(!model.index(0, MNModel::PRIV_KEY, {}).isValid());
    }
}

void PQWidgetTests::masternodeServerConfiguration()
{
    const auto oldNetwork = Params().NetworkIDString();
    struct Restore { std::string network; ~Restore() { SelectParams(network); } } restore{oldNetwork};
    const auto id = uint256S("aabbcc");
    for (const auto& network : {CBaseChainParams::MAIN, CBaseChainParams::REGTEST, CBaseChainParams::TESTNET}) {
        SelectParams(network);
        MNModel model(nullptr);
        MasterNodeWizardDialog wizard(nullptr, &model);
        wizard.findChild<QLineEdit*>("lineEditIpAddress")->setText("8.8.8.8");
        wizard.findChild<QLineEdit*>("lineEditPort")->clear();
        QCOMPARE(wizard.service(), "8.8.8.8:" + QString::number(Params().GetDefaultPort()));
        const auto endpoints = network == CBaseChainParams::MAIN ?
            std::vector<const char*> {"8.8.8.8:43736", "[2001:4860::8888]:43746"} :
            std::vector<const char*> {"8.8.8.8:49736", "[2001:4860::8888]:49746"};
        for (const auto& endpoint : endpoints) {
            const auto config = PQWalletUI::masternodeConfig(id, LookupNumeric(endpoint), "aabb");
            QCOMPARE(config, QString("pqoperatorid=%1\npqoperatorconfig=aabb\nexternalip=%2\n")
                .arg(QString::fromStdString(id.GetHex()), endpoint));
            struct ConfigArgs : ArgsManager { using ArgsManager::ReadConfigStream; } parsed;
            // Parser-only placeholders: existing checkpoint text must remain untouched.
            const auto existing = network == CBaseChainParams::MAIN ?
                QString("disablewallet=1\nstaking=0\nserver=1\nlisten=1\n"
                        "datadir=/existing/node\npqbootstrap=existing-checkpoint\nport=%1\n"
                        "rpcport=50123\nrpcbind=127.0.0.1\nrpcallowip=127.0.0.1\nrpcpassword=existing-password\n")
                    .arg(LookupNumeric(endpoint).GetPort()) :
                QString("%1=1\ndisablewallet=1\nstaking=0\nserver=1\nlisten=1\n"
                        "datadir=/existing/node\npqbootstrap=existing-checkpoint\n[%2]\nport=%3\n"
                        "rpcport=50123\nrpcbind=127.0.0.1\nrpcallowip=127.0.0.1\nrpcpassword=existing-password\n")
                    .arg(network == CBaseChainParams::TESTNET ? "testnet" : "regtest",
                         QString::fromStdString(network)).arg(LookupNumeric(endpoint).GetPort());
            std::istringstream stream((config + existing).toStdString());
            parsed.ReadConfigStream(stream);
            parsed.SelectConfigNetwork(network);
            QCOMPARE(parsed.GetChainName(), network);
            QVERIFY(parsed.GetBoolArg("-disablewallet", false));
            QVERIFY(!parsed.GetBoolArg("-staking", true));
            QVERIFY(parsed.GetBoolArg("-server", false));
            QVERIFY(parsed.GetBoolArg("-listen", false));
            QCOMPARE(parsed.GetArg("-datadir", ""), std::string("/existing/node"));
            QCOMPARE(parsed.GetArg("-pqbootstrap", ""), std::string("existing-checkpoint"));
            QCOMPARE(parsed.GetArg("-port", 0), int64_t(LookupNumeric(endpoint).GetPort()));
            QCOMPARE(parsed.GetArg("-rpcport", 0), int64_t(50123));
            QCOMPARE(parsed.GetArg("-rpcbind", ""), std::string("127.0.0.1"));
            QCOMPARE(parsed.GetArg("-rpcallowip", ""), std::string("127.0.0.1"));
            QCOMPARE(parsed.GetArg("-rpcpassword", ""), std::string("existing-password"));
            QCOMPARE(parsed.GetArg("-externalip", ""), std::string(endpoint));
            QCOMPARE(parsed.GetArg("-pqoperatorid", ""), id.GetHex());
            QCOMPARE(parsed.GetArg("-pqoperatorconfig", ""), std::string("aabb"));
        }
        QVERIFY(PQWalletUI::masternodeConfig(uint256(), LookupNumeric(endpoints[0]), "aabb").isEmpty());
        QVERIFY(PQWalletUI::masternodeConfig(id, CService(), "aabb").isEmpty());
        QVERIFY(PQWalletUI::masternodeConfig(id, LookupNumeric("8.8.8.8:0"), "aabb").isEmpty());
        QVERIFY(PQWalletUI::masternodeConfig(id, LookupNumeric("8.8.8.8:49736"), {}).isEmpty());
    }
    SelectParams(CBaseChainParams::MAIN);
    const auto mainConfig = PQWalletUI::masternodeConfig(id, LookupNumeric("8.8.8.8:43736"), "aabb");
    QCOMPARE(mainConfig, QString("pqoperatorid=%1\npqoperatorconfig=aabb\nexternalip=8.8.8.8:43736\n")
        .arg(QString::fromStdString(id.GetHex())));
    QVERIFY(!mainConfig.contains("testnet"));
    QVERIFY(!mainConfig.contains("regtest"));
}

void PQWidgetTests::masternodeControllerCancelsWithoutWrites_data()
{
    QTest::addColumn<QString>("network");
    QTest::newRow("regtest") << QString::fromStdString(CBaseChainParams::REGTEST);
    QTest::newRow("testnet") << QString::fromStdString(CBaseChainParams::TESTNET);
}

void PQWidgetTests::masternodeControllerCancelsWithoutWrites()
{
    QFETCH(QString, network);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
#ifdef WIN32
    // QTemporaryDir grants other principals metadata access. Operator exports
    // deliberately require a stricter owner-only parent, even in this fixture.
    auto directoryPath = directory.path().toStdWString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    const DWORD readSecurity = GetNamedSecurityInfoW(directoryPath.data(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor);
    std::unique_ptr<void, decltype(&LocalFree)> ownedDescriptor(descriptor, LocalFree);
    QCOMPARE(readSecurity, DWORD(ERROR_SUCCESS));
    QVERIFY(owner && IsValidSid(owner));
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.ptstrName = static_cast<wchar_t*>(owner);
    PACL acl = nullptr;
    const DWORD createACL = SetEntriesInAclW(1, &access, nullptr, &acl);
    std::unique_ptr<void, decltype(&LocalFree)> ownedACL(acl, LocalFree);
    QCOMPARE(createACL, DWORD(ERROR_SUCCESS));
    QCOMPARE(SetNamedSecurityInfoW(directoryPath.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr), DWORD(ERROR_SUCCESS));
#endif
    const auto oldNetwork = Params().NetworkIDString();
    const auto oldDataDir = gArgs.GetArg("-datadir", "");
    struct Restore {
        std::string network, datadir;
        ~Restore() { evoDb.reset(); gArgs.ForceSetArg("-datadir", datadir); ClearDatadirCache(); SelectParams(network); ECC_Stop(); }
    } restore{oldNetwork, oldDataDir};
    QVERIFY(!evoDb);
    ECC_Start();
    InitSignatureCache();
    ECCVerifyHandle verify;
    SelectParams(network.toStdString());
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 1);
    QVERIFY(!chainActive.Tip());
    CBlockIndex genesis;
    const uint256 genesisHash = Params().GetConsensus().hashGenesisBlock;
    genesis.phashBlock = &genesisHash;
    chainActive.SetTip(&genesis);
    CCoinsView view;
    struct ClearChain {
        CBlockIndex* oldHeader{pindexBestHeader};
        ~ClearChain() { mempool.clear(); pcoinsTip.reset(); mapBlockIndex.clear(); chainActive.SetTip(nullptr); pindexBestHeader = oldHeader; }
    } clearChain;
    gArgs.ForceSetArg("-datadir", directory.path().toStdString());
    ClearDatadirCache();
    evoDb = std::make_unique<CEvoDB>(1 << 20, true);
    CWallet wallet("pq-mn-ui", WalletDatabase::Create(fs::path(directory.path().toStdString()) / "wallet"));
    bool firstRun;
    QCOMPARE(wallet.LoadWallet(firstRun), DB_LOAD_OK);
    CKey key; key.MakeNewKey(true);
    QVERIFY(wallet.AddKeyPubKey(key, key.GetPubKey()));
    QVERIFY(wallet.EncryptWallet(SecureString("test-only")));
    QVERIFY(wallet.Unlock(SecureString("test-only")));
    for (int i = 0; i < 3; ++i) { std::string address; QVERIFY(wallet.GeneratePQAddress(address)); }
    mldsa44::PublicKey operatorKey{};
    std::string reason;
    QVERIFY(wallet.GetPQOperators().empty()); // The creation wizard must prepare its own identity.
    OptionsModel options;
    WalletModel model(&wallet, &options);
    model.init();
    CWallet replacementWallet("pq-mn-replacement", WalletDatabase::Create(fs::path(directory.path().toStdString()) / "replacement"));
    QCOMPARE(replacementWallet.LoadWallet(firstRun), DB_LOAD_OK);
    WalletModel replacementModel(&replacementWallet, &options);
    replacementModel.init();
    auto style = std::unique_ptr<const NetworkStyle>(NetworkStyle::instantiate(network));
    OrganicLifeGUI window(style.get());
    auto* page = window.findChild<MasterNodesWidget*>();
    QVERIFY(page);
    page->setWalletModel(&model);
    auto* mnCoinControl = page->findChild<CoinControlDialog*>();
    QVERIFY(mnCoinControl);
    QVERIFY(mnCoinControl->hasModel());
    QVERIFY2(page->findChild<MNModel*>()->registryError().isEmpty(), qPrintable(page->findChild<MNModel*>()->registryError()));
    QVERIFY(page->findChild<QPushButton*>("pushButtonSave")->isEnabled());
    const auto before = QDir(directory.path()).entryList();
    bool sawDialog = false;
    QTimer::singleShot(0, page, [&] {
        auto* dialog = page->findChild<MasterNodeWizardDialog*>();
        sawDialog = dialog && dialog->isVisible();
        const auto preview = qEnvironmentVariable("OLC_MN_PREVIEW_DIR");
        if (sawDialog && !preview.isEmpty()) QVERIFY(dialog->grab().save(preview + "/register.png"));
        if (dialog) dialog->reject();
        else if (auto* old = page->findChild<QDialog*>("pqMasternodeDialog")) old->reject();
    });
    QVERIFY(QMetaObject::invokeMethod(page, "onCreateMNClicked", Qt::DirectConnection));
    QVERIFY(sawDialog);
    QVERIFY(wallet.mapWallet.empty());
    QCOMPARE(QDir(directory.path()).entryList(), before);
    // A wallet switch during a modal form must cancel it, not submit with the replacement wallet.
    mnCoinControl->coinControl->Select(COutPoint(uint256S("ee"), 0));
    QTimer::singleShot(0, page, [&] { page->clearWalletModel(); });
    QVERIFY(QMetaObject::invokeMethod(page, "onCreateMNClicked", Qt::DirectConnection));
    QVERIFY(wallet.mapWallet.empty());
    QCOMPARE(QDir(directory.path()).entryList(), before);
    QVERIFY(!mnCoinControl->hasModel());
    QVERIFY(!mnCoinControl->coinControl->HasSelected());
    page->setWalletModel(&model);

    // Fund the controller with a confirmed PQ output, as in the standard send fixture.
    pq::KeyID fundingId;
    QVERIFY(pq::DecodeAddress(wallet.GetPQAddresses().front(), Params().NetworkIDString(), fundingId));
    CMutableTransaction incoming;
    incoming.nType = CTransaction::PQ;
    incoming.vin.emplace_back(uint256S("dd"), 0);
    incoming.vout.emplace_back(Params().GetConsensus().nMNCollateralAmt + 10 * COIN, pq::GetScript(fundingId));
    const auto funding = MakeTransactionRef(incoming);
    {
        LOCK2(cs_main, wallet.cs_wallet);
        mapBlockIndex.emplace(genesisHash, &genesis);
        pindexBestHeader = &genesis;
        pcoinsTip = std::make_unique<CCoinsViewCache>(&view);
        pcoinsTip->SetBestBlock(genesisHash);
        wallet.SetLastBlockProcessed(&genesis);
        QVERIFY(wallet.AddToWalletIfInvolvingMe(funding, {CWalletTx::Status::CONFIRMED, 0, genesisHash, 0}, true));
        pcoinsTip->AddCoin(COutPoint(funding->GetHash(), 0), Coin(funding->vout[0], 0, false, false), false);
    }
    const auto backupDirectory = directory.path() + "/backups";
    QVERIFY(QDir().mkdir(backupDirectory));
    QSettings().setValue(PQWalletUI::backupSettingsKey(&model), backupDirectory);
    QCOMPARE(PQWalletUI::backupDirectory(&model), backupDirectory);
    QString reviewMessage, errorMessage;
    bool acceptTransaction = false;
    WalletModel* replaceOnConfirmation = nullptr;
    disconnect(page, &MasterNodesWidget::message, &window, &OrganicLifeGUI::message);
    connect(page, &MasterNodesWidget::message, page, [&](const QString&, const QString& text, unsigned int, bool* accepted) {
        if (accepted) {
            reviewMessage = text;
            if (replaceOnConfirmation) page->setWalletModel(replaceOnConfirmation);
            *accepted = acceptTransaction;
        }
        else errorMessage = text;
    });
    const auto review = [&](const QString& ip = QString(), const QString& port = QString("49736")) {
        QTimer::singleShot(0, page, [&] {
            auto* dialog = page->findChild<MasterNodeWizardDialog*>();
            QVERIFY(dialog);
            auto* next = dialog->findChild<QPushButton*>("btnNext");
            next->click();
            dialog->findChild<QLineEdit*>("lineEditName")->setText("Test-masternode");
            next->click();
            dialog->findChild<QLineEdit*>("lineEditIpAddress")->setText(ip.isEmpty() ? (network == "test" ? "2001:4860::8888" : "8.8.8.8") : ip);
            dialog->findChild<QLineEdit*>("lineEditPort")->setText(port);
            QVERIFY(dialog->findChildren<QComboBox*>().empty()); // No address or operator selection.
            const auto preview = qEnvironmentVariable("OLC_MN_PREVIEW_DIR");
            if (!preview.isEmpty()) QVERIFY(dialog->grab().save(preview + "/create-service.png"));
            next->click();
        });
        return QMetaObject::invokeMethod(page, "onCreateMNClicked", Qt::DirectConnection);
    };
    for (const auto& endpoint : {std::make_pair(QString("example.com"), QString("49736")),
                                std::make_pair(QString("8.8.8.8"), QString("0")),
                                std::make_pair(QString("2001:4860::8888"), QString("65536"))}) {
        QVERIFY(review(endpoint.first, endpoint.second));
        QVERIFY(reviewMessage.isEmpty());
        QVERIFY2(errorMessage.contains("numeric IP"), qPrintable(errorMessage));
        QVERIFY(wallet.GetPQOperators().empty());
        QCOMPARE(QDir(backupDirectory).entryList({"*.dat"}, QDir::Files).size(), 0);
    }
    mnCoinControl->coinControl->Select(COutPoint(uint256S("ee"), 0));
    QVERIFY(review());
    QVERIFY(reviewMessage.isEmpty());
    QVERIFY2(errorMessage.contains("selected coin"), qPrintable(errorMessage));
    QCOMPARE(QDir(backupDirectory).entryList({"*.dat"}, QDir::Files).size(), 1); // Operator recovery only, no transaction.
    mnCoinControl->coinControl->SetNull();
    mnCoinControl->coinControl->Select(COutPoint(funding->GetHash(), 0));
    errorMessage.clear();
    QVERIFY(review());
    QVERIFY2(!reviewMessage.isEmpty(), qPrintable(errorMessage));
    QVERIFY(reviewMessage.contains("Fee:"));
    QVERIFY(reviewMessage.contains("Test-masternode"));
    QVERIFY(reviewMessage.contains("49736"));
    QCOMPARE(wallet.mapWallet.size(), size_t(1)); // Rejected confirmation never persists or relays.
    QCOMPARE(mempool.size(), size_t(0));
    QCOMPARE(QDir(backupDirectory).entryList({"*.dat"}, QDir::Files).size(), 3);
    reviewMessage.clear(); errorMessage.clear();
    QSettings().setValue(PQWalletUI::backupSettingsKey(&model), directory.path() + "/missing/backup");
    QVERIFY(review());
    QVERIFY(reviewMessage.isEmpty());
    QVERIFY2(errorMessage.contains("backup", Qt::CaseInsensitive), qPrintable(errorMessage));
    QCOMPARE(wallet.mapWallet.size(), size_t(1));
    QCOMPARE(mempool.size(), size_t(0));
    QSettings().setValue(PQWalletUI::backupSettingsKey(&model), backupDirectory);
    acceptTransaction = true;
    errorMessage.clear();
    CScheduler scheduler;
    std::thread schedulerThread([&] { scheduler.serviceQueue(); });
    GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);
    const bool submitted = review();
    scheduler.stop();
    schedulerThread.join();
    GetMainSignals().FlushBackgroundCallbacks();
    GetMainSignals().UnregisterBackgroundSignalScheduler();
    QVERIFY(submitted);
    QVERIFY2(errorMessage.contains("Transaction submitted"), qPrintable(errorMessage));
    QVERIFY(!mnCoinControl->coinControl->HasSelected());
    QCOMPARE(wallet.mapWallet.size(), size_t(2));
    QCOMPARE(mempool.size(), size_t(1));
    QCOMPARE(QDir(backupDirectory).entryList({"*.dat"}, QDir::Files).size(), 5);
    CTransactionRef registration;
    for (const auto& entry : wallet.mapWallet) if (entry.first != funding->GetHash()) registration = entry.second.tx;
    QVERIFY(registration);
    const auto nameKey = "pqMasternodeNames/" + QString::fromStdString(genesisHash.GetHex()) + "/" +
                         QString::fromStdString(registration->GetHash().GetHex());
    QCOMPARE(QSettings().value(nameKey).toString(), QString("Test-masternode"));
    QSettings().remove(nameKey);
    QCOMPARE(registration->vout.at(0).nValue, Params().GetConsensus().nMNCollateralAmt);
    pqmn::Payload payload;
    pq::Payload envelope;
    QVERIFY(pq::DecodePayload(*registration, envelope));
    QVERIFY(pqmn::Decode(envelope.data, payload));
    QVERIFY(payload.operatorKey != operatorKey);
    operatorKey = payload.operatorKey;
    QVERIFY(payload.owner != payload.collateralKey);
    QCOMPARE(payload.service.GetPort(), uint16_t(49736));
    const auto backupFile = reviewMessage.section("Encrypted backup: ", 1).section('\n', 0, 0);
    {
        // Berkeley DB must close the source before opening its backup copy
        // (the copies intentionally have the same database file ID).
        wallet.GetDBHandle().Flush(false);
        struct BackupWallet : CWallet {
            using CWallet::CWallet;
            ~BackupWallet() { GetDBHandle().Flush(true); }
        } restored("mn-backup", WalletDatabase::Create(fs::path(backupFile.toStdString())));
        QCOMPARE(restored.LoadWallet(firstRun), DB_LOAD_OK);
        QVERIFY(restored.IsCrypted());
        QVERIFY(restored.Unlock(SecureString("test-only")));
        const auto recoveredOperators = restored.GetPQOperators();
        QVERIFY(std::find(recoveredOperators.begin(), recoveredOperators.end(), operatorKey) != recoveredOperators.end());
        for (const auto& id : {*pq::GetID(payload.owner, Params().NetworkIDString()),
                               *pq::GetID(payload.collateralKey, Params().NetworkIDString()), payload.payout}) {
            mldsa44::Key recovered;
            QVERIFY(restored.GetPQKey(id, recovered, false));
        }
    }
    const auto operatorCount = wallet.GetPQOperators().size();
    bool operatorDialogSeen = false;
    QTimer::singleShot(0, page, [&] {
        auto* dialog = page->findChild<QDialog*>("pqOperatorsDialog");
        operatorDialogSeen = dialog && dialog->isVisible();
        if (!dialog) return;
        struct CloseDialog { QDialog* dialog; ~CloseDialog() { dialog->reject(); } } close{dialog};
        auto* create = dialog->findChild<QPushButton*>("createPQOperator");
        auto* exportButton = dialog->findChild<QPushButton*>("exportPQOperator");
        auto* path = dialog->findChild<QLineEdit*>("pqOperatorExportPath");
        QVERIFY(create && exportButton && path);
        const auto count = wallet.GetPQOperators().size();
        QSettings().setValue(PQWalletUI::backupSettingsKey(&model), directory.path() + "/missing/backup");
        create->click();
        QCOMPARE(wallet.GetPQOperators().size(), count);
        QSettings().setValue(PQWalletUI::backupSettingsKey(&model), backupDirectory);
        create->click();
        QCOMPARE(wallet.GetPQOperators().size(), count + 1); // Retry publishes the pending key, not two identities.
        const auto preview = qEnvironmentVariable("OLC_MN_PREVIEW_DIR");
        if (!preview.isEmpty()) QVERIFY(dialog->grab().save(preview + "/operators.png"));
        path->setText(directory.path() + "/operator-credentials");
        exportButton->click();
        const QDir credentials(path->text());
        QVERIFY2(credentials.exists("olc-pq-operator-record"), qPrintable(errorMessage));
        QVERIFY(credentials.exists("olc-pq-operator-key"));
        QCOMPARE(credentials.entryList(QDir::Files).size(), 2);
        mldsa44::Key exported;
        std::string loadError;
        QVERIFY2(pqwallet::LoadOperatorCredentials(path->text().toStdString(), Params().NetworkIDString(), genesisHash, exported, loadError), loadError.c_str());
        const auto backed = wallet.GetPQOperators();
        QVERIFY(std::find(backed.begin(), backed.end(), exported.GetPublicKey()) != backed.end());
        QVERIFY(exported.GetPublicKey() != operatorKey);
        const auto fingerprint = [&](const QString& name) {
            QFile file(credentials.filePath(name));
            if (!file.open(QIODevice::ReadOnly)) return QByteArray();
            return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
        };
        const auto recordHash = fingerprint("olc-pq-operator-record");
        const auto keyHash = fingerprint("olc-pq-operator-key");
        const auto files = credentials.entryList(QDir::Files);
        exportButton->click(); // Exclusive writer must not overwrite an existing pair.
        QVERIFY2(errorMessage.contains("Could not publish"), qPrintable(errorMessage));
        QCOMPARE(credentials.entryList(QDir::Files), files);
        QCOMPARE(fingerprint("olc-pq-operator-record"), recordHash);
        QCOMPARE(fingerprint("olc-pq-operator-key"), keyHash);
        path->setText(directory.path() + "/cancelled-export");
        replaceOnConfirmation = &replacementModel;
        exportButton->click();
        replaceOnConfirmation = nullptr;
        QVERIFY(!QDir(path->text()).exists());
        QVERIFY(replacementWallet.GetPQOperators().empty());
        dialog->reject();
    });
    QVERIFY(QMetaObject::invokeMethod(page, "onPQOperatorsClicked", Qt::DirectConnection));
    QVERIFY(operatorDialogSeen);
    page->setWalletModel(&model);
    QCOMPARE(wallet.GetPQOperators().size(), operatorCount + 1);
    QCOMPARE(wallet.mapWallet.size(), size_t(2)); // Provisioning never creates a spend.
    QTimer::singleShot(0, page, [&] { page->clearWalletModel(); });
    QVERIFY(QMetaObject::invokeMethod(page, "onPQOperatorsClicked", Qt::DirectConnection));
    QVERIFY(!page->findChild<QPushButton*>("pushButtonStartMissing")->isEnabled());
    QCOMPARE(wallet.GetPQOperators().size(), operatorCount + 1);
    page->setWalletModel(&model);
    QSettings().remove(PQWalletUI::backupSettingsKey(&model));

    // Read-only persisted registry snapshots exercise rendering and fail-closed refresh;
    // consensus validity of records is covered by the registry core/functional suites.
    auto* registryModel = page->findChild<MNModel*>();
    pqmn::Record record;
    record.operatorKey = operatorKey;
    record.collateral = COutPoint(uint256S("aa"), 2);
    record.collateralHeight = 10;
    const auto recordKey = std::make_pair(std::make_pair(std::string("pqmn1r"), genesisHash), uint256S("bb"));
    evoDb->Write(recordKey, record);
    registryModel->updateMNList();
    QCOMPARE(registryModel->rowCount(), 1);
    QVERIFY(registryModel->index(0, MNModel::ALIAS, {}).data().toString().size() < 40);
    QCOMPARE(registryModel->index(0, MNModel::ALIAS, {}).data(Qt::ToolTipRole).toString(), QString::fromStdString(uint256S("bb").GetHex()));
    QCOMPARE(registryModel->index(0, MNModel::STATUS, {}).data().toString(), QString("IMMATURE"));
    const auto preview = qEnvironmentVariable("OLC_MN_PREVIEW_DIR");
    if (!preview.isEmpty()) {
        window.resize(1280, 820);
        window.show();
        window.findChild<QToolButton*>("btnMaster")->click();
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(preview + "/masternodes.png"));
    }
    QVERIFY(!registryModel->index(0, MNModel::PRIV_KEY, {}).data().isValid());
    record.service = LookupNumeric("[2001:4860::8888]:49736");
    evoDb->Write(recordKey, record);
    registryModel->updateMNList();
    QVERIFY(QMetaObject::invokeMethod(page, "onMNClicked", Qt::DirectConnection,
                                     Q_ARG(QModelIndex, registryModel->index(0, MNModel::ALIAS, {}))));
    bool infoSeen = false;
    QTimer::singleShot(0, page, [&] {
        auto* dialog = page->findChild<MnInfoDialog*>();
        infoSeen = dialog && dialog->isVisible();
        if (dialog) {
            QVERIFY(dialog->findChild<QPushButton*>("pushExport"));
            if (!preview.isEmpty()) QVERIFY(dialog->grab().save(preview + "/info.png"));
            dialog->reject();
        } else if (auto* old = page->findChild<QDialog*>()) old->reject();
    });
    QVERIFY(QMetaObject::invokeMethod(page, "onInfoMNClicked", Qt::DirectConnection));
    QVERIFY(infoSeen);
    bool closedOnUnload = false;
    QTimer::singleShot(0, page, [&] {
        auto* dialog = page->findChild<MnInfoDialog*>();
        if (!dialog) return;
        page->clearWalletModel();
        closedOnUnload = !dialog->isVisible();
        dialog->reject();
    });
    QVERIFY(QMetaObject::invokeMethod(page, "onInfoMNClicked", Qt::DirectConnection));
    QVERIFY(closedOnUnload);
    page->setWalletModel(&model);
    QVERIFY(QMetaObject::invokeMethod(page, "onMNClicked", Qt::DirectConnection,
                                     Q_ARG(QModelIndex, registryModel->index(0, MNModel::ALIAS, {}))));
    const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    struct RestoreDialogs { bool disabled; ~RestoreDialogs() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, disabled); } } restoreDialogs{nativeDialogsDisabled};
    bool copiedFromInfo = false;
    bool folderPickerSeen = false;
    QTimer exportTimer;
    connect(&exportTimer, &QTimer::timeout, page, [&] {
        if (auto* dialog = page->findChild<MnInfoDialog*>(); dialog && dialog->isVisible()) {
            dialog->findChild<QPushButton*>("pushExport")->click();
            copiedFromInfo = true;
        }
        if (auto* chooser = page->findChild<QFileDialog*>(); chooser && chooser->isVisible()) {
            folderPickerSeen = true;
            chooser->reject();
        }
    });
    exportTimer.start(10);
    QTimer::singleShot(5000, &exportTimer, [&] { // Bound a broken dialog test, rather than hanging the suite.
        exportTimer.stop();
        for (auto* dialog : page->findChildren<QDialog*>()) dialog->reject();
    });
    QApplication::clipboard()->setText("unchanged");
    QVERIFY(QMetaObject::invokeMethod(page, "onInfoMNClicked", Qt::DirectConnection));
    exportTimer.stop();
    QVERIFY(copiedFromInfo);
    QVERIFY(!folderPickerSeen);
    const auto exports = QDir(directory.path()).entryList({"olc-masternode-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(exports.size(), 0);
    const auto copiedConfig = QApplication::clipboard()->text();
    QCOMPARE(copiedConfig.count('\n'), 3);
    QVERIFY(copiedConfig.startsWith("pqoperatorid="));
    QVERIFY(copiedConfig.contains("\npqoperatorconfig="));
    QVERIFY(copiedConfig.contains("\nexternalip="));
    QVERIFY(!copiedConfig.contains("pqoperatorcredentials="));
    QVERIFY(!copiedConfig.contains("\ndatadir="));
    struct ConfigArgs : ArgsManager { using ArgsManager::ReadConfigStream; } copiedArgs;
    std::istringstream configStream(copiedConfig.toStdString());
    copiedArgs.ReadConfigStream(configStream);
    copiedArgs.SelectConfigNetwork(Params().NetworkIDString());
    mldsa44::Key infoKey;
    QVERIFY2(pqwallet::DecodeOperatorConfig(copiedArgs.GetArg("-pqoperatorconfig", ""),
        Params().NetworkIDString(), genesisHash, infoKey, reason), reason.c_str());
    QVERIFY(infoKey.GetPublicKey() == operatorKey);
    QCOMPARE(copiedArgs.GetArg("-pqoperatorid", ""), uint256S("bb").GetHex());
    QCOMPARE(copiedArgs.GetArg("-externalip", ""), record.service.ToString());
    QCOMPARE(wallet.mapWallet.size(), size_t(2)); // Export neither spends nor copies controller keys.
    mldsa44::Key spendingKey;
    QVERIFY(!wallet.GetPQKey(*pq::GetID(operatorKey, Params().NetworkIDString()), spendingKey, false));
    // Declining the secret-export warning must leave the clipboard unchanged.
    acceptTransaction = false;
    QApplication::clipboard()->setText("keep-existing-clipboard");
    exportTimer.start(10);
    QVERIFY(QMetaObject::invokeMethod(page, "onInfoMNClicked", Qt::DirectConnection));
    exportTimer.stop();
    QCOMPARE(QApplication::clipboard()->text(), QString("keep-existing-clipboard"));
    QPersistentModelIndex selected = registryModel->index(0, MNModel::ALIAS, {});
    record.revoked = true;
    evoDb->Write(recordKey, record);
    registryModel->updateMNList();
    QVERIFY(!selected.isValid());
    QCOMPARE(registryModel->index(0, MNModel::STATUS, {}).data().toString(), QString("REVOKED"));
    evoDb->Write(std::make_pair(std::string("pqmn1b"), genesisHash), uint256S("cc"));
    registryModel->updateMNList();
    QCOMPARE(registryModel->rowCount(), 0);
    QVERIFY(!registryModel->registryError().isEmpty());
    QVERIFY(!page->findChild<QPushButton*>("pushButtonSave")->isEnabled());
    page->clearWalletModel();
}

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

void PQWidgetTests::mainnetPQWalletFlowAndBackup()
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
    SelectParams(CBaseChainParams::MAIN);
    gArgs.ForceSetArg("-datadir", directory.path().toStdString());
    ClearDatadirCache();

    CWallet wallet("pq-mainnet-ui", WalletDatabase::Create(fs::path(directory.path().toStdString()) / "wallet"));
    bool firstRun;
    QCOMPARE(wallet.LoadWallet(firstRun), DB_LOAD_OK);
    CKey key;
    key.MakeNewKey(true);
    QVERIFY(wallet.AddKeyPubKey(key, key.GetPubKey()));
    QVERIFY(wallet.EncryptWallet(SecureString("pq-mainnet-passphrase")));
    QVERIFY(wallet.Unlock(SecureString("pq-mainnet-passphrase")));
    OptionsModel options;
    WalletModel model(&wallet, &options);
    model.init();

    const QString backupPath = directory.path() + "/backups";
    QVERIFY(QDir().mkdir(backupPath));
    QSettings().setValue(PQWalletUI::backupSettingsKey(&model), backupPath);

    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle);
    OrganicLifeGUI mainWindow(networkStyle.get());
    ReceiveWidget page(&mainWindow);
    page.setWalletModel(&model);
    page.onNewAddressClicked();

    auto* addressLabel = page.findChild<QLabel*>("labelAddress");
    QVERIFY(addressLabel);
    QVERIFY(addressLabel->text().startsWith("olcpq1"));
    QCOMPARE(wallet.GetPQAddresses().size(), size_t(1));
    QCOMPARE(QDir(backupPath).entryList({"*.dat"}, QDir::Files).size(), 1);
    QVERIFY(!page.findChild<QWidget*>("btnRequest")->isHidden());
    QSettings().remove(PQWalletUI::backupSettingsKey(&model));
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

void PQWidgetTests::balancePollingDoesNotInvertChainLock()
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

    CWallet wallet("pq-balance-lock",
        WalletDatabase::Create(fs::path(directory.path().toStdString()) / "wallet"));
    bool firstRun;
    QCOMPARE(wallet.LoadWallet(firstRun), DB_LOAD_OK);
    OptionsModel options;
    WalletModel model(&wallet, &options);
    model.init();
    model.setfForceCheckBalanceChanged(true);

    QSemaphore completed;
    std::thread poller;
    bool result = true;
    bool completedWhileChainLocked = false;
    bool walletAvailable = false;
    {
        LOCK(cs_main);
        poller = std::thread([&] {
            result = model.processBalanceChangeInternal();
            completed.release();
        });
        completedWhileChainLocked = completed.tryAcquire(1, 1000);
        TRY_LOCK(wallet.cs_wallet, lockWallet);
        walletAvailable = bool(lockWallet);
    }
    // Release cs_main before joining or asserting, so a regression fails rather than hangs.
    poller.join();
    QVERIFY(completedWhileChainLocked);
    QVERIFY(walletAvailable);
    QVERIFY(!result);
    QVERIFY(model.hasForceCheckBalance());
    QVERIFY(model.processBalanceChangeInternal());
    QVERIFY(!model.hasForceCheckBalance());
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
        auto* topBar = window.findChild<TopBar*>();
        QVERIFY(topBar);
        topBar->setWalletModel(&model);
        QToolButton* lockButton = nullptr;
        for (auto* button : window.findChildren<QToolButton*>()) {
            if (button->text() == tr("Lock wallet")) lockButton = button;
        }
        QVERIFY(lockButton);
        window.resize(1280, 800);
        window.show();
        QCoreApplication::processEvents();
        QTest::mouseClick(lockButton, Qt::LeftButton);
        QCoreApplication::processEvents();
        auto* lockMenu = window.findChild<LockUnlock*>();
        QVERIFY(lockMenu && lockMenu->isVisible());
        const QPoint anchor = lockButton->mapToGlobal(QPoint(lockButton->width(), lockButton->height()));
        QVERIFY(lockMenu->mapToGlobal(QPoint()).x() >= anchor.x());
        QVERIFY(lockMenu->mapToGlobal(QPoint()).y() > window.mapToGlobal(QPoint()).y() + 200);
        QVERIFY(lockMenu->mapToGlobal(QPoint(0, lockMenu->height())).y() <= anchor.y());
        QVERIFY(lockMenu->findChild<QPushButton*>("pushButtonUnlocked")->isChecked());
        QTest::keyClick(lockMenu, Qt::Key_Escape);
        QVERIFY(!lockMenu->isVisible());
        QTest::mouseClick(lockButton, Qt::LeftButton);
        QCoreApplication::processEvents();
        QVERIFY(lockMenu->isVisible());
        topBar->setWalletModel(nullptr);
        QVERIFY(!lockMenu->isVisible());
        QVERIFY(!wallet.IsLocked());
        window.hide();
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
                topBar->setWalletModel(&model);
                QTest::mouseClick(lockButton, Qt::LeftButton);
                QCoreApplication::processEvents();
                QVERIFY(lockMenu->grab().save(previewDirectory + "/" + theme + "-lock-menu.png"));
                QTest::keyClick(lockMenu, Qt::Key_Escape);
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
