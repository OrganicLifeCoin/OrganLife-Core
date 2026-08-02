// Copyright (c) 2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "focuseddialog.h"

#include <QKeyEvent>

FocusedDialog::FocusedDialog(QWidget *parent, Qt::WindowFlags flags) :
    ContainerDialog(parent, flags)
{}

void FocusedDialog::showEvent(QShowEvent *event)
{
    ContainerDialog::showEvent(event);
    setFocus();
}

void FocusedDialog::keyPressEvent(QKeyEvent *e)
{
    if (e->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(e);
        // Detect Enter key press
        if (ke->key() == Qt::Key_Enter || ke->key() == Qt::Key_Return) accept();
        // Detect Esc key press
        if (ke->key() == Qt::Key_Escape) reject();
    }
}

FocusedDialog::~FocusedDialog()
{}
