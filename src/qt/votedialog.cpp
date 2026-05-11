// Copyright (c) 2021 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "votedialog.h"
#include "ui_votedialog.h"

#include "bitcoinunits.h"
#include "chainparams.h"
#include "mnmodel.h"
#include "mnselectiondialog.h"
#include "qtutils.h"
#include "walletmodel.h"

#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QtSvg/QSvgRenderer>
#include <QStyle>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace {
QString FormatDurationLabel(int64_t seconds)
{
    if (seconds <= 0) {
        return QObject::tr("unknown");
    }
    const int64_t days = (seconds + (24 * 60 * 60) - 1) / (24 * 60 * 60);
    if (days % 7 == 0) {
        const int64_t weeks = days / 7;
        return weeks == 1 ? QObject::tr("1 week") : QObject::tr("%1 weeks").arg(weeks);
    }
    return days == 1 ? QObject::tr("1 day") : QObject::tr("%1 days").arg(days);
}

int64_t BudgetCycleSeconds()
{
    const auto& consensus = Params().GetConsensus();
    const int64_t cycleBlocks = consensus.nBudgetCycleBlocks > 0 ? consensus.nBudgetCycleBlocks : 1;
    const int64_t targetSpacing = consensus.nTargetSpacing > 0 ? consensus.nTargetSpacing : 1;
    return cycleBlocks * targetSpacing;
}

QScreen* ResolveDialogScreen(QWidget* widget)
{
    if (!widget) return QGuiApplication::primaryScreen();
    if (widget->screen()) return widget->screen();
    if (widget->parentWidget() && widget->parentWidget()->screen()) return widget->parentWidget()->screen();
    const QPoint globalCenter = widget->mapToGlobal(widget->rect().center());
    if (QScreen* screen = QGuiApplication::screenAt(globalCenter)) return screen;
    return QGuiApplication::primaryScreen();
}

QString ResolveVoteDialogCloseIcon()
{
    return isLightTheme() ? QStringLiteral(":/ic-close") : QStringLiteral(":/ic-close-white");
}

QString ResolveVoteDialogLinkIcon()
{
    return isLightTheme() ? QStringLiteral(":/ic-link") : QStringLiteral(":/ic-link-hover");
}

QIcon ResolveVoteDialogQIcon(const QString& resourcePath, const QSize& targetSize)
{
    QIcon icon(resourcePath);
    if (!icon.isNull()) {
        return icon;
    }

    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid()) {
        return icon;
    }

    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const QSize pixelSize(
            std::max(1, static_cast<int>(std::lround(targetSize.width() * dpr))),
            std::max(1, static_cast<int>(std::lround(targetSize.height() * dpr))));

    QPixmap pixmap(pixelSize);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(dpr);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    painter.end();
    return QIcon(pixmap);
}

QIcon ResolveVoteDialogCloseQIcon(const QSize& targetSize)
{
    return ResolveVoteDialogQIcon(ResolveVoteDialogCloseIcon(), targetSize);
}

QIcon ResolveVoteDialogLinkQIcon(const QSize& targetSize)
{
    return ResolveVoteDialogQIcon(ResolveVoteDialogLinkIcon(), targetSize);
}
} // namespace

