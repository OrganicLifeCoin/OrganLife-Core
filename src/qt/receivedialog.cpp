// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "receivedialog.h"
#include "ui_receivedialog.h"

#include "qt/walletmodel.h"
#include "qtutils.h"

#include <QPixmap>

ReceiveDialog::ReceiveDialog(QWidget *parent) :
    FocusedDialog(parent),
    ui(new Ui::ReceiveDialog)
{
    ui->setupUi(this);

    // Stylesheet
    applyParentOrAppStyleSheet(parent);

    // Title
    ui->labelTitle->setProperty("cssClass", "text-title-dialog");

    // Address with modern styling
    ui->labelAddress->setProperty("cssClass", "label-address-box");
    ui->labelAddress->setAlignment(Qt::AlignCenter);

    // QR image
    QPixmap pixmap(":/img-qr-test-big");
    if (!pixmap.isNull()) {
        ui->labelQrImg->setPixmap(pixmap.scaled(
                                      ui->labelQrImg->width(),
                                      ui->labelQrImg->height(),
                                      Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation)
                                  );
    }

    // Modern close button (top right)
    initDialogCloseButton(ui->btnEsc);
    ui->btnEsc->setFixedSize(32, 32);
    initDraggableHeaderChrome(ui->frameContainer, ui->labelTitle, ui->btnEsc, 16);
    ui->frameContainer->setContentsMargins(20, 20, 20, 20);

    // Hide cancel button, we use the X button instead
    ui->btnCancel->setVisible(false);

    // Modern primary button
    ui->btnSave->setProperty("cssClass", "btn-primary");
    ui->btnSave->setCursor(Qt::PointingHandCursor);
    ui->btnSave->setMinimumHeight(50);

    connect(ui->btnEsc, &QPushButton::clicked, this, &ReceiveDialog::close);
    connect(ui->btnSave, &QPushButton::clicked, this, &ReceiveDialog::onCopy);

    // Set window title
    setWindowTitle(tr("Receive"));
}

void ReceiveDialog::updateQr(const QString& address)
{
    if (!info) info = new SendCoinsRecipient();
    info->address = address;
    QString uri = GUIUtil::formatBitcoinURI(*info);
    ui->labelQrImg->setText("");
    ui->labelAddress->setText(address);
    QString error;

    // Use QR code with transparent background - theme-aware color
    QColor qrColor = isLightTheme() ? QColor("#111827") : QColor("#FFFFFF");
    QColor bgColor(Qt::transparent);
    QPixmap pixmap = encodeToQrModern(uri, error, qrColor, bgColor, 0, 4, 6);

    if (!pixmap.isNull()) {
        ui->labelQrImg->setPixmap(pixmap.scaled(ui->labelQrImg->width(), ui->labelQrImg->height(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        ui->labelQrImg->setText(!error.isEmpty() ? error : "Error encoding address");
    }
}

void ReceiveDialog::onCopy()
{
    GUIUtil::setClipboard(info->address);
    accept();
}

ReceiveDialog::~ReceiveDialog()
{
    delete ui;
}
