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
#include <QColor>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsLayout>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QList>
#include <QLocale>
#include <QModelIndex>
#include <QParallelAnimationGroup>
#include <QPainter>
#include <QPropertyAnimation>
#include <QCursor>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QTime>
#include <QVBoxLayout>
#include <functional>

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

void SetStatusIcon(QLabel* icon, const QString& resourcePath)
{
    if (!icon) return;
    icon->setProperty("statusIcon", resourcePath);
    icon->setPixmap(QPixmap(resourcePath).scaled(13, 13, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ApplyStatusState(QWidget* badge, const QString& state, const QColor& glow = QColor())
{
    if (!badge) return;
    badge->setProperty("statusState", state);
    badge->setGraphicsEffect(nullptr);
    if (glow.isValid()) {
        auto* effect = new QGraphicsDropShadowEffect(badge);
        effect->setBlurRadius(12.0);
        effect->setOffset(0, 0);
        effect->setColor(glow);
        badge->setGraphicsEffect(effect);
    }
    updateStyle(badge);
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
    setCssProperty(ui->labelTitle2, "dashboard-feed-title");
    ui->labelTitle2->setMinimumHeight(0);
    {
        const int hour = QTime::currentTime().hour();
        const QString daypart = hour < 5 ? tr("night") : (hour < 12 ? tr("morning") : (hour < 18 ? tr("afternoon") : tr("evening")));
        ui->labelTitle->setText(tr("Good %1, grower").arg(daypart));
        ui->labelSubtitle->setText(tr("%1 network").arg(QString::fromStdString(Params().NetworkIDString())));
    }

    /* Subtitle */
    setCssProperty(ui->labelSubtitle, "screen-header-subtitle");
    ui->labelSubtitle->setWordWrap(true);
    setCssProperty(ui->labelMessage, "dashboard-feed-subtitle");
    ui->labelMessage->setText(tr("staking & masternode income"));

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
    ui->verticalLayout_31->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout_31->setSpacing(0);
    ui->verticalLayout_2->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout_2->setSpacing(8);
    ui->horizontalLayout_3->setContentsMargins(0, 2, 0, 2);
    ui->horizontalLayout_3->setSpacing(8);
    ui->verticalLayout_6->setSpacing(1);

    const int primaryCardHeight = 310;
    auto* analyticsModule = new QWidget(ui->right);
    analyticsModule->setObjectName("analyticsModule");
    analyticsModule->setProperty("dashboardCardRole", "primary");
    analyticsModule->setAttribute(Qt::WA_StyledBackground, true);
    analyticsModule->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    analyticsModule->setFixedHeight(primaryCardHeight);
    setCssProperty(analyticsModule, "dashboard-analytics-card");
    auto* analyticsLayout = new QVBoxLayout(analyticsModule);
    analyticsLayout->setContentsMargins(16, 10, 16, 10);
    analyticsLayout->setSpacing(5);

    // The .ui title row is removed here and rebuilt as a fixed-height header
    // widget further below, so the collapsed card keeps a compact header.
    ui->verticalLayout_2->removeItem(ui->horizontalLayout_3);
    if (ui->verticalSpacer_4) {
        ui->verticalLayout_2->removeItem(ui->verticalSpacer_4);
        delete ui->verticalSpacer_4;
        ui->verticalSpacer_4 = nullptr;
    }

    auto* rewardSummaryRow = new QWidget(analyticsModule);
    rewardSummaryRow->setObjectName("rewardSummaryRow");
    rewardSummaryRow->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(rewardSummaryRow, "dashboard-reward-summary");
    rewardSummaryRow->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    rewardSummaryRow->setMaximumHeight(58);
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
    // Compact reward markers share the approved green family while remaining
    // distinguishable in both themes.
    const auto initialMarkerStyle = [](bool stakingMarker) {
        if (isLightTheme()) {
            return stakingMarker
                ? QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #77A95A,stop:1 #477B31);border:none;border-radius:4px;")
                : QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #A2B893,stop:1 #688F51);border:none;border-radius:4px;");
        }
        return stakingMarker
            ? QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #8BC65B,stop:1 #568A3C);border:none;border-radius:4px;")
            : QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #A9C58E,stop:1 #6E934F);border:none;border-radius:4px;");
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

    chartBody = new QWidget(analyticsModule);
    chartBody->setMinimumHeight(100);
    chartBody->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(chartBody, "dashboard-chart-body");
    auto* chartBodyLayout = new QVBoxLayout(chartBody);
    chartBodyLayout->setContentsMargins(0, 0, 0, 0);
    chartBodyLayout->setSpacing(2);
    chartBodyLayout->addWidget(ui->layoutChart, 1);
    chartBodyLayout->addWidget(ui->emptyContainerChart, 1);

    // Collapsible body: reward tiles + chart. Collapsing hides only this,
    // leaving the decorated header card visible like the field notes card.
    chartContent = new QWidget(analyticsModule);
    chartContent->setObjectName("dashboardChartContent");
    chartContent->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(chartContent, "dashboard-chart-body");
    auto* chartContentLayout = new QVBoxLayout(chartContent);
    chartContentLayout->setContentsMargins(0, 2, 0, 0);
    chartContentLayout->setSpacing(5);
    chartContentLayout->addWidget(rewardSummaryRow);
    chartContentLayout->addWidget(chartBody, 1);
    analyticsLayout->addWidget(chartContent, 1);
    analyticsCard = analyticsModule;
    ui->verticalLayout_2->addWidget(analyticsModule, 1);
    ui->verticalLayout_2->addStretch(0);
    chartBottomStretchIdx = ui->verticalLayout_2->count() - 1;

    // Keep the legacy collapse behavior available internally, but the approved
    // fintech dashboard presents the analytics card as a stable, always-visible
    // module instead of exposing a text-glyph disclosure control.
    chartToggle = new QPushButton(ui->right);
    chartToggle->setCursor(Qt::PointingHandCursor);
    chartToggle->setFlat(true);
    chartToggle->setFixedSize(28, 28);
    setCssProperty(chartToggle, "dashboard-feed-toggle");
    chartToggle->setVisible(false);
    connect(chartToggle, &QPushButton::clicked, this, [this]() { setChartExpanded(!chartExpanded); });

    // Rebuild the .ui title row as a fixed-height header widget inside the
    // card: a nested layout would soak up all leftover vertical space when
    // the chart body is collapsed, spreading the title and subtitle apart.
    auto* chartHeader = new QWidget(analyticsModule);
    chartHeader->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(chartHeader, "dashboard-chart-content");
    chartHeader->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* chartHeaderLayout = new QHBoxLayout(chartHeader);
    chartHeaderLayout->setContentsMargins(0, 2, 0, 2);
    chartHeaderLayout->setSpacing(8);
    auto* chartTitleCol = new QVBoxLayout();
    chartTitleCol->setContentsMargins(0, 0, 0, 0);
    chartTitleCol->setSpacing(1);
    ui->verticalLayout_6->removeWidget(ui->labelTitle2);
    ui->verticalLayout_6->removeWidget(ui->labelMessage);
    chartTitleCol->addWidget(ui->labelTitle2);
    chartTitleCol->addWidget(ui->labelMessage);
    chartHeaderLayout->addLayout(chartTitleCol, 1);

    // Move the real chart period buttons out of the data-dependent chart body,
    // so All / Month / Year remain available in the empty state as well.
    ui->horizontalLayout_7->removeWidget(ui->pushButtonAll);
    ui->horizontalLayout_7->removeWidget(ui->pushButtonMonth);
    ui->horizontalLayout_7->removeWidget(ui->pushButtonYear);
    ui->pushButtonAll->setText(tr("All"));
    ui->pushButtonMonth->setText(tr("Month"));
    ui->pushButtonYear->setText(tr("Year"));
    ui->pushButtonAll->setMinimumSize(48, 30);
    ui->pushButtonMonth->setMinimumSize(62, 30);
    ui->pushButtonYear->setMinimumSize(50, 30);

    auto* rangeSelector = new QWidget(chartHeader);
    rangeSelector->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(rangeSelector, "dashboard-range-selector");
    auto* rangeSelectorLayout = new QHBoxLayout(rangeSelector);
    rangeSelectorLayout->setContentsMargins(1, 1, 1, 1);
    rangeSelectorLayout->setSpacing(0);
    rangeSelectorLayout->addWidget(ui->pushButtonAll);
    rangeSelectorLayout->addWidget(ui->pushButtonMonth);
    rangeSelectorLayout->addWidget(ui->pushButtonYear);
    chartHeaderLayout->addWidget(rangeSelector, 0, Qt::AlignVCenter);
    chartHeaderLayout->addWidget(chartToggle, 0, Qt::AlignVCenter);
    ui->groupBoxChart->setVisible(false);
    analyticsLayout->insertWidget(0, chartHeader);
    delete ui->horizontalLayout_3;

    ui->layoutChart->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->layoutChart, "dashboard-chart-content");
    ui->verticalLayout_8->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout_8->setSpacing(10);
    ui->verticalWidgetChart->setAttribute(Qt::WA_StyledBackground, true);
    ui->verticalWidgetChart->setMinimumHeight(108);
    ui->chartContainer2->setMinimumHeight(96);
    setCssProperty(ui->verticalWidgetChart, "dashboard-chart-content");
    ui->chartContainer->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->chartContainer, "dashboard-chart-canvas");
    ui->emptyContainerChart->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->emptyContainerChart, "dashboard-chart-content");
    ui->verticalLayout_71->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout_71->setSpacing(0);

    // Approved dashboard composition: global header, balance + analytics row,
    // and one full-width recent-transactions card.
    ui->horizontalLayout_2->removeWidget(ui->left);
    ui->horizontalLayout_2->removeWidget(ui->right);
    delete ui->horizontalLayout_2;

    auto* rootV = new QVBoxLayout(this);
    rootV->setContentsMargins(38, 28, 36, 30);
    rootV->setSpacing(14);

    dashboardHeader = new QWidget(this);
    dashboardHeader->setObjectName("dashboardHeader");
    dashboardHeader->setAttribute(Qt::WA_StyledBackground, true);
    dashboardHeader->setMinimumHeight(72);
    dashboardHeader->setMaximumHeight(82);
    setCssProperty(dashboardHeader, "dashboard-global-header");
    auto* dashboardHeaderLayout = new QHBoxLayout(dashboardHeader);
    dashboardHeaderLayout->setContentsMargins(6, 0, 6, 0);
    auto* headingColumn = new QVBoxLayout();
    headingColumn->setSpacing(3);
    auto* heading = new QLabel(tr("Dashboard"), dashboardHeader);
    setCssProperty(heading, "dashboard-global-title");
    auto* headingSubtitle = new QLabel(tr("Overview of your OrganicLife Coin wallet"), dashboardHeader);
    setCssProperty(headingSubtitle, "dashboard-global-subtitle");
    headingColumn->addWidget(heading);
    headingColumn->addWidget(headingSubtitle);
    dashboardHeaderLayout->addLayout(headingColumn, 1);
    auto* balanceColumn = new QVBoxLayout();
    balanceColumn->setSpacing(2);
    headerAvailableBalance = new QLabel(tr("-- OLC"), dashboardHeader);
    headerAvailableBalance->setObjectName("headerAvailableBalance");
    headerAvailableBalance->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setCssProperty(headerAvailableBalance, "dashboard-header-balance");
    auto* availableCaption = new QLabel(tr("AVAILABLE BALANCE"), dashboardHeader);
    availableCaption->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setCssProperty(availableCaption, "dashboard-header-caption");
    balanceColumn->addWidget(headerAvailableBalance);
    balanceColumn->addWidget(availableCaption);

    auto* statusCluster = new QWidget(dashboardHeader);
    statusCluster->setObjectName("dashboardStatusCluster");
    statusCluster->setAttribute(Qt::WA_StyledBackground, true);
    statusCluster->setMaximumHeight(30);
    setCssProperty(statusCluster, "dashboard-status-cluster");
    auto* statusLayout = new QHBoxLayout(statusCluster);
    statusLayout->setContentsMargins(2, 2, 2, 2);
    statusLayout->setSpacing(6);
    const auto addStatus = [statusCluster, statusLayout](const QString& name,
                                                         const QString& iconPath,
                                                         const QString& text,
                                                         QWidget*& badge,
                                                         QLabel*& iconLabel,
                                                         QLabel*& valueLabel) {
        auto* item = new QWidget(statusCluster);
        item->setObjectName(name + QStringLiteral("Badge"));
        item->setAttribute(Qt::WA_StyledBackground, true);
        setCssProperty(item, "dashboard-status-item");
        auto* itemLayout = new QHBoxLayout(item);
        itemLayout->setContentsMargins(6, 2, 7, 2);
        itemLayout->setSpacing(4);
        auto* icon = new QLabel(item);
        icon->setObjectName(name + QStringLiteral("Icon"));
        icon->setFixedSize(14, 14);
        SetStatusIcon(icon, iconPath);
        valueLabel = new QLabel(text, item);
        valueLabel->setObjectName(name + QStringLiteral("Status"));
        setCssProperty(valueLabel, "dashboard-status-text");
        itemLayout->addWidget(icon);
        itemLayout->addWidget(valueLabel);
        statusLayout->addWidget(item);
        badge = item;
        iconLabel = icon;
    };
    const bool light = isLightTheme();
    addStatus(QStringLiteral("dashboardSync"),
              light ? QStringLiteral(":/ic-check-sync-dark") : QStringLiteral(":/ic-check-sync"),
              tr("Syncing"), dashboardSyncBadge, dashboardSyncIcon, dashboardSyncStatus);
    addStatus(QStringLiteral("dashboardConnection"),
              QStringLiteral(":/ic-check-connect-off"),
              tr("0 connections"), dashboardConnectionBadge, dashboardConnectionIcon, dashboardConnectionStatus);
    addStatus(QStringLiteral("dashboardStaking"), QStringLiteral(":/ic-check-staking-off"),
              tr("Staking off"), dashboardStakingBadge, dashboardStakingIcon, dashboardStakingStatus);

    auto* syncLayout = qobject_cast<QHBoxLayout*>(dashboardSyncBadge->layout());
    auto* syncDivider = new QFrame(dashboardSyncBadge);
    syncDivider->setObjectName(QStringLiteral("dashboardSyncDivider"));
    syncDivider->setFrameShape(QFrame::VLine);
    syncDivider->setFixedSize(1, 12);
    setCssProperty(syncDivider, "dashboard-status-divider");
    dashboardBlockHeight = new QLabel(tr("Block —"), dashboardSyncBadge);
    dashboardBlockHeight->setObjectName(QStringLiteral("dashboardBlockHeight"));
    dashboardBlockHeight->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setCssProperty(dashboardBlockHeight, "dashboard-status-text");
    syncLayout->addWidget(syncDivider);
    syncLayout->addWidget(dashboardBlockHeight);
    ApplyStatusState(dashboardSyncBadge, QStringLiteral("neutral"));
    setNumConnections(0);
    setStakingStatusActive(false);
    balanceColumn->addWidget(statusCluster, 0, Qt::AlignRight);
    dashboardHeaderLayout->addLayout(balanceColumn);
    rootV->addWidget(dashboardHeader);

    topCardsContainer = new QWidget(this);
    topCardsContainer->setObjectName("dashboardTopCards");
    auto* topCards = new QHBoxLayout(topCardsContainer);
    topCards->setContentsMargins(0, 0, 0, 0);
    topCards->setSpacing(24);

    auto* balanceCard = new QWidget(this);
    balanceCard->setObjectName("balanceCard");
    balanceCard->setProperty("dashboardCardRole", "primary");
    balanceCard->setAttribute(Qt::WA_StyledBackground, true);
    balanceCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    balanceCard->setFixedHeight(primaryCardHeight);
    setCssProperty(balanceCard, "dashboard-balance-card");
    auto* balanceCardLayout = new QVBoxLayout(balanceCard);
    balanceCardLayout->setContentsMargins(20, 16, 20, 16);
    balanceCardLayout->setSpacing(10);
    auto* brandRow = new QHBoxLayout();
    auto* cardLogo = new QLabel(balanceCard);
    cardLogo->setObjectName(QStringLiteral("dashboardBrandLogo"));
    cardLogo->setProperty("sourceAsset", QStringLiteral(":/img-logo-pivx"));
    cardLogo->setFixedSize(52, 52);
    cardLogo->setPixmap(QPixmap(":/img-logo-pivx").scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    brandRow->addWidget(cardLogo, 0, Qt::AlignVCenter);
    auto* cardBrandText = new QVBoxLayout();
    auto* cardBrand = new QLabel(tr("OrganicLife Coin"), balanceCard);
    setCssProperty(cardBrand, "dashboard-card-title");
    auto* cardTagline = new QLabel(tr("Local growth, global roots"), balanceCard);
    setCssProperty(cardTagline, "dashboard-card-subtitle");
    cardBrandText->addWidget(cardBrand);
    cardBrandText->addWidget(cardTagline);
    brandRow->addLayout(cardBrandText, 1);
    balanceCardLayout->addLayout(brandRow);

    auto* balanceDivider = new QFrame(balanceCard);
    balanceDivider->setFrameShape(QFrame::HLine);
    setCssProperty(balanceDivider, "dashboard-card-divider");
    balanceCardLayout->addWidget(balanceDivider);
    auto* totalCaption = new QLabel(tr("Total balance"), balanceCard);
    setCssProperty(totalCaption, "dashboard-card-subtitle");
    balanceCardLayout->addWidget(totalCaption);
    statValueAvailable = new QLabel(tr("-- OLC"), balanceCard);
    statValueAvailable->setObjectName("totalBalanceValue");
    setCssProperty(statValueAvailable, "dashboard-total-balance");
    balanceCardLayout->addWidget(statValueAvailable);
    statValueStaking = new QLabel(tr("Locked: --  •  Immature: --"), balanceCard);
    setCssProperty(statValueStaking, "dashboard-card-subtitle");
    balanceCardLayout->addWidget(statValueStaking);

    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(10);
    auto* sendButton = new QPushButton(QIcon(":/ic-nav-send"), tr("Send"), balanceCard);
    sendButton->setIconSize(QSize(18, 18));
    sendButton->setCursor(Qt::PointingHandCursor);
    setCssProperty(sendButton, "dashboard-action-primary");
    auto* receiveButton = new QPushButton(QIcon(":/ic-nav-receive"), tr("Receive"), balanceCard);
    receiveButton->setIconSize(QSize(18, 18));
    receiveButton->setCursor(Qt::PointingHandCursor);
    setCssProperty(receiveButton, "dashboard-action-secondary");
    actionRow->addWidget(sendButton);
    actionRow->addWidget(receiveButton);
    connect(sendButton, &QPushButton::clicked, window, &OrganicLifeGUI::goToSend);
    connect(receiveButton, &QPushButton::clicked, window, &OrganicLifeGUI::goToReceive);
    balanceCardLayout->addLayout(actionRow);

    balanceCardLayout->addStretch(1);

    // Wrap the two existing reward tiles in the semantic stat row expected by
    // the selected reference and add the third average/reward metric.
    auto* analyticsStatRow = new QWidget(analyticsModule);
    analyticsStatRow->setObjectName("analyticsStatRow");
    analyticsStatRow->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(analyticsStatRow, "dashboard-reward-summary");
    auto* analyticsStatsLayout = new QHBoxLayout(analyticsStatRow);
    analyticsStatsLayout->setContentsMargins(0, 0, 0, 0);
    analyticsStatsLayout->setSpacing(6);
    chartContentLayout->removeWidget(rewardSummaryRow);
    analyticsStatsLayout->addWidget(rewardSummaryRow, 2);
    auto* averageTile = new QWidget(analyticsStatRow);
    averageTile->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(averageTile, "dashboard-reward-tile");
    auto* averageTileLayout = new QVBoxLayout(averageTile);
    averageTileLayout->setContentsMargins(10, 7, 10, 7);
    auto* averageCaption = new QLabel(tr("AVG / DAY"), averageTile);
    setCssProperty(averageCaption, "dashboard-reward-tile-label");
    statValueRewards = new QLabel(tr("-- OLC"), averageTile);
    setCssProperty(statValueRewards, "dashboard-reward-stat-stakes");
    averageTileLayout->addWidget(averageCaption);
    averageTileLayout->addWidget(statValueRewards);
    analyticsStatsLayout->addWidget(averageTile, 1);
    chartContentLayout->insertWidget(0, analyticsStatRow);

    // Keep the real month/year selectors inside the analytics card as a
    // compact control row. The legacy group boxes were tall enough to escape
    // the card at common desktop heights.
    ui->verticalLayout_8->removeWidget(ui->containerSort);
    ui->containerSort->setVisible(false);
    ui->horizontalLayout_9->removeWidget(ui->comboBoxMonths);
    ui->horizontalLayout_10->removeWidget(ui->comboBoxYears);

    periodFilterRow = new QWidget(analyticsModule);
    periodFilterRow->setObjectName("dashboardPeriodFilter");
    periodFilterRow->setAttribute(Qt::WA_StyledBackground, true);
    periodFilterRow->setMaximumHeight(38);
    setCssProperty(periodFilterRow, "dashboard-period-filter");
    auto* periodLayout = new QHBoxLayout(periodFilterRow);
    periodLayout->setContentsMargins(8, 3, 8, 3);
    periodLayout->setSpacing(6);
    auto* periodLabel = new QLabel(tr("Period"), periodFilterRow);
    setCssProperty(periodLabel, "dashboard-period-label");
    periodLayout->addWidget(periodLabel);
    periodLayout->addStretch(1);
    for (QComboBox* combo : {ui->comboBoxMonths, ui->comboBoxYears}) {
        combo->setParent(periodFilterRow);
        combo->setMinimumSize(92, 30);
        combo->setMaximumHeight(30);
        setCssProperty(combo, "dashboard-period-combo", true);
        periodLayout->addWidget(combo);
    }
    ui->container_chart_dropboxes->setVisible(false);
    chartContentLayout->insertWidget(1, periodFilterRow);

    ui->verticalLayout_2->removeWidget(analyticsModule);
    analyticsModule->setParent(topCardsContainer);
    ui->right->hide();
    topCards->addWidget(balanceCard, 1);
    topCards->addWidget(analyticsModule, 1);
    topCards->setStretch(0, 1);
    topCards->setStretch(1, 1);
    rootV->addWidget(topCardsContainer, 0);

    ui->labelTitle->setText(tr("Recent transactions"));
    ui->labelSubtitle->setText(tr("Your latest wallet activity"));
    ui->labelSubtitle->hide();
    ui->labelTitle->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui->verticalLayout_5->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout_5->setSpacing(0);
    recentTransactionsCard = new QWidget(this);
    recentTransactionsCard->setObjectName("recentTransactionsCard");
    recentTransactionsCard->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(recentTransactionsCard, "dashboard-transactions-card");
    auto* transactionsLayout = new QVBoxLayout(recentTransactionsCard);
    transactionsLayout->setContentsMargins(0, 0, 0, 0);
    transactionsLayout->addWidget(ui->left);
    rootV->addWidget(recentTransactionsCard, 1);
    chartExpanded = true;

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

    ui->left_top_container->setMinimumHeight(56);
    ui->left_top_container->setMaximumHeight(62);

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
    ui->pushImgEmptyChart->setVisible(false);
    ui->verticalLayout->setSpacing(2);
    ui->verticalSpacer11->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Minimum);
    ui->verticalSpacer_2->changeSize(0, 4, QSizePolicy::Minimum, QSizePolicy::Fixed);
    setCssProperty(ui->labelEmptyChart, "dashboard-empty-title");
    ui->labelMessageEmpty->setText(tr("Rewards will appear here after the wallet is synced and staking begins."));
    setCssProperty(ui->labelMessageEmpty, "dashboard-empty-copy");

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
    setTransactionsOnly(false);
}

void DashboardWidget::setTransactionsOnly(bool showTransactionsOnly)
{
    transactionsOnly = showTransactionsOnly;
    setProperty("transactionsOnly", transactionsOnly);
    if (dashboardHeader) dashboardHeader->setVisible(!transactionsOnly);
    if (topCardsContainer) topCardsContainer->setVisible(!transactionsOnly);
    if (recentTransactionsCard) recentTransactionsCard->setVisible(true);
    ui->labelTitle->setText(transactionsOnly ? tr("Transactions") : tr("Recent transactions"));
    ui->labelSubtitle->setText(transactionsOnly ? tr("Complete wallet activity") : tr("Your latest wallet activity"));
    ui->labelSubtitle->setVisible(false);
    updateGeometry();
}

void DashboardWidget::setNumConnections(int count)
{
    dashboardConnectionCount = std::max(0, count);
    if (dashboardConnectionStatus) {
        dashboardConnectionStatus->setText(dashboardConnectionCount == 1
                                               ? tr("1 connection")
                                               : tr("%1 connections").arg(dashboardConnectionCount));
    }
    const bool connected = dashboardConnectionCount > 0;
    SetStatusIcon(dashboardConnectionIcon,
                  connected
                      ? (isLightTheme() ? QStringLiteral(":/ic-check-connect-dark")
                                        : QStringLiteral(":/ic-check-connect"))
                      : QStringLiteral(":/ic-check-connect-off"));
    if (dashboardConnectionCount == 0) {
        ApplyStatusState(dashboardConnectionBadge, QStringLiteral("danger"), QColor(214, 79, 70, 105));
    } else if (dashboardConnectionCount <= 5) {
        ApplyStatusState(dashboardConnectionBadge, QStringLiteral("warning"), QColor(224, 142, 54, 95));
    } else {
        ApplyStatusState(dashboardConnectionBadge, QStringLiteral("connected"));
    }
}

void DashboardWidget::setStakingStatusActive(bool active)
{
    dashboardStakingActive = active;
    if (dashboardStakingStatus) {
        dashboardStakingStatus->setText(active ? tr("Staking active") : tr("Staking off"));
    }
    SetStatusIcon(dashboardStakingIcon,
                  active ? QStringLiteral(":/ic-check-staking")
                         : QStringLiteral(":/ic-check-staking-off"));
    ApplyStatusState(dashboardStakingBadge,
                     active ? QStringLiteral("active") : QStringLiteral("inactive"),
                     active ? QColor(104, 162, 73, 100) : QColor());
}

void DashboardWidget::setBlockHeight(int height)
{
    dashboardCurrentBlockHeight = std::max(0, height);
    if (!dashboardBlockHeight) return;
    const QLocale displayLocale(QLocale::English, QLocale::UnitedStates);
    dashboardBlockHeight->setText(tr("Block %1").arg(displayLocale.toString(dashboardCurrentBlockHeight)));
    dashboardSyncBadge->updateGeometry();
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
        updateStatBalances(walletModel->GetWalletBalances());
        updateStatRewards();
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

void DashboardWidget::animateSection(QWidget* body, bool expand, const std::function<void()>& onFinish)
{
    if (!body) { if (onFinish) onFinish(); return; }
    auto* anim = new QPropertyAnimation(body, "maximumHeight", body);
    anim->setDuration(220);
    anim->setEasingCurve(QEasingCurve::InOutCubic);
    const int contentH = body->sizeHint().height();
    if (expand) {
        body->setMaximumHeight(0);
        body->setVisible(true);
        anim->setStartValue(0);
        anim->setEndValue(contentH > 0 ? contentH : 200);
    } else {
        anim->setStartValue(body->height());
        anim->setEndValue(0);
    }
    connect(anim, &QPropertyAnimation::finished, body, [body, expand, onFinish]() {
        if (expand) {
            body->setMaximumHeight(QWIDGETSIZE_MAX);
        } else {
            body->setVisible(false);
        }
        if (onFinish) onFinish();
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void DashboardWidget::setChartExpanded(bool expanded)
{
    if (chartExpanded == expanded && chartContent && chartContent->isVisible() == expanded) return;
    chartExpanded = expanded;
    if (chartToggle) chartToggle->setText(expanded ? "▾" : "▸");
    QSettings settings;
    settings.setValue("dashboardChartExpanded", expanded);
    // Keep the header card pinned to the top: hand the leftover stretch
    // to a bottom spacer while the body is collapsed.
    if (analyticsCard && chartBottomStretchIdx >= 0) {
        ui->verticalLayout_2->setStretchFactor(analyticsCard, expanded ? 1 : 0);
        ui->verticalLayout_2->setStretch(chartBottomStretchIdx, expanded ? 0 : 1);
    }
    animateSection(chartContent, expanded);
}

void DashboardWidget::updateStatBalances(const interfaces::WalletBalances& balances)
{
    if (!statValueAvailable || !walletModel) return;
    const int unit = walletModel->getOptionsModel()->getDisplayUnit();
    const QString available = GUIUtil::formatBalance(balances.balance, unit);
    statValueAvailable->setText(available);
    if (headerAvailableBalance) headerAvailableBalance->setText(available);
    if (statValueStaking) {
        statValueStaking->setText(tr("Locked: %1  •  Immature: %2")
            .arg(GUIUtil::formatBalance(balances.delegate_balance + balances.coldstaked_balance, unit),
                 GUIUtil::formatBalance(balances.immature_balance, unit)));
    }
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
    statValueRewards->setText(GUIUtil::formatBalance(total / 30, unit));
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
    if (dashboardSyncStatus) dashboardSyncStatus->setText(sync ? tr("Synced") : tr("Syncing"));
    if (this->isSync != sync) {
        this->isSync = sync;
        ui->layoutWarning->setVisible(!this->isSync);
#ifdef USE_QTCHARTS
        if (!isVisible()) return;
        tryChartRefresh();
#endif
    }
}

void DashboardWidget::changeTheme(bool lightTheme, QString& theme)
{
    static_cast<TxViewHolder*>(this->txViewDelegate->getRowFactory())->isLightTheme = lightTheme;
    if (ui && ui->listTransactions) ui->listTransactions->viewport()->update();
    SetStatusIcon(dashboardSyncIcon,
                  lightTheme ? QStringLiteral(":/ic-check-sync-dark") : QStringLiteral(":/ic-check-sync"));
    setNumConnections(dashboardConnectionCount);
    setStakingStatusActive(dashboardStakingActive);
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
    if (chartValueTooltip && (showEmpty || loading)) chartValueTooltip->hide();
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
    chart->setMargins({0, 0, 0, 22});
    chart->setBackgroundRoundness(0);
    chart->setAnimationOptions(QChart::NoAnimation);
    chart->setAnimationDuration(0);
    // Axis
    chart->addAxis(axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignRight);
    axisY->setTickCount(5);
    axisY->setMinorTickCount(0);
    axisX->setGridLineVisible(false);
    axisX->setLabelsVisible(false);
    axisX->setLineVisible(false);
    axisY->setGridLineVisible(true);
    axisY->setLabelsVisible(false);
    axisY->setLineVisible(false);

    chartView = new QChartView(chart);
    chartView->setRenderHint(QPainter::Antialiasing);
    chartView->setRubberBand(QChartView::HorizontalRubberBand);
    chartView->setContentsMargins(0,0,0,0);
    chartView->setMinimumHeight(80);

    QVBoxLayout *baseScreensContainer = new QVBoxLayout();
    baseScreensContainer->setContentsMargins(0, 0, 0, 0);
    baseScreensContainer->setSpacing(0);
    baseScreensContainer->addWidget(chartView);

    chartTimeline = new QWidget(chartView->viewport());
    chartTimeline->setObjectName(QStringLiteral("dashboardChartTimeline"));
    chartTimeline->setFixedHeight(20);
    chartTimeline->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* timelineLayout = new QHBoxLayout(chartTimeline);
    timelineLayout->setContentsMargins(4, 0, 4, 0);
    timelineLayout->setSpacing(0);
    chartTimelineLabels.clear();
    for (int i = 0; i < 5; ++i) {
        auto* label = new QLabel(chartTimeline);
        label->setAlignment(i == 0 ? Qt::AlignLeft : (i == 4 ? Qt::AlignRight : Qt::AlignCenter));
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCssProperty(label, "dashboard-chart-timeline-label");
        timelineLayout->addWidget(label, 1);
        chartTimelineLabels.append(label);
    }
    ui->chartContainer->setLayout(baseScreensContainer);
    ui->chartContainer->setContentsMargins(0,0,0,0);
    setCssProperty(ui->chartContainer, "dashboard-chart-canvas");

    chartValueTooltip = new QLabel(chartView->viewport());
    chartValueTooltip->setObjectName(QStringLiteral("dashboardChartValueTooltip"));
    chartValueTooltip->setAttribute(Qt::WA_StyledBackground, true);
    chartValueTooltip->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setCssProperty(chartValueTooltip, "dashboard-chart-tooltip");
    chartValueTooltip->hide();
    QTimer::singleShot(0, this, &DashboardWidget::updateChartTimelineGeometry);
}

void DashboardWidget::updateChartTimelineGeometry()
{
    if (!chartTimeline || !chartView) return;
    const int inset = 8;
    QWidget* viewport = chartView->viewport();
    int visibleBottom = viewport->height();
    if (analyticsCard) {
        const QPoint cardBottomGlobal = analyticsCard->mapToGlobal(
            QPoint(0, analyticsCard->height() - inset));
        visibleBottom = std::min(visibleBottom, viewport->mapFromGlobal(cardBottomGlobal).y());
    }
    chartTimeline->setGeometry(inset,
                               std::max(0, visibleBottom - chartTimeline->height()),
                               std::max(0, viewport->width() - (inset * 2)),
                               chartTimeline->height());
    chartTimeline->raise();
}

void DashboardWidget::onChartPointHovered(const QPointF& point, bool state)
{
    if (!chartValueTooltip || !chartView) return;
    if (!state) {
        chartValueTooltip->hide();
        return;
    }

    const QLocale displayLocale(QLocale::English, QLocale::UnitedStates);
    chartValueTooltip->setText(tr("%1 OLC").arg(displayLocale.toString(point.y(), 'f', 2)));
    chartValueTooltip->adjustSize();

    QWidget* viewport = chartView->viewport();
    QPoint position = viewport->mapFromGlobal(QCursor::pos()) + QPoint(10, -chartValueTooltip->height() - 8);
    position.setX(std::clamp(position.x(), 4, std::max(4, viewport->width() - chartValueTooltip->width() - 4)));
    position.setY(std::clamp(position.y(), 4, std::max(4, viewport->height() - chartValueTooltip->height() - 4)));
    chartValueTooltip->move(position);
    chartValueTooltip->show();
    chartValueTooltip->raise();
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
    // Approved semantic palette anchors for the area chart.
    if (isLightTheme()) {
        gridLineColorX = QColor("#DDE5DA");
        gridY = QColor("#D7E0D5");
        linePenColor = QColor("#D4DDD2");
        labelColor = QColor("#5D6961");
        backgroundColor = QColor("#FFFFFF");
        pivHintColor = QColor("#56863B");
        mnHintColor = QColor("#7A9D65");
        axisY->setGridLineColor(gridY);
        axisY->setMinorGridLineColor(QColor("#F3F6F1"));
    } else {
        gridLineColorX = QColor("#30443A");
        gridY = QColor("#33493D");
        linePenColor = QColor("#2B3D34");
        labelColor = QColor("#A9B0A9");
        backgroundColor = QColor("#121F1A");
        pivHintColor = QColor("#8BC65B");
        mnHintColor = QColor("#A1BF89");
        axisY->setGridLineColor(gridY);
        axisY->setMinorGridLineColor(QColor("#172720"));
    }

    axisX->setGridLineColor(gridLineColorX);
    QPen horizontalGridPen(gridY);
    horizontalGridPen.setWidthF(1.0);
    axisY->setGridLinePen(horizontalGridPen);
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
        return QString("background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 %1,stop:1 %2);border:none;border-radius:4px;")
            .arg(color.lighter(116).name(), color.darker(112).name());
    };
    ui->labelSquarePiv->setStyleSheet(markerStyle(pivHintColor));
    ui->labelSquareMN->setStyleSheet(markerStyle(mnHintColor));
    if (stakesLine) {
        QPen linePen(pivHintColor);
        linePen.setWidth(2);
        linePen.setCapStyle(Qt::RoundCap);
        linePen.setJoinStyle(Qt::RoundJoin);
        stakesLine->setPen(linePen);
        stakesLine->setPointsVisible(true);
        stakesLine->setMarkerSize(5.5);
    }
    if (areaStakes) {
        QLinearGradient fillGrad(0, 0, 0, 1);
        fillGrad.setCoordinateMode(QGradient::ObjectBoundingMode);
        QColor top = pivHintColor;
        top.setAlpha(isLightTheme() ? 54 : 68);
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
        mnPen.setCapStyle(Qt::RoundCap);
        mnPen.setJoinStyle(Qt::RoundJoin);
        mnLine->setPen(mnPen);
        mnLine->setPointsVisible(true);
        mnLine->setMarkerSize(5.0);
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

    std::map<int, std::pair<long long, long long>> rewards;
    for (auto it = chartData->amountsByCache.cbegin(); it != chartData->amountsByCache.cend(); ++it) {
        rewards.emplace(it.key(), std::make_pair(it.value().first, it.value().second));
    }
    const auto cumulative = BuildCumulativeRewardSeries(rewards, range.first, range.second);

    int seriesIndex = 0;
    for (int num = range.first; num < range.second; num++, seriesIndex++) {
        qreal piv = cumulative.at(seriesIndex).first / 100000000.0;
        qreal mn = cumulative.at(seriesIndex).second / 100000000.0;
        if (chartData->amountsByCache.contains(num)) {
            std::pair <qint64, qint64> pair = chartData->amountsByCache[num];
            chartData->totalPiv += pair.first;
            chartData->totalMN += pair.second;
        }

        chartData->xLabels << ((withMonthNames) ? monthsNames[num - 1] : QString::number(num));

        chartData->valuesPiv.append(piv);
        chartData->valuesMN.append(mn);

        qreal max = piv + mn;
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
    if (chartValueTooltip) chartValueTooltip->hide();
    if (chart && axisX) {
        axisX->clear();
    }

    // V2 harvest chart: filled area for stakes, accent line for masternodes
    if (!stakesLine) {
        stakesLine = new QLineSeries();
        areaStakes = new QAreaSeries(stakesLine);
        areaStakes->setName(tr("Stakes"));
        connect(areaStakes, &QAreaSeries::hovered, this, &DashboardWidget::onChartPointHovered);
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

    QList<qreal> targetTotalValues;
    targetTotalValues.reserve(targetSize);
    for (int i = 0; i < targetSize; ++i) {
        targetTotalValues.append(targetPivValues.at(i) + targetMnValues.at(i));
    }
    const qreal maxForRange = MaxValue(targetTotalValues);

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
    const int timelineCount = chartTimelineLabels.size();
    const int sourceCount = chartData->xLabels.size();
    for (int i = 0; i < timelineCount; ++i) {
        QString text;
        if (sourceCount == 1) {
            if (i == timelineCount / 2) text = chartData->xLabels.first();
        } else if (sourceCount > 1) {
            const int sourceIndex = qRound((sourceCount - 1) * (i / static_cast<double>(timelineCount - 1)));
            text = chartData->xLabels.at(sourceIndex);
        }
        chartTimelineLabels.at(i)->setText(text);
    }
    updateChartTimelineGeometry();
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

    ReplaceLineSeriesValues(stakesLine, targetTotalValues);
    ReplaceLineSeriesValues(mnLine, targetMnValues);
    mnLine->setVisible(false);
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
            if (periodFilterRow) periodFilterRow->setVisible(false);
            break;
        }
        case YEAR: {
            if (periodFilterRow) periodFilterRow->setVisible(true);
            ui->comboBoxMonths->setVisible(false);
            ui->comboBoxYears->setVisible(true);
            break;
        }
        case MONTH: {
            if (periodFilterRow) periodFilterRow->setVisible(true);
            ui->comboBoxMonths->setVisible(true);
            ui->comboBoxYears->setVisible(true);
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
    Q_UNUSED(event);
    updateChartTimelineGeometry();
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
