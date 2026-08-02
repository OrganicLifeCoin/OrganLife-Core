// Copyright (c) 2021-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "proposalcard.h"
#include "ui_proposalcard.h"

#include "chainparams.h"
#include "qtutils.h"

#include <QFont>
#include <QGraphicsDropShadowEffect>

#include <algorithm>
#include <cmath>

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
} // namespace

ProposalCard::ProposalCard(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ProposalCard)
{
    ui->setupUi(this);
    if (parent) {
        setStyleSheet(parent->styleSheet());
    }

    auto* shadow = new QGraphicsDropShadowEffect(this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    shadow->setBlurRadius(36.0);
    shadow->setOffset(0.0, 10.0);
    shadow->setColor(QColor(58, 36, 24, 38));
#else
    shadow->setBlurRadius(30.0);
    shadow->setOffset(0.0, 8.0);
    shadow->setColor(QColor(58, 36, 24, 28));
#endif
    setGraphicsEffect(shadow);

    setCssProperty(ui->btnVote, "btn-primary");
    setCssProperty(ui->card, "card-governance");
    setCssProperty(ui->labelPropName, "card-title");
    setCssProperty(ui->labelPropAmount, "card-amount");
    setCssProperty(ui->labelPropMonths, "card-time");
    setCssProperty(ui->labelStatus, "card-status-passing");
    setCssProperty(ui->btnVote, "card-btn-vote");
    setCssProperty(ui->btnLink, "btn-menu");
    setCssProperty(ui->containerVotes, "card-progress-box");
    ui->labelNo->setStyleSheet(QString());
    ui->labelYes->setStyleSheet(QString());
    setCssProperty(ui->labelNo, "label-progress-no");
    setCssProperty(ui->labelYes, "label-progress-yes");
    ui->containerVotes->setContentsMargins(0, 0, 0, 0);
    ui->containerVotes->setMinimumHeight(32);
    ui->containerVotes->layout()->setContentsMargins(0, 0, 0, 0);
    ui->containerText->setFixedHeight(ui->containerVotes->minimumHeight());
    ui->containerText->layout()->setContentsMargins(12, 0, 12, 0);

    QFont titleFont = ui->labelPropName->font();
    titleFont.setPointSize(std::max(titleFont.pointSize(), 15));
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setKerning(true);
    titleFont.setStyleStrategy(QFont::PreferAntialias);
    ui->labelPropName->setFont(titleFont);
    ui->labelPropName->setWordWrap(true);

    QFont amountFont = ui->labelPropAmount->font();
    amountFont.setPointSize(std::max(amountFont.pointSize(), 18));
    amountFont.setWeight(QFont::Bold);
    amountFont.setKerning(true);
    amountFont.setStyleStrategy(QFont::PreferAntialias);
    ui->labelPropAmount->setFont(amountFont);

    QFont metaFont = ui->labelPropMonths->font();
    metaFont.setPointSize(std::max(metaFont.pointSize(), 11));
    metaFont.setWeight(QFont::Medium);
    metaFont.setStyleStrategy(QFont::PreferAntialias);
    ui->labelPropMonths->setFont(metaFont);

    QFont statusFont = ui->labelStatus->font();
    statusFont.setPointSize(std::max(statusFont.pointSize(), 10));
    statusFont.setWeight(QFont::DemiBold);
    statusFont.setStyleStrategy(QFont::PreferAntialias);
    ui->labelStatus->setFont(statusFont);
    ui->labelStatus->setAlignment(Qt::AlignCenter);

    QFont voteFont = ui->labelNo->font();
    voteFont.setPointSize(std::max(voteFont.pointSize(), 10));
    voteFont.setWeight(QFont::DemiBold);
    voteFont.setStyleStrategy(QFont::PreferAntialias);
    ui->labelNo->setFont(voteFont);
    ui->labelYes->setFont(voteFont);
    ui->labelNo->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    ui->labelYes->setAlignment(Qt::AlignVCenter | Qt::AlignRight);

    ui->votesNoBar->setMaximum(100);
    ui->votesNoBar->setMinimum(0);
    ui->votesNoBar->setTextVisible(false);
    ui->votesNoBar->setContentsMargins(0, 0, 0, 0);
    ui->votesNoBar->setFixedHeight(32);
    ui->votesNoBar->setInvertedAppearance(true);
    setCssProperty(ui->votesNoBar, "card-progress-no");
    ui->votesYesBar->setMaximum(100);
    ui->votesYesBar->setMinimum(0);
    ui->votesYesBar->setTextVisible(false);
    ui->votesYesBar->setContentsMargins(0, 0, 0, 0);
    ui->votesYesBar->setFixedHeight(32);
    setCssProperty(ui->votesYesBar, "card-progress-yes");

    QFont voteButtonFont = ui->btnVote->font();
    voteButtonFont.setPointSize(std::max(voteButtonFont.pointSize(), 12));
    voteButtonFont.setWeight(QFont::DemiBold);
    voteButtonFont.setStyleStrategy(QFont::PreferAntialias);
    ui->btnVote->setFont(voteButtonFont);

    connect(ui->btnVote, &QPushButton::clicked, [this](){ Q_EMIT voteClicked(proposalInfo); });
    connect(ui->btnLink, &QPushButton::clicked, this, &ProposalCard::onCopyUrlClicked);
}

void ProposalCard::setProposal(const ProposalInfo& _proposalInfo)
{
    proposalInfo = _proposalInfo;
    const bool canVote = proposalInfo.status != ProposalInfo::FINISHED;
    ui->btnVote->setVisible(canVote);
    ui->btnVote->setEnabled(canVote);
    ui->btnVote->setToolTip(canVote ? QString() : tr("Voting is closed for finished proposals"));

    ui->labelPropName->setText(QString::fromStdString(proposalInfo.name));
    ui->labelPropName->setToolTip(QString::fromStdString(proposalInfo.name));
    ui->labelPropAmount->setText(GUIUtil::formatBalance(proposalInfo.amount));
    if (proposalInfo.remainingPayments < 0) {
        ui->labelPropMonths->setText(tr("Inactive proposal"));
    } else if (proposalInfo.remainingPayments == 0) {
        ui->labelPropMonths->setText(tr("Final cycle in progress"));
    } else {
        const QString duration = FormatDurationLabel(static_cast<int64_t>(proposalInfo.remainingPayments) * BudgetCycleSeconds());
        ui->labelPropMonths->setText(tr("%1 of %2 cycles left (~%3)")
                                             .arg(proposalInfo.remainingPayments)
                                             .arg(proposalInfo.totalPayments)
                                             .arg(duration));
    }
    const double totalVotes = _proposalInfo.votesYes + _proposalInfo.votesNo;
    const double totalCoinVotes = _proposalInfo.coinVotesYes + _proposalInfo.coinVotesNo;
    const bool hasMnVotes = totalVotes > 0;
    const bool hasCoinVotes = totalCoinVotes > 0;
    const double mnNoPct = hasMnVotes ? (_proposalInfo.votesNo / totalVotes) * 100 : 0;
    const double mnYesPct = hasMnVotes ? (_proposalInfo.votesYes / totalVotes) * 100 : 0;
    const double coinNoPct = hasCoinVotes ? (_proposalInfo.coinVotesNo / totalCoinVotes) * 100 : 0;
    const double coinYesPct = hasCoinVotes ? (_proposalInfo.coinVotesYes / totalCoinVotes) * 100 : 0;
    const double percentageNo = hasMnVotes && hasCoinVotes ? (mnNoPct + coinNoPct) / 2.0
                             : (hasMnVotes ? mnNoPct : coinNoPct);
    const double percentageYes = hasMnVotes && hasCoinVotes ? (mnYesPct + coinYesPct) / 2.0
                              : (hasMnVotes ? mnYesPct : coinYesPct);
    const int noBarValue = std::clamp(static_cast<int>(std::lround(percentageNo)), 0, 100);
    const int yesBarValue = std::clamp(static_cast<int>(std::lround(percentageYes)), 0, 100);
    if (hasCoinVotes) {
        ui->labelNo->setText(tr("No %1 (coin %2)")
                                     .arg(_proposalInfo.votesNo)
                                     .arg(_proposalInfo.coinVotesNo));
        ui->labelYes->setText(tr("Yes %1 (coin %2)")
                                      .arg(_proposalInfo.votesYes)
                                      .arg(_proposalInfo.coinVotesYes));
    } else {
        ui->labelNo->setText(tr("No %1 (%2%)")
                                     .arg(_proposalInfo.votesNo)
                                     .arg(QString::number(percentageNo, 'f', 1)));
        ui->labelYes->setText(tr("Yes %1 (%2%)")
                                      .arg(_proposalInfo.votesYes)
                                      .arg(QString::number(percentageYes, 'f', 1)));
    }

    QString cssClassStatus;
    if (proposalInfo.status == ProposalInfo::WAITING_FOR_APPROVAL) {
        cssClassStatus = "card-status-no-votes";
        setStatusAndVotes(tr("Waiting"), 50, 50);
    } else if (proposalInfo.status == ProposalInfo::FINISHED) {
        cssClassStatus = "card-status-no-votes";
        setStatusAndVotes(QString::fromStdString(proposalInfo.statusToStr()), 50, 50);
    } else if (proposalInfo.status == ProposalInfo::PASSING) {
        cssClassStatus = "card-status-passing";
        setStatusAndVotes(tr("Passing"), noBarValue, yesBarValue);
    } else if (proposalInfo.status == ProposalInfo::PASSING_NOT_FUNDED) {
        cssClassStatus = "card-status-not-passing";
        setStatusAndVotes(tr("Over Budget"), noBarValue, yesBarValue);
    } else if (proposalInfo.status == ProposalInfo::NOT_PASSING && totalVotes == 0 && totalCoinVotes == 0) {
        cssClassStatus = "card-status-no-votes";
        setStatusAndVotes(tr("No Votes"), 50, 50);
    } else if (proposalInfo.status == ProposalInfo::NOT_PASSING) {
        cssClassStatus = "card-status-not-passing";
        setStatusAndVotes(tr("Not Passing"), noBarValue, yesBarValue);
    }
    setCssProperty(ui->labelStatus, cssClassStatus, true);
}

void ProposalCard::setStatusAndVotes(const QString& msg, int noValue, int yesValue)
{
    ui->labelStatus->setText(msg);
    ui->votesNoBar->setValue(std::clamp(noValue, 0, 100));
    ui->votesYesBar->setValue(std::clamp(yesValue, 0, 100));
}

void ProposalCard::onCopyUrlClicked()
{
    Q_EMIT onMenuClicked(this);
}

ProposalCard::~ProposalCard()
{
    delete ui;
}
