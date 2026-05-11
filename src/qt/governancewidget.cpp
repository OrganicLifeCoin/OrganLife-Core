// Copyright (c) 2021-2022 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "governancewidget.h"
#include "ui_governancewidget.h"

#include "chainparams.h"
#include "createproposaldialog.h"
#include "governancemodel.h"
#include "mnmodel.h"
#include "proposalinfodialog.h"
#include "qtutils.h"
#include "votedialog.h"

#include <QDesktopServices>
#include <QGraphicsDropShadowEffect>
#include <QScrollBar>
#include <QTimer>

void initComboView(PWidget* parent, QComboBox* comboBox, const QString& filterHint, const QList<QString>& values)
{
    auto* modelFilter = new QStandardItemModel(parent);
    Delegate* delegateFilter = new Delegate(parent);
    for (int i = 0; i < values.size(); ++i) {
        auto item = new QStandardItem(QString(filterHint+": %1").arg(values.value(i)));
        item->setData(i);
        modelFilter->appendRow(item);
    }
    delegateFilter->setValues(values);
    comboBox->setModel(modelFilter);
    comboBox->setItemDelegate(delegateFilter);
    comboBox->setCurrentIndex(0);
}

GovernanceWidget::GovernanceWidget(PIVXGUI* parent) :
        PWidget(parent),
        ui(new Ui::governancewidget)
{
    ui->setupUi(this);
    this->setStyleSheet(parent->styleSheet());

    ui->left->setAttribute(Qt::WA_StyledBackground, true);
    ui->containerTitles->setAttribute(Qt::WA_StyledBackground, true);
    ui->containerSort->setAttribute(Qt::WA_StyledBackground, true);
    ui->containerFilter->setAttribute(Qt::WA_StyledBackground, true);
    ui->layoutWarning->setAttribute(Qt::WA_StyledBackground, true);
    ui->mainContainer->setAttribute(Qt::WA_StyledBackground, true);
    ui->emptyContainer->setAttribute(Qt::WA_StyledBackground, true);
    ui->right->setAttribute(Qt::WA_StyledBackground, true);
    ui->containerBudget->setAttribute(Qt::WA_StyledBackground, true);
    ui->btnCreateProposal->setAttribute(Qt::WA_StyledBackground, true);
    ui->btnCreateProposal->setAttribute(Qt::WA_Hover, true);

    setCssProperty(ui->left, "governance-dashboard-shell");
    ui->left->setContentsMargins(0, 0, 0, 0);
    setCssProperty(ui->containerTitles, "governance-dashboard-band");
    setCssProperty(ui->containerSort, "governance-dashboard-filter-shell");
    setCssProperty(ui->containerFilter, "governance-dashboard-filter-shell");
    setCssProperty(ui->right, "governance-side-rail");
    ui->right->setContentsMargins(0, 0, 0, 0);
    setCssProperty(ui->mainContainer, "governance-dashboard-surface");
    setCssProperty(ui->scrollArea, "governance-dashboard-surface");
    setCssProperty(ui->emptyContainer, "governance-dashboard-surface");

    /* Title */
    ui->labelTitle->setText(tr("Governance"));
    setCssProperty(ui->labelTitle, "governance-title");
    ui->labelSubtitle1->setText(tr("View, follow, vote and submit network budget proposals. Be part of the DAO."));
    ui->labelSubtitle1->setWordWrap(true);
    setCssProperty(ui->labelSubtitle1, "governance-subtitle");
    setCssProperty(ui->pushImgEmpty, "img-empty-governance");
    setCssProperty(ui->labelEmpty, "text-empty");

    // Font
    QFont font;
    font.setPointSize(14);

    // Combo box sort
    SortEdit* lineEdit = new SortEdit(ui->comboBoxSort);
    lineEdit->setFont(font);
    lineEdit->setAlignment(Qt::AlignRight);
    initComboBox(ui->comboBoxSort, lineEdit, "governance-dashboard-filter", false);
    setCssProperty(ui->comboBoxSort, "governance-dashboard-filter", true);
    connect(lineEdit, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSort->showPopup();});
    QList<QString> values{tr("Date"), tr("Amount"), tr("Name")};
    initComboView(this, ui->comboBoxSort, tr("Sort by"), values);
    ui->comboBoxSort->setVisible(false); // Future: add sort actions

    // Filter
    SortEdit* lineEditFilter = new SortEdit(ui->comboBoxFilter);
    lineEditFilter->setFont(font);
    lineEditFilter->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lineEditFilter->setFrame(false);
    lineEditFilter->setTextMargins(0, 0, 0, 0);
    initComboBox(ui->comboBoxFilter, lineEditFilter, "governance-dashboard-filter", false);
    setCssProperty(ui->comboBoxFilter, "governance-dashboard-filter", true);
    connect(lineEditFilter, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxFilter->showPopup();});
    QList<QString> valuesFilter{tr("All"), tr("Passing"), tr("Not Passing"), tr("Waiting"), tr("Finished")};
    initComboView(this, ui->comboBoxFilter, tr("Filter"), valuesFilter);
    connect(ui->comboBoxFilter, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentTextChanged),
            this, &GovernanceWidget::onFilterChanged);

    // Budget
    setCssProperty(ui->containerBudget, "governance-budget-panel");
    setCssProperty(ui->labelBudget, "governance-budget-title");
    setCssProperty(ui->labelBudgetSubTitle, "governance-subtitle");
    setCssProperty(ui->labelAvailableTitle, "governance-budget-label");
    setCssProperty(ui->labelAllocatedTitle, "governance-budget-label");
    setCssProperty(ui->labelAvailableAmount, "governance-budget-value");
    setCssProperty(ui->labelAllocatedAmount, "governance-budget-value-allocated");
    setCssProperty(ui->iconClock , "ic-time");
    setCssProperty(ui->labelNextSuperblock, "governance-budget-label");

    // Sync Warning
    ui->layoutWarning->setVisible(true);
    ui->lblWarning->setText(tr("Sync in progress..."));
    setCssProperty(ui->layoutWarning, "governance-dashboard-warning");
    setCssProperty(ui->lblWarning, "governance-warning-text");
    setCssProperty(ui->imgWarning, "ic-warning");

    // Create proposal
    setCssProperty(ui->btnCreateProposal, "governance-header-cta", true);
    ui->btnCreateProposal->setTitleClassAndText("governance-cta-title", tr("Create Proposal"));
    ui->btnCreateProposal->setSubTitleClassAndText("governance-cta-subtitle", tr("Prepare and submit a new proposal."));
    ui->btnCreateProposal->setRightIconClass("governance-cta-arrow", true);
    if (QWidget* ctaBody = ui->btnCreateProposal->findChild<QWidget*>("layoutOptions2")) {
        ctaBody->setAttribute(Qt::WA_Hover, true);
        ctaBody->setContentsMargins(14, 10, 10, 10);
    }
    connect(ui->btnCreateProposal, &OptionButton::clicked, this, &GovernanceWidget::onCreatePropClicked);
    ui->emptyContainer->setVisible(false);
}

