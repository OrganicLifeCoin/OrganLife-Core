// Copyright (c) 2019-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dashboardwidget.h"
#include "ui_dashboardwidget.h"

#include "chainparams.h"
#include "chartutils.h"
#include "clientmodel.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "qtutils.h"
#include "sendconfirmdialog.h"
#include "txrow.h"
#include "utiltime.h"
#include <QHBoxLayout>
#include <QGraphicsLayout>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QList>
#include <QModelIndex>
#include <QParallelAnimationGroup>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QThread>
#include <QTime>
#include <QVBoxLayout>

#define DECORATION_SIZE 65
#define NUM_ITEMS 3
#define SHOW_EMPTY_CHART_VIEW_THRESHOLD 0
#define REQUEST_LOAD_TASK 1
#define CHART_LOAD_MIN_TIME_INTERVAL 15

namespace {
void ReplaceLineSeriesValues(QLineSeries* lineSeries, const QList<qreal>& values)
{
    if (!lineSeries) return;
    QVector<QPointF> points;
    points.reserve(values.size());
    for (int i = 0; i < values.size(); ++i) {
        // bar-category buckets span [i, i+1]; center points at i+0.5
        points.append(QPointF(i + 0.5, values.at(i)));
    }
    lineSeries->replace(points);
}

QList<qreal> NormalizeValues(const QList<qreal>& values, int targetCount)
{
    QList<qreal> normalized = values;
    if (targetCount <= 0) {
        normalized.clear();
        return normalized;
    }
    if (normalized.size() > targetCount) {
        normalized.erase(normalized.begin() + targetCount, normalized.end());
    } else if (normalized.size() < targetCount) {
        normalized.reserve(targetCount);
        while (normalized.size() < targetCount) {
            normalized.append(0);
        }
    }
    return normalized;
}

qreal MaxValue(const QList<qreal>& values)
{
    qreal maxValue = 0;
    for (const qreal value : values) {
        maxValue = std::max(maxValue, value);
    }
    return maxValue;
}

bool ToChartBucketMode(const ChartShowType type, ChartBucketMode& mode)
{
    switch (type) {
    case ALL:
        mode = ChartBucketMode::All;
        return true;
    case YEAR:
        mode = ChartBucketMode::Year;
        return true;
    case MONTH:
        mode = ChartBucketMode::Month;
        return true;
    default:
        return false;
    }
}
} // namespace