VoteDialog::VoteDialog(QWidget *parent, GovernanceModel* _govModel, MNModel* _mnModel, WalletModel* _walletModel) :
    ContainerDialog(parent),
    ui(new Ui::VoteDialog),
    govModel(_govModel),
    mnModel(_mnModel),
    walletModel(_walletModel)
{
    ui->setupUi(this);
    applyParentOrAppStyleSheet(parent);
    initRoundedContainerFrame(ui->frame, 16);
    setDialogAutoSizeToContents(this, false);
    setDialogOwnsOpenPosition(this, true);
    setupStickyHeader();
    setCssProperty(ui->labelTitle, "vote-dialog-title", true);
    setCssProperty(ui->labelSubtitle, "vote-dialog-subtitle", true);
    ui->labelSubtitle->setWordWrap(true);
    ui->labelSubtitle->setAlignment(Qt::AlignCenter);
    ui->labelSubtitle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->labelSubtitle->setFixedHeight(64);

    // Vote Info
    setCssProperty(ui->labelTitleVote, "vote-title");
    setCssProperty(ui->labelAmount, "vote-amount");
    ui->labelAmount->setAlignment(Qt::AlignCenter);
    setCssProperty(ui->labelTime, "vote-time");
    ui->labelTime->setAlignment(Qt::AlignCenter);
    setCssProperty(ui->labelMessage, "vote-message");
    ui->labelMessage->setAlignment(Qt::AlignCenter);
    ui->labelMessage->setMinimumHeight(38);
    QSizePolicy messagePolicy = ui->labelMessage->sizePolicy();
    messagePolicy.setRetainSizeWhenHidden(true);
    ui->labelMessage->setSizePolicy(messagePolicy);

    const QSize closeIconSize(20, 20);
    initDialogCloseButton(ui->btnEsc, "ic-close", closeIconSize);
    ui->btnEsc->setFlat(true);
    ui->btnEsc->setAutoDefault(false);
    ui->btnEsc->setDefault(false);
    ui->btnEsc->setIcon(ResolveVoteDialogCloseQIcon(closeIconSize));
    ui->btnEsc->setIconSize(closeIconSize);
    setCssProperty(ui->btnCancel, "vote-action-cancel", true);
    setCssProperty(ui->btnSave, "vote-action-primary", true);
    setCssProperty(ui->btnLink, "btn-link");
    ui->btnLink->setCursor(Qt::PointingHandCursor);
    ui->btnLink->setFlat(true);
    ui->btnLink->setAutoDefault(false);
    ui->btnLink->setDefault(false);
    ui->btnLink->setText("");
    const QSize linkIconSize(18, 18);
    ui->btnLink->setIcon(ResolveVoteDialogLinkQIcon(linkIconSize));
    ui->btnLink->setIconSize(linkIconSize);
    ui->btnLink->setMinimumSize(30, 30);
    ui->btnLink->setMaximumSize(30, 30);
    setCssProperty(ui->btnSelectMasternodes, "vote-inline-selector", true);
    setCssProperty(ui->lineEditCoinAmount, "edit-primary");
    setCssProperty(ui->frameVoteMode, "vote-mode-card", true);
    setCssProperty(ui->labelVoteMode, "vote-section-label", true);
    setCssProperty(ui->labelCoinAmount, "vote-section-label", true);
    setCssProperty(ui->radioModeMasternode, "vote-mode-toggle", true);
    setCssProperty(ui->radioModeCoinLock, "vote-mode-toggle", true);
    setCssProperty(ui->btnCoinAmount25, "btn-vote-quick");
    setCssProperty(ui->btnCoinAmount50, "btn-vote-quick");
    setCssProperty(ui->btnCoinAmount75, "btn-vote-quick");
    setCssProperty(ui->btnCoinAmountMax, "btn-vote-quick");
    setCssProperty(ui->labelCoinModeHint, "vote-dialog-subtitle", true);
    setCssProperty(ui->labelMnModeHint, "vote-dialog-subtitle", true);
    ui->containerCoinMode->setAttribute(Qt::WA_StyledBackground, true);
    ui->containerMnMode->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->containerCoinMode, "vote-body-panel", true);
    setCssProperty(ui->containerMnMode, "vote-body-panel", true);
    setCssProperty(ui->listMasternodesInline, "vote-mn-list");
    setCssProperty(ui->labelMnInlineSummary, "vote-meta-title");
    setCssProperty(ui->labelCoinProposalCapTitle, "vote-meta-title");
    setCssProperty(ui->labelCoinProposalCapValue, "vote-meta-value");
    setCssProperty(ui->labelCoinLockableBalanceTitle, "vote-meta-title");
    setCssProperty(ui->labelCoinLockableBalanceValue, "vote-meta-value");
    setCssProperty(ui->labelCoinAutoUnlockTitle, "vote-meta-title");
    setCssProperty(ui->labelCoinAutoUnlockValue, "vote-unlock-value");
    setCssProperty(ui->labelCoinValidationHint, "vote-validation-hint");
    setCssProperty(ui->labelCoinLockSafety, "vote-lock-badge");
    setCssProperty(ui->labelHybridStatus, "vote-hybrid-status");
    ui->scrollVoteContent->setFrameShape(QFrame::NoFrame);
    ui->scrollVoteContent->setWidgetResizable(true);
    ui->scrollVoteContent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->scrollVoteContent->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    progressBarNo = new QProgressBar(ui->containerNo);
    checkBoxNo = new QCheckBox(ui->containerNo);
    initVoteCheck(ui->containerNo, checkBoxNo, progressBarNo, "No", Qt::LayoutDirection::RightToLeft, false);

    progressBarYes = new QProgressBar(ui->containerYes);
    checkBoxYes = new QCheckBox(ui->containerYes);
    initVoteCheck(ui->containerYes, checkBoxYes, progressBarYes, "Yes", Qt::LayoutDirection::LeftToRight, true);

    GUIUtil::setupAmountWidget(ui->lineEditCoinAmount, this);
    ui->radioModeMasternode->setChecked(true);

    // Guard against platform/font scaling collisions in coin mode layout.
    ui->containerVotes->setMinimumHeight(40);
    ui->containerVotes->setMaximumHeight(42);
    ui->containerNo->setMinimumHeight(40);
    ui->containerNo->setMaximumHeight(42);
    ui->containerYes->setMinimumHeight(40);
    ui->containerYes->setMaximumHeight(42);
    ui->lineEditCoinAmount->setMinimumHeight(44);
    ui->btnCoinAmount25->setMinimumSize(82, 36);
    ui->btnCoinAmount50->setMinimumSize(82, 36);
    ui->btnCoinAmount75->setMinimumSize(82, 36);
    ui->btnCoinAmountMax->setMinimumSize(82, 36);
    ui->labelCoinModeHint->setWordWrap(true);
    ui->labelCoinValidationHint->setWordWrap(true);
    ui->labelCoinValidationHint->setMinimumHeight(30);
    ui->labelCoinLockSafety->setWordWrap(true);
    ui->labelCoinLockSafety->setMinimumHeight(36);
    ui->labelMnModeHint->setWordWrap(true);
    ui->labelMnInlineSummary->setWordWrap(true);
    const int bodyPanelHeight = 292;
    ui->containerCoinMode->setMinimumHeight(bodyPanelHeight);
    ui->containerCoinMode->setMaximumHeight(bodyPanelHeight);
    ui->containerMnMode->setMinimumHeight(bodyPanelHeight);
    ui->containerMnMode->setMaximumHeight(bodyPanelHeight);
    ui->listMasternodesInline->setMinimumHeight(168);
    ui->listMasternodesInline->setSelectionMode(QAbstractItemView::NoSelection);
    ui->listMasternodesInline->setUniformItemSizes(true);
    ui->listMasternodesInline->setAlternatingRowColors(false);
    ui->listMasternodesInline->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->btnSelectMasternodes->setMinimumHeight(40);
    if (ui->gridLayoutCoinMode) {
        ui->gridLayoutCoinMode->setVerticalSpacing(12);
        ui->gridLayoutCoinMode->setHorizontalSpacing(10);
        ui->gridLayoutCoinMode->setRowMinimumHeight(0, 46);
        ui->gridLayoutCoinMode->setRowMinimumHeight(1, 40);
    }
    if (ui->horizontalLayoutCoinAmountQuick) {
        ui->horizontalLayoutCoinAmountQuick->setContentsMargins(0, 6, 0, 2);
        ui->horizontalLayoutCoinAmountQuick->setSpacing(8);
    }
    if (ui->verticalLayoutCoinMode) {
        ui->verticalLayoutCoinMode->setSpacing(8);
    }
    if (ui->verticalLayoutMnMode) {
        ui->verticalLayoutMnMode->setSpacing(8);
    }
    if (ui->verticalLayout_2) {
        ui->verticalLayout_2->setSpacing(10);
    }
    if (ui->horizontalLayout_2) {
        ui->horizontalLayout_2->setSpacing(8);
    }
    if (ui->horizontalLayout_4) {
        ui->horizontalLayout_4->setContentsMargins(36, 0, 36, 0);
        ui->horizontalLayout_4->setSpacing(12);
        ui->horizontalLayout_4->setStretch(0, 1);
        ui->horizontalLayout_4->setStretch(1, 1);
    }
    auto ensureVoteModeRadioMinWidth = [](QRadioButton* radio) {
        if (!radio) return;
        const int indicator = radio->style()->pixelMetric(QStyle::PM_ExclusiveIndicatorWidth, nullptr, radio);
        const int spacing = radio->style()->pixelMetric(QStyle::PM_RadioButtonLabelSpacing, nullptr, radio);
        const int textWidth = radio->fontMetrics().horizontalAdvance(radio->text());
        radio->setMinimumWidth(textWidth + indicator + spacing + 16);
        radio->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    };
    ensureVoteModeRadioMinWidth(ui->radioModeMasternode);
    ensureVoteModeRadioMinWidth(ui->radioModeCoinLock);
    ui->btnCancel->setMinimumWidth(220);
    ui->btnSave->setMinimumWidth(220);
    ui->btnCancel->setMaximumWidth(260);
    ui->btnSave->setMaximumWidth(260);
    baseDialogWidth = std::max(width(), 820);
    baseDialogHeight = std::max(height(), 760);
    const QFont baseFont = font().pointSizeF() > 0 ? font() : QApplication::font();
    baseFontPointSize = baseFont.pointSizeF() > 0 ? baseFont.pointSizeF() : 10.0;
    setMinimumSize(520, 280);
    resize(baseDialogWidth, baseDialogHeight);

    connect(ui->btnSelectMasternodes, &QPushButton::clicked, this, &VoteDialog::onMnSelectionClicked);
    connect(ui->btnEsc, &QPushButton::clicked, this, &VoteDialog::close);
    connect(ui->btnCancel, &QPushButton::clicked, this, &VoteDialog::close);
    connect(ui->btnSave, &QPushButton::clicked, this, &VoteDialog::onAcceptClicked);
    connect(ui->radioModeMasternode, &QRadioButton::toggled, this, &VoteDialog::onVoteModeChanged);
    connect(ui->radioModeCoinLock, &QRadioButton::toggled, this, &VoteDialog::onVoteModeChanged);
    connect(ui->btnCoinAmount25, &QPushButton::clicked, [this]() { applyCoinAmountPreset(2500); });
    connect(ui->btnCoinAmount50, &QPushButton::clicked, [this]() { applyCoinAmountPreset(5000); });
    connect(ui->btnCoinAmount75, &QPushButton::clicked, [this]() { applyCoinAmountPreset(7500); });
    connect(ui->btnCoinAmountMax, &QPushButton::clicked, [this]() { applyCoinAmountPreset(10000); });
    connect(ui->lineEditCoinAmount, &QLineEdit::textChanged, this, [this](const QString&) {
        updateCoinAmountValidationState();
    });
    connect(ui->listMasternodesInline, &QListWidget::itemChanged, [this](QListWidgetItem* /*item*/) {
        if (updatingInlineMnList) return;
        syncSelectedMasternodesFromInlineList();
        updateMnSelectionNum();
    });
    ui->containerNo->installEventFilter(this);
    ui->containerYes->installEventFilter(this);
    progressBarNo->installEventFilter(this);
    progressBarYes->installEventFilter(this);

    refreshInlineMnList();
    updateVoteModeUi();
}

