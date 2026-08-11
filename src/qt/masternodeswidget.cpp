// Copyright (c) 2019-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeswidget.h"
#include "ui_masternodeswidget.h"

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
}

void MasterNodesWidget::showEvent(QShowEvent *event)
{
    if (mnModel) mnModel->updateMNList();
    if (!timer) {
        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, [this]() {mnModel->updateMNList();});
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
    ui->pushButtonStartAll->setVisible(show);
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
        this->menu->setEditBtnText(tr("Start"));
        this->menu->setDeleteBtnText(tr("Delete"));
        this->menu->setCopyBtnText(tr("Info"));
        connect(this->menu, &TooltipMenu::message, this, &AddressesWidget::message);
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