DashboardWidget::DashboardWidget(OrganicLifeGUI* parent) :
    PWidget(parent),
    ui(new Ui::DashboardWidget)
{
    ui->setupUi(this);

    txHolder = new TxViewHolder(isLightTheme());
    txViewDelegate = new FurAbstractListItemDelegate(
        DECORATION_SIZE,
        txHolder,
        this
    );

    this->setStyleSheet(parent->styleSheet());
    this->setContentsMargins(0,0,0,0);

    // Containers
    setCssProperty(this, "dashboard-root");
    ui->left->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->left, "dashboard-shell-left");
    ui->left->setContentsMargins(0,0,0,0);
    ui->verticalLayout22->setContentsMargins(12, 12, 12, 12);
    ui->verticalLayout22->setSpacing(12);
    ui->left_top_container->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->left_top_container, "dashboard-header-band");
    ui->left_top_container->setMinimumHeight(96);
    ui->horizontalLayout->setContentsMargins(24, 12, 24, 12);
    ui->horizontalLayout->setSpacing(12);
    ui->verticalLayout_5->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout_5->setSpacing(4);
    ui->verticalSpacer->changeSize(0, 10, QSizePolicy::Minimum, QSizePolicy::Fixed);

    // Title: V2 greeting band
    setCssProperty(ui->labelTitle, "screen-header-title");
    setCssProperty(ui->labelTitle2, "dashboard-side-title");
    {
        const int hour = QTime::currentTime().hour();
        const QString daypart = hour < 5 ? tr("night") : (hour < 12 ? tr("morning") : (hour < 18 ? tr("afternoon") : tr("evening")));
        ui->labelTitle->setText(tr("Good %1, grower").arg(daypart));
        ui->labelSubtitle->setText(tr("%1 network").arg(QString::fromStdString(Params().NetworkIDString())));
    }

    /* Subtitle */
    setCssProperty(ui->labelSubtitle, "screen-header-subtitle");
    ui->labelSubtitle->setWordWrap(true);
    setCssProperty(ui->labelMessage, "dashboard-side-subtitle");
    ui->labelMessage->setWordWrap(true);
    ui->labelMessage->setVisible(false);

    // Staking Information
    setCssProperty(ui->labelSquarePiv, "square-chart-piv");
    setCssProperty(ui->labelSquareMN, "square-chart-mn");
    setCssProperty(ui->labelPiv, "text-chart-piv");
    setCssProperty(ui->labelMN, "text-chart-mn");

    // Staking Amount
    QFont fontBold;
    fontBold.setWeight(QFont::Bold);

    ui->labelChart->setVisible(false);
    setCssProperty(ui->labelAmountPiv, "dashboard-reward-stat-muted");
    setCssProperty(ui->labelAmountMN, "dashboard-reward-stat-muted");

    setCssProperty({ui->pushButtonAll,  ui->pushButtonMonth, ui->pushButtonYear}, "btn-check-time");
    setCssProperty({ui->comboBoxMonths,  ui->comboBoxYears}, "btn-combo-chart-selected");

    ui->comboBoxMonths->setView(new QListView());
    ui->comboBoxMonths->setStyleSheet("selection-background-color:transparent;");
    ui->comboBoxYears->setView(new QListView());
    ui->comboBoxYears->setStyleSheet("selection-background-color:transparent;");
    ui->pushButtonYear->setChecked(true);

    setCssProperty(ui->pushButtonChartArrow, "btn-chart-arrow");
    setCssProperty(ui->pushButtonChartRight, "btn-chart-arrow-right");

    ui->right->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->right, "dashboard-shell-right");
    ui->right->setContentsMargins(0,0,0,0);
    ui->verticalLayout_31->setContentsMargins(12, 0, 0, 0);
    ui->verticalLayout_31->setSpacing(0);
    ui->verticalLayout_2->setContentsMargins(8, 8, 8, 8);
    ui->verticalLayout_2->setSpacing(8);
    ui->horizontalLayout_3->setContentsMargins(10, 8, 10, 0);
    ui->horizontalLayout_3->setSpacing(12);

    auto* analyticsModule = new QWidget(ui->right);
    analyticsModule->setObjectName("analyticsModule");
    analyticsModule->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(analyticsModule, "dashboard-analytics-card");
    auto* analyticsLayout = new QVBoxLayout(analyticsModule);
    analyticsLayout->setContentsMargins(8, 8, 8, 8);
    analyticsLayout->setSpacing(2);

    auto* rewardSummaryRow = new QWidget(analyticsModule);
    rewardSummaryRow->setObjectName("rewardSummaryRow");
    rewardSummaryRow->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(rewardSummaryRow, "dashboard-reward-summary");
    rewardSummaryRow->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    rewardSummaryRow->setMaximumHeight(88);
    auto* rewardSummaryLayout = new QHBoxLayout(rewardSummaryRow);
    rewardSummaryLayout->setContentsMargins(0, 0, 0, 0);
    rewardSummaryLayout->setSpacing(6);

    auto* stakingTile = new QWidget(rewardSummaryRow);
    stakingTile->setObjectName("stakingRewardTile");
    stakingTile->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(stakingTile, "dashboard-reward-tile");
    auto* stakingTileLayout = new QVBoxLayout(stakingTile);
    stakingTileLayout->setContentsMargins(10, 7, 10, 7);
    stakingTileLayout->setSpacing(2);
    auto* stakingHeader = new QWidget(stakingTile);
    auto* stakingHeaderLayout = new QHBoxLayout(stakingHeader);
    stakingHeaderLayout->setContentsMargins(0, 0, 0, 0);
    stakingHeaderLayout->setSpacing(6);
    auto* stakingLabel = new QLabel(tr("Staking"), stakingTile);
    setCssProperty(stakingLabel, "dashboard-reward-tile-label");
    ui->horizontalLayout_4->removeWidget(ui->labelSquarePiv);
    ui->labelSquarePiv->setFixedSize(14, 14);
    stakingHeaderLayout->addWidget(stakingLabel);
    stakingHeaderLayout->addStretch(1);
    stakingHeaderLayout->addWidget(ui->labelSquarePiv, 0, Qt::AlignVCenter);
    stakingTileLayout->addWidget(stakingHeader);

    auto* masternodeTile = new QWidget(rewardSummaryRow);
    masternodeTile->setObjectName("masternodeRewardTile");
    masternodeTile->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(masternodeTile, "dashboard-reward-tile");
    auto* masternodeTileLayout = new QVBoxLayout(masternodeTile);
    masternodeTileLayout->setContentsMargins(10, 7, 10, 7);
    masternodeTileLayout->setSpacing(2);
    auto* masternodeHeader = new QWidget(masternodeTile);
    auto* masternodeHeaderLayout = new QHBoxLayout(masternodeHeader);
    masternodeHeaderLayout->setContentsMargins(0, 0, 0, 0);
    masternodeHeaderLayout->setSpacing(6);
    auto* masternodeLabel = new QLabel(tr("Masternodes"), masternodeTile);
    setCssProperty(masternodeLabel, "dashboard-reward-tile-label");
    ui->horizontalLayout_4->removeWidget(ui->labelSquareMN);
    ui->labelSquareMN->setFixedSize(14, 14);
    masternodeHeaderLayout->addWidget(masternodeLabel);
    masternodeHeaderLayout->addStretch(1);
    masternodeHeaderLayout->addWidget(ui->labelSquareMN, 0, Qt::AlignVCenter);
    masternodeTileLayout->addWidget(masternodeHeader);

    ui->horizontalLayout_5->removeWidget(ui->labelAmountPiv);
    ui->horizontalLayout_5->removeWidget(ui->labelAmountMN);
    ui->labelAmountPiv->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui->labelAmountMN->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    stakingTileLayout->addWidget(ui->labelAmountPiv);
    masternodeTileLayout->addWidget(ui->labelAmountMN);
    rewardSummaryLayout->addWidget(stakingTile, 1);
    rewardSummaryLayout->addWidget(masternodeTile, 1);

    ui->verticalLayout_2->removeItem(ui->horizontalLayout_5);
    ui->labelPiv->setVisible(false);
    ui->labelMN->setVisible(false);
    ui->verticalLayout_8->removeItem(ui->horizontalLayout_4);
    // Brown palette candidate. Previous marker gradients:
    // light staking #FF8A3D -> #F24A09, light masternode #7D7877 -> #22254A,
    // dark staking #A78BFA -> #F24A09, dark masternode #C4B5FD -> #7C3AED.
    const auto initialMarkerStyle = [](bool stakingMarker) {
        if (isLightTheme()) {
            return stakingMarker
                ? QStringLiteral("background:#9C4E1A;border:none;border-radius:4px;")
                : QStringLiteral("background:#3A2418;border:none;border-radius:4px;");
        }
        return stakingMarker
            ? QStringLiteral("background:#E5A15E;border:none;border-radius:4px;")
            : QStringLiteral("background:#B69B82;border:none;border-radius:4px;");
    };
    ui->labelSquarePiv->setStyleSheet(initialMarkerStyle(true));
    ui->labelSquareMN->setStyleSheet(initialMarkerStyle(false));
    if (ui->verticalSpacer_6) {
        ui->verticalLayout_2->removeItem(ui->verticalSpacer_6);
        delete ui->verticalSpacer_6;
        ui->verticalSpacer_6 = nullptr;
    }
    ui->verticalLayout_2->removeWidget(ui->layoutChart);
    ui->verticalLayout_2->removeWidget(ui->emptyContainerChart);

    analyticsLayout->addWidget(rewardSummaryRow);
    analyticsLayout->addWidget(ui->layoutChart, 1);
    analyticsLayout->addWidget(ui->emptyContainerChart, 1);
    ui->verticalLayout_2->addWidget(analyticsModule, 1);

    ui->layoutChart->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->layoutChart, "dashboard-chart-content");
    ui->verticalLayout_8->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout_8->setSpacing(6);
    ui->verticalWidgetChart->setAttribute(Qt::WA_StyledBackground, true);
    ui->verticalWidgetChart->setMinimumHeight(220);
    setCssProperty(ui->verticalWidgetChart, "dashboard-chart-content");
    ui->chartContainer->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->chartContainer, "dashboard-chart-canvas");
    ui->emptyContainerChart->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->emptyContainerChart, "dashboard-chart-content");
    ui->verticalLayout_71->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout_71->setSpacing(0);

    // --- V2: stat row across the top (Available / Staking / Rewards 30d) ---
    auto* statRow = new QWidget(this);
    statRow->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(statRow, "dashboard-stat-row");
    auto* statRowLayout = new QHBoxLayout(statRow);
    statRowLayout->setContentsMargins(20, 12, 20, 10);
    statRowLayout->setSpacing(14);
    auto makeTile = [&](const QString& chipRes, const QString& caption, QLabel*& valueOut) {
        auto* tile = new QWidget(statRow);
        tile->setAttribute(Qt::WA_StyledBackground, true);
        setCssProperty(tile, "dashboard-stat-tile");
        auto* lay = new QHBoxLayout(tile);
        lay->setContentsMargins(12, 10, 16, 10);
        tile->setMinimumWidth(230);
        lay->setSpacing(12);
        auto* chip = new QLabel(tile);
        chip->setFixedSize(44, 44);
        chip->setPixmap(QPixmap(chipRes).scaled(44, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        lay->addWidget(chip, 0, Qt::AlignVCenter);
        auto* texts = new QVBoxLayout();
        texts->setContentsMargins(0, 0, 0, 0);
        texts->setSpacing(2);
        auto* cap = new QLabel(caption.toUpper(), tile);
        setCssProperty(cap, "dashboard-stat-tile-label");
        valueOut = new QLabel("--", tile);
        setCssProperty(valueOut, "dashboard-stat-value");
        texts->addWidget(cap);
        texts->addWidget(valueOut);
        lay->addLayout(texts, 1);
        statRowLayout->addWidget(tile);
    };
    makeTile(":/ic-chip-available", tr("Available"), statValueAvailable);
    makeTile(":/ic-chip-staking", tr("Staking"), statValueStaking);
    makeTile(":/ic-chip-rewards", tr("Rewards 30d"), statValueRewards);
    statRowLayout->addStretch(1);

    // Restructure root: stat row on top, original two columns below
    ui->horizontalLayout_2->removeWidget(ui->left);
    ui->horizontalLayout_2->removeWidget(ui->right);
    auto* columnsLayout = new QHBoxLayout();
    columnsLayout->setContentsMargins(0, 0, 14, 0);
    columnsLayout->setSpacing(14);
    columnsLayout->addWidget(ui->left, 3);
    columnsLayout->addWidget(ui->right, 2);
    delete ui->horizontalLayout_2;
    auto* rootV = new QVBoxLayout(this);
    rootV->setContentsMargins(0, 0, 0, 0);
    rootV->setSpacing(0);
    rootV->addWidget(statRow);
    rootV->addLayout(columnsLayout, 1);

    // --- V2: "Field notes" activity feed, top of the right column (collapsible) ---
    auto* feedCard = new QWidget(ui->right);
    feedCard->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(feedCard, "dashboard-feed-card");
    auto* feedCardLayout = new QVBoxLayout(feedCard);
    feedCardLayout->setContentsMargins(20, 10, 20, 10);
    feedCardLayout->setSpacing(2);
    auto* feedHeader = new QHBoxLayout();
    feedHeader->setContentsMargins(0, 0, 0, 0);
    feedHeader->setSpacing(8);
    auto* feedTitleCol = new QVBoxLayout();
    feedTitleCol->setContentsMargins(0, 0, 0, 0);
    feedTitleCol->setSpacing(1);
    auto* feedTitle = new QLabel(tr("Field notes"), feedCard);
    setCssProperty(feedTitle, "dashboard-feed-title");
    auto* feedSubtitle = new QLabel(tr("recent activity"), feedCard);
    setCssProperty(feedSubtitle, "dashboard-feed-subtitle");
    feedTitleCol->addWidget(feedTitle);
    feedTitleCol->addWidget(feedSubtitle);
    feedHeader->addLayout(feedTitleCol, 1);
    feedToggle = new QPushButton(feedCard);
    feedToggle->setCursor(Qt::PointingHandCursor);
    feedToggle->setFlat(true);
    feedToggle->setFixedSize(28, 28);
    setCssProperty(feedToggle, "dashboard-feed-toggle");
    feedHeader->addWidget(feedToggle, 0, Qt::AlignVCenter);
    feedCardLayout->addLayout(feedHeader);
    feedBody = new QWidget(feedCard);
    feedBody->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(feedBody, "dashboard-feed-body");
    auto* feedBodyLayout = new QVBoxLayout(feedBody);
    feedBodyLayout->setContentsMargins(0, 0, 0, 0);
    feedBodyLayout->setSpacing(0);
    feedRowsLayout = new QVBoxLayout();
    feedRowsLayout->setContentsMargins(0, 6, 0, 0);
    feedRowsLayout->setSpacing(0);
    feedBodyLayout->addLayout(feedRowsLayout);
    feedCardLayout->addWidget(feedBody);
    feedCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    ui->verticalLayout_2->setSpacing(10);
    ui->verticalLayout_2->insertWidget(0, feedCard, 0);
    connect(feedToggle, &QPushButton::clicked, this, [this]() { setFeedExpanded(!feedExpanded); });
    setFeedExpanded(false);
    updateFeedNotes();

#ifdef USE_QTCHARTS
    connect(ui->comboBoxYears, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentTextChanged),
        this, &DashboardWidget::onChartYearChanged);
#else
    // hide charts container if not USE_QTCHARTS
    ui->right->setVisible(false);
#endif // USE_QTCHARTS

    // Sort Transactions
    SortEdit* lineEdit = new SortEdit(ui->comboBoxSort);
    connect(lineEdit, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSort->showPopup();});
    setSortTx(ui->comboBoxSort, lineEdit);
    setCssProperty(ui->comboBoxSort, "dashboard-filter-pill", true);
    setCssProperty(lineEdit, "dashboard-filter-line");
    lineEdit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lineEdit->setFrame(false);
    lineEdit->setTextMargins(0, 0, 0, 0);
    ui->comboBoxSort->setMinimumHeight(38);
    connect(ui->comboBoxSort, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentTextChanged), this, &DashboardWidget::onSortChanged);

    // Sort type
    SortEdit* lineEditType = new SortEdit(ui->comboBoxSortType);
    connect(lineEditType, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSortType->showPopup();});
    setSortTxTypeFilter(ui->comboBoxSortType, lineEditType);
    setCssProperty(ui->comboBoxSortType, "dashboard-filter-pill", true);
    setCssProperty(lineEditType, "dashboard-filter-line");
    lineEditType->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    lineEditType->setFrame(false);
    lineEditType->setTextMargins(0, 0, 0, 0);
    ui->comboBoxSortType->setMinimumHeight(38);
    ui->comboBoxSortType->setCurrentIndex(0);
    connect(ui->comboBoxSortType, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentTextChanged),
        this, &DashboardWidget::onSortTypeChanged);

    // Transactions
    setCssProperty(ui->listTransactions, "dashboard-transactions-list");
    ui->listTransactions->setItemDelegate(txViewDelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listTransactions->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->listTransactions->setUniformItemSizes(true);
    ui->listTransactions->setFrameShape(QFrame::NoFrame);
    ui->listTransactions->setSpacing(0);
    ui->listTransactions->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->listTransactions->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // Sync Warning
    ui->layoutWarning->setVisible(true);
    ui->containerWarning->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->containerWarning, "dashboard-warning-pill");
    ui->verticalLayout_21->setContentsMargins(0, 6, 0, 0);
    ui->horizontalLayout11->setContentsMargins(14, 10, 14, 10);
    ui->horizontalLayout11->setSpacing(0);
    ui->lblWarning->setText(tr("Please wait until the wallet is fully synced to see your correct balance"));
    setCssProperty(ui->lblWarning, "dashboard-warning-text");
    setCssProperty(ui->imgWarning, "ic-warning");

    //Empty List
    ui->emptyContainer->setVisible(false);
    setCssProperty(ui->pushImgEmpty, "img-empty-transactions");
    setCssProperty(ui->labelEmpty, "text-empty");
    setCssProperty(ui->pushImgEmptyChart, "img-empty-staking-on");

    setCssProperty(ui->labelEmptyChart, "text-empty");
    setCssSubtitleScreen(ui->labelMessageEmpty);

    // Chart State
    ui->layoutChart->setVisible(false);
    ui->emptyContainerChart->setVisible(true);
    setShadow(ui->layoutShadow);

    connect(ui->listTransactions, &QListView::clicked, this, &DashboardWidget::handleTransactionClicked);

