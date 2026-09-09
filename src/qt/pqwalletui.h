// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_QT_PQWALLETUI_H
#define ORGANICLIFE_QT_PQWALLETUI_H

#include <QString>

class WalletModel;
class QWidget;
class CService;
class uint256;

namespace PQWalletUI {
QString backupSettingsKey(WalletModel* model);
QString backupDirectory(WalletModel* model);
bool ensureBackupDirectory(QWidget* parent, WalletModel* model, QString& directory);
bool backupSnapshot(WalletModel* model, QString& directory);
QString masternodeConfig(const uint256& registration, const CService& service, const QString& credential);
}

#endif