void VoteDialog::setProposal(const ProposalInfo& prop)
{
    proposal = std::make_unique<ProposalInfo>(prop);
    ui->labelTitleVote->setText(QString::fromStdString(prop.name));
    ui->labelAmount->setText(GUIUtil::formatBalance(prop.amount));
    const QString cycleText = prop.remainingPayments <= 0 ?
            tr("Final cycle in progress") :
            tr("%1 of %2 cycles remaining").arg(prop.remainingPayments).arg(prop.totalPayments);
    const QString durationText = prop.remainingPayments <= 0 ? QString() :
            tr(" (~%1)").arg(FormatDurationLabel(static_cast<int64_t>(prop.remainingPayments) * BudgetCycleSeconds()));
    ui->labelTime->setText(cycleText + durationText);
    double totalVotes = prop.votesYes + prop.votesNo;
    double percentageNo = (totalVotes == 0) ? 0 :  (prop.votesNo / totalVotes) * 100;
    double percentageYes = (totalVotes == 0) ? 0 : (prop.votesYes / totalVotes) * 100;
    progressBarNo->setValue((int)percentageNo);
    progressBarYes->setValue((int)percentageYes);
    checkBoxNo->setText(QString::number(prop.votesNo) + " /  " + QString::number(percentageNo) + "% " + tr("No"));
    checkBoxYes->setText(tr("Yes") + " " + QString::number(prop.votesYes) + " / " + QString::number(percentageYes) + "%");
    votes = govModel->getLocalMNsVotesForProposal(prop);
    refreshInlineMnList();
    updateCoinModeInfo();
    updateHybridStatusText();
    updateVoteModeUi();
    updateMnSelectionNum();
}

void VoteDialog::onAcceptClicked()
{
    if (!proposal) {
        inform(tr("No proposal selected"));
        return;
    }

    bool isPositive = checkBoxYes->isChecked();
    bool isNegative = checkBoxNo->isChecked();

    if (!isPositive && !isNegative) {
        if (isCoinVoteMode()) {
            ui->labelCoinValidationHint->setText(tr("Select Yes or No before submitting a coin vote"));
            applyCoinAmountInvalidState(true);
        }
        inform(tr("Select a vote direction"));
        return;
    }

    if (isCoinVoteMode()) {
        std::unique_ptr<WalletModel::UnlockContext> unlockCtx;
        if (walletModel) {
            unlockCtx = std::make_unique<WalletModel::UnlockContext>(walletModel->requestUnlock());
            if (!unlockCtx->isValid()) {
                inform(tr("Wallet must be unlocked to vote with coin locks"));
                return;
            }
        }

        CAmount lockAmount = 0;
        const OperationResult amountValidation = validateCoinLockAmount(&lockAmount);
        if (!amountValidation) {
            ui->labelCoinValidationHint->setText(QString::fromStdString(amountValidation.getError()));
            applyCoinAmountInvalidState(true);
            inform(QString::fromStdString(amountValidation.getError()));
            return;
        }

        const uint32_t unlockHeight = autoUnlockHeight();
        if (unlockHeight == 0) {
            inform(tr("Cannot compute unlock height for this proposal"));
            return;
        }
        auto res = govModel->createVoteLockAndCast(*proposal, isPositive, lockAmount, unlockHeight);
        if (!res) {
            ui->labelCoinValidationHint->setText(QString::fromStdString(res.getError()));
            applyCoinAmountInvalidState(true);
            inform(QString::fromStdString(res.getError()));
            return;
        }
        accept();
        return;
    }

    syncSelectedMasternodesFromInlineList();
    if (vecSelectedMn.empty()) {
        inform(tr("Missing voting masternodes selection"));
        return;
    }

    // Check time between votes.
    for (const auto& vote : votes) {
        auto it = std::find(vecSelectedMn.begin(), vecSelectedMn.end(), vote.mnAlias);
        if (it != vecSelectedMn.end()) {
            if (vote.time + govModel->getProposalVoteUpdateMinTime() > GetAdjustedTime()) {
                inform(tr("Time between votes is too soon, have to wait %1 minutes").arg(govModel->getProposalVoteUpdateMinTime()/60));
                return;
            }
        }
    }

    // Craft and broadcast vote
    auto res = govModel->voteForProposal(*proposal, isPositive, vecSelectedMn);
    if (!res) {
        inform(QString::fromStdString(res.getError()));
        return;
    }
    accept();
}

