// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2017-2020 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_UTILITYDIALOG_H
#define PIVX_QT_UTILITYDIALOG_H

#include "containerdialog.h"
#include <QObject>
#include <QMainWindow>
#include <QPointer>

class ClientModel;

namespace Ui
{
class HelpMessageDialog;
}

/** "Help message" dialog box */
class HelpMessageDialog : public ContainerDialog
{
    Q_OBJECT

public:
    explicit HelpMessageDialog(QWidget* parent, bool about);
    ~HelpMessageDialog();

    void printToConsole();
    void showOrPrint();

private:
    Ui::HelpMessageDialog* ui;
    QString text;
    QString consoleText;
};


/** "Shutdown" window */
class ShutdownWindow : public ContainerDialog
{
    Q_OBJECT

public:
    explicit ShutdownWindow(QWidget* parent = nullptr, Qt::WindowFlags f = Qt::Dialog);
    static void showShutdownWindow(QMainWindow* window);
    static void raiseShutdownWindow();
    static void closeShutdownWindow();

protected:
    void closeEvent(QCloseEvent* event);
};

#endif // PIVX_QT_UTILITYDIALOG_H
