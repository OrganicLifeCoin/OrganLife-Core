// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include "pqwalletui.h"

#include "walletmodel.h"

#include <chainparams.h>
#include <netaddress.h>
#include <wallet/walletutil.h>

#include <QCryptographicHash>
#include <QDir>
#include <QSettings>
#include <QUuid>

QString PQWalletUI::masternodeConfig(const uint256& registration, const CService& service, const QString& credential)
{
    if (!Params().SupportsPQ() || registration.IsNull() || !service.IsValid() || !service.GetPort() || credential.isEmpty()) return {};
    return QString("pqoperatorid=%1\npqoperatorconfig=%2\nexternalip=%3\n")
        .arg(QString::fromStdString(registration.GetHex()), credential,
             QString::fromStdString(service.ToString()));
}

QString PQWalletUI::backupSettingsKey(WalletModel* model)
{
    if (!model) return {};
    const auto identity = QByteArray::fromStdString(GetWalletDir().string() + "/" + model->getWallet()->GetName());
    return "pqBackupDirectory/" +
        QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QString PQWalletUI::backupDirectory(WalletModel* model)
{
    return QSettings().value(backupSettingsKey(model)).toString();
}

bool PQWalletUI::ensureBackupDirectory(QWidget* parent, WalletModel* model, QString& directory)
{
    Q_UNUSED(parent);
    if (!model || !Params().SupportsPQ()) return false;
    // Re-read the selected wallet's optional preference, not another wallet's cached path.
    directory = backupDirectory(model);
    return true;
}

bool PQWalletUI::backupSnapshot(WalletModel* model, QString& directory)
{
    if (!model || !Params().SupportsPQ()) return false;
    if (directory.isEmpty()) return true;
    const QString filename = QDir(directory).filePath("olc-pq-" +
        QString::fromStdString(Params().NetworkIDString()) + "-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + ".dat");
    return model->backupWallet(filename, true);
}