int VoteDialog::scaledMetric(int baseValue, double scaleFactor, int minValue) const
{
    const int scaled = static_cast<int>(std::lround(static_cast<double>(baseValue) * scaleFactor));
    return std::max(minValue, scaled);
}

void VoteDialog::setupStickyHeader()
{
    if (!ui->verticalLayout_1 || !ui->scrollVoteContent || !ui->labelTitle || !ui->btnEsc) {
        return;
    }

    shellContainer = new QFrame(this);
    shellContainer->setObjectName("voteDialogShell");
    shellContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* shellLayout = new QVBoxLayout(shellContainer);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);

    ui->verticalLayout_1->removeWidget(ui->scrollVoteContent);
    ui->verticalLayout_1->addWidget(shellContainer);

    headerBar = new QWidget(shellContainer);
    headerBar->setObjectName("voteDialogHeader");
    headerBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    headerBar->setMinimumHeight(52);
    headerBar->setMaximumHeight(58);

    auto* headerLayout = new QHBoxLayout(headerBar);
    headerLayout->setContentsMargins(18, 10, 14, 10);
    headerLayout->setSpacing(8);

    if (ui->horizontalLayout_1) {
        ui->horizontalLayout_1->removeWidget(ui->labelTitle);
        ui->horizontalLayout_1->removeWidget(ui->btnEsc);
        while (ui->horizontalLayout_1->count() > 0) {
            delete ui->horizontalLayout_1->takeAt(0);
        }
    }

    ui->labelTitle->setParent(headerBar);
    ui->labelTitle->setAlignment(Qt::AlignCenter);
    ui->btnEsc->setParent(headerBar);
    ui->btnEsc->setMinimumSize(30, 30);
    ui->btnEsc->setMaximumSize(30, 30);

    auto* leftAnchor = new QWidget(headerBar);
    leftAnchor->setFixedSize(ui->btnEsc->minimumSize());
    leftAnchor->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    headerLayout->addWidget(leftAnchor, 0, Qt::AlignLeft | Qt::AlignVCenter);
    headerLayout->addWidget(ui->labelTitle, 1, Qt::AlignCenter);
    headerLayout->addWidget(ui->btnEsc, 0, Qt::AlignRight | Qt::AlignVCenter);

    shellLayout->addWidget(headerBar, 0);
    shellLayout->addWidget(ui->scrollVoteContent, 1);
    setDialogPopAnimationTarget(this, shellContainer);

    ui->scrollVoteContent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->scrollVoteContent->setFrameShape(QFrame::NoFrame);
    ui->scrollVoteContent->setAutoFillBackground(false);
    if (QWidget* viewport = ui->scrollVoteContent->viewport()) {
        viewport->setAutoFillBackground(false);
        viewport->setAttribute(Qt::WA_NoSystemBackground, true);
        viewport->setAttribute(Qt::WA_TranslucentBackground, true);
    }

    setCssProperty(shellContainer, "vote-dialog-shell", true);
    setCssProperty(headerBar, "vote-dialog-header", true);
    ui->frame->setFrameShape(QFrame::NoFrame);
    setCssProperty(ui->frame, "vote-dialog-body", true);

    headerBar->installEventFilter(this);
}

bool VoteDialog::tryStartSystemMove()
{
    if (QWindow* win = windowHandle()) {
        return win->startSystemMove();
    }
    return false;
}