bool hasCharts = false;
#ifdef USE_QTCHARTS
    hasCharts = true;
    isLoading = false;
    setChartShow(YEAR);
    connect(ui->pushButtonYear, &QPushButton::clicked, [this](){setChartShow(YEAR);});
    connect(ui->pushButtonMonth, &QPushButton::clicked, [this](){setChartShow(MONTH);});
    connect(ui->pushButtonAll, &QPushButton::clicked, [this](){setChartShow(ALL);});
    if (window)
        connect(window, &OrganicLifeGUI::windowResizeEvent, this, &DashboardWidget::windowResizeEvent);
#endif

    if (hasCharts) {
        ui->labelEmptyChart->setText(tr("You have no staking rewards"));
    } else {
        ui->labelEmptyChart->setText(tr("No charts library"));
    }
}

void DashboardWidget::handleTransactionClicked(const QModelIndex &index)
{
    ui->listTransactions->setCurrentIndex(index);
    QModelIndex rIndex = filter->mapToSource(index);

    window->showHide(true);
    TxDetailDialog *dialog = new TxDetailDialog(window, false);
    dialog->setData(walletModel, rIndex);
    openDialogWithOpaqueBackgroundY(dialog, window, 3, 17);

    // Back to regular status
    ui->listTransactions->scrollTo(index);
    ui->listTransactions->clearSelection();
    ui->listTransactions->setFocus();
    dialog->deleteLater();
}

void DashboardWidget::loadWalletModel()
{
    if (walletModel && walletModel->getOptionsModel()) {
        txModel = walletModel->getTransactionTableModel();
        // Set up transaction list
        filter = new TransactionFilterProxy(this);
        filter->setDynamicSortFilter(true);
        filter->setSortCaseSensitivity(Qt::CaseInsensitive);
        filter->setFilterCaseSensitivity(Qt::CaseInsensitive);
        filter->setSortRole(Qt::EditRole);

        // Read filter settings
        QSettings settings;
        quint32 filterByType = settings.value("transactionType", TransactionFilterProxy::ALL_TYPES).toInt();
        int filterIndex = ui->comboBoxSortType->findData(filterByType); // Find index
        filterByType = (filterIndex == -1) ? TransactionFilterProxy::ALL_TYPES : filterByType;
        filter->setTypeFilter(filterByType); // Set filter
        ui->comboBoxSortType->setCurrentIndex(filterIndex); // Set item in ComboBox
        // Read sort settings
        changeSort(settings.value("transactionSort", SortTx::DATE_DESC).toInt());

        filter->setSourceModel(txModel);
        txHolder->setFilter(filter);
        ui->listTransactions->setModel(filter);
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);
        updateTransactionViewState(txModel->size() > 0, filter->rowCount() > 0);

        connect(ui->pushImgEmpty, &QPushButton::clicked, [this](){window->openFAQ();});
        connect(txModel, &TransactionTableModel::txArrived, this, &DashboardWidget::onTxArrived);

        // Capture current viewport before insertion so we can animate old list content shifting down.
        connect(txModel, &TransactionTableModel::rowsAboutToBeInserted, this, &DashboardWidget::prepareTransactionInsertionAnimation);
        // Notification pop-up for new transaction
        connect(txModel, &TransactionTableModel::rowsInserted, this, &DashboardWidget::processNewTransaction);
#ifdef USE_QTCHARTS
        onHideChartsChanged(walletModel->getOptionsModel()->isHideCharts());
        connect(walletModel->getOptionsModel(), &OptionsModel::hideChartsChanged, this,
                &DashboardWidget::onHideChartsChanged);
#endif
        // V2 stat row data
        connect(walletModel, &WalletModel::balanceChanged, this, &DashboardWidget::updateStatBalances);
        updateStatBalances(interfaces::WalletBalances());
        updateStatRewards();
        updateFeedNotes();
    }
    // update the display unit, to not use the default ("PIV")
    updateDisplayUnit();
}

