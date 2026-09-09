// Copyright (c) 2019-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeswidget.h"
#include "ui_masternodeswidget.h"

#include "addresstablemodel.h"
#include "coincontrol.h"
#include "masternodewizarddialog.h"
#include "mninfodialog.h"
#include "mnrow.h"
#include "qtutils.h"

#include "clientmodel.h"
#include "guiutil.h"
#include "mnmodel.h"
#include "optionbutton.h"
#include "qt/walletmodel.h"
#include "pqwalletui.h"
#include <chainparams.h>
#include <net.h>
#include <netbase.h>
#include <support/cleanse.h>
#include <utilstrencodings.h>
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QDir>
#include <QFileDialog>
#include <QSettings>
#include <QUuid>

#define DECORATION_SIZE 65
#define NUM_ITEMS 3
#define REQUEST_START_ALL 1
#define REQUEST_START_MISSING 2

class MNHolder : public FurListRow<QWidget*>
{
public:
    explicit MNHolder(bool _isLightTheme) : FurListRow(), isLightTheme(_isLightTheme) {}

    MNRow* createHolder(int pos) override
    {
        if (!cachedRow) cachedRow = new MNRow();
        return cachedRow;
    }

    void init(QWidget* holder,const QModelIndex &index, bool isHovered, bool isSelected) const override
    {
        MNRow* row = static_cast<MNRow*>(holder);
        QString label = index.data(Qt::DisplayRole).toString();
        QString address = index.sibling(index.row(), MNModel::ADDRESS).data(Qt::DisplayRole).toString();
        QString status = index.sibling(index.row(), MNModel::STATUS).data(Qt::DisplayRole).toString();
        bool wasCollateralAccepted = index.sibling(index.row(), MNModel::WAS_COLLATERAL_ACCEPTED).data(Qt::DisplayRole).toBool();
        row->updateView("Address: " + address, label, status, wasCollateralAccepted);
    }

    QColor rectColor(bool isHovered, bool isSelected) override
    {
        return getRowColor(isLightTheme, isHovered, isSelected);
    }

    ~MNHolder() override{}

    bool isLightTheme;
    MNRow* cachedRow = nullptr;
};

MasterNodesWidget::MasterNodesWidget(OrganicLifeGUI *parent) :
    PWidget(parent),
    ui(new Ui::MasterNodesWidget),
    isLoading(false)
{
    ui->setupUi(this);

    delegate = new FurAbstractListItemDelegate(
            DECORATION_SIZE,
            new MNHolder(isLightTheme()),
            this
    );

    this->setStyleSheet(parent->styleSheet());

    /* Containers */
    setCssProperty(ui->left, "screen-main-surface");
    ui->left->setContentsMargins(0,0,0,20);
    ui->right->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->right, "screen-side-rail");
    ui->right->setContentsMargins(0,10,0,0);
    ui->container_right->setContentsMargins(0, 0, 0, 0);
    ui->container_right->setSpacing(6);
    ui->verticalLayout_4->removeItem(ui->verticalSpacer_8);
    delete ui->verticalSpacer_8;
    ui->verticalSpacer_8 = nullptr;
    ui->verticalLayout_4->setSpacing(6);
    ui->containerHeader->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->containerHeader, "screen-header-band");
    ui->listMn->setProperty("designRole", QStringLiteral("content-card"));
    ui->emptyContainer->setProperty("designRole", QStringLiteral("content-card"));
    ui->emptyContainer->setAttribute(Qt::WA_StyledBackground, true);

    /* Light Font */
    QFont fontLight;
    fontLight.setWeight(QFont::Light);

    /* Title */
    setCssProperty(ui->labelTitle, "screen-header-title");
    ui->labelTitle->setFont(fontLight);
    setCssProperty(ui->labelSubtitle1, "screen-header-subtitle");
    ui->labelSubtitle1->setWordWrap(true);

    /* Buttons */
    setCssBtnPrimary(ui->pushButtonSave);
    setCssBtnPrimary(ui->pushButtonStartAll);
    setCssBtnPrimary(ui->pushButtonStartMissing);

    /* Coin control */
    this->coinControlDialog = new CoinControlDialog(this);

    /* Options */
    setCssProperty(ui->btnAbout, "screen-side-option", true);
    ui->btnAbout->setTitleClassAndText("btn-title-grey", tr("What is a Masternode?"));
    ui->btnAbout->setSubTitleClassAndText("text-subtitle", tr("FAQ explaining what Masternodes are"));
    setCssProperty(ui->btnAboutController, "screen-side-option", true);
    ui->btnAboutController->setTitleClassAndText("btn-title-grey", tr("What is a Controller?"));
    ui->btnAboutController->setSubTitleClassAndText("text-subtitle", tr("FAQ explaining what is a Masternode Controller"));
    setCssProperty(ui->btnCoinControl, "screen-side-option", true);
    ui->btnCoinControl->setTitleClassAndText("btn-title-grey", "Coin Control");
    ui->btnCoinControl->setSubTitleClassAndText("text-subtitle", "Select the source of coins to create a Masternode");

    setCssProperty(ui->listMn, "container");
    ui->listMn->setItemDelegate(delegate);
    ui->listMn->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listMn->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listMn->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listMn->setSelectionBehavior(QAbstractItemView::SelectRows);

    ui->emptyContainer->setVisible(false);
    setCssProperty(ui->pushImgEmpty, "img-empty-master");
    setCssProperty(ui->labelEmpty, "text-empty");

    connect(ui->pushButtonSave, &QPushButton::clicked, this, &MasterNodesWidget::onCreateMNClicked);
    connect(ui->pushButtonStartAll, &QPushButton::clicked, [this]() {
        onStartAllClicked(REQUEST_START_ALL);
    });
    connect(ui->pushButtonStartMissing, &QPushButton::clicked, [this]() {
        if (Params().IsTestChain()) onPQOperatorsClicked();
        else onStartAllClicked(REQUEST_START_MISSING);
    });
    connect(ui->listMn, &QListView::clicked, this, &MasterNodesWidget::onMNClicked);
    connect(ui->btnAbout, &OptionButton::clicked, [this](){window->openFAQ(SettingsFaqWidget::Section::MASTERNODE);});
    connect(ui->btnAboutController, &OptionButton::clicked, [this](){window->openFAQ(SettingsFaqWidget::Section::MNCONTROLLER);});
    connect(ui->btnCoinControl, &OptionButton::clicked, this, &MasterNodesWidget::onCoinControlClicked);
    if (Params().IsTestChain()) {
        setMNModel(new MNModel(this));
        ui->pushButtonSave->setText(tr("Create Masternode"));
        ui->pushButtonSave->setEnabled(false);
        ui->pushButtonStartMissing->setText(tr("Operator keys"));
        ui->pushButtonStartMissing->setEnabled(false);
        ui->btnCoinControl->setEnabled(false);
        ui->btnCoinControl->setSubTitleClassAndText("text-subtitle", tr("Select up to two fee/funding inputs. Existing collateral cannot fund masternode fees."));
        ui->labelEmpty->setText(tr("No confirmed PQ registrations"));
        updateListState();
    }
}