void VoteDialog::applyScaleFactor(double scaleFactor)
{
    const double scale = std::max(0.65, std::min(1.0, scaleFactor));

    QFont dialogFont = font();
    if (baseFontPointSize > 0.0) {
        dialogFont.setPointSizeF(std::max(8.0, baseFontPointSize * scale));
        setFont(dialogFont);
    }

    if (ui->verticalLayout_2) {
        ui->verticalLayout_2->setContentsMargins(
                scaledMetric(18, scale, 12),
                scaledMetric(14, scale, 8),
                scaledMetric(18, scale, 12),
                scaledMetric(14, scale, 8));
        ui->verticalLayout_2->setSpacing(scaledMetric(10, scale, 6));
    }
    if (headerBar) {
        const int headerHeight = scaledMetric(52, scale, 42);
        headerBar->setMinimumHeight(headerHeight);
        headerBar->setMaximumHeight(headerHeight + 4);
        if (auto* headerLayout = qobject_cast<QHBoxLayout*>(headerBar->layout())) {
            headerLayout->setContentsMargins(
                    scaledMetric(18, scale, 12),
                    scaledMetric(10, scale, 6),
                    scaledMetric(14, scale, 10),
                    scaledMetric(10, scale, 6));
            headerLayout->setSpacing(scaledMetric(8, scale, 4));
        }
    }
    if (ui->verticalLayoutCoinMode) {
        ui->verticalLayoutCoinMode->setSpacing(scaledMetric(8, scale, 5));
    }
    if (ui->verticalLayoutMnMode) {
        ui->verticalLayoutMnMode->setSpacing(scaledMetric(6, scale, 4));
    }
    if (ui->gridLayoutCoinMode) {
        ui->gridLayoutCoinMode->setVerticalSpacing(scaledMetric(12, scale, 7));
        ui->gridLayoutCoinMode->setHorizontalSpacing(scaledMetric(10, scale, 6));
        ui->gridLayoutCoinMode->setRowMinimumHeight(0, scaledMetric(46, scale, 32));
        ui->gridLayoutCoinMode->setRowMinimumHeight(1, scaledMetric(40, scale, 28));
    }
    if (ui->horizontalLayoutCoinAmountQuick) {
        ui->horizontalLayoutCoinAmountQuick->setContentsMargins(
                0,
                scaledMetric(6, scale, 3),
                0,
                scaledMetric(2, scale, 1));
        ui->horizontalLayoutCoinAmountQuick->setSpacing(scaledMetric(8, scale, 5));
    }

    const int voteRowHeight = scaledMetric(40, scale, 32);
    ui->containerVotes->setMinimumHeight(voteRowHeight);
    ui->containerVotes->setMaximumHeight(voteRowHeight + 2);
    ui->containerNo->setMinimumHeight(voteRowHeight);
    ui->containerNo->setMaximumHeight(voteRowHeight + 2);
    ui->containerYes->setMinimumHeight(voteRowHeight);
    ui->containerYes->setMaximumHeight(voteRowHeight + 2);

    ui->lineEditCoinAmount->setMinimumHeight(scaledMetric(44, scale, 36));
    const int quickButtonWidth = scaledMetric(82, scale, 66);
    const int quickButtonHeight = scaledMetric(36, scale, 30);
    ui->btnCoinAmount25->setMinimumSize(quickButtonWidth, quickButtonHeight);
    ui->btnCoinAmount50->setMinimumSize(quickButtonWidth, quickButtonHeight);
    ui->btnCoinAmount75->setMinimumSize(quickButtonWidth, quickButtonHeight);
    ui->btnCoinAmountMax->setMinimumSize(quickButtonWidth, quickButtonHeight);
    ui->labelCoinValidationHint->setMinimumHeight(scaledMetric(30, scale, 24));
    ui->labelCoinLockSafety->setMinimumHeight(scaledMetric(36, scale, 28));
    const int bodyPanelHeight = scaledMetric(292, scale, 224);
    ui->containerCoinMode->setMinimumHeight(bodyPanelHeight);
    ui->containerCoinMode->setMaximumHeight(bodyPanelHeight);
    ui->containerMnMode->setMinimumHeight(bodyPanelHeight);
    ui->containerMnMode->setMaximumHeight(bodyPanelHeight);
    ui->listMasternodesInline->setMinimumHeight(scaledMetric(168, scale, 126));
    ui->btnSelectMasternodes->setMinimumHeight(scaledMetric(40, scale, 34));

    if (ui->verticalSpacerBeforeActions) {
        ui->verticalSpacerBeforeActions->changeSize(
                20,
                scaledMetric(12, scale, 6),
                QSizePolicy::Minimum,
                QSizePolicy::Fixed);
    }

    const int actionHeight = scaledMetric(46, scale, 38);
    ui->btnCancel->setMinimumHeight(actionHeight);
    ui->btnSave->setMinimumHeight(actionHeight);
    ui->btnCancel->setMinimumWidth(scaledMetric(220, scale, 180));
    ui->btnSave->setMinimumWidth(scaledMetric(220, scale, 180));
    ui->btnCancel->setMaximumWidth(scaledMetric(260, scale, 320));
    ui->btnSave->setMaximumWidth(scaledMetric(260, scale, 320));
    if (ui->horizontalLayout_4) {
        const int horizontalInset = scaledMetric(36, scale, 20);
        ui->horizontalLayout_4->setContentsMargins(horizontalInset, 0, horizontalInset, 0);
        ui->horizontalLayout_4->setSpacing(scaledMetric(12, scale, 8));
    }

    if (layout()) {
        layout()->invalidate();
        layout()->activate();
    }
}

void VoteDialog::applyAdaptiveLayoutForScreen()
{
    constexpr int kOuterMargin = 20;
    constexpr int kMinDialogHeight = 300;
    constexpr int kMinDialogWidth = 520;

    QRect available;
    QWidget* host = parentWidget();
    if (host) {
        host = host->window();
    }

    if (host && host->isVisible() && host->width() > 0 && host->height() > 0) {
        available = host->rect();
    } else {
        QScreen* screen = ResolveDialogScreen(this);
        if (!screen) return;
        available = screen->availableGeometry();
    }

    const int maxHeight = std::max(260, available.height() - kOuterMargin);
    const int maxWidth = std::max(kMinDialogWidth, available.width() - kOuterMargin);
    const int referenceHeight = std::max(baseDialogHeight, sizeHint().height());
    const double rawScale = std::min(1.0, static_cast<double>(maxHeight) / static_cast<double>(referenceHeight));

    applyScaleFactor(rawScale);

    const int minScaledHeight = std::min(kMinDialogHeight, maxHeight);
    const int targetHeight = std::min(maxHeight, scaledMetric(referenceHeight, std::max(rawScale, 0.65), minScaledHeight));
    const int targetWidth = std::min(baseDialogWidth, maxWidth);
    resize(targetWidth, targetHeight);

    if (layout()) {
        layout()->invalidate();
        layout()->activate();
    }

    if (ui->scrollVoteContent) {
        ui->scrollVoteContent->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        if (ui->scrollVoteContent->widget()) {
            ui->scrollVoteContent->widget()->adjustSize();
        }
        if (QScrollBar* sb = ui->scrollVoteContent->verticalScrollBar()) {
            sb->setSingleStep(24);
            sb->setPageStep(std::max(120, ui->scrollVoteContent->viewport()->height() / 3));
        }

        // Grow the full shell width to eliminate hidden body horizontal overflow.
        for (int attempt = 0; attempt < 4; ++attempt) {
            QScrollBar* hBar = ui->scrollVoteContent->horizontalScrollBar();
            const int overflow = hBar ? hBar->maximum() : 0;
            if (overflow <= 0 || width() >= maxWidth) {
                break;
            }

            const int nextWidth = std::min(maxWidth, width() + overflow + 2);
            if (nextWidth <= width()) {
                break;
            }

            resize(nextWidth, height());
            if (layout()) {
                layout()->invalidate();
                layout()->activate();
            }
            if (ui->scrollVoteContent->widget()) {
                ui->scrollVoteContent->widget()->adjustSize();
            }
        }
    }
}