void DashboardWidget::clearWalletModel()
{
    if (txRowFadeAnimation) {
        txRowFadeAnimation->stop();
        txRowFadeAnimation.clear();
    }
    if (animatedTxProxyIndex.isValid()) {
        txViewDelegate->clearTransientRowOpacity(QModelIndex(animatedTxProxyIndex));
        animatedTxProxyIndex = QPersistentModelIndex();
    }
    txInsertAnimationPending = false;
    preInsertViewportSnapshot = QPixmap();

    if (ui && ui->listTransactions) {
        ui->listTransactions->clearSelection();
        ui->listTransactions->setModel(nullptr);
    }

    if (filter) {
        filter->setSourceModel(nullptr);
        filter->deleteLater();
        filter = nullptr;
    }
    txHolder->setFilter(nullptr);
    txModel = nullptr;

#ifdef USE_QTCHARTS
    if (stakesFilter) {
        stakesFilter->setSourceModel(nullptr);
    }
    hasStakes = false;
    filterUpdateNeeded = false;
#endif

    updateTransactionViewState(false, false);
    PWidget::clearWalletModel();
}

void DashboardWidget::onTxArrived(const QString& hash, const bool isCoinStake, const bool isMNReward, const bool isCSAnyType)
{
    showList();
    updateFeedNotes();
    if (!isVisible()) return;
#ifdef USE_QTCHARTS
    if (isCoinStake || isMNReward) {
        // Update value if this is our first stake/reward
        if (!hasStakes && stakesFilter)
            hasStakes = stakesFilter->rowCount() > 0;
        tryChartRefresh();
    }
#endif
}

void DashboardWidget::showList()
{
    const bool hasTransactions = txModel && txModel->size() > 0;
    const bool hasVisibleTransactions = filter && filter->rowCount() > 0;
    updateTransactionViewState(hasTransactions, hasVisibleTransactions);
}

void DashboardWidget::setFeedExpanded(bool expanded)
{
    feedExpanded = expanded;
    if (feedBody) feedBody->setVisible(expanded);
    if (feedToggle) feedToggle->setText(expanded ? "▾" : "▸");
}

void DashboardWidget::updateFeedNotes()
{
    if (!feedRowsLayout) return;
    while (QLayoutItem* item = feedRowsLayout->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    auto addEmpty = [this]() {
        auto* empty = new QLabel(tr("no activity yet"), this);
        setCssProperty(empty, "dashboard-feed-empty");
        feedRowsLayout->addWidget(empty);
    };

    if (!filter || filter->rowCount() == 0) { addEmpty(); return; }

    const int unit = walletModel && walletModel->getOptionsModel()
        ? walletModel->getOptionsModel()->getDisplayUnit() : 0;
    const int rows = std::min(3, filter->rowCount());
    for (int i = 0; i < rows; ++i) {
        const QModelIndex idx = filter->index(i, TransactionTableModel::ToAddress);
        if (!idx.isValid()) continue;
        const int type = idx.data(TransactionTableModel::TypeRole).toInt();
        const CAmount amount = idx.data(TransactionTableModel::AmountRole).toLongLong();
        const QDateTime date = idx.data(TransactionTableModel::DateRole).toDateTime();

        QString label, color;
        switch (type) {
            case TransactionRecord::MNReward:      label = tr("MN reward");    color = "#8FB35F"; break;
            case TransactionRecord::StakeMint:
            case TransactionRecord::StakeZPIV:
            case TransactionRecord::StakeDelegated:
            case TransactionRecord::StakeHot:      label = tr("Stake found");  color = "#B77E35"; break;
            case TransactionRecord::Generated:     label = tr("Mined");        color = "#B77E35"; break;
            default:
                if (amount >= 0) { label = tr("Received"); color = "#4F7A2E"; }
                else             { label = tr("Sent");     color = "#B0502E"; }
                break;
        }

        auto* row = new QWidget(this);
        row->setAttribute(Qt::WA_StyledBackground, true);
        setCssProperty(row, "dashboard-feed-row");
        row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(2, 7, 2, 7);
        lay->setSpacing(10);
        auto* dot = new QLabel(row);
        dot->setFixedSize(10, 10);
        dot->setStyleSheet(QString("background:%1;border-radius:5px;").arg(color));
        lay->addWidget(dot, 0, Qt::AlignVCenter);
        auto* texts = new QVBoxLayout();
        texts->setContentsMargins(0, 0, 0, 0);
        texts->setSpacing(1);
        auto* typeLabel = new QLabel(label, row);
        typeLabel->setStyleSheet(QString("color:%1;font-weight:600;font-size:13px;background:transparent;")
                                 .arg(isLightTheme() ? "#22331a" : "#f4f6e2"));
        auto* dateLabel = new QLabel(date.date().toString("MMM d"), row);
        dateLabel->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;")
                                 .arg(isLightTheme() ? "#5c6b4a" : "#c2c7ae"));
        texts->addWidget(typeLabel);
        texts->addWidget(dateLabel);
        lay->addLayout(texts, 1);
        auto* amountLabel = new QLabel(QString("%1%2").arg(amount >= 0 ? "+" : "")
                                       .arg(GUIUtil::formatBalance(std::abs(amount), unit)), row);
        amountLabel->setStyleSheet(QString("color:%1;font-weight:700;font-size:13px;background:transparent;").arg(color));
        lay->addWidget(amountLabel, 0, Qt::AlignVCenter | Qt::AlignRight);
        feedRowsLayout->addWidget(row);
    }
}

void DashboardWidget::updateStatBalances(const interfaces::WalletBalances&)
{
    if (!statValueAvailable || !walletModel) return;
    const int unit = walletModel->getOptionsModel()->getDisplayUnit();
    statValueAvailable->setText(GUIUtil::formatBalance(walletModel->getBalance(), unit));
    statValueStaking->setText(GUIUtil::formatBalance(walletModel->getDelegatedBalance(), unit));
}

void DashboardWidget::updateStatRewards()
{
    if (!statValueRewards || !walletModel) return;
    const QDate cutoff = QDate::currentDate().addDays(-30);
    CAmount total = 0;
    for (const auto& sample : chartStakeRowsSnapshot) {
        if (QDate(sample.year, sample.month, sample.day) >= cutoff) total += sample.amount;
    }
    const int unit = walletModel->getOptionsModel()->getDisplayUnit();
    statValueRewards->setText(GUIUtil::formatBalance(total, unit));
}

void DashboardWidget::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
        txHolder->setDisplayUnit(nDisplayUnit);
        ui->listTransactions->update();
    }
}

void DashboardWidget::onSortChanged(const QString& value)
{
    if (!filter) return;

    if (!value.isNull()) {
        changeSort(ui->comboBoxSort->currentIndex());
    } else {
        changeSort(SortTx::DATE_DESC);
    }
}

void DashboardWidget::changeSort(int nSortIndex)
{
    int nColumnIndex = TransactionTableModel::Date;
    Qt::SortOrder order = Qt::DescendingOrder;

    switch (nSortIndex) {
        case SortTx::DATE_DESC:
        {
            nColumnIndex = TransactionTableModel::Date;
            break;
        }
        case SortTx::DATE_ASC:
        {
            nColumnIndex = TransactionTableModel::Date;
            order = Qt::AscendingOrder;
            break;
        }
        case SortTx::AMOUNT_DESC:
        {
            nColumnIndex = TransactionTableModel::Amount;
            break;
        }
        case SortTx::AMOUNT_ASC:
        {
            nColumnIndex = TransactionTableModel::Amount;
            order = Qt::AscendingOrder;
            break;
        }
    }

    ui->comboBoxSort->setCurrentIndex(nSortIndex);
    filter->sort(nColumnIndex, order);

    // Store settings
    QSettings settings;
    settings.setValue("transactionSort", nSortIndex);
}