void MasterNodesWidget::loadWalletModel()
{
    if (Params().IsTestChain() && mnModel) {
        coinControlDialog->setModel(walletModel);
        mnModel->updateMNList();
        updateListState();
    }
}

void MasterNodesWidget::onPQOperatorsClicked()
{
    QPointer<WalletModel> controller = walletModel;
    if (!Params().IsTestChain() || !controller) return;
    QDialog dialog(this);
    dialog.setObjectName("pqOperatorsDialog");
    dialog.setWindowTitle(tr("Operator keys"));
    auto* form = new QFormLayout(&dialog);
    form->setSpacing(14);
    auto* explanation = new QLabel(tr("Recovery keys stay in this encrypted controller wallet. Creating an identity first verifies a wallet backup. Export produces operator-only credentials, never wallet spending keys. Both exported files are secret: transfer them securely and seal them on the operator host."), &dialog);
    explanation->setWordWrap(true);
    form->addRow(explanation);
    QComboBox operators;
    QLineEdit destination;
    destination.setObjectName("pqOperatorExportPath");
    destination.setPlaceholderText(tr("New credential directory inside a private folder"));
    auto* create = new QPushButton(tr("Create backed operator"), &dialog);
    auto* exportButton = new QPushButton(tr("Export selected operator"), &dialog);
    auto* browse = new QPushButton(tr("Choose private folder…"), &dialog);
    create->setObjectName("createPQOperator");
    exportButton->setObjectName("exportPQOperator");
    setCssBtnPrimary(create);
    setCssBtnPrimary(exportButton);
    form->addRow(tr("Operator fingerprint"), &operators);
    form->addRow(create);
    form->addRow(tr("New credential directory"), &destination);
    form->addRow(browse);
    form->addRow(exportButton);
    const auto refresh = [&] {
        operators.clear();
        if (!controller || controller != walletModel) return;
        for (const auto& key : controller->getWallet()->GetPQOperators()) {
            const auto id = pq::GetID(key, Params().NetworkIDString());
            if (id) operators.addItem(QString::fromStdString(HexStr(*id)), QString::fromStdString(HexStr(key)));
        }
        exportButton->setEnabled(operators.count() > 0);
    };
    refresh();
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto parent = QFileDialog::getExistingDirectory(&dialog, tr("Choose a private folder for operator credentials"));
        if (!parent.isEmpty() && controller && controller == walletModel)
            destination.setText(QDir(parent).filePath("olc-pq-operator-" + QUuid::createUuid().toString(QUuid::WithoutBraces)));
    });
    connect(create, &QPushButton::clicked, &dialog, [&] {
        if (!controller || controller != walletModel) return;
        WalletModel::UnlockContext unlock(controller->requestUnlock());
        if (!unlock.isValid() || !controller || controller != walletModel) return;
        QString directory;
        if (!PQWalletUI::ensureBackupDirectory(&dialog, controller, directory) || !controller || controller != walletModel) return;
        const auto backup = QDir(directory).filePath("olc-pq-operator-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".dat");
        mldsa44::PublicKey key;
        std::string reason;
        if (!controller->getWallet()->PreparePQOperator(backup.toStdString(), key, reason)) {
            warn(tr("Operator keys"), QString::fromStdString(reason)); return;
        }
        refresh();
        operators.setCurrentIndex(operators.findData(QString::fromStdString(HexStr(key))));
        inform(tr("Operator created. Encrypted recovery backup: %1").arg(backup));
    });
    connect(exportButton, &QPushButton::clicked, &dialog, [&] {
        if (!controller || controller != walletModel) return;
        const auto path = destination.text().trimmed();
        const auto bytes = ParseHex(operators.currentData().toString().toStdString());
        mldsa44::PublicKey key;
        if (path.isEmpty() || !QDir::isAbsolutePath(path) || bytes.size() != key.size()) {
            warn(tr("Operator keys"), tr("Select an operator and an absolute path for a new private credential directory.")); return;
        }
        std::copy(bytes.begin(), bytes.end(), key.begin());
        if (!ask(tr("Export operator credentials"), tr("Export operator %1 to %2?\nAnyone with both files can impersonate this operator, but cannot spend controller funds. This does not configure or start a remote node.")
                .arg(operators.currentText(), path)) || !controller || controller != walletModel) return;
        WalletModel::UnlockContext unlock(controller->requestUnlock());
        if (!unlock.isValid() || !controller || controller != walletModel) return;
        std::string reason;
        if (!controller->getWallet()->ExportPQOperator(key, path.toStdString(), reason)) {
            warn(tr("Operator keys"), QString::fromStdString(reason)); return;
        }
        inform(tr("Operator-only credentials written to %1. Transfer and protect both files; remote deployment remains separate.").arg(path));
    });
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(close);
    dialog.resize(740, dialog.sizeHint().height());
    dialog.exec();
}