void VoteDialog::showEvent(QShowEvent *event)
{
    applyAdaptiveLayoutForScreen();
    ContainerDialog::showEvent(event);
}

void VoteDialog::onMnSelectionClicked()
{
    PIVXGUI* window = dynamic_cast<PIVXGUI*>(parent());
    if (!mnSelectionDialog) {
        mnSelectionDialog = new MnSelectionDialog(window);
        mnSelectionDialog->setModel(mnModel, govModel->getProposalVoteUpdateMinTime());
    }
    mnSelectionDialog->setMnVoters(votes);
    mnSelectionDialog->updateView();
    mnSelectionDialog->resize(size());
    if (openDialogWithOpaqueBackgroundY(mnSelectionDialog, window, 4.5, 5, false)) {
        vecSelectedMn = mnSelectionDialog->getSelectedMnAlias();
        setInlineSelectedMasternodes(vecSelectedMn);
        updateMnSelectionNum();
    }
}

void VoteDialog::onCheckBoxClicked(QCheckBox* checkBox, QProgressBar* /*progressBar*/, bool isVoteYes)
{
    if (isVoteYes) {
        checkBoxNo->setCheckState(Qt::Unchecked);
    } else {
        checkBoxYes->setCheckState(Qt::Unchecked);
    }
    updateCoinAmountValidationState();
}

void VoteDialog::initVoteCheck(QWidget* container, QCheckBox* checkBox, QProgressBar* progressBar, const QString& text, Qt::LayoutDirection direction, bool isVoteYes)
{
    QGridLayout* gridLayout = dynamic_cast<QGridLayout*>(container->layout());
    progressBar->setMaximum(100);
    progressBar->setMinimum(0);
    progressBar->setLayoutDirection(direction);
    progressBar->setTextVisible(false);
    progressBar->setAlignment(Qt::AlignCenter);
    progressBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    progressBar->setOrientation(Qt::Horizontal);
    progressBar->setContentsMargins(0,0,0,0);
    progressBar->setMinimumHeight(40);
    progressBar->setMaximumHeight(40);
    setCssProperty(progressBar, "vote-progress-yes");
    gridLayout->addWidget(progressBar, 0, 0, 1, 1);
    progressBar->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    checkBox->setText(text);
    checkBox->setLayoutDirection(direction);
    checkBox->setMinimumHeight(40);
    checkBox->setMaximumHeight(40);
    checkBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    checkBox->setContentsMargins(0, 0, 0, 0);
    setCssProperty(checkBox, "check-vote");
    checkBox->setObjectName(isVoteYes ? "checkVoteYes" : "checkVoteNo");
    checkBox->setProperty("voteChoice", isVoteYes ? "yes" : "no");
    gridLayout->addWidget(checkBox, 0, 0, 1, 1);
    setCssProperty(container, "vote-grid");
    gridLayout->setContentsMargins(0, 0, 0, 0);
    container->setContentsMargins(0,0,0,0);
    connect(checkBox, &QCheckBox::clicked, [this, checkBox, progressBar, isVoteYes](){ onCheckBoxClicked(checkBox, progressBar, isVoteYes); });
    checkBox->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    checkBox->show();
}

void VoteDialog::onVoteModeChanged()
{
    updateVoteModeUi();
}