void DashboardWidget::onSortTypeChanged(const QString& value)
{
    if (!filter) return;
    int filterIndex = ui->comboBoxSortType->currentIndex();
    int filterByType = ui->comboBoxSortType->itemData(filterIndex).toInt();

    filter->setTypeFilter(filterByType);
    ui->listTransactions->update();
    showList();

    // Store settings
    QSettings settings;
    settings.setValue("transactionType", filterByType);
}

void DashboardWidget::updateTransactionViewState(bool hasTransactions, bool hasVisibleTransactions)
{
    if (!ui) return;

    ui->emptyContainer->setVisible(!hasVisibleTransactions);
    ui->listTransactions->setVisible(hasVisibleTransactions);
    ui->comboBoxSortType->setVisible(hasTransactions);
    ui->comboBoxSort->setVisible(hasTransactions);
}

void DashboardWidget::walletSynced(bool sync)
{
    if (this->isSync != sync) {
        this->isSync = sync;
        ui->layoutWarning->setVisible(!this->isSync);
#ifdef USE_QTCHARTS
        if (!isVisible()) return;
        tryChartRefresh();
#endif
    }
}

void DashboardWidget::changeTheme(bool isLightTheme, QString& theme)
{
    static_cast<TxViewHolder*>(this->txViewDelegate->getRowFactory())->isLightTheme = isLightTheme;
    if (ui && ui->listTransactions) ui->listTransactions->viewport()->update();
#ifdef USE_QTCHARTS
    if (chart) this->changeChartColors();
#endif
}

#ifdef USE_QTCHARTS

void DashboardWidget::tryChartRefresh()
{
    if (!fShowCharts)
        return;
    if (hasStakes) {
        // First check that everything was loaded properly.
        if (!chart) {
            loadChart();
        } else {
            // Check for min update time to not reload the UI so often if the node is syncing.
            int64_t now = GetTime();
            int chartLoadIntervalTime = CHART_LOAD_MIN_TIME_INTERVAL;
            if (clientModel->inInitialBlockDownload()) chartLoadIntervalTime *= 6; // 90 seconds update
            if (lastRefreshTime + chartLoadIntervalTime < now) {
                lastRefreshTime = now;
                refreshChart();
            }
        }
    }
}

void DashboardWidget::setChartShow(ChartShowType type)
{
    this->chartShow = type;
    if (chartShow == MONTH) {
        ui->containerChartArrow->setVisible(true);
        resolveMonthWindowForFilters();
    } else {
        ui->containerChartArrow->setVisible(false);
    }
    updateMonthArrowState();
    if (isChartInitialized) refreshChart();
}

const QStringList monthsNames = {QObject::tr("Jan"), QObject::tr("Feb"), QObject::tr("Mar"), QObject::tr("Apr"),
                                 QObject::tr("May"), QObject::tr("Jun"), QObject::tr("Jul"), QObject::tr("Aug"),
                                 QObject::tr("Sep"), QObject::tr("Oct"), QObject::tr("Nov"), QObject::tr("Dec")};

void DashboardWidget::loadChart()
{
    if (hasStakes) {
        if (!chart) {
            showHideEmptyChart(false, false);
            initChart();
            QDate currentDate = QDate::currentDate();
            monthFilter = currentDate.month();
            yearFilter = currentDate.year();
            for (int i = 1; i < 13; ++i) ui->comboBoxMonths->addItem(QString(monthsNames[i-1]), QVariant(i));
            ui->comboBoxMonths->setCurrentIndex(monthFilter - 1);
            connect(ui->comboBoxMonths, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentTextChanged),
                this, &DashboardWidget::onChartMonthChanged);
            connect(ui->pushButtonChartArrow, &QPushButton::clicked, [this](){ onChartArrowClicked(true); });
            connect(ui->pushButtonChartRight, &QPushButton::clicked, [this](){ onChartArrowClicked(false); });
            resolveMonthWindowForFilters();
            updateMonthArrowState();
        }
        refreshChart();
        changeChartColors();
    } else {
        showHideEmptyChart(true, false);
    }
}

void DashboardWidget::showHideEmptyChart(bool showEmpty, bool loading, bool forceView)
{
    const bool keepChartVisibleWhileLoading = loading && chartHasRenderedData;
    if ((stakesFilter && stakesFilter->rowCount() > SHOW_EMPTY_CHART_VIEW_THRESHOLD) || forceView) {
        ui->layoutChart->setVisible(keepChartVisibleWhileLoading || !showEmpty);
        ui->emptyContainerChart->setVisible(!keepChartVisibleWhileLoading && showEmpty);
    }
    // Enable/Disable sort buttons
    bool invLoading = !loading;
    ui->comboBoxMonths->setEnabled(invLoading);
    ui->comboBoxYears->setEnabled(invLoading);
    ui->pushButtonMonth->setEnabled(invLoading);
    ui->pushButtonAll->setEnabled(invLoading);
    ui->pushButtonYear->setEnabled(invLoading);
    ui->labelEmptyChart->setText(loading ? tr("Loading chart..") : tr("You have no staking rewards"));
}

void DashboardWidget::initChart()
{
    chart = new QChart();
    axisX = new QBarCategoryAxis();
    axisY = new QValueAxis();

    // Chart style
    chart->legend()->setVisible(false);
    chart->legend()->setAlignment(Qt::AlignTop);
    chart->layout()->setContentsMargins(0, 0, 0, 0);
    chart->setMargins({0, 0, 0, 0});
    chart->setBackgroundRoundness(0);
    chart->setAnimationOptions(QChart::NoAnimation);
    chart->setAnimationDuration(0);
    // Axis
    chart->addAxis(axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignRight);
    axisY->setTickCount(5);
    axisY->setMinorTickCount(0);
    axisX->setGridLineVisible(false);
    axisY->setGridLineVisible(true);

    chartView = new QChartView(chart);
    chartView->setRenderHint(QPainter::Antialiasing);
    chartView->setRubberBand(QChartView::HorizontalRubberBand);
    chartView->setContentsMargins(0,0,0,0);
    chartView->setMinimumHeight(120);

    QHBoxLayout *baseScreensContainer = new QHBoxLayout();
    baseScreensContainer->setContentsMargins(0, 0, 0, 0);
    baseScreensContainer->addWidget(chartView);
    ui->chartContainer->setLayout(baseScreensContainer);
    ui->chartContainer->setContentsMargins(0,0,0,0);
    setCssProperty(ui->chartContainer, "dashboard-chart-canvas");
}

