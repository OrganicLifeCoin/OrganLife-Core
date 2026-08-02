// Copyright (c) 2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_FOCUSEDDIALOG_H
#define PIVX_QT_FOCUSEDDIALOG_H

#include "containerdialog.h"

class FocusedDialog : public ContainerDialog
{
    Q_OBJECT

public:
    explicit FocusedDialog(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
    ~FocusedDialog();

    // Sets focus on show
    void showEvent(QShowEvent *event) override;

protected:
    // Detects a key press and calls accept() on ENTER and reject() on ESC
    void keyPressEvent(QKeyEvent *e) override;
};

#endif // PIVX_QT_FOCUSEDDIALOG_H