void MasterNodesWidget::pqOperation(int action)
{
    QPointer<WalletModel> controller = walletModel;
    if (!controller || !mnModel || !mnModel->registryError().isEmpty()) return;
    const CCoinControl selectedCoins = *coinControlDialog->coinControl;
    pqmn::Payload op;
    op.action = static_cast<pqmn::Action>(action);
    {
        const auto entry = mnModel->pqRecord(index);
        if (!entry || entry->second.sequence == UINT64_MAX) return;
        op.registration = entry->first;
        op.sequence = entry->second.sequence + 1;
        op.operatorKey = entry->second.operatorKey;
        op.payout = entry->second.payout;
        op.operatorPayout = entry->second.operatorPayout;
        op.service = entry->second.service;
    }
    QDialog dialog(this);
    dialog.setObjectName("pqMasternodeDialog");
    dialog.setWindowTitle(tr("Manage masternode"));
    auto* form = new QFormLayout(&dialog);
    form->setSpacing(14);
    auto* explanation = new QLabel(
        tr("Registration: %1\nNext sequence: %2").arg(QString::fromStdString(op.registration.GetHex())).arg(QString::number(op.sequence)), &dialog);
    explanation->setWordWrap(true);
    form->addRow(explanation);
    QComboBox payout, operators, operation;
    QLineEdit service, operatorPayout;
    for (const auto& address : controller->getWallet()->GetPQAddresses()) {
        const auto text = QString::fromStdString(address);
        payout.addItem(text);
    }
    for (const auto& key : controller->getWallet()->GetPQOperators()) {
        const auto hex = QString::fromStdString(HexStr(key));
        operators.addItem(hex.left(20) + "…", hex);
    }
    payout.setEditable(true);
    if (op.action == pqmn::Action::SERVICE) {
        operation.addItem(tr("Update service endpoint"), static_cast<int>(pqmn::Action::SERVICE));
        operation.addItem(tr("Update payout / rotate operator"), static_cast<int>(pqmn::Action::UPDATE));
        form->addRow(tr("Action"), &operation);
        service.setText(op.service == CService() ? "" : QString::fromStdString(op.service.ToString()));
        operatorPayout.setText(op.operatorPayout == pq::KeyID{} ? "" : QString::fromStdString(pq::EncodeAddress(op.operatorPayout, Params().NetworkIDString())));
        payout.setCurrentText(QString::fromStdString(pq::EncodeAddress(op.payout, Params().NetworkIDString())));
        const auto currentKey = QString::fromStdString(HexStr(op.operatorKey));
        if (operators.findData(currentKey) < 0) operators.addItem(currentKey.left(20) + "…", currentKey);
        operators.setCurrentIndex(operators.findData(currentKey));
        form->addRow(tr("Service IP:port"), &service);
        form->addRow(tr("Operator payout"), &operatorPayout);
        form->addRow(tr("Payout address"), &payout);
        form->addRow(tr("Backed replacement / current operator"), &operators);
        const auto updateFields = [&] {
            const bool update = operation.currentData().toInt() == static_cast<int>(pqmn::Action::UPDATE);
            payout.setEnabled(update); operators.setEnabled(update);
            service.setEnabled(!update); operatorPayout.setEnabled(!update);
        };
        connect(&operation, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, updateFields);
        updateFields();
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Review transaction"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(680, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted || !controller || controller != walletModel) return;
    if (op.action == pqmn::Action::SERVICE) op.action = static_cast<pqmn::Action>(operation.currentData().toInt());
    WalletModel::UnlockContext unlock(controller->requestUnlock());
    if (!unlock.isValid() || !controller || controller != walletModel) return;
    auto* wallet = controller->getWallet();
    const auto address = [&](const QString& text, pq::KeyID& id) {
        return pq::DecodeAddress(text.trimmed().toStdString(), Params().NetworkIDString(), id);
    };
    if (op.action == pqmn::Action::UPDATE) {
        const auto bytes = ParseHex(operators.currentData().toString().toStdString());
        if (bytes.size() != op.operatorKey.size() || !address(payout.currentText(), op.payout)) {
            warn(tr("Masternode"), tr("Choose a backed operator and a valid payout address.")); return;
        }
        std::copy(bytes.begin(), bytes.end(), op.operatorKey.begin());
    }
    if (op.action == pqmn::Action::SERVICE) {
        op.service = LookupNumeric(service.text().trimmed().toStdString());
        if (!op.service.IsValid() || !op.service.GetPort()) { warn(tr("Masternode"), tr("Enter a numeric IP:port.")); return; }
    }
    if (op.action == pqmn::Action::SERVICE && !operatorPayout.text().isEmpty() && !address(operatorPayout.text(), op.operatorPayout)) {
        warn(tr("Masternode"), tr("Invalid operator payout address.")); return;
    }
    QString directory;
    if (!PQWalletUI::ensureBackupDirectory(this, controller, directory) || !controller || controller != walletModel) return;
    const auto backup = QDir(directory).filePath("olc-pq-masternode-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".dat");
    CTransactionRef tx;
    CAmount fee;
    std::string reason;
    if (!wallet->PreparePQMasternodeTransaction(op, backup.toStdString(), tx, fee, reason, &selectedCoins)) {
        warn(tr("Masternode"), QString::fromStdString(reason)); return;
    }
    const auto actionName = op.action == pqmn::Action::REVOKE ? tr("Revoke") :
        op.action == pqmn::Action::SERVICE ? tr("Update service") : tr("Update payout / operator");
    auto summary = tr("Action: %1\nRegistration: %2\nFee: %3\nEncrypted backup: %4")
        .arg(actionName).arg(QString::fromStdString(op.registration.GetHex()))
        .arg(GUIUtil::formatBalance(fee, BitcoinUnits::PIV))
        .arg(backup);
    if (op.action == pqmn::Action::UPDATE) {
        const auto id = pq::GetID(op.operatorKey, Params().NetworkIDString());
        summary += tr("\nPayout: %1\nOperator fingerprint: %2").arg(payout.currentText(), id ? QString::fromStdString(HexStr(*id)) : "");
    }
    if (op.action == pqmn::Action::SERVICE)
        summary += tr("\nService: %1").arg(service.text());
    if (op.action == pqmn::Action::SERVICE) summary += tr("\nOperator payout: %1").arg(operatorPayout.text());
    if (op.action == pqmn::Action::UPDATE) summary += tr("\nChanging the operator clears its service endpoint and payout.");
    if (op.action == pqmn::Action::REVOKE) summary += tr("\nRevocation disables this operator; it does not unlock collateral.");
    if (!ask(tr("Confirm masternode transaction"), summary) || !controller || controller != walletModel) return;
    const auto committed = wallet->CommitTransaction(tx, nullptr, g_connman.get());
    if (committed.status != CWallet::CommitStatus::OK) { warn(tr("Masternode"), QString::fromStdString(committed.ToString())); return; }
    resetCoinControl();
    inform(tr("Transaction submitted: %1. Wait for confirmation before another change.").arg(QString::fromStdString(committed.hashTx.GetHex())));
}

void MasterNodesWidget::createPQMasternode()
{
    QPointer<WalletModel> controller = walletModel;
    if (!controller || !mnModel || !mnModel->registryError().isEmpty()) return;
    const CCoinControl selectedCoins = *coinControlDialog->coinControl;
    MasterNodeWizardDialog wizard(controller, mnModel, this);
    if (wizard.exec() != QDialog::Accepted || !controller || controller != walletModel) return;
    if (!wizard.isOk) { warn(tr("Error creating masternode"), wizard.returnStr); return; }
    WalletModel::UnlockContext unlock(controller->requestUnlock());
    if (!unlock.isValid() || !controller || controller != walletModel) return;
    QString directory;
    if (!PQWalletUI::ensureBackupDirectory(this, controller, directory) || !controller || controller != walletModel) return;
    auto* wallet = controller->getWallet();
    pqmn::Payload op;
    op.action = pqmn::Action::REGISTER;
    op.collateral = COutPoint(uint256(), 0);
    op.service = LookupNumeric(wizard.service().toStdString());
    const auto backupBase = QDir(directory).filePath("olc-pq-masternode-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    std::string reason;
    if (!wallet->PreparePQOperator((backupBase + "-wallet-before-registration.dat").toStdString(), op.operatorKey, reason)) {
        warn(tr("Error creating masternode"), QString::fromStdString(reason)); return;
    }
    std::string ownerAddress, collateralAddress, payoutAddress;
    mldsa44::Key owner, collateral;
    if (!wallet->GeneratePQAddress(ownerAddress) || !wallet->GeneratePQAddress(collateralAddress) ||
        !wallet->GeneratePQAddress(payoutAddress) || !wallet->GetPQKey(ownerAddress, owner) ||
        !wallet->GetPQKey(collateralAddress, collateral) || !pq::DecodeAddress(payoutAddress, Params().NetworkIDString(), op.payout)) {
        warn(tr("Error creating masternode"), tr("Could not prepare wallet keys. No transaction was sent.")); return;
    }
    op.owner = owner.GetPublicKey(); op.collateralKey = collateral.GetPublicKey();
    CTransactionRef tx;
    CAmount fee;
    const auto backup = backupBase + ".dat";
    if (!wallet->PreparePQMasternodeTransaction(op, backup.toStdString(), tx, fee, reason, &selectedCoins)) {
        warn(tr("Error creating masternode"), QString::fromStdString(reason)); return;
    }
    const auto summary = tr("Name: %1\nService: %2\nCollateral: %3\nFee: %4\nEncrypted backup: %5\n\nThe collateral and rewards stay in this wallet. Create this masternode?")
        .arg(wizard.alias(), QString::fromStdString(op.service.ToString()),
             GUIUtil::formatBalance(Params().GetConsensus().nMNCollateralAmt, BitcoinUnits::PIV),
             GUIUtil::formatBalance(fee, BitcoinUnits::PIV), backup);
    if (!ask(tr("Create Masternode"), summary) || !controller || controller != walletModel) return;
    const auto committed = wallet->CommitTransaction(tx, nullptr, g_connman.get());
    if (committed.status != CWallet::CommitStatus::OK) { warn(tr("Masternode"), QString::fromStdString(committed.ToString())); return; }
    QSettings().setValue("pqMasternodeNames/" + QString::fromStdString(Params().GetConsensus().hashGenesisBlock.GetHex()) + "/" +
                         QString::fromStdString(committed.hashTx.GetHex()), wizard.alias());
    resetCoinControl();
    inform(tr("Transaction submitted: %1. After confirmation, open the masternode information to export its server configuration.")
        .arg(QString::fromStdString(committed.hashTx.GetHex())));
}

void MasterNodesWidget::clearWalletModel()
{
    PWidget::clearWalletModel();
    coinControlDialog->setModel(nullptr);
    resetCoinControl();
    if (auto* dialog = findChild<QDialog*>("pqMasternodeDialog")) dialog->reject();
    if (auto* dialog = findChild<MasterNodeWizardDialog*>()) dialog->reject();
    if (auto* dialog = findChild<MnInfoDialog*>()) dialog->reject();
    if (auto* dialog = findChild<QDialog*>("pqOperatorsDialog")) dialog->reject();
    if (menu) menu->hide();
    index = QPersistentModelIndex();
    if (Params().IsTestChain()) {
        ui->pushButtonSave->setEnabled(false);
        ui->btnCoinControl->setEnabled(false);
        ui->pushButtonStartMissing->setEnabled(false);
    }
}

void MasterNodesWidget::showEvent(QShowEvent *event)
{
    if (mnModel) mnModel->updateMNList();
    if (!timer) {
        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() { if (mnModel) mnModel->updateMNList(); });
    }
    timer->start(30000);
}

void MasterNodesWidget::hideEvent(QHideEvent *event)
{
    if (timer) timer->stop();
}

void MasterNodesWidget::setMNModel(MNModel* _mnModel)
{
    if (mnModel) {
        disconnect(mnModel, nullptr, this, nullptr);
    }

    mnModel = _mnModel;
    ui->listMn->setModel(mnModel);
    ui->listMn->setModelColumn(AddressTableModel::Label);

    if (mnModel) {
        connect(mnModel, &QAbstractItemModel::modelReset, this, &MasterNodesWidget::updateListState);
        connect(mnModel, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex&, int, int) {
            updateListState();
        });
        connect(mnModel, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex&, int, int) {
            updateListState();
        });
    }

    updateListState();
}