void DashboardWidget::changeChartColors()
{
    QColor gridLineColorX;
    QColor linePenColor;
    QColor labelColor;
    QColor backgroundColor;
    QColor gridY;
    QColor pivHintColor;
    QColor mnHintColor;
    // Harvest palette anchors for the V2 area chart.
    if (isLightTheme()) {
        gridLineColorX = QColor("#E3E8CE");
        gridY = QColor("#E3E8CE");
        linePenColor = QColor("#B9C49A");
        labelColor = QColor("#7E7A62");
        backgroundColor = QColor("#FFFFFF");
        pivHintColor = QColor("#4F7A2E");
        mnHintColor = QColor("#C4552A");
        axisY->setGridLineColor(gridY);
        axisY->setMinorGridLineColor(QColor("#EFF2DF"));
    } else {
        gridLineColorX = QColor("#3B4426");
        gridY = QColor("#3B4426");
        linePenColor = QColor("#55613F");
        labelColor = QColor("#C2C7AE");
        backgroundColor = QColor("#0D1008");
        pivHintColor = QColor("#8FB35F");
        mnHintColor = QColor("#D08A63");
        axisY->setGridLineColor(gridY);
        axisY->setMinorGridLineColor(QColor("#1A2010"));
    }

    axisX->setGridLineColor(gridLineColorX);
    axisX->setLinePenColor(linePenColor);
    axisY->setLinePenColor(linePenColor);
    axisX->setLabelsColor(labelColor);
    axisY->setLabelsColor(labelColor);
    chart->setBackgroundBrush(QBrush(backgroundColor));
    chart->setPlotAreaBackgroundVisible(true);
    chart->setPlotAreaBackgroundBrush(QBrush(backgroundColor));
    chart->setPlotAreaBackgroundPen(QPen(QColor(0, 0, 0, 0)));
    ui->labelPiv->setStyleSheet(QString("color:%1;").arg(pivHintColor.name()));
    ui->labelMN->setStyleSheet(QString("color:%1;").arg(mnHintColor.name()));
    const auto markerStyle = [](const QColor& color) {
        return QString("background:%1;border:none;border-radius:4px;").arg(color.name());
    };
    ui->labelSquarePiv->setStyleSheet(markerStyle(pivHintColor));
    ui->labelSquareMN->setStyleSheet(markerStyle(mnHintColor));
    if (stakesLine) {
        QPen linePen(pivHintColor);
        linePen.setWidth(2);
        stakesLine->setPen(linePen);
    }
    if (areaStakes) {
        QLinearGradient fillGrad(0, 0, 0, 1);
        fillGrad.setCoordinateMode(QGradient::ObjectBoundingMode);
        QColor top = pivHintColor;
        top.setAlpha(isLightTheme() ? 72 : 96);
        QColor bottom = pivHintColor;
        bottom.setAlpha(8);
        fillGrad.setColorAt(0, top);
        fillGrad.setColorAt(1, bottom);
        areaStakes->setBrush(QBrush(fillGrad));
        QPen areaPen(pivHintColor);
        areaPen.setWidth(2);
        areaStakes->setPen(areaPen);
    }
    if (mnLine) {
        QPen mnPen(mnHintColor);
        mnPen.setWidth(2);
        mnLine->setPen(mnPen);
    }
}

void DashboardWidget::updateStakeFilter()
{
    if (!stakesFilter) return;
    if (chartShow != ALL) {
        bool filterByMonth = false;
        if (monthFilter != 0 && chartShow == MONTH) {
            filterByMonth = true;
        }
        if (yearFilter != 0) {
            if (filterByMonth) {
                QDate monthFirst = QDate(yearFilter, monthFilter, 1);
                QDate monthLast = QDate(yearFilter, monthFilter, monthFirst.daysInMonth());
                stakesFilter->setDateRange(
                        QDateTime(monthFirst, QTime(0, 0, 0)),
                        QDateTime(monthLast, QTime(23, 59, 59))
                );
            } else {
                stakesFilter->setDateRange(
                        QDateTime(QDate(yearFilter, 1, 1), QTime(0, 0, 0)),
                        QDateTime(QDate(yearFilter, 12, 31), QTime(23, 59, 59))
                );
            }
        } else if (filterByMonth) {
            QDate currentDate = QDate::currentDate();
            QDate monthFirst = QDate(currentDate.year(), monthFilter, 1);
            QDate monthLast = QDate(currentDate.year(), monthFilter, monthFirst.daysInMonth());
            stakesFilter->setDateRange(
                    QDateTime(monthFirst, QTime(0, 0, 0)),
                    QDateTime(monthLast, QTime(23, 59, 59))
            );
            ui->comboBoxYears->setCurrentText(QString::number(currentDate.year()));
        } else {
            stakesFilter->clearDateRange();
        }
    } else {
        stakesFilter->clearDateRange();
    }
}

void DashboardWidget::snapshotChartRows()
{
    Q_ASSERT(QThread::currentThread() == thread());
    chartStakeRowsSnapshot.clear();

    if (!stakesFilter) return;

    if (filterUpdateNeeded) {
        filterUpdateNeeded = false;
        updateStakeFilter();
    }

    const int size = stakesFilter->rowCount();
    chartStakeRowsSnapshot.reserve(size);
    for (int i = 0; i < size; ++i) {
        const QModelIndex modelIndex = stakesFilter->index(i, TransactionTableModel::ToAddress);
        if (!modelIndex.isValid()) continue;

        const QDate date = modelIndex.data(TransactionTableModel::DateRole).toDateTime().date();
        if (!date.isValid()) continue;

        chartStakeRowsSnapshot.push_back({
            date.year(),
            date.month(),
            date.day(),
            modelIndex.data(TransactionTableModel::AmountRole).toLongLong(),
            modelIndex.data(TransactionTableModel::TypeRole).toInt()
        });
    }
}

// pair PIV, MN Reward
QMap<int, std::pair<qint64, qint64>> DashboardWidget::getAmountBy()
{
    QMap<int, std::pair<qint64, qint64>> amountBy;
    ChartBucketMode mode;
    if (!ToChartBucketMode(chartShow, mode)) {
        inform(tr("Error loading chart, invalid show option"));
        return amountBy;
    }

    const ChartRewardAggregation aggregation = AggregateChartRewards(chartStakeRowsSnapshot, mode);
    hasMNRewards = aggregation.hasMasternodeRewards;
    for (const auto& item : aggregation.amountsBy) {
        amountBy.insert(item.first, {
            static_cast<qint64>(item.second.first),
            static_cast<qint64>(item.second.second)
        });
    }
    return amountBy;
}

bool DashboardWidget::loadChartData(bool withMonthNames)
{
    if (chartData) {
        delete chartData;
        chartData = nullptr;
    }

    chartData = new ChartData();
    chartData->amountsByCache = getAmountBy(); // pair PIV, MN Reward

    std::pair<int,int> range = getChartRange(chartData->amountsByCache);
    if (range.first == 0 && range.second == 0) {
        // Problem loading the chart.
        return false;
    }

    for (int num = range.first; num < range.second; num++) {
        qreal piv = 0;
        qreal mn = 0;
        if (chartData->amountsByCache.contains(num)) {
            std::pair <qint64, qint64> pair = chartData->amountsByCache[num];
            piv = (pair.first != 0) ? pair.first / 100000000 : 0;
            mn = (pair.second != 0) ? pair.second / 100000000 : 0;
            chartData->totalPiv += pair.first;
            chartData->totalMN += pair.second;
        }

        chartData->xLabels << ((withMonthNames) ? monthsNames[num - 1] : QString::number(num));

        chartData->valuesPiv.append(piv);
        chartData->valuesMN.append(mn);

        qreal max = std::max(piv, mn);
        if (max > chartData->maxValue) {
            chartData->maxValue = max;
        }
    }

    return true;
}

void DashboardWidget::onChartYearChanged(const QString& yearStr)
{
    if (isChartInitialized) {
        int newYear = yearStr.toInt();
        if (newYear != yearFilter) {
            yearFilter = newYear;
            if (chartShow == MONTH) {
                resolveMonthWindowForFilters();
            }
            filterUpdateNeeded = true;
            refreshChart();
        }
    }
}

void DashboardWidget::onChartMonthChanged(const QString& monthStr)
{
    Q_UNUSED(monthStr);
    if (isChartInitialized) {
        int newMonth = ui->comboBoxMonths->currentData().toInt();
        if (newMonth != monthFilter) {
            monthFilter = newMonth;
            if (chartShow == MONTH) {
                resolveMonthWindowForFilters();
            }
            filterUpdateNeeded = true;
            refreshChart();
        }
    }
}

bool DashboardWidget::refreshChart()
{
    if (isLoading) return false;
    isLoading = true;
    isChartMin = width() < 1300;
    isChartInitialized = false;
    snapshotChartRows();
    showHideEmptyChart(!chartHasRenderedData, true, true);
    return execute(REQUEST_LOAD_TASK);
}

