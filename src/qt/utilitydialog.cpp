// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2021 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/pivx-config.h"
#endif

#include "utilitydialog.h"

#include "ui_helpmessagedialog.h"

#include "clientmodel.h"
#include "clientversion.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "init.h"
#include "intro.h"
#include "qtutils.h"
#include "util/system.h"

#include <stdio.h>

#include <QCloseEvent>
#include <QLabel>
#include <QPixmap>
#include <QRegularExpression>
#include <QTextTable>
#include <QTextCursor>
#include <QVBoxLayout>

namespace {
QPointer<ShutdownWindow> g_shutdown_window;
} // namespace

/** "Help message" or "About" dialog box */
HelpMessageDialog::HelpMessageDialog(QWidget* parent, bool about) : ContainerDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint),
                                                                    ui(new Ui::HelpMessageDialog)
{
    ui->setupUi(this);
    applyParentOrAppStyleSheet(parent);
    GUIUtil::restoreWindowGeometry("nHelpMessageDialogWindow", this->size(), this);

    // Scale logo to fit while maintaining aspect ratio (center fit, not stretch)
    QPixmap logoPixmap(":/logo1776");
    if (!logoPixmap.isNull()) {
        ui->graphic->setPixmap(logoPixmap.scaled(QSize(400, 200), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    const QString aboutVersion = QString{PACKAGE_NAME} + " " + tr("version") + " " + QString::fromStdString(FormatVersionFriendly());
    const QString cliVersion = QString{PACKAGE_NAME} + " " + tr("version") + " " + QString::fromStdString(FormatFullVersion());

    setCssBtnPrimary(ui->pushButtonOk);
    connect(ui->pushButtonOk, &QPushButton::clicked, this, &HelpMessageDialog::close);
    if (about) {
        setWindowTitle(tr("About %1").arg(PACKAGE_NAME));

        /// HTML-format the license message from the core
        QString licenseInfo = QString::fromStdString(LicenseInfo());
        QString licenseInfoHTML = licenseInfo;

        // Make URLs clickable
        static const QRegularExpression uri(QStringLiteral("<(.*?)>"));
        licenseInfoHTML.replace(uri, "<a style='color: #60A5FA;text-decoration:none'  href=\"\\1\">\\1</a>");
        // Replace newlines with HTML breaks
        licenseInfoHTML.replace("\n\n", "<br><br>");

        ui->aboutMessage->setTextFormat(Qt::RichText);
        ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        text = aboutVersion + "\n" + licenseInfo;
        consoleText = cliVersion + "\n" + licenseInfo;
        ui->aboutMessage->setText(aboutVersion + "<br><br>" + licenseInfoHTML);
        ui->aboutMessage->setWordWrap(true);
        ui->helpMessage->setVisible(false);
    } else {
        setWindowTitle(tr("Command-line options"));
        QString header = "Usage:  cteam-qt [command-line options]                     \n";
        QTextCursor cursor(ui->helpMessage->document());
        cursor.insertText(cliVersion);
        cursor.insertBlock();
        cursor.insertText(header);
        cursor.insertBlock();

        std::string strUsage = HelpMessage(HMM_BITCOIN_QT);
        strUsage += HelpMessageGroup("UI Options:");
        strUsage += HelpMessageOpt("-choosedatadir", strprintf("Choose data directory on startup (default: %u)", DEFAULT_CHOOSE_DATADIR));
        strUsage += HelpMessageOpt("-lang=<lang>", "Set language, for example \"de_DE\" (default: system locale)");
        strUsage += HelpMessageOpt("-min", "Start minimized");
        strUsage += HelpMessageOpt("-splash", strprintf("Show splash screen on startup (default: %u)", DEFAULT_SPLASHSCREEN));
        strUsage += HelpMessageOpt("-hidecharts", strprintf("Hide QT staking charts on startup (default: %u)", false));
        QString coreOptions = QString::fromStdString(strUsage);
        text = cliVersion + "\n\n" + header + "\n" + coreOptions;

        QTextTableFormat tf;
        tf.setBorderStyle(QTextFrameFormat::BorderStyle_None);
        tf.setCellPadding(2);
        QVector<QTextLength> widths;
        widths << QTextLength(QTextLength::PercentageLength, 35);
        widths << QTextLength(QTextLength::PercentageLength, 65);
        tf.setColumnWidthConstraints(widths);

        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);

        for (const QString &line : coreOptions.split("\n")) {
            if (line.startsWith("  -"))
            {
                cursor.currentTable()->appendRows(1);
                cursor.movePosition(QTextCursor::PreviousCell);
                cursor.movePosition(QTextCursor::NextRow);
                cursor.insertText(line.trimmed());
                cursor.movePosition(QTextCursor::NextCell);
            } else if (line.startsWith("   ")) {
                cursor.insertText(line.trimmed()+' ');
            } else if (line.size() > 0) {
                //Title of a group
                if (cursor.currentTable())
                    cursor.currentTable()->appendRows(1);
                cursor.movePosition(QTextCursor::Down);
                cursor.insertText(line.trimmed(), bold);
                cursor.insertTable(1, 2, tf);
            }
        }

        ui->helpMessage->moveCursor(QTextCursor::Start);
        ui->scrollArea->setVisible(false);
    }
}

HelpMessageDialog::~HelpMessageDialog()
{
    GUIUtil::saveWindowGeometry("nHelpMessageDialogWindow", this);
    delete ui;
}

void HelpMessageDialog::printToConsole()
{
    // On other operating systems, the expected action is to print the message to the console.
    fprintf(stdout, "%s\n", qPrintable(consoleText.isEmpty() ? text : consoleText));
}

void HelpMessageDialog::showOrPrint()
{
#if defined(WIN32)
    // On Windows, show a message box, as there is no stderr/stdout in windowed applications
    exec();
#else
    // On other operating systems, print help text to console
    printToConsole();
#endif
}


/** "Shutdown" window */
ShutdownWindow::ShutdownWindow(QWidget* parent, Qt::WindowFlags f) : ContainerDialog(parent, f)
{
    QVBoxLayout* layout = new QVBoxLayout();
    layout->addWidget(new QLabel(
        tr("%1 is shutting down...").arg(PACKAGE_NAME) + "<br /><br />" +
        tr("Do not shut down the computer until this window disappears.")));
    setLayout(layout);
}

void ShutdownWindow::raiseShutdownWindow()
{
    if (g_shutdown_window.isNull()) return;
    g_shutdown_window->show();
    g_shutdown_window->raise();
    g_shutdown_window->activateWindow();
}

void ShutdownWindow::closeShutdownWindow()
{
    if (g_shutdown_window.isNull()) return;
    // Use hide() instead of close() because closeEvent ignores the event
    g_shutdown_window->hide();
    g_shutdown_window->deleteLater();
    g_shutdown_window.clear();
}

void ShutdownWindow::showShutdownWindow(QMainWindow* window)
{
    if (!g_shutdown_window.isNull()) {
        raiseShutdownWindow();
        return;
    }
    if (!window) return;

    // Show a simple window indicating shutdown status
    ShutdownWindow* shutdownWindow = new ShutdownWindow();
    g_shutdown_window = shutdownWindow;
    // We don't hold a direct pointer to the shutdown window after creation, so use
    // Qt::WA_DeleteOnClose to make sure that the window will be deleted eventually.
    shutdownWindow->setAttribute(Qt::WA_DeleteOnClose);
    shutdownWindow->setWindowTitle(window->windowTitle());

    // Center shutdown window at where main window was
    shutdownWindow->adjustSize();
    const QPoint global = window->mapToGlobal(window->rect().center());
    shutdownWindow->move(global.x() - shutdownWindow->width() / 2, global.y() - shutdownWindow->height() / 2);
    shutdownWindow->show();
    shutdownWindow->raise();
    shutdownWindow->activateWindow();
}

void ShutdownWindow::closeEvent(QCloseEvent* event)
{
    event->ignore();
}
