// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include "pqwalletui.h"

#include "walletmodel.h"

#include <chainparams.h>
#include <netaddress.h>
#include <wallet/walletutil.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QPointer>
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
    if (!model || !Params().SupportsPQ()) return false;
    if (directory.isEmpty()) directory = backupDirectory(model);
    if (!directory.isEmpty()) return true;
    const QString settingsKey = backupSettingsKey(model);
    QPointer<WalletModel> guardedModel(model);
    directory = QFileDialog::getExistingDirectory(parent,
        QObject::tr("Choose encrypted wallet backup folder"));
    if (directory.isEmpty() || guardedModel.isNull()) return false;
    QSettings().setValue(settingsKey, directory);
    return true;
}

bool PQWalletUI::backupSnapshot(WalletModel* model, QString& directory)
{
    if (!model || !Params().SupportsPQ()) return false;
    if (model->getEncryptionStatus() == WalletModel::Unencrypted || directory.isEmpty()) return false;
    const QString filename = QDir(directory).filePath("olc-pq-" +
        QString::fromStdString(Params().NetworkIDString()) + "-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + ".dat");
    return model->backupWallet(filename, true);
}