void MasterNodesWidget::updateListState()
{
    bool show = mnModel && mnModel->rowCount() > 0;
    ui->listMn->setVisible(show);
    ui->emptyContainer->setVisible(!show);
    ui->pushButtonStartAll->setVisible(show && !Params().IsTestChain());
    if (Params().IsTestChain()) {
        if (menu) menu->hide();
        const auto error = mnModel ? mnModel->registryError() : tr("Registry unavailable");
        ui->labelSubtitle1->setText(error.isEmpty() ?
            tr("Confirmed PQ registry · Registration alone does not establish service, reward or finality eligibility.") : error);
        ui->pushButtonSave->setEnabled(walletModel && error.isEmpty());
        ui->btnCoinControl->setEnabled(walletModel && error.isEmpty());
        ui->pushButtonStartMissing->setEnabled(walletModel && error.isEmpty());
    }
}

void MasterNodesWidget::onMNClicked(const QModelIndex& _index)
{
    ui->listMn->setCurrentIndex(_index);
    QRect rect = ui->listMn->visualRect(_index);
    QPoint pos = rect.topRight();
    pos.setX(pos.x() - (DECORATION_SIZE * 2));
    pos.setY(pos.y() + (DECORATION_SIZE * 1.5));
    if (!this->menu) {
        this->menu = new TooltipMenu(window, this);
        this->menu->setEditBtnText(Params().IsTestChain() ? tr("Update service") : tr("Start"));
        this->menu->setDeleteBtnText(Params().IsTestChain() ? tr("Revoke") : tr("Delete"));
        this->menu->setCopyBtnText(tr("Info"));
        connect(this->menu, &TooltipMenu::message, this, &MasterNodesWidget::message);
        connect(this->menu, &TooltipMenu::onEditClicked, this, &MasterNodesWidget::onEditMNClicked);
        connect(this->menu, &TooltipMenu::onDeleteClicked, this, &MasterNodesWidget::onDeleteMNClicked);
        connect(this->menu, &TooltipMenu::onCopyClicked, this, &MasterNodesWidget::onInfoMNClicked);
        this->menu->adjustSize();
    } else {
        this->menu->hide();
    }
    this->index = _index;
    menu->move(pos);
    menu->show();

    // Back to regular status
    ui->listMn->scrollTo(index);
    ui->listMn->clearSelection();
    ui->listMn->setFocus();
}

