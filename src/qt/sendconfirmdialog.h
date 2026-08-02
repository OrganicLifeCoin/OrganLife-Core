// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_SENDCONFIRMDIALOG_H
#define PIVX_QT_SENDCONFIRMDIALOG_H

#include "focuseddialog.h"
#include "snackbar.h"
#include "transactionrecord.h"
#include "walletmodeltransaction.h"

class WalletModelTransaction;
class WalletModel;

namespace Ui {
class TxDetailDialog;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

class TxDetailDialog : public FocusedDialog
{
    Q_OBJECT

public:
    enum class ConflictAction {
        None,
        Abandon,
        HideFromHistory,
    };

    explicit TxDetailDialog(QWidget *parent = nullptr, bool isConfirmDialog = true, const QString& warningStr = QString());
    ~TxDetailDialog();
    static bool shouldShowConflictActions(TransactionStatus::Status status, bool hasWalletConflicts);
    static ConflictAction resolveConflictAction(TransactionStatus::Status status, bool hasWalletConflicts, bool canAbandon);

    bool isConfirm() { return this->confirm;}
    WalletModel::SendCoinsReturn getStatus() { return this->sendStatus;}
    void setSendOnAccept(bool enabled) { this->sendOnAccept = enabled; }

    void setData(WalletModel *model, WalletModelTransaction* tx);
    void setData(WalletModel *model, const QModelIndex &index);
    void setDisplayUnit(int unit){this->nDisplayUnit = unit;};

public Q_SLOTS:
    void accept() override;
    void reject() override;
    void onInputsClicked();
    void onOutputsClicked();
    void onAbandonTransactionClicked();

private:
    Ui::TxDetailDialog *ui;
    SnackBar *snackBar = nullptr;
    int nDisplayUnit = 0;
    bool isConfirmDialog = false;
    bool confirm = false;
    bool sendOnAccept = true;
    WalletModel *model = nullptr;
    WalletModel::SendCoinsReturn sendStatus;
    WalletModelTransaction* tx{nullptr};
    uint256 txHash;
    ConflictAction conflictAction{ConflictAction::None};
    // Shielded tx with not inputs data
    bool isShieldedToShieldedRecv{false};

    bool inputsLoaded = false;
    bool outputsLoaded = false;

    void setInputsType(CTransactionRef _tx);
};

#endif // PIVX_QT_SENDCONFIRMDIALOG_H