void DashboardWidget::onChartRefreshed()
{
    if (chart && axisX) {
        axisX->clear();
    }

    // V2 harvest chart: filled area for stakes, accent line for masternodes
    if (!stakesLine) {
        stakesLine = new QLineSeries();
        areaStakes = new QAreaSeries(stakesLine);
        areaStakes->setName(tr("Stakes"));
        chart->addSeries(areaStakes);
        areaStakes->attachAxis(axisX);
        areaStakes->attachAxis(axisY);
    }
    if (!mnLine) {
        mnLine = new QLineSeries();
        mnLine->setName(tr("MN"));
        chart->addSeries(mnLine);
        mnLine->attachAxis(axisX);
        mnLine->attachAxis(axisY);
    }
    changeChartColors();

    QList<qreal> targetPivValues = chartData->valuesPiv;
    QList<qreal> targetMnValues = chartData->valuesMN;
    const int targetSize = std::max(targetPivValues.size(), targetMnValues.size());
    targetPivValues = NormalizeValues(targetPivValues, targetSize);
    targetMnValues = NormalizeValues(targetMnValues, targetSize);

    const qreal maxForRange = std::max(MaxValue(targetPivValues), MaxValue(targetMnValues));

    // Total
    nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
    if (chartData->totalPiv > 0 || chartData->totalMN > 0) {
        setCssProperty(ui->labelAmountPiv,
                       chartData->totalPiv > 0 ? "dashboard-reward-stat-stakes" : "dashboard-reward-stat-muted");
        setCssProperty(ui->labelAmountMN,
                       chartData->totalMN > 0 ? "dashboard-reward-stat-mn" : "dashboard-reward-stat-muted");
    } else {
        setCssProperty(ui->labelAmountPiv, "dashboard-reward-stat-muted");
        setCssProperty(ui->labelAmountMN, "dashboard-reward-stat-muted");
    }
    forceUpdateStyle({ui->labelAmountPiv, ui->labelAmountMN});
    ui->labelAmountPiv->setText(GUIUtil::formatBalance(chartData->totalPiv, nDisplayUnit));
    ui->labelAmountMN->setText(GUIUtil::formatBalance(chartData->totalMN, nDisplayUnit));

    axisX->append(chartData->xLabels);
    const ChartAxisSpec axisSpec = ComputeNiceAxisSpec(std::max(0.0, static_cast<double>(maxForRange)), 5);
    axisY->setRange(0.0, axisSpec.top);
    axisY->setTickCount(axisSpec.tickCount);
    if (axisSpec.step < 1.0) {
        axisY->setLabelFormat("%.2f");
    } else if (axisSpec.step < 10.0) {
        axisY->setLabelFormat("%.1f");
    } else {
        axisY->setLabelFormat("%.0f");
    }

    ReplaceLineSeriesValues(stakesLine, targetPivValues);
    ReplaceLineSeriesValues(mnLine, targetMnValues);
    mnLine->setVisible(hasMNRewards);
    updateStatRewards();

    bool hasVisibleValues = false;
    for (const qreal value : targetPivValues) {
        if (value > 0.0) { hasVisibleValues = true; break; }
    }
    if (!hasVisibleValues) {
        for (const qreal value : targetMnValues) {
            if (value > 0.0) { hasVisibleValues = true; break; }
        }
    }

    // Controllers
    switch (chartShow) {
        case ALL: {
            ui->container_chart_dropboxes->setVisible(false);
            break;
        }
        case YEAR: {
            ui->container_chart_dropboxes->setVisible(true);
            ui->containerBoxMonths->setVisible(false);
            break;
        }
        case MONTH: {
            ui->container_chart_dropboxes->setVisible(true);
            ui->containerBoxMonths->setVisible(true);
            break;
        }
        default: break;
    }

    // Refresh years filter, first address created is the start
    int yearStart = GUIUtil::dateTimeFromTimeT(static_cast<qint64>(walletModel->getCreationTime())).date().year();
    int currentYear = QDateTime::currentDateTime().date().year();

    QString selection;
    if (ui->comboBoxYears->count() > 0) {
        selection = ui->comboBoxYears->currentText();
        isChartInitialized = false;
    }
    ui->comboBoxYears->clear();
    if (yearStart == currentYear) {
        ui->comboBoxYears->addItem(QString::number(currentYear));
    } else {
        for (int i = yearStart; i < (currentYear + 1); ++i)ui->comboBoxYears->addItem(QString::number(i));
    }

    if (!selection.isEmpty()) {
        ui->comboBoxYears->setCurrentText(selection);
        isChartInitialized = true;
    } else {
        ui->comboBoxYears->setCurrentText(QString::number(currentYear));
    }

    updateMonthArrowState();

    const bool hasFilteredData = !chartData->amountsByCache.isEmpty();
    const bool showEmptyChart = ShouldShowEmptyChart(hasVisibleValues, hasFilteredData, chartShow == MONTH);

    // back to normal
    isChartInitialized = true;
    chartHasRenderedData = !showEmptyChart;
    showHideEmptyChart(showEmptyChart, false, true);
    isLoading = false;
}

std::pair<int, int> DashboardWidget::getChartRange(const QMap<int, std::pair<qint64, qint64>>& amountsBy)
{
    switch (chartShow) {
        case YEAR:
            return std::make_pair(1, 13);
        case ALL: {
            QList<int> keys = amountsBy.keys();
            if (keys.isEmpty()) {
                // This should never happen, ALL means from the beginning of time and if this is called then it must have at least one stake..
                inform(tr("Error loading chart, invalid data"));
                return std::make_pair(0, 0);
            }
            std::sort(keys.begin(), keys.end());
            return std::make_pair(keys.first(), keys.last() + 1);
        }
        case MONTH:
            return std::make_pair(visibleDayFirst, visibleDayLast + 1);
        default:
            inform(tr("Error loading chart, invalid show option"));
            return std::make_pair(0, 0);
    }
}

void DashboardWidget::updateAxisX(const QStringList* args)
{
    axisX->clear();
    QStringList months;
    std::pair<int,int> range = getChartRange(chartData->amountsByCache);
    if (args) {
        months = *args;
    } else {
        for (int i = range.first; i < range.second; i++) months << QString::number(i);
    }
    axisX->append(months);
}

void DashboardWidget::onChartArrowClicked(bool goLeft)
{
    if (chartShow != MONTH) return;
    pageMonthWindow(goLeft);
    filterUpdateNeeded = true;
    refreshChart();
}

int DashboardWidget::monthDaysInFilter() const
{
    const int safeYear = std::max(2000, yearFilter);
    const int safeMonth = std::max(1, std::min(12, monthFilter));
    const QDate monthDate(safeYear, safeMonth, 1);
    return monthDate.isValid() ? monthDate.daysInMonth() : 30;
}

void DashboardWidget::resolveMonthWindowForFilters()
{
    if (chartShow != MONTH) return;

    const int daysInMonth = monthDaysInFilter();
    const QDate currentDate = QDate::currentDate();
    int anchorDay = daysInMonth;
    if (yearFilter == currentDate.year() && monthFilter == currentDate.month()) {
        anchorDay = currentDate.day();
    }

    const WeekWindow window = ResolveMonthWeekWindow(anchorDay, daysInMonth, weekSpanDays);
    visibleDayFirst = window.firstDay;
    visibleDayLast = window.lastDay;
    updateMonthArrowState();
}

void DashboardWidget::pageMonthWindow(bool goLeft)
{
    if (chartShow != MONTH) return;

    const int daysInMonth = monthDaysInFilter();
    if (goLeft) {
        visibleDayFirst = std::max(1, visibleDayFirst - weekSpanDays);
    } else {
        const int maxStart = std::max(1, daysInMonth - weekSpanDays + 1);
        visibleDayFirst = std::min(maxStart, visibleDayFirst + weekSpanDays);
    }
    visibleDayLast = std::min(daysInMonth, visibleDayFirst + weekSpanDays - 1);
    updateMonthArrowState();
}

void DashboardWidget::updateMonthArrowState()
{
    const bool monthView = chartShow == MONTH;
    ui->pushButtonChartArrow->setEnabled(monthView && visibleDayFirst > 1);
    ui->pushButtonChartRight->setEnabled(monthView && visibleDayLast < monthDaysInFilter());
}