bool VoteDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == headerBar && event) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent && mouseEvent->button() == Qt::LeftButton) {
                if (tryStartSystemMove()) {
                    headerDragging = false;
                    return true;
                }
                headerDragging = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                dragStartGlobal = mouseEvent->globalPosition().toPoint();
#else
                dragStartGlobal = mouseEvent->globalPos();
#endif
                dragStartDialogPos = pos();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && headerDragging) {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent && (mouseEvent->buttons() & Qt::LeftButton)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                const QPoint currentGlobal = mouseEvent->globalPosition().toPoint();
#else
                const QPoint currentGlobal = mouseEvent->globalPos();
#endif
                move(dragStartDialogPos + (currentGlobal - dragStartGlobal));
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent && mouseEvent->button() == Qt::LeftButton) {
                headerDragging = false;
                return true;
            }
        }
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent && mouseEvent->button() == Qt::LeftButton) {
            if ((watched == ui->containerYes || watched == progressBarYes) && checkBoxYes && checkBoxYes->isEnabled()) {
                checkBoxYes->click();
                return true;
            }
            if ((watched == ui->containerNo || watched == progressBarNo) && checkBoxNo && checkBoxNo->isEnabled()) {
                checkBoxNo->click();
                return true;
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

bool VoteDialog::isCoinVoteMode() const
{
    return ui->radioModeCoinLock->isChecked();
}

CAmount VoteDialog::parseCoinLockAmount(bool& ok) const
{
    const CAmount amount = GUIUtil::parseValue(ui->lineEditCoinAmount->text(), BitcoinUnits::PIV);
    ok = amount > 0;
    return amount;
}

CAmount VoteDialog::getProposalCap() const
{
    if (!proposal || proposal->amount <= 0 || proposal->remainingPayments <= 0) {
        return 0;
    }

    const auto remainingPayments = static_cast<CAmount>(proposal->remainingPayments);
    if (proposal->amount > std::numeric_limits<CAmount>::max() / remainingPayments) {
        return std::numeric_limits<CAmount>::max();
    }
    return proposal->amount * remainingPayments;
}

QString VoteDialog::getCoinAmountLockedReason() const
{
    if (!proposal) {
        return tr("No proposal selected");
    }

    if (getProposalCap() <= 0) {
        if (proposal->isFinished() || proposal->remainingPayments <= 0) {
            return tr("Coin voting is unavailable because this proposal is finished");
        }
        return tr("Coin voting is unavailable because this proposal has no remaining payout");
    }

    if (getCoinLockableBalance() <= 0) {
        return tr("Coin voting is unavailable because you have no lockable balance");
    }

    return {};
}

OperationResult VoteDialog::validateCoinLockAmount(CAmount* amountOut) const
{
    const QString lockedReason = getCoinAmountLockedReason();
    if (!lockedReason.isEmpty()) {
        return errorOut(lockedReason.toStdString());
    }

    const CAmount proposalCap = getProposalCap();
    const CAmount lockable = getCoinLockableBalance();
    bool amountValid = false;
    const CAmount lockAmount = parseCoinLockAmount(amountValid);
    if (!amountValid) {
        return errorOut("Enter a valid coin lock amount");
    }
    if (lockAmount > proposalCap) {
        return errorOut("Lock amount exceeds proposal cap");
    }
    if (lockAmount > lockable) {
        return errorOut("Lock amount exceeds lockable balance");
    }

    if (amountOut) {
        *amountOut = lockAmount;
    }
    return {true};
}

void VoteDialog::applyCoinAmountInvalidState(bool invalid)
{
    ui->lineEditCoinAmount->setProperty("coinAmountInvalid", invalid);
    ui->labelCoinValidationHint->setProperty("coinAmountError", invalid);
    ui->lineEditCoinAmount->style()->unpolish(ui->lineEditCoinAmount);
    ui->lineEditCoinAmount->style()->polish(ui->lineEditCoinAmount);
    ui->lineEditCoinAmount->update();
    ui->labelCoinValidationHint->style()->unpolish(ui->labelCoinValidationHint);
    ui->labelCoinValidationHint->style()->polish(ui->labelCoinValidationHint);
    ui->labelCoinValidationHint->update();
}

void VoteDialog::updateCoinAmountValidationState()
{
    if (!isCoinVoteMode()) {
        applyCoinAmountInvalidState(false);
        ui->labelCoinValidationHint->clear();
        ui->labelCoinValidationHint->setVisible(false);
        ui->btnSave->setEnabled(true);
        return;
    }

    if (!ui->lineEditCoinAmount->isEnabled()) {
        applyCoinAmountInvalidState(true);
        ui->labelCoinValidationHint->setVisible(!ui->labelCoinValidationHint->text().isEmpty());
        ui->btnSave->setEnabled(false);
        return;
    }

    const CAmount proposalCap = getProposalCap();
    const CAmount lockable = getCoinLockableBalance();
    const CAmount maxLockableForProposal = std::min(proposalCap, lockable);

    if (ui->lineEditCoinAmount->text().trimmed().isEmpty()) {
        applyCoinAmountInvalidState(false);
        Q_UNUSED(maxLockableForProposal);
        ui->labelCoinValidationHint->clear();
        ui->labelCoinValidationHint->setVisible(false);
        ui->btnSave->setEnabled(false);
        return;
    }

    const OperationResult validation = validateCoinLockAmount();
    const bool valid = static_cast<bool>(validation);
    ui->btnSave->setEnabled(valid);

    if (valid) {
        applyCoinAmountInvalidState(false);
        Q_UNUSED(maxLockableForProposal);
        ui->labelCoinValidationHint->clear();
        ui->labelCoinValidationHint->setVisible(false);
        return;
    }

    applyCoinAmountInvalidState(true);
    ui->labelCoinValidationHint->setText(QString::fromStdString(validation.getError()));
    ui->labelCoinValidationHint->setVisible(true);
}

uint32_t VoteDialog::autoUnlockHeight() const
{
    if (!proposal || proposal->endBlock <= 0) return 0;
    return static_cast<uint32_t>(proposal->endBlock);
}

CAmount VoteDialog::getCoinLockableBalance() const
{
    if (!govModel) return 0;
    return govModel->getCoinLockableBalance();
}

void VoteDialog::applyCoinAmountPreset(int basisPoints)
{
    if (basisPoints <= 0) {
        return;
    }

    const CAmount proposalCap = getProposalCap();
    const CAmount lockable = getCoinLockableBalance();
    if (proposalCap <= 0 || lockable <= 0) {
        ui->lineEditCoinAmount->clear();
        updateCoinAmountValidationState();
        return;
    }

    CAmount amount = 0;
    if (basisPoints >= 10000) {
        amount = proposalCap;
    } else {
        const CAmount quotient = proposalCap / 10000;
        const CAmount remainder = proposalCap % 10000;
        amount = quotient * basisPoints + (remainder * basisPoints) / 10000;
    }

    amount = std::min(amount, lockable);
    if (amount <= 0) {
        amount = std::min(proposalCap, lockable);
    }
    ui->lineEditCoinAmount->setText(BitcoinUnits::format(BitcoinUnits::PIV, amount, false, BitcoinUnits::separatorNever, true));
    updateCoinAmountValidationState();
}

void VoteDialog::refreshInlineMnList()
{
    if (!ui->listMasternodesInline) {
        return;
    }

    std::unordered_set<std::string> selectedAliases(vecSelectedMn.begin(), vecSelectedMn.end());
    if (selectedAliases.empty()) {
        for (const auto& vote : votes) {
            selectedAliases.insert(vote.mnAlias);
        }
    }

    updatingInlineMnList = true;
    ui->listMasternodesInline->clear();

    if (!mnModel || mnModel->rowCount() == 0) {
        auto* emptyItem = new QListWidgetItem(tr("No masternodes available"), ui->listMasternodesInline);
        emptyItem->setFlags(Qt::NoItemFlags);
        updatingInlineMnList = false;
        vecSelectedMn.clear();
        updateInlineMnSummary();
        return;
    }

    for (int i = 0; i < mnModel->rowCount(); ++i) {
        const QString alias = mnModel->index(i, MNModel::ALIAS, QModelIndex()).data().toString();
        const QString status = mnModel->index(i, MNModel::STATUS, QModelIndex()).data().toString();
        if (alias.isEmpty()) {
            continue;
        }

        auto* item = new QListWidgetItem(tr("%1  •  %2").arg(alias, status), ui->listMasternodesInline);
        item->setData(Qt::UserRole, alias);
        item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        item->setCheckState(selectedAliases.count(alias.toStdString()) > 0 ? Qt::Checked : Qt::Unchecked);

        if (status != "ENABLED") {
            item->setFlags(Qt::NoItemFlags);
            item->setToolTip(tr("Only ENABLED masternodes can vote"));
            item->setCheckState(Qt::Unchecked);
        }
    }

    updatingInlineMnList = false;
    syncSelectedMasternodesFromInlineList();
    updateInlineMnSummary();
}

void VoteDialog::syncSelectedMasternodesFromInlineList()
{
    if (!ui->listMasternodesInline) {
        return;
    }

    std::vector<std::string> selected;
    selected.reserve(static_cast<size_t>(ui->listMasternodesInline->count()));
    for (int i = 0; i < ui->listMasternodesInline->count(); ++i) {
        QListWidgetItem* item = ui->listMasternodesInline->item(i);
        if (!item) {
            continue;
        }
        const QVariant aliasData = item->data(Qt::UserRole);
        if (!aliasData.isValid() || !item->flags().testFlag(Qt::ItemIsEnabled)) {
            continue;
        }
        if (item->checkState() == Qt::Checked) {
            selected.emplace_back(aliasData.toString().toStdString());
        }
    }
    vecSelectedMn = std::move(selected);
    updateInlineMnSummary();
}

void VoteDialog::setInlineSelectedMasternodes(const std::vector<std::string>& selected)
{
    if (!ui->listMasternodesInline) {
        return;
    }

    std::unordered_set<std::string> selectedAliases(selected.begin(), selected.end());
    updatingInlineMnList = true;
    for (int i = 0; i < ui->listMasternodesInline->count(); ++i) {
        QListWidgetItem* item = ui->listMasternodesInline->item(i);
        if (!item) {
            continue;
        }
        const QVariant aliasData = item->data(Qt::UserRole);
        if (!aliasData.isValid() || !item->flags().testFlag(Qt::ItemIsEnabled)) {
            continue;
        }
        item->setCheckState(selectedAliases.count(aliasData.toString().toStdString()) > 0 ? Qt::Checked : Qt::Unchecked);
    }
    updatingInlineMnList = false;
    syncSelectedMasternodesFromInlineList();
}

void VoteDialog::updateInlineMnSummary()
{
    if (vecSelectedMn.empty() && !votes.empty()) {
        ui->labelMnInlineSummary->setText(tr("No masternodes selected (you already voted with %1)").arg(votes.size()));
        return;
    }
    ui->labelMnInlineSummary->setText(tr("%1 masternodes selected").arg(vecSelectedMn.size()));
}

void VoteDialog::updateVoteModeUi()
{
    const bool coinMode = isCoinVoteMode();
    ui->containerCoinMode->setVisible(coinMode);
    ui->containerMnMode->setVisible(!coinMode);
    ui->labelSubtitle->setText(coinMode ?
            tr("Select vote direction and coin lock details") :
            tr("Select vote direction and the masternodes that will vote for it"));
    ui->labelMessage->setText(tr("You can change your vote later"));
    ui->labelMessage->setVisible(!coinMode);
    if (!coinMode) {
        refreshInlineMnList();
    }
    updateCoinModeInfo();
    updateCoinAmountValidationState();
}

void VoteDialog::updateCoinModeInfo()
{
    const CAmount proposalCap = getProposalCap();
    const CAmount lockable = getCoinLockableBalance();
    ui->labelCoinProposalCapValue->setText(BitcoinUnits::formatWithUnit(BitcoinUnits::PIV, proposalCap));
    ui->labelCoinLockableBalanceValue->setText(BitcoinUnits::formatWithUnit(BitcoinUnits::PIV, lockable));
    const uint32_t unlockHeight = autoUnlockHeight();
    ui->labelCoinAutoUnlockValue->setText(QString::number(unlockHeight));
    const int remainingCycles = proposal ? (proposal->remainingPayments > 0 ? proposal->remainingPayments : 0) : 0;
    if (remainingCycles > 0) {
        const QString duration = FormatDurationLabel(static_cast<int64_t>(remainingCycles) * BudgetCycleSeconds());
        ui->labelCoinLockSafety->setText(tr("Locked and excluded from coin control until proposal end (~%1)").arg(duration));
    } else {
        ui->labelCoinLockSafety->setText(tr("Locked and excluded from coin control until proposal end"));
    }

    const QString lockedReason = getCoinAmountLockedReason();
    const bool hasSelectableAmount = lockedReason.isEmpty();
    ui->lineEditCoinAmount->setEnabled(hasSelectableAmount);
    ui->lineEditCoinAmount->setToolTip(lockedReason);
    ui->btnCoinAmount25->setEnabled(hasSelectableAmount);
    ui->btnCoinAmount50->setEnabled(hasSelectableAmount);
    ui->btnCoinAmount75->setEnabled(hasSelectableAmount);
    ui->btnCoinAmountMax->setEnabled(hasSelectableAmount);

    if (!hasSelectableAmount) {
        ui->labelCoinValidationHint->setText(lockedReason);
        ui->labelCoinValidationHint->setVisible(true);
        applyCoinAmountInvalidState(true);
        return;
    }

    applyCoinAmountInvalidState(false);
    ui->labelCoinValidationHint->clear();
    ui->labelCoinValidationHint->setVisible(false);
}

void VoteDialog::updateHybridStatusText()
{
    if (!proposal) {
        ui->labelHybridStatus->setVisible(false);
        return;
    }

    HybridVoteStatus status;
    auto result = govModel->getProposalHybridVoteStatus(*proposal, status);
    if (!result) {
        ui->labelHybridStatus->setVisible(false);
        return;
    }

    ui->labelHybridStatus->setVisible(true);
    ui->labelHybridStatus->setText(
            tr("MN %1/%2 | Coin %3/%4 | Score %5")
                    .arg(status.mnYes)
                    .arg(status.mnNo)
                    .arg(status.coinYes)
                    .arg(status.coinNo)
                    .arg(QString::number(status.combinedScore, 'f', 2)));
}

void VoteDialog::updateMnSelectionNum()
{
    updateInlineMnSummary();

    QString text;
    if (vecSelectedMn.empty()) {
        text = !votes.empty() ? tr("You have voted with %1 Masternodes for this proposal\nChange votes").arg(votes.size()) :
                tr("Open advanced masternode selector");
    } else {
        text = tr("%1 Masternodes selected to vote").arg(vecSelectedMn.size());
    }
    ui->btnSelectMasternodes->setText(text);
}

void VoteDialog::inform(const QString& text)
{
    if (!snackBar) snackBar = new SnackBar(nullptr, this);
    snackBar->setText(text);
    snackBar->resize(this->width(), snackBar->height());
    PIVXGUI* gui = dynamic_cast<PIVXGUI*>(parentWidget());
    if (!gui) {
        QWidget* parent = parentWidget();
        while (parent && !gui) {
            gui = dynamic_cast<PIVXGUI*>(parent);
            parent = parent->parentWidget();
        }
    }
    openDialog(snackBar, gui ? static_cast<QWidget*>(gui) : this);
}

VoteDialog::~VoteDialog()
{
    delete ui;
}
