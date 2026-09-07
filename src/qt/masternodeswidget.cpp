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
#include <utilstrencodings.h>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QDir>
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
    this->coinControlDialog = new CoinControlDialog();

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
        onStartAllClicked(REQUEST_START_MISSING);
    });
    connect(ui->listMn, &QListView::clicked, this, &MasterNodesWidget::onMNClicked);
    connect(ui->btnAbout, &OptionButton::clicked, [this](){window->openFAQ(SettingsFaqWidget::Section::MASTERNODE);});
    connect(ui->btnAboutController, &OptionButton::clicked, [this](){window->openFAQ(SettingsFaqWidget::Section::MNCONTROLLER);});
    connect(ui->btnCoinControl, &OptionButton::clicked, this, &MasterNodesWidget::onCoinControlClicked);
    if (Params().IsRegTestNet()) {
        setMNModel(new MNModel(this));
        ui->pushButtonSave->setText(tr("Register masternode"));
        ui->pushButtonSave->setEnabled(false);
        ui->pushButtonStartMissing->hide();
        ui->btnCoinControl->hide(); // The shared controller builder currently selects fee inputs.
        ui->labelEmpty->setText(tr("No confirmed PQ registrations"));
        updateListState();
    }
}

void MasterNodesWidget::loadWalletModel()
{
    if (Params().IsRegTestNet() && mnModel) {
        mnModel->updateMNList();
        updateListState();
    }
}

