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
    if (!Params().IsTestChain() || registration.IsNull() || !service.IsValid() || !service.GetPort()) return {};
    const auto id = QString::fromStdString(registration.GetHex());
    const auto data = "/var/lib/organiclifecoin/pq-" + id;
    auto config = QString("%1=1\ndatadir=%2\ndisablewallet=1\nstaking=0\nserver=1\nlisten=1\n"
                   "pqoperatorid=%3\npqoperatorcredentials=%2/operator\n\n[%4]\n"
                   "externalip=%5\nport=%6\nrpcbind=127.0.0.1\nrpcallowip=127.0.0.1\nrpcport=%7\n")
        .arg(Params().NetworkIDString() == "test" ? "testnet" : "regtest", data, id,
             QString::fromStdString(Params().NetworkIDString()), QString::fromStdString(service.ToString()))
        .arg(service.GetPort()).arg(service.GetPort() == 65535 ? 65534 : service.GetPort() + 1);
    if (!credential.isEmpty()) {
        // Use this node instance's existing datadir; copying configuration must
        // not redirect it to a nonexistent folder or abandon signing history.
        config.remove("datadir=" + data + "\n");
        config.replace("pqoperatorcredentials=" + data + "/operator", "pqoperatorconfig=" + credential);
        config.prepend("# SECRET operator configuration: cannot spend controller funds.\n"
                       "# Paste at the TOP of this node's config, replacing any old operator settings.\n"
                       "# Keep this file readable only by the node account (Linux: chmod 600).\n"
                       "# Use one node instance/data directory per masternode. Preserve its signing history.\n");
    }
    return config;
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
    if (!model || !Params().IsTestChain()) return false;
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
    if (!model || !Params().IsTestChain()) return false;
    if (model->getEncryptionStatus() == WalletModel::Unencrypted || directory.isEmpty()) return false;
    const QString filename = QDir(directory).filePath("olc-pq-" +
        QString::fromStdString(Params().NetworkIDString()) + "-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + ".dat");
    return model->backupWallet(filename, true);
}
