// Copyright (c) 2019-2021 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txrow.h"
#include "ui_txrow.h"

#include "guiutil.h"
#include "qtutils.h"

TxRow::TxRow(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::TxRow)
{
    ui->setupUi(this);
    ui->lblAmountBottom->setVisible(false);
}

void TxRow::init(bool isLightTheme)
{
    ui->rowContainer->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->rowContainer, "dashboard-tx-row");
    setConfirmStatus(true);
    updateStatus(isLightTheme, false, false);
}

void TxRow::showHideSecondAmount(bool show) {
    if (show != isDoubleAmount) {
        isDoubleAmount = show;
        ui->lblAmountBottom->setVisible(show);
    }
}

void TxRow::setConfirmStatus(bool isConfirm){
    if(isConfirm){
        setCssProperty(ui->lblAddress, "dashboard-tx-address");
        setCssProperty(ui->lblDate, "dashboard-tx-date");
    } else {
        setCssProperty(ui->lblAddress, "dashboard-tx-address-muted");
        setCssProperty(ui->lblDate,"dashboard-tx-date-muted");
    }
}

void TxRow::updateStatus(bool isLightTheme, bool isHover, bool isSelected)
{
    if (isSelected) {
        setCssProperty(ui->rowContainer, "dashboard-tx-row-selected", true);
    } else if (isHover) {
        setCssProperty(ui->rowContainer, "dashboard-tx-row-hover", true);
    } else {
        setCssProperty(ui->rowContainer, "dashboard-tx-row", true);
    }

    const QString dividerColor = isLightTheme
            ? (isSelected ? QStringLiteral("#BFDBFE") : QStringLiteral("#D9E7FF"))
            : (isSelected ? QStringLiteral("rgba(96, 165, 250, 0.32)") : QStringLiteral("rgba(96, 165, 250, 0.16)"));
    ui->lblDivisory->setStyleSheet(QStringLiteral("background-color:%1;").arg(dividerColor));
}

void TxRow::setDate(QDateTime date)
{
    ui->lblDate->setText(GUIUtil::dateTimeStr(date));
}

void TxRow::setLabel(QString str)
{
    ui->lblAddress->setText(str);
}

void TxRow::setAmount(QString top, QString bottom)
{
    ui->lblAmountTop->setText(top);
    ui->lblAmountBottom->setText(bottom);
}

void TxRow::setType(bool isLightTheme, int type, bool isConfirmed)
{
    QString path;
    QString css;
    QString cssAmountBottom;
    bool sameIcon = false;
    switch (type) {
        case TransactionRecord::ZerocoinMint:
            path = "://ic-transaction-mint";
            css = "dashboard-tx-amount-negative";
            break;
        case TransactionRecord::Generated:
        case TransactionRecord::StakeZPIV:
        case TransactionRecord::MNReward:
        case TransactionRecord::StakeMint:
        case TransactionRecord::BudgetPayment:
            path = "://ic-transaction-staked";
            css = "dashboard-tx-amount-positive";
            break;
        case TransactionRecord::RecvWithAddress:
        case TransactionRecord::RecvFromOther:
        case TransactionRecord::RecvFromZerocoinSpend:
        case TransactionRecord::RecvWithShieldedAddress:
            path = "://ic-transaction-received";
            css = "dashboard-tx-amount-positive";
            break;
        case TransactionRecord::RecvWithShieldedAddressMemo:
            path = "://ic-transaction-received-memo";
            css = "dashboard-tx-amount-positive";
            break;
        case TransactionRecord::SendToAddress:
        case TransactionRecord::SendToOther:
        case TransactionRecord::ZerocoinSpend:
        case TransactionRecord::ZerocoinSpend_Change_zPiv:
        case TransactionRecord::ZerocoinSpend_FromMe:
        case TransactionRecord::SendToShielded:
        case TransactionRecord::SendToNobody:
            path = "://ic-transaction-sent";
            css = "dashboard-tx-amount-negative";
            break;
        case TransactionRecord::SendToSelf:
        case TransactionRecord::SendToSelfShieldToShieldChangeAddress:
            path = "://ic-transaction-mint";
            css = "dashboard-tx-amount-negative";
            break;
        case TransactionRecord::StakeDelegated:
            path = "://ic-transaction-stake-delegated";
            css = "dashboard-tx-amount-positive";
            break;
        case TransactionRecord::StakeHot:
            path = "://ic-transaction-stake-hot";
            css = "dashboard-tx-amount-muted";
            break;
        case TransactionRecord::P2CSDelegationSent:
        case TransactionRecord::P2CSDelegationSentOwner:
            path = "://ic-transaction-cs-contract";
            css = "dashboard-tx-amount-negative";
            break;
        case TransactionRecord::P2CSDelegation:
            path = "://ic-transaction-cs-contract";
            css = "dashboard-tx-amount-muted";
            break;
        case TransactionRecord::P2CSUnlockOwner:
        case TransactionRecord::P2CSUnlockStaker:
            path = "://ic-transaction-cs-contract";
            css = "dashboard-tx-amount-negative";
            break;
        case TransactionRecord::SendToSelfShieldedAddress:
        case TransactionRecord::SendToSelfShieldToTransparent:
            path = "://ic-transaction-mint";
            css = "dashboard-tx-amount-muted";
            cssAmountBottom = "dashboard-tx-amount-secondary";
            break;
        default:
            path = "://ic-pending";
            sameIcon = true;
            css = "dashboard-tx-amount-muted";
            break;
    }

    if (!isLightTheme && !sameIcon){
        path += "-dark";
    }

    if (!isConfirmed){
        css = "dashboard-tx-amount-muted";
        cssAmountBottom = "dashboard-tx-amount-muted";
        path += "-inactive";
        setConfirmStatus(false);
    } else {
        setConfirmStatus(true);
    }
    setCssProperty(ui->lblAmountTop, css, true);
    if (isDoubleAmount) setCssProperty(ui->lblAmountBottom, cssAmountBottom, true);
    ui->icon->setIcon(QIcon(path));
}

TxRow::~TxRow()
{
    delete ui;
}