bool MasterNodesWidget::checkMNsNetwork()
{
    bool isTierTwoSync = mnModel->isMNsNetworkSynced();
    if (!isTierTwoSync) inform(tr("Please wait until the node is fully synced"));
    return isTierTwoSync;
}

void MasterNodesWidget::onEditMNClicked()
{
    if (Params().IsTestChain()) { pqOperation(static_cast<int>(pqmn::Action::SERVICE)); return; }
    if (walletModel) {
        if (!walletModel->isRegTestNetwork() && !checkMNsNetwork()) return;
        // Start MN
        QString strAlias = this->index.data(Qt::DisplayRole).toString();
        if (ask(tr("Start Masternode"), tr("Are you sure you want to start masternode %1?\n").arg(strAlias))) {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock());
            if (!ctx.isValid()) {
                // Unlock wallet was cancelled
                inform(tr("Cannot edit masternode, wallet locked"));
                return;
            }
            startAlias(strAlias);
        }
    }
}

void MasterNodesWidget::startAlias(const QString& strAlias)
{
    QString strStatusHtml;
    strStatusHtml += "Alias: " + strAlias + " ";

    int failed_amount = 0;
    int success_amount = 0;
    std::string alias = strAlias.toStdString();
    std::string strError;
    mnModel->startAllMNs(false, failed_amount, success_amount, &alias, &strError);
    if (failed_amount > 0) {
        strStatusHtml = tr("failed to start.\nError: %1").arg(QString::fromStdString(strError));
    } else if (success_amount > 0) {
        strStatusHtml = tr("successfully started");
    }
    // update UI and notify
    updateModelAndInform(strStatusHtml);
}

