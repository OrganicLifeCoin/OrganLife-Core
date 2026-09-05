// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include "pqwidget.h"
#include "bitcoinunits.h"
#include "qtutils.h"
#include <chainparams.h>
#include <net.h>
#include <pqtransaction.h>
#include <validation.h>
#include <wallet/walletutil.h>
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString displayAmount(CAmount value)
{
    return BitcoinUnits::format(BitcoinUnits::PIV, value, false, BitcoinUnits::separatorStandard, false) +
           " " + BitcoinUnits::name(BitcoinUnits::PIV);
}
}

PQWidget::PQWidget(OrganicLifeGUI* window, QWidget* parent) : PWidget(window, parent)
{
    setObjectName("pqWalletPage");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    auto* title = new QLabel(tr("Post-quantum test wallet"), this);
    setCssProperty(title, "text-title-screen");
    layout->addWidget(title);
    status = new QLabel(this);
    status->setWordWrap(true);
    layout->addWidget(status);
    balance = new QLabel(this);
    balance->setObjectName("pqBalance");
    layout->addWidget(balance);

    auto* backupRow = new QHBoxLayout;
    auto* backup = new QPushButton(tr("Choose backup folder / Back up now"), this);
    backup->setObjectName("pqBackup");
    backupStatus = new QLabel(this);
    backupStatus->setWordWrap(true);
    backupRow->addWidget(backup);
    backupRow->addWidget(backupStatus, 1);
    layout->addLayout(backupRow);
    auto* note = new QLabel(tr("Each new receive or change key needs a wallet-file backup. "
        "This page saves a new encrypted backup before showing an address or sending. "
        "Keep copies on another device; your ordinary recovery seed does not restore these keys."), this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* encrypt = new QPushButton(tr("Encrypt wallet"), this);
    layout->addWidget(encrypt, 0, Qt::AlignLeft);
    connect(encrypt, &QPushButton::clicked, this, [this]() {
        if (!walletModel) return;
        if (walletModel->getEncryptionStatus() != WalletModel::Unencrypted) {
            QMessageBox::information(this, tr("Wallet encryption"), tr("This wallet is already encrypted."));
            return;
        }
        Q_EMIT encryptWalletRequested();
        refresh();
    });
    connect(backup, &QPushButton::clicked, this, [this]() { if (chooseBackupDirectory()) backupSnapshot(); });

    auto* receiveRow = new QHBoxLayout;
    createAddress = new QPushButton(tr("New receive address"), this);
    createAddress->setObjectName("pqNewAddress");
    receiveAddress = new QLineEdit(this);
    receiveAddress->setObjectName("pqReceiveAddress");
    receiveAddress->setReadOnly(true);
    auto* copy = new QPushButton(tr("Copy"), this);
    receiveRow->addWidget(createAddress);
    receiveRow->addWidget(receiveAddress, 1);
    receiveRow->addWidget(copy);
    layout->addLayout(receiveRow);
    connect(createAddress, &QPushButton::clicked, this, &PQWidget::receive);
    connect(copy, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(receiveAddress->text()); });

    auto* form = new QFormLayout;
    recipient = new QLineEdit(this);
    recipient->setObjectName("pqRecipient");
    amount = new QLineEdit(this);
    amount->setObjectName("pqAmount");
    amount->setPlaceholderText(tr("0.00000000"));
    form->addRow(tr("Recipient"), recipient);
    form->addRow(tr("Amount (OLC)"), amount);
    layout->addLayout(form);
    send = new QPushButton(tr("Review payment"), this);
    send->setObjectName("pqSend");
    setCssBtnPrimary(send);
    layout->addWidget(send, 0, Qt::AlignLeft);
    connect(send, &QPushButton::clicked, this, &PQWidget::pay);
    history = new QTableWidget(0, 5, this);
    history->setObjectName("pqHistory");
    history->setHorizontalHeaderLabels({tr("Date"), tr("Operation"), tr("PQ balance change"), tr("Status"), tr("Transaction ID")});
    history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    history->setSelectionBehavior(QAbstractItemView::SelectRows);
    history->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    history->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(history, 1);
    connect(history, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (auto* item = history->item(row, 4)) QApplication::clipboard()->setText(item->text());
    });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this]() { if (isVisible()) refresh(); });
    timer->start(2000);
    refresh();
}