GovernanceWidget::~GovernanceWidget()
{
    delete ui;
}

void GovernanceWidget::onFilterChanged(const QString& value)
{
    int filterByType = ui->comboBoxFilter->currentIndex();
    switch (filterByType) {
        case 1:
            statusFilter = ProposalInfo::Status::PASSING;
            break;
        case 2:
            statusFilter = ProposalInfo::Status::NOT_PASSING;
            break;
        case 3:
            statusFilter = ProposalInfo::Status::WAITING_FOR_APPROVAL;
            break;
        case 4:
            statusFilter = ProposalInfo::Status::FINISHED;
            break;
        default:
            statusFilter = nullopt;
            break;
    }
    refreshCardsGrid(true);
}

void GovernanceWidget::onVoteForPropClicked(const ProposalInfo& proposalInfo)
{
    if (!governanceModel->isTierTwoSync()) {
        inform(tr("Node is syncing. Please wait."));
        return;
    }

    if (proposalInfo.status == ProposalInfo::Status::WAITING_FOR_APPROVAL) {
        inform(tr("Cannot vote for the proposal yet, wait until it's confirmed by the network"));
        return;
    }
    if (proposalInfo.status == ProposalInfo::Status::FINISHED) {
        inform(tr("Cannot vote for this proposal because voting is finished"));
        return;
    }
    window->showHide(true);
    VoteDialog* dialog = new VoteDialog(window, governanceModel, mnModel, walletModel.get());
    dialog->setProposal(proposalInfo);
    const bool accepted = openDialogWithOpaqueBackgroundY(dialog, window, 4.5, 12, false);
    window->showHide(false);
    if (accepted) {
        // future: make this refresh atomic, no need to refresh the entire grid.
        tryGridRefresh(true);
        inform(tr("Vote submitted successfully!"));
    }
    dialog->deleteLater();
}