void MasterNodesWidget::updateModelAndInform(const QString& informText)
{
    mnModel->updateMNList();
    inform(informText);
}

void MasterNodesWidget::onStartAllClicked(int type)
{
    if (!Params().IsRegTestNet() && !checkMNsNetwork()) return;     // skip on RegNet: so we can test even if tier two not synced

    if (isLoading) {
        inform(tr("Background task is being executed, please wait"));
    } else {
        std::unique_ptr<WalletModel::UnlockContext> pctx = std::make_unique<WalletModel::UnlockContext>(walletModel->requestUnlock());
        if (!pctx->isValid()) {
            warn(tr("Start ALL masternodes failed"), tr("Wallet unlock cancelled"));
            return;
        }
        isLoading = true;
        if (!execute(type, std::move(pctx))) {
            isLoading = false;
            inform(tr("Cannot perform Masternodes start"));
        }
    }
}

bool MasterNodesWidget::startAll(QString& failText, bool onlyMissing)
{
    int amountOfMnFailed = 0;
    int amountOfMnStarted = 0;
    mnModel->startAllMNs(onlyMissing, amountOfMnFailed, amountOfMnStarted);
    if (amountOfMnFailed > 0) {
        failText = tr("%1 Masternodes failed to start, %2 started").arg(amountOfMnFailed).arg(amountOfMnStarted);
        return false;
    }
    return true;
}

