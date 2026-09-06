// Copyright (c) 2019-2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "lockunlock.h"
#include "ui_lockunlock.h"
#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <algorithm>

LockUnlock::LockUnlock(QWidget *parent) :
    QWidget(parent, Qt::Popup | Qt::FramelessWindowHint),
    ui(new Ui::LockUnlock)
{
    ui->setupUi(this);

    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(280);
    ui->container->setProperty("cssClass", "wallet-lock-menu");
    ui->verticalLayout->setContentsMargins(12, 12, 12, 12);
    ui->verticalLayout->setSpacing(6);
    auto* title = new QLabel(tr("Wallet access"), this);
    title->setProperty("cssClass", "wallet-lock-title");
    ui->verticalLayout->insertWidget(0, title);
    for (auto* button : {ui->pushButtonUnlocked, ui->pushButtonLocked, ui->pushButtonStaking}) {
        button->setProperty("cssClass", "wallet-lock-action");
        button->setFixedHeight(44);
        button->setCursor(Qt::PointingHandCursor);
    }

    // Connect
    connect(ui->pushButtonUnlocked, &QPushButton::clicked, this, &LockUnlock::onUnlockClicked);
    connect(ui->pushButtonLocked, &QPushButton::clicked, this, &LockUnlock::onLockClicked);
    connect(ui->pushButtonStaking, &QPushButton::clicked, this, &LockUnlock::onStakingClicked);
}

LockUnlock::~LockUnlock()
{
    delete ui;
}

void LockUnlock::updateStatus(WalletModel::EncryptionStatus status)
{
    switch (status) {
        case WalletModel::EncryptionStatus::Unlocked:
            ui->pushButtonUnlocked->setChecked(true);
            ui->pushButtonLocked->setChecked(false);
            ui->pushButtonStaking->setChecked(false);
            break;
        case WalletModel::EncryptionStatus::UnlockedForStaking:
            ui->pushButtonUnlocked->setChecked(false);
            ui->pushButtonLocked->setChecked(false);
            ui->pushButtonStaking->setChecked(true);
            break;
        case WalletModel::EncryptionStatus::Locked:
            ui->pushButtonUnlocked->setChecked(false);
            ui->pushButtonLocked->setChecked(true);
            ui->pushButtonStaking->setChecked(false);
            break;
        default:
            break;
    }
}

void LockUnlock::onLockClicked()
{
    lock = 0;
    Q_EMIT lockClicked(StateClicked::LOCK);
}

void LockUnlock::onUnlockClicked()
{
    lock = 1;
    Q_EMIT lockClicked(StateClicked::UNLOCK);
}

void LockUnlock::onStakingClicked()
{
    lock = 2;
    Q_EMIT lockClicked(StateClicked::UNLOCK_FOR_STAKING);
}

void LockUnlock::showBeside(QWidget* anchor)
{
    if (!anchor) return;
    setStyleSheet(parentWidget()->styleSheet());
    adjustSize();
    const QPoint bottomRight = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height()));
    QPoint position(bottomRight.x() + 8, bottomRight.y() - height());
    if (auto* screen = QGuiApplication::screenAt(anchor->mapToGlobal(anchor->rect().center()))) {
        const QRect area = screen->availableGeometry();
        position.setX(std::clamp(position.x(), area.left(), std::max(area.left(), area.right() - width() + 1)));
        position.setY(std::clamp(position.y(), area.top(), std::max(area.top(), area.bottom() - height() + 1)));
    }
    move(position);
    show();
    ui->pushButtonUnlocked->setFocus(Qt::PopupFocusReason);
}