void DashboardWidget::windowResizeEvent(QResizeEvent* event)
{
    if (hasStakes && axisX) {
        if (width() > 1300) {
            if (isChartMin) {
                isChartMin = false;
                switch (chartShow) {
                    case YEAR: {
                        updateAxisX(&monthsNames);
                        break;
                    }
                    case ALL: break;
                    case MONTH: {
                        updateAxisX();
                        break;
                    }
                    default:
                        inform(tr("Error loading chart, invalid show option"));
                        return;
                }
                chartView->repaint();
            }
        } else {
            if (!isChartMin) {
                updateAxisX();
                isChartMin = true;
            }
        }
    }
}

void DashboardWidget::onHideChartsChanged(bool fHide)
{
    fShowCharts = !fHide;

    if (fShowCharts) {
        if (!stakesFilter) {
            stakesFilter = new TransactionFilterProxy(this);
            stakesFilter->setDynamicSortFilter(false);
            stakesFilter->setSortCaseSensitivity(Qt::CaseInsensitive);
            stakesFilter->setFilterCaseSensitivity(Qt::CaseInsensitive);
            stakesFilter->setTypeFilter(TransactionFilterProxy::TYPE(TransactionRecord::StakeMint) |
                                        TransactionFilterProxy::TYPE(TransactionRecord::StakeZPIV) |
                                        TransactionFilterProxy::TYPE(TransactionRecord::StakeDelegated) |
                                        TransactionFilterProxy::TYPE(TransactionRecord::MNReward) |
                                        TransactionFilterProxy::TYPE(TransactionRecord::BudgetPayment));
        }
        stakesFilter->setSourceModel(txModel);
        hasStakes = stakesFilter->rowCount() > 0;
        filterUpdateNeeded = true;
    } else {
        if (stakesFilter) {
            stakesFilter->setSourceModel(nullptr);
        }
    }

    // Hide charts if requested
    ui->right->setVisible(fShowCharts);
    if (fShowCharts) tryChartRefresh();
}

#endif

void DashboardWidget::run(int type)
{
#ifdef USE_QTCHARTS
    if (type == REQUEST_LOAD_TASK) {
        bool withMonthNames = !isChartMin && (chartShow == YEAR);
        if (loadChartData(withMonthNames))
            QMetaObject::invokeMethod(this, "onChartRefreshed", Qt::QueuedConnection);
    }
#endif
}
void DashboardWidget::onError(QString error, int type)
{
    inform(tr("Error loading chart: %1").arg(error));
}

void DashboardWidget::processNewTransaction(const QModelIndex& parent, int start, int /*end*/)
{
    // Prevent notifications-spam when initial block download is in progress
    if (!walletModel || !clientModel || clientModel->inInitialBlockDownload())
        return;

    if (!txModel || txModel->processingQueuedTransactions())
        return;

    QString date = txModel->index(start, TransactionTableModel::Date, parent).data().toString();
    qint64 amount = txModel->index(start, TransactionTableModel::Amount, parent).data(Qt::EditRole).toULongLong();
    QString type = txModel->index(start, TransactionTableModel::Type, parent).data().toString();
    QString address = txModel->index(start, TransactionTableModel::ToAddress, parent).data().toString();

    const QModelIndex sourceBaseIndex = txModel->index(start, 0, parent);
    const QModelIndex proxyIndex = (filter && sourceBaseIndex.isValid()) ? filter->mapFromSource(sourceBaseIndex) : QModelIndex();
    startInsertedRowAnimations(proxyIndex);

    Q_EMIT incomingTransaction(date, walletModel->getOptionsModel()->getDisplayUnit(), amount, type, address);
}

void DashboardWidget::prepareTransactionInsertionAnimation(const QModelIndex& parent, int start, int end)
{
    Q_UNUSED(parent);
    Q_UNUSED(start);
    Q_UNUSED(end);

    txInsertAnimationPending = false;
    preInsertViewportSnapshot = QPixmap();

    if (!ui || !ui->listTransactions || !ui->listTransactions->isVisible()) return;
    if (!walletModel || !clientModel || clientModel->inInitialBlockDownload()) return;
    if (!filter) return;
    if (ui->listTransactions->verticalScrollBar()->value() != 0) return;

    preInsertViewportSnapshot = ui->listTransactions->viewport()->grab();
    txInsertAnimationPending = !preInsertViewportSnapshot.isNull();
}

void DashboardWidget::startInsertedRowAnimations(const QModelIndex& proxyIndex)
{
    if (!ui || !ui->listTransactions || !txViewDelegate) return;

    if (animatedTxProxyIndex.isValid()) {
        txViewDelegate->clearTransientRowOpacity(animatedTxProxyIndex);
        animatedTxProxyIndex = QPersistentModelIndex();
    }
    if (txRowFadeAnimation) {
        txRowFadeAnimation->stop();
    }

    if (proxyIndex.isValid()) {
        animatedTxProxyIndex = QPersistentModelIndex(proxyIndex);
        txViewDelegate->setTransientRowOpacity(proxyIndex, 0.0);
        ui->listTransactions->viewport()->update(ui->listTransactions->visualRect(proxyIndex));

        txRowFadeAnimation = new QVariantAnimation(this);
        txRowFadeAnimation->setDuration(520);
        txRowFadeAnimation->setStartValue(0.0);
        txRowFadeAnimation->setEndValue(1.0);
        txRowFadeAnimation->setEasingCurve(QEasingCurve::OutCubic);
        connect(txRowFadeAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            if (!animatedTxProxyIndex.isValid() || !ui || !ui->listTransactions || !txViewDelegate) return;
            const QModelIndex index(animatedTxProxyIndex);
            txViewDelegate->setTransientRowOpacity(index, value.toReal());
            ui->listTransactions->viewport()->update(ui->listTransactions->visualRect(index));
        });
        connect(txRowFadeAnimation, &QVariantAnimation::finished, this, [this]() {
            if (!animatedTxProxyIndex.isValid() || !ui || !ui->listTransactions || !txViewDelegate) return;
            const QModelIndex index(animatedTxProxyIndex);
            txViewDelegate->clearTransientRowOpacity(index);
            ui->listTransactions->viewport()->update(ui->listTransactions->visualRect(index));
            animatedTxProxyIndex = QPersistentModelIndex();
        });
        txRowFadeAnimation->start();
    }

    if (!txInsertAnimationPending || preInsertViewportSnapshot.isNull()) return;
    txInsertAnimationPending = false;

    if (!proxyIndex.isValid() || proxyIndex.row() != 0) {
        preInsertViewportSnapshot = QPixmap();
        return;
    }

    QWidget* viewport = ui->listTransactions->viewport();
    if (!viewport) {
        preInsertViewportSnapshot = QPixmap();
        return;
    }

    QLabel* overlay = new QLabel(viewport);
    overlay->setPixmap(preInsertViewportSnapshot);
    overlay->setGeometry(viewport->rect());
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    overlay->show();
    overlay->raise();

    auto* opacityEffect = new QGraphicsOpacityEffect(overlay);
    overlay->setGraphicsEffect(opacityEffect);

    const int rowHeight = std::max(ui->listTransactions->sizeHintForRow(0), DECORATION_SIZE);
    auto* moveAnim = new QPropertyAnimation(overlay, "pos", overlay);
    moveAnim->setDuration(420);
    moveAnim->setStartValue(QPoint(0, 0));
    moveAnim->setEndValue(QPoint(0, std::min(rowHeight, viewport->height() / 2)));
    moveAnim->setEasingCurve(QEasingCurve::OutCubic);

    auto* fadeAnim = new QPropertyAnimation(opacityEffect, "opacity", overlay);
    fadeAnim->setDuration(420);
    fadeAnim->setStartValue(1.0);
    fadeAnim->setEndValue(0.0);
    fadeAnim->setEasingCurve(QEasingCurve::OutCubic);

    auto* group = new QParallelAnimationGroup(overlay);
    group->addAnimation(moveAnim);
    group->addAnimation(fadeAnim);
    connect(group, &QParallelAnimationGroup::finished, overlay, &QLabel::deleteLater);
    group->start(QAbstractAnimation::DeleteWhenStopped);

    preInsertViewportSnapshot = QPixmap();
}

DashboardWidget::~DashboardWidget()
{
#ifdef USE_QTCHARTS
    delete chart;
#endif
    delete ui;
}