void MasterNodesWidget::pqOperation(int action)
{
    QPointer<WalletModel> controller = walletModel;
    if (!controller || !mnModel || !mnModel->registryError().isEmpty()) return;
    pqmn::Payload op;
    op.action = static_cast<pqmn::Action>(action);
    const bool registering = op.action == pqmn::Action::REGISTER;
    if (!registering) {
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
    dialog.setWindowTitle(registering ? tr("Register masternode") : tr("Manage masternode"));
    auto* form = new QFormLayout(&dialog);
    form->setSpacing(14);
    auto* explanation = new QLabel(registering ?
        tr("Uses existing wallet addresses and a backed operator identity. Creates a new collateral output. Remote startup, rewards and finality are not enabled.") :
        tr("Registration: %1\nNext sequence: %2").arg(QString::fromStdString(op.registration.GetHex())).arg(QString::number(op.sequence)), &dialog);
    explanation->setWordWrap(true);
    form->addRow(explanation);
    QComboBox owner, collateral, payout, operators, operation;
    QLineEdit service, operatorPayout;
    for (const auto& address : controller->getWallet()->GetPQAddresses()) {
        const auto text = QString::fromStdString(address);
        owner.addItem(text); collateral.addItem(text); payout.addItem(text);
    }
    if (collateral.count() > 1) collateral.setCurrentIndex(1);
    for (const auto& key : controller->getWallet()->GetPQOperators()) {
        const auto hex = QString::fromStdString(HexStr(key));
        operators.addItem(hex.left(20) + "…", hex);
    }
    payout.setEditable(true);
    if (registering) {
        form->addRow(tr("Owner address"), &owner);
        form->addRow(tr("Collateral address"), &collateral);
        form->addRow(tr("Payout address"), &payout);
        form->addRow(tr("Backed operator"), &operators);
        form->addRow(tr("Service IP:port (optional)"), &service);
        op.collateral = COutPoint(uint256(), 0);
    } else if (op.action == pqmn::Action::SERVICE) {
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
    if (!registering && op.action == pqmn::Action::SERVICE) op.action = static_cast<pqmn::Action>(operation.currentData().toInt());
    WalletModel::UnlockContext unlock(controller->requestUnlock());
    if (!unlock.isValid() || !controller || controller != walletModel) return;
    auto* wallet = controller->getWallet();
    const auto address = [&](const QString& text, pq::KeyID& id) {
        return pq::DecodeAddress(text.trimmed().toStdString(), Params().NetworkIDString(), id);
    };
    if (registering || op.action == pqmn::Action::UPDATE) {
        const auto bytes = ParseHex(operators.currentData().toString().toStdString());
        if (bytes.size() != op.operatorKey.size() || !address(payout.currentText(), op.payout)) {
            warn(tr("Masternode"), tr("Choose a backed operator and a valid payout address.")); return;
        }
        std::copy(bytes.begin(), bytes.end(), op.operatorKey.begin());
    }
    if (registering) {
        mldsa44::Key ownerKey, collateralKey;
        if (owner.currentText() == collateral.currentText() ||
            !wallet->GetPQKey(owner.currentText().toStdString(), ownerKey) ||
            !wallet->GetPQKey(collateral.currentText().toStdString(), collateralKey)) {
            warn(tr("Masternode"), tr("Choose distinct owner and collateral addresses from this wallet.")); return;
        }
        op.owner = ownerKey.GetPublicKey(); op.collateralKey = collateralKey.GetPublicKey();
    }
    if (op.action == pqmn::Action::SERVICE || (registering && !service.text().isEmpty())) {
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
    if (!wallet->PreparePQMasternodeTransaction(op, backup.toStdString(), tx, fee, reason)) {
        warn(tr("Masternode"), QString::fromStdString(reason)); return;
    }
    const auto actionName = registering ? tr("Register") : op.action == pqmn::Action::REVOKE ? tr("Revoke") :
        op.action == pqmn::Action::SERVICE ? tr("Update service") : tr("Update payout / operator");
    auto summary = tr("Action: %1\nRegistration: %2\nFee: %3\nCollateral created: %4\nEncrypted backup: %5")
        .arg(actionName).arg(QString::fromStdString(registering ? tx->GetHash().GetHex() : op.registration.GetHex()))
        .arg(GUIUtil::formatBalance(fee, BitcoinUnits::PIV))
        .arg(GUIUtil::formatBalance(registering ? Params().GetConsensus().nMNCollateralAmt : 0, BitcoinUnits::PIV)).arg(backup);
    if (registering) summary += tr("\nOwner: %1\nCollateral address: %2").arg(owner.currentText(), collateral.currentText());
    if (registering || op.action == pqmn::Action::UPDATE) {
        const auto id = pq::GetID(op.operatorKey, Params().NetworkIDString());
        summary += tr("\nPayout: %1\nOperator fingerprint: %2").arg(payout.currentText(), id ? QString::fromStdString(HexStr(*id)) : "");
    }
    if (registering || op.action == pqmn::Action::SERVICE)
        summary += tr("\nService: %1").arg(service.text());
    if (op.action == pqmn::Action::SERVICE) summary += tr("\nOperator payout: %1").arg(operatorPayout.text());
    if (op.action == pqmn::Action::UPDATE) summary += tr("\nChanging the operator clears its service endpoint and payout.");
    if (op.action == pqmn::Action::REVOKE) summary += tr("\nRevocation disables this operator; it does not unlock collateral.");
    if (!ask(tr("Confirm masternode transaction"), summary) || !controller || controller != walletModel) return;
    const auto committed = wallet->CommitTransaction(tx, nullptr, g_connman.get());
    if (committed.status != CWallet::CommitStatus::OK) { warn(tr("Masternode"), QString::fromStdString(committed.ToString())); return; }
    inform(tr("Transaction submitted: %1. Wait for confirmation before another change.").arg(QString::fromStdString(committed.hashTx.GetHex())));
}

void MasterNodesWidget::clearWalletModel()
{
    PWidget::clearWalletModel();
    if (auto* dialog = findChild<QDialog*>("pqMasternodeDialog")) dialog->reject();
    if (menu) menu->hide();
    index = QPersistentModelIndex();
    if (Params().IsRegTestNet()) ui->pushButtonSave->setEnabled(false);
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
    ui->pushButtonStartAll->setVisible(show && !Params().IsRegTestNet());
    if (Params().IsRegTestNet()) {
        if (menu) menu->hide();
        const auto error = mnModel ? mnModel->registryError() : tr("Registry unavailable");
        ui->labelSubtitle1->setText(error.isEmpty() ?
            tr("Confirmed PQ registry · Local testing only. Registration is not service, reward or finality eligibility.") : error);
        ui->pushButtonSave->setEnabled(walletModel && error.isEmpty());
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
        this->menu->setEditBtnText(Params().IsRegTestNet() ? tr("Update service") : tr("Start"));
        this->menu->setDeleteBtnText(Params().IsRegTestNet() ? tr("Revoke") : tr("Delete"));
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
    if (Params().IsRegTestNet()) { pqOperation(static_cast<int>(pqmn::Action::SERVICE)); return; }
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
    if (Params().IsRegTestNet()) {
        const auto entry = mnModel ? mnModel->pqRecord(index) : nullopt;
        if (!entry) return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Masternode registration"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* details = new QPlainTextEdit(&dialog);
        details->setReadOnly(true);
        const auto& r = entry->second;
        details->setPlainText(tr("Registration: %1\nSequence: %2\nCollateral: %3:%4\nService: %5\nPayout: %6\nOperator public key: %7\n\nRegistration does not establish service, rewards or finality.")
            .arg(QString::fromStdString(entry->first.GetHex())).arg(QString::number(r.sequence))
            .arg(QString::fromStdString(r.collateral.hash.GetHex())).arg(r.collateral.n)
            .arg(QString::fromStdString(r.service.ToString()))
            .arg(QString::fromStdString(pq::EncodeAddress(r.payout, Params().NetworkIDString())))
            .arg(QString::fromStdString(HexStr(r.operatorKey))));
        layout->addWidget(details);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.resize(660, 350);
        dialog.exec();
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
    if (Params().IsRegTestNet()) { pqOperation(static_cast<int>(pqmn::Action::REVOKE)); return; }
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
    if (Params().IsRegTestNet()) { pqOperation(static_cast<int>(pqmn::Action::REGISTER)); return; }
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
    if (!coinControlDialog->hasModel()) coinControlDialog->setModel(walletModel);
    coinControlDialog->setSelectionType(true);
    coinControlDialog->refreshDialog();
    coinControlDialog->setStyleSheet(GUIUtil::loadStyleSheet());
    coinControlDialog->exec();
    ui->btnCoinControl->setActive(coinControlDialog->coinControl->HasSelected());
}

void MasterNodesWidget::resetCoinControl()
{
    if (coinControlDialog) coinControlDialog->coinControl->SetNull();
    mnModel->resetCoinControl();
    ui->btnCoinControl->setActive(false);
}

MasterNodesWidget::~MasterNodesWidget()
{
    delete ui;
}
