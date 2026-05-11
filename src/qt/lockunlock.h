// Copyright (c) 2019 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_LOCKUNLOCK_H
#define PIVX_QT_LOCKUNLOCK_H

#include <QWidget>
#include <QEnterEvent>
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
    int lock = 0;
    bool isHovered();
Q_SIGNALS:
    void Mouse_Entered();
    void Mouse_Leave();

    void lockClicked(const StateClicked& state);
protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent* event) override;
#else
    void enterEvent(QEvent* event) override;
#endif
    void leaveEvent(QEvent* event) override;

public Q_SLOTS:
    void onLockClicked();
    void onUnlockClicked();
    void onStakingClicked();

private:
    Ui::LockUnlock *ui;
    bool isOnHover = false;
};

#endif // PIVX_QT_LOCKUNLOCK_H