void MasterNodesWidget::run(int type)
{
    bool isStartMissing = type == REQUEST_START_MISSING;
    if (type == REQUEST_START_ALL || isStartMissing) {
        QString failText;
        QString inform = startAll(failText, isStartMissing) ? tr("All Masternodes started!") : failText;
        QMetaObject::invokeMethod(this, "updateModelAndInform", Qt::QueuedConnection,
                                  Q_ARG(QString, inform));
    }

    isLoading = false;
}

void MasterNodesWidget::onError(QString error, int type)
{
    if (type == REQUEST_START_ALL) {
        QMetaObject::invokeMethod(this, "inform", Qt::QueuedConnection,
                                  Q_ARG(QString, "Error starting all Masternodes"));
    }
}

void MasterNodesWidget::onInfoMNClicked()
{
    if (Params().IsTestChain()) {
        QPointer<WalletModel> controller = walletModel;
        const auto entry = mnModel ? mnModel->pqRecord(index) : nullopt;
        if (!entry) return;
        MnInfoDialog dialog(this);
        const auto& r = entry->second;
        dialog.setData(QString::fromStdString(HexStr(r.operatorKey)), index.data().toString(),
                       QString::fromStdString(r.service.ToString()), QString::fromStdString(r.collateral.hash.GetHex()),
                       QString::number(r.collateral.n), index.sibling(index.row(), MNModel::STATUS).data().toString());
        dialog.exec();
        if (!dialog.exportMN || !controller || controller != walletModel) return;
        const auto current = mnModel->pqRecord(index);
        if (!current || current->first != entry->first || current->second.operatorKey != r.operatorKey) return;
        if (entry->first.IsNull() || !r.service.IsValid() || !r.service.GetPort()) {
            warn(tr("Masternode"), tr("Set a valid service address before copying.")); return;
        }
        if (!ask(tr("Copy Masternode Configuration"), tr("Copy only the registration ID, secret operator credential and IP/port?\n\nUse an existing VPS node on the same network with disablewallet=1 and the registered listening port. Replace old pqoperatorid, pqoperatorconfig, pqoperatorcredentials and externalip entries; paste above any [network] section. Keep the config private (Linux: chmod 600), preserve signing history and checkpoint settings, and restart the node.\n\nAnyone with these credentials can operate this masternode, but cannot spend your coins. Clear clipboard history after pasting.")) ||
            !controller || controller != walletModel) return;
        WalletModel::UnlockContext unlock(controller->requestUnlock());
        if (!unlock.isValid() || !controller || controller != walletModel) return;
        std::string credential, reason;
        if (!controller->getWallet()->ExportPQOperatorConfig(r.operatorKey, credential, reason)) {
            warn(tr("Masternode"), QString::fromStdString(reason)); return;
        }
        const auto config = PQWalletUI::masternodeConfig(entry->first, r.service, QString::fromStdString(credential));
        memory_cleanse(credential.data(), credential.size());
        // Secret export belongs only in the explicit clipboard, not Linux's
        // primary selection (which can be pasted accidentally by middle-click).
        QApplication::clipboard()->setText(config, QClipboard::Clipboard);
        inform(tr("Three masternode settings copied. Paste into the existing private VPS configuration and restart that node."));
        return;
    }
    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid()) {
        // Unlock wallet was cancelled
        inform(tr("Cannot show Masternode information, wallet locked"));
        return;
    }
    showHideOp(true);
    MnInfoDialog* dialog = new MnInfoDialog(window);
    QString label = index.data(Qt::DisplayRole).toString();
    QString address = index.sibling(index.row(), MNModel::ADDRESS).data(Qt::DisplayRole).toString();
    QString status = index.sibling(index.row(), MNModel::STATUS).data(Qt::DisplayRole).toString();
    QString txId = index.sibling(index.row(), MNModel::COLLATERAL_ID).data(Qt::DisplayRole).toString();
    QString outIndex = index.sibling(index.row(), MNModel::COLLATERAL_OUT_INDEX).data(Qt::DisplayRole).toString();
    QString pubKey = index.sibling(index.row(), MNModel::PUB_KEY).data(Qt::DisplayRole).toString();
    dialog->setData(pubKey, label, address, txId, outIndex, status);
    dialog->adjustSize();
    showDialog(dialog, 3, 17);
    if (dialog->exportMN) {
        if (ask(tr("Remote Masternode Data"),
                tr("You are just about to export the required data to run a Masternode\non a remote server to your clipboard.\n\n\n"
                   "You will only have to paste the data in the organiclifecoin.conf file\nof your remote server and start it, "
                   "then start the Masternode using\nthis controller wallet (select the Masternode in the list and press \"start\").\n"
                ))) {
            // export data
            QString exportedMN = "mnoperatorprivatekey=" + index.sibling(index.row(), MNModel::PRIV_KEY).data(Qt::DisplayRole).toString() + "\n"
                                 "externalip=" + address.left(address.lastIndexOf(":")) + "\n";
            GUIUtil::setClipboard(exportedMN);
            inform(tr("Masternode data copied to the clipboard."));
        }
    }

    dialog->deleteLater();
}