void GovernanceWidget::onCreatePropClicked()
{
    if (!walletModel || !governanceModel || !clientModel) return;

    if (!governanceModel->isTierTwoSync()) {
        inform(tr("Node is syncing. Please wait."));
        return;
    }

    // Do not allow proposal submission too close to the next superblock.
    // Keep a one-day realtime safety window (regtest stays permissive for tests).
    // future: customizable future superblock height selection (for now, we are automatically using the next superblock).
    const int chainHeight = clientModel->getLastBlockProcessedHeight();
    const int nextSuperblock = governanceModel->getNextSuperblockHeight();
    const int64_t targetSpacing = Params().GetConsensus().nTargetSpacing > 0 ? Params().GetConsensus().nTargetSpacing : 1;
    const int oneDayBlocks = static_cast<int>((24 * 60 * 60) / targetSpacing);
    const int acceptedRange = walletModel->isRegTestNetwork() ? 10 : (oneDayBlocks > 0 ? oneDayBlocks : 1);
    if (nextSuperblock - acceptedRange < chainHeight) {
        inform(tr("Cannot create proposal, superblock is too close. Need to wait %1 blocks").arg(nextSuperblock - chainHeight));
        return;
    }

    auto ptrUnlockedContext = std::make_unique<WalletModel::UnlockContext>(walletModel->requestUnlock());
    if (!ptrUnlockedContext->isValid()) {
        inform(tr("Cannot create proposal, wallet locked"));
        return;
    }

    auto balance = walletModel->GetWalletBalances();
    if (balance.balance <= governanceModel->getProposalFeeAmount()) {
        inform(tr("Cannot create proposal, need to have at least %1 to pay for the proposal fee").arg(
                  GUIUtil::formatBalance(governanceModel->getProposalFeeAmount() + walletModel->getNetMinFee()).toStdString().c_str()));
        return;
    }

    window->showHide(true);
    CreateProposalDialog* dialog = new CreateProposalDialog(window, governanceModel, walletModel);
    const bool accepted = openDialogWithOpaqueBackgroundY(dialog, window, 4.5, ui->left->height() < 700 ? 12 : 5, false);
    window->showHide(false);
    if (accepted) {
        // future: make this refresh atomic, no need to refresh the entire grid.
        tryGridRefresh(true);
        inform(tr("Proposal transaction fee broadcasted!"));
    }
    dialog->deleteLater();
}

void GovernanceWidget::onMenuClicked(ProposalCard* card)
{
    if (!propMenu) {
        propMenu = new TooltipMenu(window, this);
        propMenu->setCopyBtnText(tr("Copy Url"));
        propMenu->setEditBtnText(tr("Open Url"));
        propMenu->setDeleteBtnText(tr("More Info"));
        propMenu->setMaximumWidth(propMenu->maximumWidth() + 5);
        propMenu->setFixedWidth(propMenu->width() + 5);
        connect(propMenu, &TooltipMenu::message, this, &GovernanceWidget::message);
        connect(propMenu, &TooltipMenu::onCopyClicked, this, &GovernanceWidget::onCopyUrl);
        connect(propMenu, &TooltipMenu::onEditClicked, this, &GovernanceWidget::onOpenClicked);
        connect(propMenu, &TooltipMenu::onDeleteClicked, this, &GovernanceWidget::onMoreInfoClicked);
    } else {
        propMenu->hide();
    }
    menuCard = card;
    QRect rect = card->geometry();
    QPoint pos = rect.topRight();
    pos.setX(pos.x() - 22);
    pos.setY(pos.y() + (isSync ? 100 : 140) - ui->scrollArea->verticalScrollBar()->value());
    propMenu->move(pos);
    propMenu->show();
}

