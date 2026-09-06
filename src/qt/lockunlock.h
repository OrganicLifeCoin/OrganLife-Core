// Copyright (c) 2019 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_LOCKUNLOCK_H
#define PIVX_QT_LOCKUNLOCK_H

#include <QWidget>
#include "walletmodel.h"

namespace Ui {
class LockUnlock;
}

enum StateClicked{
    LOCK,UNLOCK,UNLOCK_FOR_STAKING
};


class LockUnlock : public QWidget
{
    Q_OBJECT

public:
    explicit LockUnlock(QWidget *parent = nullptr);
    ~LockUnlock();
    void updateStatus(WalletModel::EncryptionStatus status);
    void showBeside(QWidget* anchor);
    int lock = 0;
Q_SIGNALS:
    void lockClicked(const StateClicked& state);

public Q_SLOTS:
    void onLockClicked();
    void onUnlockClicked();
    void onStakingClicked();

private:
    Ui::LockUnlock *ui;
};

#endif // PIVX_QT_LOCKUNLOCK_H
