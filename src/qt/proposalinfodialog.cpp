// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proposalinfodialog.h"
#include "ui_proposalinfodialog.h"

#include "guiutil.h"
#include "qtutils.h"
#include "snackbar.h"

#include <QDateTime>

ProposalInfoDialog::ProposalInfoDialog(QWidget *parent) :
    FocusedDialog(parent),
    ui(new Ui::ProposalInfoDialog)
{
    ui->setupUi(this);
    applyParentOrAppStyleSheet(parent);
    setCssProperty(ui->labelTitle, "text-title-dialog");
    setCssProperty({ui->labelAmount, ui->labelName, ui->labelUrl, ui->labelRecipient, ui->labelPosVotes, ui->labelPosCoinVotes, ui->labelId, ui->labelNegVotes, ui->labelNegCoinVotes, ui->labelEndDate, ui->labelDate, ui->labelStatus}, "text-subtitle");
    setCssProperty({ui->labelDividerID, ui->labelDividerName, ui->labelDividerRecipient, ui->labelDividerChange, ui->labelDividerMemo}, "container-divider");
    setCssProperty({ui->textAmount, ui->textName, ui->textUrl, ui->textRecipient, ui->textPosVotes, ui->textPosCoinVotes, ui->textId, ui->textNegVotes, ui->textNegCoinVotes, ui->textEndDate, ui->textDate, ui->textStatus} , "text-body3-dialog");
    setCssProperty({ui->pushCopy, ui->btnUrlCopy, ui->btnNameCopy, ui->btnRecipientCopy}, "ic-copy-big");
    initDialogCloseButton(ui->btnEsc);
    initDraggableHeaderChrome(ui->frame, ui->labelTitle, ui->btnEsc, 16);
    connect(ui->btnEsc, &QPushButton::clicked, this, &ProposalInfoDialog::close);
    connect(ui->pushCopy, &QPushButton::clicked, [this](){
        GUIUtil::setClipboard(QString::fromStdString(info.id.GetHex()));
        inform(tr("ID copied to clipboard"));
    });
    connect(ui->btnUrlCopy, &QPushButton::clicked, [this](){
        GUIUtil::setClipboard(QString::fromStdString(info.url));
        inform(tr("URL copied to clipboard"));
    });
    connect(ui->btnNameCopy, &QPushButton::clicked, [this](){
        GUIUtil::setClipboard(QString::fromStdString(info.name));
        inform(tr("Proposal name copied to clipboard"));
    });
    connect(ui->btnRecipientCopy, &QPushButton::clicked, [this]() {
        GUIUtil::setClipboard(QString::fromStdString(info.recipientAdd));
        inform(tr("Recipient copied to clipboard"));
    });
}

void ProposalInfoDialog::setProposal(const ProposalInfo& _info)
{
    info = _info;
    QString id{QString::fromStdString(info.id.GetHex())};
    ui->textId->setText(id.left(20)+"..."+id.right(20));
    ui->textName->setText(QString::fromStdString(info.name));
    ui->textUrl->setText(QString::fromStdString(info.url));
    ui->textRecipient->setText(QString::fromStdString(info.recipientAdd));
    ui->textNegVotes->setText(QString::number(info.votesNo));
    ui->textPosVotes->setText(QString::number(info.votesYes));
    ui->textNegCoinVotes->setText(QString::number(info.coinVotesNo));
    ui->textPosCoinVotes->setText(QString::number(info.coinVotesYes));
    ui->textAmount->setText(GUIUtil::formatBalance(info.amount));
    ui->textDate->setText(QString::number(info.startBlock));
    ui->textEndDate->setText(QString::number(info.endBlock));
    ui->textStatus->setText(info.statusToStr().c_str());
}

void ProposalInfoDialog::accept()
{
    if (snackBar && snackBar->isVisible()) snackBar->hide();
    QDialog::accept();
}

void ProposalInfoDialog::reject()
{
    if (snackBar && snackBar->isVisible()) snackBar->hide();
    QDialog::reject();
}

void ProposalInfoDialog::inform(const QString& msg)
{
    if (!snackBar) snackBar = new SnackBar(nullptr, this);
    snackBar->setText(msg);
    snackBar->resize(this->width(), snackBar->height());
    openDialog(snackBar, this);
}

ProposalInfoDialog::~ProposalInfoDialog()
{
    delete snackBar;
    delete ui;
}