void GovernanceWidget::onCopyUrl()
{
    if (!menuCard) return;
    GUIUtil::setClipboard(QString::fromStdString(menuCard->getProposal().url));
    inform(tr("Proposal URL copied to clipboard"));
}

void GovernanceWidget::onOpenClicked()
{
    if (!menuCard) return;
    if (ask(tr("Open Proposal URL"),
            tr("The following URL will be opened in the default browser") + "\n\n" +
            QString::fromStdString(menuCard->getProposal().url) + "\n\n" +
            tr("Are you sure?\n(Always verify the URL validity before opening it)\n"))) {
        if (!QDesktopServices::openUrl(QUrl(QString::fromStdString(menuCard->getProposal().url)))) {
            inform(tr("Failed to open proposal URL"));
        }
    }
}

void GovernanceWidget::onMoreInfoClicked()
{
    window->showHide(true);
    ProposalInfoDialog* dialog = new ProposalInfoDialog(window);
    dialog->setProposal(menuCard->getProposal());
    openDialogWithOpaqueBackgroundY(dialog, window, 4.5, ui->left->height() < 700 ? 12 : 5, false);
    window->showHide(false);
    dialog->deleteLater();
}

void GovernanceWidget::loadClientModel()
{
    connect(clientModel, &ClientModel::numBlocksChanged, this, &GovernanceWidget::chainHeightChanged);
}

void GovernanceWidget::chainHeightChanged(int height)
{
    if (!clientModel || !governanceModel) return;
    if (!isVisible() || clientModel->inInitialBlockDownload()) return;
    int remainingBlocks = governanceModel->getNextSuperblockHeight() - height;
    if (remainingBlocks < 0) {
        remainingBlocks = 0;
    }
    const int64_t targetSpacing = Params().GetConsensus().nTargetSpacing > 0 ? Params().GetConsensus().nTargetSpacing : 1;
    const int64_t remainingSeconds = static_cast<int64_t>(remainingBlocks) * targetSpacing;
    const int64_t remainingDays = remainingSeconds / (24 * 60 * 60);

    QString text;
    if (remainingDays >= 1) {
        text = tr("Next superblock in %1 days.\n%2 blocks to go.").arg(remainingDays).arg(remainingBlocks);
    } else {
        const int64_t remainingHours = (remainingSeconds + (60 * 60) - 1) / (60 * 60);
        text = tr("Next superblock in %1 hours.\n%2 blocks to go.")
                       .arg(remainingHours > 0 ? remainingHours : 0)
                       .arg(remainingBlocks);
    }
    ui->labelNextSuperblock->setText(text);
}

void GovernanceWidget::setGovModel(GovernanceModel* _model)
{
    governanceModel = _model;
}

void GovernanceWidget::setMNModel(MNModel* _mnModel)
{
    mnModel = _mnModel;
}

void GovernanceWidget::loadWalletModel()
{
    governanceModel->setWalletModel(walletModel);
}

void GovernanceWidget::showEvent(QShowEvent *event)
{
    if (!refreshTimer) {
        refreshTimer = new QTimer(this);
        connect(refreshTimer, &QTimer::timeout, this, &GovernanceWidget::onRefreshTimerTimeout, Qt::UniqueConnection);
    }

    if (clientModel && governanceModel) {
        clientModel->startMasternodesTimer();
        tryGridRefresh(true); // future: move to background worker
        if (!refreshTimer->isActive()) {
            refreshTimer->start(1000 * 60 * 3.5); // Try to refresh screen 3.5 minutes
        }
    }
}

void GovernanceWidget::onRefreshTimerTimeout()
{
    tryGridRefresh(true);
}

void GovernanceWidget::hideEvent(QHideEvent *event)
{
    if (refreshTimer) {
        refreshTimer->stop();
    }
    if (clientModel) {
        clientModel->stopMasternodesTimer();
    }
}

void GovernanceWidget::wheelEvent(QWheelEvent* event)
{
    if (propMenu && propMenu->isVisible()) {
        propMenu->hide();
    }
}

void GovernanceWidget::resizeEvent(QResizeEvent *event)
{
    if (!isVisible()) return;
    tryGridRefresh();
}