QString PQWidget::backupSettingsKey() const
{
    const auto identity = QByteArray::fromStdString(GetWalletDir().string() + "/" + walletModel->getWallet()->GetName());
    return "pqBackupDirectory/" + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

void PQWidget::loadWalletModel()
{
    backupDirectory = QSettings().value(backupSettingsKey()).toString();
    backupStatus->setText(backupDirectory.isEmpty() ? tr("Choose a backup folder before receiving or sending.") : backupDirectory);
    refresh();
}

void PQWidget::clearWalletModel()
{
    PWidget::clearWalletModel();
    backupDirectory.clear();
    receiveAddress->clear();
    recipient->clear();
    amount->clear();
    history->setRowCount(0);
    balance->clear();
    backupStatus->clear();
    createAddress->setEnabled(false);
    send->setEnabled(false);
}

bool PQWidget::chooseBackupDirectory()
{
    if (!walletModel || !Params().IsTestChain()) return false;
    const auto selectedWallet = walletModel;
    const QString path = QFileDialog::getExistingDirectory(this, tr("Choose encrypted wallet backup folder"), backupDirectory);
    if (path.isEmpty() || !selectedWallet || selectedWallet != walletModel) return false;
    backupDirectory = path;
    QSettings().setValue(backupSettingsKey(), path);
    return true;
}

bool PQWidget::backupSnapshot()
{
    if (!walletModel || !Params().IsTestChain()) return false;
    if (walletModel->getEncryptionStatus() == WalletModel::Unencrypted) {
        QMessageBox::warning(this, tr("Encrypt wallet first"), tr("Encrypt this wallet before creating PQ backups or keys."));
        return false;
    }
    if (backupDirectory.isEmpty() && !chooseBackupDirectory()) return false;
    const QString filename = QDir(backupDirectory).filePath("olc-pq-" +
        QString::fromStdString(Params().NetworkIDString()) + "-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces) + ".dat");
    if (!walletModel->backupWallet(filename)) {
        QMessageBox::critical(this, tr("Backup failed"), tr("The wallet backup could not be saved. No address will be shown and no payment will be sent. Choose a writable backup folder and retry."));
        return false;
    }
    backupStatus->setText(tr("Latest backup: %1").arg(filename));
    return true;
}

void PQWidget::receive()
{
    if (!walletModel || !Params().IsTestChain()) return;
    const auto selectedWallet = walletModel;
    if (backupDirectory.isEmpty() && !chooseBackupDirectory()) return;
    auto unlock = walletModel->requestUnlock();
    if (!unlock.isValid() || !selectedWallet || selectedWallet != walletModel) return;
    std::string address;
    if (!walletModel->getWallet()->GeneratePQAddress(address)) {
        QMessageBox::warning(this, tr("Cannot create address"), tr("Encrypt and fully unlock the wallet first."));
        return;
    }
    if (backupSnapshot()) {
        receiveAddress->setText(QString::fromStdString(address));
    } else {
        walletModel->getWallet()->ErasePQAddress(address);
    }
    refresh();
}

void PQWidget::pay()
{
    if (!walletModel || !Params().IsTestChain()) return;
    const auto selectedWallet = walletModel;
    CAmount value = 0;
    if (!BitcoinUnits::parse(BitcoinUnits::PIV, amount->text(), &value) || value <= 0) {
        QMessageBox::warning(this, tr("Invalid amount"), tr("Enter a positive OLC amount with at most eight decimal places."));
        return;
    }
    if (backupDirectory.isEmpty() && !chooseBackupDirectory()) return;
    auto unlock = walletModel->requestUnlock();
    if (!unlock.isValid() || !selectedWallet || selectedWallet != walletModel) return;
    auto* wallet = walletModel->getWallet();
    CTransactionRef transaction;
    CAmount fee = 0;
    std::string reason;
    const auto addressesBefore = wallet->GetPQAddresses();
    const auto cleanupNewKeys = [&] {
        for (const auto& address : wallet->GetPQAddresses())
            if (std::find(addressesBefore.begin(), addressesBefore.end(), address) == addressesBefore.end())
                wallet->ErasePQAddress(address);
    };
    const QString destination = recipient->text().trimmed();
    if (!wallet->CreatePQTransaction(destination.toStdString(), value, transaction, fee, reason)) {
        QMessageBox::warning(this, tr("Payment could not be prepared"), QString::fromStdString(reason));
        return;
    }
    QMessageBox confirmation(QMessageBox::Question, tr("Confirm test payment"),
        tr("Recipient: %1\nAmount: %2\nFee: %3\nTotal debit: %4")
            .arg(destination, displayAmount(value), displayAmount(fee), displayAmount(value + fee)),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    confirmation.setTextFormat(Qt::PlainText);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    if (confirmation.exec() != QMessageBox::Yes || !selectedWallet || selectedWallet != walletModel) {
        if (selectedWallet && selectedWallet == walletModel) cleanupNewKeys();
        return;
    }
    if (!backupSnapshot()) {
        cleanupNewKeys();
        return;
    }
    const auto result = wallet->CommitTransaction(transaction, nullptr, g_connman.get());
    if (result.status != CWallet::CommitStatus::OK) {
        QMessageBox::warning(this, tr("Payment not sent"), QString::fromStdString(result.ToString()));
        return;
    }
    amount->clear();
    QMessageBox::information(this, tr("Payment sent"), QString::fromStdString(result.hashTx.ToString()));
    refresh();
}

void PQWidget::refresh()
{
    const bool available = walletModel && Params().IsTestChain();
    createAddress->setEnabled(available);
    send->setEnabled(false);
    if (!available) { status->setText(tr("Select a testnet wallet to use PQ payments.")); return; }
    auto* wallet = walletModel->getWallet();
    TRY_LOCK(cs_main, chainLock);
    if (!chainLock) return;
    TRY_LOCK(wallet->cs_wallet, walletLock);
    if (!walletLock) return;
    const bool active = CWallet::PQPaymentsActive();
    send->setEnabled(active);
    status->setText(active ? tr("PQ payments active.") : tr("Waiting for this network's PQ activation height."));
    CAmount confirmed = 0, pending = 0;
    for (const auto& coin : wallet->GetPQUnspent()) confirmed += coin.Value();
    std::vector<const CWalletTx*> rows;
    for (const auto& entry : wallet->mapWallet) {
        const auto& tx = entry.second;
        if (!wallet->InvolvesPQ(*tx.tx)) continue;
        rows.push_back(&tx);
        if (tx.isUnconfirmed() && tx.InMempool()) {
            for (size_t i = 0; i < tx.tx->vout.size(); ++i)
                if (wallet->IsPQMine(tx.tx->vout[i]) && !wallet->IsSpent(COutPoint(entry.first, i))) pending += tx.tx->vout[i].nValue;
        }
    }
    balance->setText(tr("Confirmed PQ: %1     Pending PQ: %2").arg(displayAmount(confirmed), displayAmount(pending)));
    std::sort(rows.begin(), rows.end(), [](const CWalletTx* a, const CWalletTx* b) { return a->GetTxTime() > b->GetTxTime(); });
    if (rows.size() > 50) rows.resize(50);
    history->setRowCount(rows.size());
    for (size_t row = 0; row < rows.size(); ++row) {
        const auto& tx = *rows[row];
        CAmount change = 0;
        for (const auto& out : tx.tx->vout) if (wallet->IsPQMine(out)) change += out.nValue;
        for (const auto& in : tx.tx->vin) {
            const auto previous = wallet->mapWallet.find(in.prevout.hash);
            if (previous != wallet->mapWallet.end() && in.prevout.n < previous->second.tx->vout.size()) {
                const auto& out = previous->second.tx->vout[in.prevout.n];
                if (wallet->IsPQMine(out)) change -= out.nValue;
            }
        }
        pq::Payload payload;
        const bool decoded = pq::DecodePayload(*tx.tx, payload);
        const auto operation = decoded ? tr("PQ payment") : tr("PQ reward");
        const auto state = tx.isAbandoned() ? tr("Abandoned") : tx.isConflicted() ? tr("Conflicted") :
            tx.GetDepthInMainChain() > 0 ? tr("%1 confirmations").arg(tx.GetDepthInMainChain()) : tr("Pending");
        const QStringList columns{QDateTime::fromSecsSinceEpoch(tx.GetTxTime()).toString(Qt::ISODate), operation,
            displayAmount(change), state, QString::fromStdString(tx.GetHash().ToString())};
        for (int column = 0; column < columns.size(); ++column) history->setItem(row, column, new QTableWidgetItem(columns[column]));
    }
}