void MasterNodesWidget::onDeleteMNClicked()
{
    if (Params().IsTestChain()) { pqOperation(static_cast<int>(pqmn::Action::REVOKE)); return; }
    QString qAliasString = index.data(Qt::DisplayRole).toString();

    if (!ask(tr("Delete Masternode"), tr("You are just about to delete Masternode:\n%1\n\nAre you sure?").arg(qAliasString))) {
        return;
    }

    masternodeConfig.remove(qAliasString.toStdString());
    // Update list
    mnModel->removeMn(index);
    updateListState();
}

void MasterNodesWidget::onCreateMNClicked()
{
    if (Params().IsTestChain()) { createPQMasternode(); return; }
    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid()) {
        // Unlock wallet was cancelled
        inform(tr("Cannot create Masternode controller, wallet locked"));
        return;
    }

    CAmount mnCollateralAmount = mnModel->getMNCollateralRequiredAmount();
    if (walletModel->getBalance() <= mnCollateralAmount) {
        inform(tr("Not enough balance to create a masternode, %1 required.")
            .arg(GUIUtil::formatBalance(mnCollateralAmount, BitcoinUnits::PIV)));
        return;
    }

    if (coinControlDialog->coinControl && coinControlDialog->coinControl->HasSelected()) {
        std::vector<OutPointWrapper> coins;
        coinControlDialog->coinControl->ListSelected(coins);
        CAmount selectedBalance = 0;
        for (const auto& coin : coins) {
            selectedBalance += coin.value;
        }
        if (selectedBalance <= mnCollateralAmount) {
            inform(tr("Not enough coins selected to create a masternode, %1 required.")
                       .arg(GUIUtil::formatBalance(mnCollateralAmount, BitcoinUnits::PIV)));
            return;
        }
        mnModel->setCoinControl(coinControlDialog->coinControl);
    }

    showHideOp(true);
    MasterNodeWizardDialog *dialog = new MasterNodeWizardDialog(walletModel, mnModel, window);
    if (openDialogWithOpaqueBackgroundY(dialog, window, 5, 7)) {
        if (dialog->isOk) {
            // Update list
            mnModel->addMn(dialog->mnEntry);
            updateListState();
            // add mn
            inform(dialog->returnStr);
        } else {
            warn(tr("Error creating masternode"), dialog->returnStr);
        }
    }
    dialog->deleteLater();
    resetCoinControl();
}

void MasterNodesWidget::changeTheme(bool isLightTheme, QString& theme)
{
    static_cast<MNHolder*>(this->delegate->getRowFactory())->isLightTheme = isLightTheme;
}

void MasterNodesWidget::onCoinControlClicked()
{
    if (!walletModel) return;
    coinControlDialog->setModel(walletModel);
    coinControlDialog->setSelectionType(true);
    coinControlDialog->refreshDialog();
    coinControlDialog->setStyleSheet(GUIUtil::loadStyleSheet());
    coinControlDialog->exec();
    ui->btnCoinControl->setActive(coinControlDialog->coinControl->HasSelected());
}

void MasterNodesWidget::resetCoinControl()
{
    if (coinControlDialog) coinControlDialog->coinControl->SetNull();
    if (mnModel) mnModel->resetCoinControl();
    ui->btnCoinControl->setActive(false);
}

MasterNodesWidget::~MasterNodesWidget()
{
    delete ui;
}