void GovernanceWidget::tryGridRefresh(bool force)
{
    if (!clientModel || !governanceModel) return;
    int _propsPerRow = calculateColumnsPerRow();
    if (_propsPerRow != propsPerRow || force) {
        propsPerRow = _propsPerRow;
        refreshCardsGrid(true);

        // refresh budget distribution values
        chainHeightChanged(clientModel->getNumBlocks());
        ui->labelAllocatedAmount->setText(GUIUtil::formatBalance(governanceModel->getBudgetAllocatedAmount()));
        ui->labelAvailableAmount->setText(GUIUtil::formatBalance(governanceModel->getBudgetAvailableAmount()));
    }
}

static void setCardShadow(QWidget* edit)
{
    QGraphicsDropShadowEffect* shadowEffect = new QGraphicsDropShadowEffect();
    shadowEffect->setColor(QColor(77, 77, 77, 30));
    shadowEffect->setXOffset(0);
    shadowEffect->setYOffset(4);
    shadowEffect->setBlurRadius(6);
    edit->setGraphicsEffect(shadowEffect);
}

ProposalCard* GovernanceWidget::newCard()
{
    ProposalCard* propCard = new ProposalCard(ui->scrollAreaWidgetContents);
    connect(propCard, &ProposalCard::voteClicked, this, &GovernanceWidget::onVoteForPropClicked);
    connect(propCard, &ProposalCard::inform, this, &GovernanceWidget::inform);
    connect(propCard, &ProposalCard::onMenuClicked, this, &GovernanceWidget::onMenuClicked);
    setCardShadow(propCard);
    return propCard;
}

void GovernanceWidget::showEmptyScreen(bool show)
{
    if (ui->emptyContainer->isVisible() != show) {
        ui->emptyContainer->setVisible(show);
        ui->mainContainer->setVisible(!show);
    }
}

void GovernanceWidget::refreshCardsGrid(bool forceRefresh)
{
    if (!governanceModel) return;
    if (!governanceModel->hasProposals()) {
        showEmptyScreen(true);
        return;
    }

    showEmptyScreen(false);
    if (!gridLayout) {
        gridLayout = new QGridLayout();
        gridLayout->setAlignment(Qt::AlignTop);
        gridLayout->setHorizontalSpacing(16);
        gridLayout->setVerticalSpacing(16);
        ui->scrollArea->setWidgetResizable(true);
        ui->scrollAreaWidgetContents->setLayout(gridLayout);
    }

    // Refresh grid only if needed
    if (!(forceRefresh || governanceModel->isRefreshNeeded())) return;

    std::list<ProposalInfo> props = governanceModel->getProposals(statusFilter.get_ptr(), false);

    // Start marking all the cards
    for (ProposalCard* card : cards) {
        card->setNeedsUpdate(true);
    }

    // Refresh the card if exists or create a new one.
    int column = 0;
    int row = 0;
    for (const auto& prop : props) {
        QLayoutItem* item = gridLayout->itemAtPosition(row, column);
        ProposalCard* card{nullptr};
        if (item) {
            card = dynamic_cast<ProposalCard*>(item->widget());
            card->setNeedsUpdate(false);
        } else {
            card = newCard();
            cards.emplace_back(card);
            gridLayout->addWidget(card, row, column, 1, 1);
        }
        card->setProposal(prop);
        column++;
        if (column == propsPerRow) {
            column = 0;
            row++;
        }
    }

    // Now delete the not longer needed cards
    auto it = cards.begin();
    while (it != cards.end()) {
        ProposalCard* card = (*it);
        if (!card->isUpdateNeeded()) {
            it++;
            continue;
        }
        gridLayout->takeAt(gridLayout->indexOf(card));
        it = cards.erase(it);
        if (card == menuCard) menuCard = nullptr;
        delete card;
    }
}

int GovernanceWidget::calculateColumnsPerRow()
{
    int widgetWidth = ui->left->width();
    if (widgetWidth < 785) {
        return 2;
    } else if (widgetWidth < 1100){
        return 3;
    } else {
        return 4; // max amount of cards
    }
}

void GovernanceWidget::tierTwoSynced(bool sync)
{
    if (isSync != sync) {
        isSync = sync;
        ui->layoutWarning->setVisible(!isSync);
        if (!isVisible()) return;
        tryGridRefresh();
    }
}
