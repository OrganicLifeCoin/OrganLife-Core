// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "openuridialog.h"
#include "ui_openuridialog.h"

#include "guiutil.h"
#include "qtutils.h"
#include "walletmodel.h"

#include <QUrl>
#include <QFile>
#include <QPushButton>

OpenURIDialog::OpenURIDialog(QWidget* parent) : ContainerDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint),
                                                ui(new Ui::OpenURIDialog)
{
    ui->setupUi(this);
    applyParentOrAppStyleSheet(parent);
    ui->uriEdit->setPlaceholderText("cteam:");

    ui->labelSubtitle->setText("URI");
    setCssProperty(ui->labelSubtitle, "text-title2-dialog");
    setCssProperty(ui->labelTitle, "text-title-dialog");

    auto* btnEsc = new QPushButton(this);
    btnEsc->setObjectName("btnEsc");
    btnEsc->setFocusPolicy(Qt::NoFocus);
    btnEsc->setMinimumSize(24, 24);
    btnEsc->setMaximumSize(24, 24);
    initDialogCloseButton(btnEsc);
    initDraggableHeaderChrome(ui->frame, ui->labelTitle, btnEsc, 16);

    setCssBtnPrimary(ui->pushButtonOK);
    setCssProperty(ui->pushButtonCancel, "btn-dialog-cancel");

    initCssEditLine(ui->uriEdit, true);
    connect(ui->pushButtonOK, &QPushButton::clicked, this, &OpenURIDialog::accept);
    connect(ui->pushButtonCancel, &QPushButton::clicked, this, &OpenURIDialog::close);
    connect(btnEsc, &QPushButton::clicked, this, &OpenURIDialog::close);
}

void OpenURIDialog::showEvent(QShowEvent *event)
{
    ContainerDialog::showEvent(event);
    ui->uriEdit->setFocus();
}

OpenURIDialog::~OpenURIDialog()
{
    delete ui;
}

QString OpenURIDialog::getURI()
{
    return ui->uriEdit->text();
}

void OpenURIDialog::accept()
{
    SendCoinsRecipient rcp;
    if (GUIUtil::parseBitcoinURI(getURI(), &rcp)) {
        /* Only accept value URIs */
        QDialog::accept();
    } else {
        setCssEditLineDialog(ui->uriEdit, false, true);
    }
}

void OpenURIDialog::inform(const QString& str) {
    if (!snackBar) snackBar = new SnackBar(nullptr, this);
    snackBar->setText(str);
    snackBar->resize(this->width(), snackBar->height());
    openDialog(snackBar, this);
}
