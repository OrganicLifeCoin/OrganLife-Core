// Copyright (c) 2019-2020 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/settings/backupwallet.h"
#include "qt/settings/forms/ui_backupwallet.h"

#include "guiinterface.h"
#include "guiutil.h"
#include "qtutils.h"
#include "pqwalletui.h"
#include <chainparams.h>

#include <QGraphicsDropShadowEffect>
#include <QCheckBox>
#include <QFileDialog>
#include <QSettings>
#include <QSignalBlocker>

SettingsBackupWallet::SettingsBackupWallet(OrganicLifeGUI* _window, QWidget *parent) :
    PWidget(_window, parent),
    ui(new Ui::SettingsBackupWallet)
{
    ui->setupUi(this);
    automaticBackup = new QCheckBox(tr("Also back up newly created PQ keys to a chosen folder"), this);
    automaticBackup->setObjectName("automaticPQBackup");
    automaticBackup->setEnabled(false);
    automaticBackup->setToolTip(tr("Optional. Without a current backup, losing this wallet file can lose your coins. Unencrypted wallets also produce unencrypted backups."));
    ui->verticalLayout_9->addWidget(automaticBackup);
    connect(automaticBackup, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!walletModel) return;
        QPointer<WalletModel> selectedWallet(walletModel);
        const QString key = PQWalletUI::backupSettingsKey(selectedWallet);
        const QString directory = enabled ? QFileDialog::getExistingDirectory(this, tr("Choose wallet backup folder")) : QString();
        if (!selectedWallet || selectedWallet != walletModel) return;
        if (enabled && directory.isEmpty()) {
            QSignalBlocker blocker(automaticBackup);
            automaticBackup->setChecked(!PQWalletUI::backupDirectory(selectedWallet).isEmpty());
            return;
        }
        if (enabled) QSettings().setValue(key, directory);
        else QSettings().remove(key);
    });

    this->setStyleSheet(parent->styleSheet());

    /* Containers */
    ui->left->setProperty("cssClass", "container");
    ui->left->setContentsMargins(10,10,10,10);

    // Title
    ui->labelTitle->setProperty("cssClass", "text-title-screen");
    ui->labelTitle_2->setProperty("cssClass", "text-title-screen");
    ui->labelDivider->setProperty("cssClass", "container-divider");

    // Subtitles
    setCssProperty({ui->labelSubtitle1, ui->labelSubtitle_2}, "text-subtitle");

    // Location
    ui->labelSubtitleLocation->setProperty("cssClass", "text-title");
    ui->pushButtonDocuments->setProperty("cssClass", "btn-edit-primary-folder");
    setShadow(ui->pushButtonDocuments);

    // Buttons
    setCssBtnPrimary(ui->pushButtonSave_2);

    connect(ui->pushButtonDocuments, &QPushButton::clicked, this, &SettingsBackupWallet::selectFileOutput);
    connect(ui->pushButtonSave_2, &QPushButton::clicked, this, &SettingsBackupWallet::changePassphrase);
}

void SettingsBackupWallet::loadWalletModel()
{
    QSignalBlocker blocker(automaticBackup);
    automaticBackup->setVisible(Params().SupportsPQ());
    automaticBackup->setEnabled(bool(walletModel));
    automaticBackup->setChecked(walletModel && !PQWalletUI::backupDirectory(walletModel).isEmpty());
}

void SettingsBackupWallet::clearWalletModel()
{
    PWidget::clearWalletModel();
    loadWalletModel();
}

void SettingsBackupWallet::selectFileOutput()
{
    QString filename = GUIUtil::getSaveFileName(this,
                                        tr("Backup Wallet"), QString(),
                                        tr("Wallet Data (*.dat)"), nullptr);

    if (!filename.isEmpty() && walletModel) {
        ui->pushButtonDocuments->setText(filename);
        inform(walletModel->backupWallet(filename) ? tr("Backup created") : tr("Backup creation failed"));
    } else {
        ui->pushButtonDocuments->setText(tr("Select folder..."));
        inform(tr("Please select a folder to export the backup first."));
    }
}

void SettingsBackupWallet::changePassphrase()
{
    showHideOp(true);
    AskPassphraseDialog *dlg = nullptr;
    if (walletModel->getEncryptionStatus() == WalletModel::Unencrypted) {
        dlg = new AskPassphraseDialog(AskPassphraseDialog::Mode::Encrypt, window,
                walletModel, AskPassphraseDialog::Context::Encrypt);
    } else {
        dlg = new AskPassphraseDialog(AskPassphraseDialog::Mode::ChangePass, window,
                walletModel, AskPassphraseDialog::Context::ChangePass);
    }
    dlg->adjustSize();
    Q_EMIT execDialog(dlg);
    dlg->deleteLater();
}

SettingsBackupWallet::~SettingsBackupWallet()
{
    delete ui;
}
