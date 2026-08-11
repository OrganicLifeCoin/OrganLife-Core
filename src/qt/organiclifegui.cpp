// Copyright (c) 2019-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "organiclifegui.h"

#ifdef Q_OS_MAC
#include "macdockiconhandler.h"
#endif

#include "clientmodel.h"
#include "defaultdialog.h"
#include "guiinterface.h"
#include "interfaces/handler.h"
#include "mnmodel.h"
#include "networkstyle.h"
#include "notificator.h"
#include "optionsmodel.h"
#include "qt/guiutil.h"
#include "qtutils.h"
#include "shutdown.h"
#include "utilitydialog.h"
#include "util/system.h"

#include <QApplication>
#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeySequence>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QWindowStateChangeEvent>


#define BASE_WINDOW_WIDTH 1280
#define BASE_WINDOW_HEIGHT 800
#define BASE_WINDOW_MIN_HEIGHT 780
#define BASE_WINDOW_MIN_WIDTH 1180

class ContentCornerArcWidget : public QWidget
{
public:
    explicit ContentCornerArcWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFixedSize(24, 24);
    }

    void setFillColor(const QColor& color)
    {
        if (fillColor == color) return;
        fillColor = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fillColor);

        const qreal w = width();
        const qreal h = height();

        QPainterPath path;
        path.moveTo(0.0, h);
        path.quadTo(0.0, 0.0, w, 0.0);
        path.lineTo(0.0, 0.0);
        path.closeSubpath();

        painter.drawPath(path);
    }

private:
    QColor fillColor = Qt::transparent;
};


const QString OrganicLifeGUI::DEFAULT_WALLET = "~Default";

OrganicLifeGUI::OrganicLifeGUI(const NetworkStyle* networkStyle, QWidget* parent) :
        QMainWindow(parent),
        clientModel(0){

    /* Open CSS when configured */
    this->setStyleSheet(GUIUtil::loadStyleSheet());
    this->setMinimumSize(BASE_WINDOW_MIN_WIDTH, BASE_WINDOW_MIN_HEIGHT);


    // Adapt screen size
    QRect rec = QGuiApplication::primaryScreen()->geometry();
    int adaptedHeight = (rec.height() < BASE_WINDOW_HEIGHT) ?  BASE_WINDOW_MIN_HEIGHT : BASE_WINDOW_HEIGHT;
    int adaptedWidth = (rec.width() < BASE_WINDOW_WIDTH) ?  BASE_WINDOW_MIN_WIDTH : BASE_WINDOW_WIDTH;
    GUIUtil::restoreWindowGeometry(
            "nWindow",
            QSize(adaptedWidth, adaptedHeight),
            this
    );

#ifdef ENABLE_WALLET
    /* if compiled with wallet support, -disablewallet can still disable the wallet */
    enableWallet = !gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET);
#else
    enableWallet = false;
#endif // ENABLE_WALLET

    QString windowTitle = QString::fromStdString(gArgs.GetArg("-windowtitle", ""));
    if (windowTitle.isEmpty()) {
        // On Windows, some builds have ended up with an unexpected spacing in PACKAGE_NAME.
        // Prefer the configured organization name (used for QSettings paths) for the app title.
        QString brand = QApplication::organizationName();
        if (brand.isEmpty()) brand = QString{PACKAGE_NAME};
        windowTitle = brand + " - ";
        windowTitle += ((enableWallet) ? tr("Wallet") : tr("Node"));
    }
    windowTitle += " " + networkStyle->getTitleAddText();
    setWindowTitle(windowTitle);

    QApplication::setWindowIcon(networkStyle->getAppIcon());
    setWindowIcon(networkStyle->getAppIcon());

#ifdef ENABLE_WALLET
    // Create wallet frame
    if (enableWallet) {
        QFrame* centralWidget = new QFrame(this);
        this->setMinimumWidth(BASE_WINDOW_MIN_WIDTH);
        this->setMinimumHeight(BASE_WINDOW_MIN_HEIGHT);
        QHBoxLayout* centralWidgetLayouot = new QHBoxLayout();
        centralWidget->setLayout(centralWidgetLayouot);
        centralWidgetLayouot->setContentsMargins(0,0,0,0);
        centralWidgetLayouot->setSpacing(0);

        centralWidget->setProperty("cssClass", "container");
        centralWidget->setStyleSheet("padding:0px; border:none; margin:0px;");

        // Left shell: the brand island is deliberately a sibling of the
        // navigation widget. This gives the supplied logo its own footprint,
        // prevents clipping, and guarantees that navigation begins below it.
        auto* leftShell = new QFrame(centralWidget);
        leftShell->setObjectName("walletLeftShell");
        leftShell->setAttribute(Qt::WA_StyledBackground, true);
        leftShell->setFixedWidth(190);
        setCssProperty(leftShell, "wallet-left-shell");
        auto* leftShellLayout = new QVBoxLayout(leftShell);
        leftShellLayout->setContentsMargins(14, 16, 14, 14);
        leftShellLayout->setSpacing(14);

        auto* brandIsland = new QFrame(leftShell);
        brandIsland->setObjectName("brandIsland");
        brandIsland->setAttribute(Qt::WA_StyledBackground, true);
        brandIsland->setFixedSize(144, 136);
        setCssProperty(brandIsland, "brand-island");
        auto* brandLayout = new QVBoxLayout(brandIsland);
        brandLayout->setContentsMargins(6, 6, 6, 6);
        auto* brandLogo = new QLabel(brandIsland);
        brandLogo->setObjectName("brandIslandLogo");
        brandLogo->setProperty("sourceAsset", QStringLiteral(":/img-logo-pivx"));
        brandLogo->setAlignment(Qt::AlignCenter);
        brandLogo->setScaledContents(false);
        const QPixmap suppliedLogo(":/img-logo-pivx");
        brandLogo->setPixmap(suppliedLogo.scaled(122, 122, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        brandLayout->addWidget(brandLogo, 0, Qt::AlignCenter);
        leftShellLayout->addWidget(brandIsland, 0, Qt::AlignHCenter);

        navMenu = new NavMenuWidget(this, leftShell);
        navMenu->setObjectName("NavMenuWidget");
        leftShellLayout->addWidget(navMenu, 1);
        centralWidgetLayouot->addWidget(leftShell);

        this->setCentralWidget(centralWidget);
        this->setContentsMargins(0,0,0,0);

        QFrame *container = new QFrame(centralWidget);
        container->setContentsMargins(0,0,0,0);
        centralWidgetLayouot->addWidget(container);

        // Then topbar + the stackedWidget
        QVBoxLayout *baseScreensContainer = new QVBoxLayout();
        baseScreensContainer->setContentsMargins(0, 0, 0, 0);
        baseScreensContainer->setSpacing(0);
        container->setLayout(baseScreensContainer);

        // Keep the legacy TopBar alive as a controller for existing wallet
        // dialogs and model wiring, but remove it from the visible shell.
        topBar = new TopBar(this);
        topBar->setContentsMargins(0,0,0,0);
        topBar->showDashboard();
        baseScreensContainer->addWidget(topBar);

        // Now stacked widget
        stackedContainer = new QStackedWidget(this);
        QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        stackedContainer->setSizePolicy(sizePolicy);
        stackedContainer->setContentsMargins(0,0,0,0);
        baseScreensContainer->addWidget(stackedContainer);

        contentCornerArc = new ContentCornerArcWidget(container);
        contentCornerArc->show();
        contentCornerArc->raise();
        updateContentCornerArcStyle();

        // Init
        dashboard = new DashboardWidget(this);
        sendWidget = new SendWidget(this);
        receiveWidget = new ReceiveWidget(this);
        addressesWidget = new AddressesWidget(this);
        masterNodesWidget = new MasterNodesWidget(this);
        coldStakingWidget = new ColdStakingWidget(this);
        governancewidget = new GovernanceWidget(this);
        settingsWidget = new SettingsWidget(this);

        // Add to parent
        stackedContainer->addWidget(dashboard);
        stackedContainer->addWidget(sendWidget);
        stackedContainer->addWidget(receiveWidget);
        stackedContainer->addWidget(addressesWidget);
        stackedContainer->addWidget(masterNodesWidget);
        stackedContainer->addWidget(coldStakingWidget);
        stackedContainer->addWidget(governancewidget);
        stackedContainer->addWidget(settingsWidget);
        stackedContainer->setCurrentWidget(dashboard);
        QTimer::singleShot(0, this, [this]() { updateContentCornerArc(); });

    } else
#endif
    {
        // When compiled without wallet or -disablewallet is provided,
        // the central widget is the rpc console.
        rpcConsole = new RPCConsole(enableWallet ? this : 0);
        setCentralWidget(rpcConsole);
    }

    // Create actions for the toolbar, menu bar and tray/dock icon
    createActions(networkStyle);

    // Create system tray icon and notification
    createTrayIcon(networkStyle);

    // Connect events
    connectActions();

    // TODO: Add event filter??
    // // Install event filter to be able to catch status tip events (QEvent::StatusTip)
    //    this->installEventFilter(this);

    // Subscribe to notifications from core
    subscribeToCoreSignals();

}

void OrganicLifeGUI::createActions(const NetworkStyle* networkStyle)
{
    toggleHideAction = new QAction(networkStyle->getAppIcon(), tr("&Show / Hide"), this);
    toggleHideAction->setStatusTip(tr("Show or hide the main Window"));

    quitAction = new QAction(QIcon(":/icons/quit"), tr("E&xit"), this);
    quitAction->setStatusTip(tr("Quit application"));
    quitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    quitAction->setMenuRole(QAction::QuitRole);

    connect(toggleHideAction, &QAction::triggered, this, &OrganicLifeGUI::toggleHidden);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
}

/**
 * Here add every event connection
 */
void OrganicLifeGUI::connectActions()
{
    QShortcut *consoleShort = new QShortcut(this);
    consoleShort->setKey(QKeySequence(SHORT_KEY | Qt::Key_C));
    connect(consoleShort, &QShortcut::activated, [this](){
        navMenu->selectSettings();
        settingsWidget->showDebugConsole();
        goToSettings();
    });
    connect(topBar, &TopBar::showHide, this, &OrganicLifeGUI::showHide);
    connect(topBar, &TopBar::themeChanged, this, &OrganicLifeGUI::changeTheme);
    connect(topBar, &TopBar::themeChanged, navMenu, &NavMenuWidget::setThemeState);
    connect(navMenu, &NavMenuWidget::themeToggleRequested, topBar, &TopBar::toggleTheme);
    connect(navMenu, &NavMenuWidget::walletLockRequested, topBar, &TopBar::toggleWalletLock);
    connect(navMenu, &NavMenuWidget::walletSelectorRequested, topBar, &TopBar::showWalletSelector);
    connect(topBar, &TopBar::connectionCountChanged, dashboard, &DashboardWidget::setNumConnections);
    connect(topBar, &TopBar::stakingStatusChanged, dashboard, &DashboardWidget::setStakingStatusActive);
    connect(topBar, &TopBar::blockHeightChanged, dashboard, &DashboardWidget::setBlockHeight);
    connect(topBar, &TopBar::onShowHideColdStakingChanged, navMenu, &NavMenuWidget::onShowHideColdStakingChanged);
    connect(settingsWidget, &SettingsWidget::showHide, this, &OrganicLifeGUI::showHide);
    connect(sendWidget, &SendWidget::showHide, this, &OrganicLifeGUI::showHide);
    connect(receiveWidget, &ReceiveWidget::showHide, this, &OrganicLifeGUI::showHide);
    connect(addressesWidget, &AddressesWidget::showHide, this, &OrganicLifeGUI::showHide);
    connect(masterNodesWidget, &MasterNodesWidget::showHide, this, &OrganicLifeGUI::showHide);
    connect(masterNodesWidget, &MasterNodesWidget::execDialog, this, &OrganicLifeGUI::execDialog);
    connect(coldStakingWidget, &ColdStakingWidget::showHide, this, &OrganicLifeGUI::showHide);
    connect(coldStakingWidget, &ColdStakingWidget::execDialog, this, &OrganicLifeGUI::execDialog);
    connect(governancewidget, &GovernanceWidget::showHide, this, &OrganicLifeGUI::showHide);
    connect(governancewidget, &GovernanceWidget::execDialog, this, &OrganicLifeGUI::execDialog);
    connect(settingsWidget, &SettingsWidget::execDialog, this, &OrganicLifeGUI::execDialog);
}


void OrganicLifeGUI::createTrayIcon(const NetworkStyle* networkStyle)
{
#ifndef Q_OS_MAC
    trayIcon = new QSystemTrayIcon(this);
    QString brand = QApplication::organizationName();
    if (brand.isEmpty()) brand = QString{PACKAGE_NAME};
    QString toolTip = tr("%1 client").arg(brand) + " " + networkStyle->getTitleAddText();
    trayIcon->setToolTip(toolTip);
    trayIcon->setIcon(networkStyle->getAppIcon());
    trayIcon->hide();
#endif
    notificator = new Notificator(QApplication::applicationName(), trayIcon, this);
}

OrganicLifeGUI::~OrganicLifeGUI()
{
    // Unsubscribe from notifications from core
    unsubscribeFromCoreSignals();

    GUIUtil::saveWindowGeometry("nWindow", this);
    if (trayIcon) // Hide tray icon, as deleting will let it linger until quit (on Ubuntu)
        trayIcon->hide();
#ifdef Q_OS_MAC
    MacDockIconHandler::cleanup();
#endif
}


/** Get restart command-line parameters and request restart */
void OrganicLifeGUI::handleRestart(QStringList args)
{
    if (!ShutdownRequested())
        Q_EMIT requestedRestart(args);
}


void OrganicLifeGUI::setClientModel(ClientModel* _clientModel)
{
    m_isShuttingDown = (_clientModel == nullptr);
    this->clientModel = _clientModel;
    if (this->clientModel) {
        // Create system tray menu (or setup the dock menu) that late to prevent users from calling actions,
        // while the client has not yet fully loaded
        createTrayIconMenu();

        topBar->setClientModel(clientModel);
        dashboard->setClientModel(clientModel);
        sendWidget->setClientModel(clientModel);
        masterNodesWidget->setClientModel(clientModel);
        settingsWidget->setClientModel(clientModel);
        governancewidget->setClientModel(clientModel);

        // Receive and report messages from client model
        connect(clientModel, &ClientModel::message, this, &OrganicLifeGUI::message);
        connect(clientModel, &ClientModel::showProgress, topBar, &TopBar::setSyncProgress);
        connect(clientModel, &ClientModel::alertsChanged, [this](const QString& _alertStr) {
            message(tr("Alert!"), _alertStr, CClientUIInterface::MSG_WARNING);
        });
        connect(topBar, &TopBar::walletSynced, dashboard, &DashboardWidget::walletSynced);
        connect(topBar, &TopBar::walletSynced, coldStakingWidget, &ColdStakingWidget::walletSynced);
        connect(topBar, &TopBar::tierTwoSynced, governancewidget, &GovernanceWidget::tierTwoSynced);

        // Get restart command-line parameters and handle restart
        connect(settingsWidget, &SettingsWidget::handleRestart, [this](QStringList arg){handleRestart(arg);});

        if (rpcConsole) {
            rpcConsole->setClientModel(clientModel);
        }

        if (trayIcon) {
            trayIcon->show();
        }
    } else {
        // Disable possibility to show main window via action
        toggleHideAction->setEnabled(false);
        if (trayIconMenu) {
            // Disable context menu on tray icon
            trayIconMenu->clear();
        }
    }
}

void OrganicLifeGUI::createTrayIconMenu()
{
#ifndef Q_OS_MAC
    // return if trayIcon is unset (only on non-macOSes)
    if (!trayIcon)
        return;

    trayIconMenu = new QMenu(this);
    trayIcon->setContextMenu(trayIconMenu);

    connect(trayIcon, &QSystemTrayIcon::activated, this, &OrganicLifeGUI::trayIconActivated);
#else
    // Note: On macOS, the Dock icon is used to provide the tray's functionality.
    MacDockIconHandler* dockIconHandler = MacDockIconHandler::instance();
    connect(dockIconHandler, &MacDockIconHandler::dockIconClicked, this, &OrganicLifeGUI::macosDockIconActivated);

    trayIconMenu = new QMenu(this);
    trayIconMenu->setAsDockMenu();
#endif

    // Configuration of the tray icon (or Dock icon) icon menu
    trayIconMenu->addAction(toggleHideAction);
    trayIconMenu->addSeparator();

#ifndef Q_OS_MAC // This is built-in on macOS
    trayIconMenu->addSeparator();
    trayIconMenu->addAction(quitAction);
#endif
}

#ifndef Q_OS_MAC
void OrganicLifeGUI::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger) {
        // Click on system tray icon triggers show/hide of the main window
        toggleHidden();
    }
}
#else
void OrganicLifeGUI::macosDockIconActivated()
 {
     if (m_isShuttingDown || ShutdownRequested()) {
         ShutdownWindow::raiseShutdownWindow();
         return;
     }
     show();
     activateWindow();
 }
#endif

void OrganicLifeGUI::changeEvent(QEvent* e)
{
    QMainWindow::changeEvent(e);
#ifndef Q_OS_MAC // Ignored on Mac
    if (e->type() == QEvent::WindowStateChange) {
        if (clientModel && clientModel->getOptionsModel() && clientModel->getOptionsModel()->getMinimizeToTray()) {
            QWindowStateChangeEvent* wsevt = static_cast<QWindowStateChangeEvent*>(e);
            if (!(wsevt->oldState() & Qt::WindowMinimized) && isMinimized()) {
                QTimer::singleShot(0, this, &OrganicLifeGUI::hide);
                e->ignore();
            } else if ((wsevt->oldState() & Qt::WindowMinimized) && !isMinimized()) {
                QTimer::singleShot(0, this, &OrganicLifeGUI::show);
                e->ignore();
            }
        }
    }
#endif
}

void OrganicLifeGUI::closeEvent(QCloseEvent* event)
{
#ifndef Q_OS_MAC // Ignored on Mac
    if (clientModel && clientModel->getOptionsModel()) {
        if (!clientModel->getOptionsModel()->getMinimizeOnClose()) {
            QApplication::quit();
        } else {
            QMainWindow::showMinimized();
            event->ignore();
        }
    }
#else
    QMainWindow::closeEvent(event);
#endif
}


void OrganicLifeGUI::messageInfo(const QString& text)
{
    if (!this->snackBar) this->snackBar = new SnackBar(this, this);
    this->snackBar->setText(text);
    this->snackBar->resize(this->width(), snackBar->height());
    openDialog(this->snackBar, this);
}


void OrganicLifeGUI::message(const QString& title, const QString& message, unsigned int style, bool* ret)
{
    QString strTitle = QApplication::organizationName();
    if (strTitle.isEmpty()) strTitle = QString{PACKAGE_NAME}; // default title
    // Default to information icon
    int nNotifyIcon = Notificator::Information;

    QString msgType;

    // Prefer supplied title over style based title
    if (!title.isEmpty()) {
        msgType = title;
    } else {
        switch (style) {
            case CClientUIInterface::MSG_ERROR:
                msgType = tr("Error");
                break;
            case CClientUIInterface::MSG_WARNING:
                msgType = tr("Warning");
                break;
            case CClientUIInterface::MSG_INFORMATION:
                msgType = tr("Information");
                break;
            default:
                msgType = tr("System Message");
                break;
        }
    }

    // Check for error/warning icon
    if (style & CClientUIInterface::ICON_ERROR) {
        nNotifyIcon = Notificator::Critical;
    } else if (style & CClientUIInterface::ICON_WARNING) {
        nNotifyIcon = Notificator::Warning;
    }

    // Display message
    if (style & CClientUIInterface::MODAL) {
        // Check for buttons, use OK as default, if none was supplied
        int r = 0;
        showNormalIfMinimized();
        if (style & CClientUIInterface::BTN_MASK) {
            r = openStandardDialog(
                    (title.isEmpty() ? strTitle : title), message, "OK", "CANCEL"
                );
        } else {
            r = openStandardDialog((title.isEmpty() ? strTitle : title), message, "OK");
        }
        if (ret != nullptr)
            *ret = r;
    } else if (style & CClientUIInterface::MSG_INFORMATION_SNACK) {
        messageInfo(message);
    } else {
        // Append message type to "OrganicLife - "
        if (!msgType.isEmpty())
            strTitle += " - " + msgType;
        notificator->notify(static_cast<Notificator::Class>(nNotifyIcon), strTitle, message);
    }
}

bool OrganicLifeGUI::openStandardDialog(QString title, QString body, QString okBtn, QString cancelBtn)
{
    DefaultDialog *dialog;
    if (isVisible()) {
        showHide(true);
        dialog = new DefaultDialog(this);
        dialog->setText(title, body, okBtn, cancelBtn);
        dialog->adjustSize();
        openDialogWithOpaqueBackground(dialog, this);
    } else {
        dialog = new DefaultDialog();
        dialog->setText(title, body, okBtn);
        dialog->setWindowTitle(PACKAGE_NAME);
        dialog->adjustSize();
        dialog->raise();
        dialog->exec();
    }
    bool ret = dialog->isOk;
    dialog->deleteLater();
    return ret;
}


void OrganicLifeGUI::showNormalIfMinimized(bool fToggleHidden)
{
    if (!clientModel)
        return;
    if (!isHidden() && !isMinimized() && !GUIUtil::isObscured(this) && fToggleHidden) {
        hide();
    } else {
        GUIUtil::bringToFront(this);
    }
}

void OrganicLifeGUI::toggleHidden()
{
    showNormalIfMinimized(true);
}

void OrganicLifeGUI::detectShutdown()
{
    if (ShutdownRequested()) {
        if (rpcConsole)
            rpcConsole->hide();
        // Close shutdown window explicitly before quitting to prevent hang in Qt6
        ShutdownWindow::closeShutdownWindow();
        // Process events to ensure the shutdown window is hidden/deleted
        QCoreApplication::processEvents();
        qApp->quit();
    }
}

void OrganicLifeGUI::goToDashboard()
{
    topBar->showDashboard();
    dashboard->setTransactionsOnly(false);
    if (stackedContainer->currentWidget() != dashboard) {
        stackedContainer->setCurrentWidget(dashboard);
        QTimer::singleShot(0, this, [this]() { updateContentCornerArc(); });
    }
}

void OrganicLifeGUI::goToTransactions()
{
    topBar->showDashboard();
    dashboard->setTransactionsOnly(true);
    if (stackedContainer->currentWidget() != dashboard) {
        stackedContainer->setCurrentWidget(dashboard);
        QTimer::singleShot(0, this, [this]() { updateContentCornerArc(); });
    }
}

void OrganicLifeGUI::goToSend()
{
    showTop(sendWidget);
}

void OrganicLifeGUI::goToAddresses()
{
    showTop(addressesWidget);
}

void OrganicLifeGUI::goToMasterNodes()
{
    masterNodesWidget->resetCoinControl();
    showTop(masterNodesWidget);
}

void OrganicLifeGUI::goToColdStaking()
{
    showTop(coldStakingWidget);
}

void OrganicLifeGUI::goToGovernance()
{
    showTop(governancewidget);
}

void OrganicLifeGUI::goToSettings(){
    showTop(settingsWidget);
}

void OrganicLifeGUI::goToSettingsInfo()
{
    navMenu->selectSettings();
    settingsWidget->showInformation();
    goToSettings();
}

void OrganicLifeGUI::goToReceive()
{
    showTop(receiveWidget);
}

void OrganicLifeGUI::openNetworkMonitor()
{
    settingsWidget->openNetworkMonitor();
}

void OrganicLifeGUI::showTop(QWidget* view)
{
    topBar->showTop();
    if (stackedContainer->currentWidget() != view) {
        stackedContainer->setCurrentWidget(view);
        QTimer::singleShot(0, this, [this]() { updateContentCornerArc(); });
    }
}

void OrganicLifeGUI::changeTheme(bool isLightTheme)
{

    QString css = GUIUtil::loadStyleSheet();
    this->setStyleSheet(css);
    updateContentCornerArcStyle();
    updateContentCornerArc();

    // Notify
    Q_EMIT themeChanged(isLightTheme, css);

    // Update style
    updateStyle(this);
}

void OrganicLifeGUI::resizeEvent(QResizeEvent* event)
{
    // Parent..
    QMainWindow::resizeEvent(event);
    updateContentCornerArc();
    // background
    showHide(opEnabled);
    // Notify
    Q_EMIT windowResizeEvent(event);
}

void OrganicLifeGUI::updateContentCornerArc()
{
    if (!contentCornerArc || !stackedContainer || !contentCornerArc->parentWidget()) return;
    const QPoint topLeft = stackedContainer->mapTo(contentCornerArc->parentWidget(), QPoint(0, 0));
    contentCornerArc->move(topLeft);
    contentCornerArc->raise();
}

void OrganicLifeGUI::updateContentCornerArcStyle()
{
    if (!contentCornerArc) return;
    const QColor arcColor = isLightTheme() ? QColor("#F3F6F1") : QColor("#0D1512");
    contentCornerArc->setFillColor(arcColor);
}

bool OrganicLifeGUI::execDialog(QDialog *dialog, int xDiv, int yDiv)
{
    return openDialogWithOpaqueBackgroundY(dialog, this);
}

void OrganicLifeGUI::showHide(bool show)
{
    if (!op) {
        op = new QLabel(this);
        op->setFocusPolicy(Qt::NoFocus);
        op->setAttribute(Qt::WA_ShowWithoutActivating, true);
    }
    if (!show) {
        op->setVisible(false);
        opEnabled = false;
    } else {
        QColor bg("#19221D");
        bg.setAlpha(200);
        if (!isLightTheme()) {
            bg = QColor("#08100C");
            bg.setAlpha(150);
        }

        QPalette palette;
        palette.setColor(QPalette::Window, bg);
        op->setAutoFillBackground(true);
        op->setPalette(palette);
        op->setWindowFlags(Qt::CustomizeWindowHint);
        op->move(0,0);
        op->show();
        op->resize(width(), height());
        op->setVisible(true);
        opEnabled = true;
    }
}

int OrganicLifeGUI::getNavWidth()
{
    return this->navMenu->width();
}

void OrganicLifeGUI::openFAQ(SettingsFaqWidget::Section section)
{
    showHide(true);
    SettingsFaqWidget* dialog = new SettingsFaqWidget(this, mnModel);
    dialog->setSection(section);
    openDialogWithOpaqueBackgroundFullScreen(dialog, this);
    dialog->deleteLater();
}


#ifdef ENABLE_WALLET
QString OrganicLifeGUI::walletSettingsId(const QString& name)
{
    return name.isEmpty() ? QStringLiteral("__primary__") : name;
}

void OrganicLifeGUI::applyWalletMetadata(const QString& name, WalletModel* walletModel)
{
    if (!walletModel) {
        return;
    }

    walletModel->setProperty("walletDisplayName", walletDisplayName(name));
    walletModel->setProperty("walletPrimary", isPrimaryWallet(name));
}

void OrganicLifeGUI::refreshWalletMetadata()
{
    for (const auto& [name, walletModel] : walletStack) {
        if (!walletModel.isNull()) {
            applyWalletMetadata(name, walletModel);
        }
    }

    if (!currentWallet.isNull() && walletStack.count(currentWallet) != 0) {
        setCurrentWallet(currentWallet);
    }
}

void OrganicLifeGUI::setGovModel(GovernanceModel* govModel)
{
    if (!stackedContainer || !clientModel) return;
    this->govModel = govModel;
    governancewidget->setGovModel(govModel);
}

void OrganicLifeGUI::setMNModel(MNModel* _mnModel)
{
    if (!stackedContainer || !clientModel) return;
    mnModel = _mnModel;
    governancewidget->setMNModel(mnModel);
    masterNodesWidget->setMNModel(mnModel);
}

bool OrganicLifeGUI::addWallet(const QString& name, WalletModel* walletModel)
{
    if (!stackedContainer || !clientModel || !walletModel)
        return false;

    walletStack.emplace(name, walletModel);
    applyWalletMetadata(name, walletModel);

    if (!walletUiSignalsConnected) {
        connect(masterNodesWidget, &MasterNodesWidget::message, this, &OrganicLifeGUI::message);
        connect(coldStakingWidget, &ColdStakingWidget::message, this, &OrganicLifeGUI::message);
        connect(topBar, &TopBar::message, this, &OrganicLifeGUI::message);
        connect(sendWidget, &SendWidget::message,this, &OrganicLifeGUI::message);
        connect(receiveWidget, &ReceiveWidget::message,this, &OrganicLifeGUI::message);
        connect(addressesWidget, &AddressesWidget::message,this, &OrganicLifeGUI::message);
        connect(governancewidget, &GovernanceWidget::message,this, &OrganicLifeGUI::message);
        connect(settingsWidget, &SettingsWidget::message, this, &OrganicLifeGUI::message);
        connect(dashboard, &DashboardWidget::incomingTransaction, this, &OrganicLifeGUI::incomingTransaction);
        walletUiSignalsConnected = true;
    }

    return true;
}

bool OrganicLifeGUI::setCurrentWallet(const QString& name)
{
    if (!stackedContainer || !clientModel)
        return false;

    const auto it = walletStack.find(name);
    if (it == walletStack.end() || it->second.isNull())
        return false;

    WalletModel* walletModel = it->second.data();
    currentWallet = name;
    navMenu->setWalletName(walletDisplayName(name));

    if (mnModel) mnModel->setWalletModel(walletModel);
    if (governancewidget) governancewidget->setWalletModel(walletModel);
    if (govModel) govModel->setWalletModel(walletModel);
    navMenu->setWalletModel(walletModel);
    dashboard->setWalletModel(walletModel);
    topBar->setWalletModel(walletModel);
    receiveWidget->setWalletModel(walletModel);
    sendWidget->setWalletModel(walletModel);
    addressesWidget->setWalletModel(walletModel);
    masterNodesWidget->setWalletModel(walletModel);
    coldStakingWidget->setWalletModel(walletModel);
    settingsWidget->setWalletModel(walletModel);
    if (rpcConsole) rpcConsole->setWalletModel(walletModel);

    if (walletMessageConnection) disconnect(walletMessageConnection);
    if (walletProgressConnection) disconnect(walletProgressConnection);
    walletMessageConnection = connect(walletModel, &WalletModel::message, this, &OrganicLifeGUI::message);
    walletProgressConnection = connect(walletModel, &WalletModel::showProgress, topBar, &TopBar::setSyncProgress);

    walletModel->emitBalanceChanged();
    return true;
}

WalletModel* OrganicLifeGUI::removeWallet(const QString& name)
{
    const auto it = walletStack.find(name);
    if (it == walletStack.end()) {
        return nullptr;
    }

    WalletModel* walletModel = it->second.data();
    const bool wasCurrentWallet = currentWallet == name;
    walletStack.erase(it);

    if (wasCurrentWallet) {
        if (walletMessageConnection) disconnect(walletMessageConnection);
        if (walletProgressConnection) disconnect(walletProgressConnection);
        walletMessageConnection = QMetaObject::Connection();
        walletProgressConnection = QMetaObject::Connection();
        currentWallet.clear();
        if (govModel) govModel->setWalletModel(nullptr);
        if (mnModel) mnModel->setWalletModel(nullptr);
        navMenu->clearWalletModel();
        dashboard->clearWalletModel();
        topBar->clearWalletModel();
        receiveWidget->clearWalletModel();
        sendWidget->clearWalletModel();
        addressesWidget->clearWalletModel();
        masterNodesWidget->clearWalletModel();
        coldStakingWidget->clearWalletModel();
        governancewidget->clearWalletModel();
        settingsWidget->clearWalletModel();
        if (rpcConsole) rpcConsole->setWalletModel(nullptr);
    }

    return walletModel;
}

void OrganicLifeGUI::removeAllWallets()
{
    if (walletMessageConnection) disconnect(walletMessageConnection);
    if (walletProgressConnection) disconnect(walletProgressConnection);
    walletMessageConnection = QMetaObject::Connection();
    walletProgressConnection = QMetaObject::Connection();
    currentWallet.clear();
    walletStack.clear();
    if (govModel) govModel->setWalletModel(nullptr);
    if (mnModel) mnModel->setWalletModel(nullptr);
    navMenu->clearWalletModel();
    dashboard->clearWalletModel();
    topBar->clearWalletModel();
    receiveWidget->clearWalletModel();
    sendWidget->clearWalletModel();
    addressesWidget->clearWalletModel();
    masterNodesWidget->clearWalletModel();
    coldStakingWidget->clearWalletModel();
    governancewidget->clearWalletModel();
    settingsWidget->clearWalletModel();
    if (rpcConsole) rpcConsole->setWalletModel(nullptr);
}

QStringList OrganicLifeGUI::getWalletNames() const
{
    QStringList names;
    for (const auto& [name, walletModel] : walletStack) {
        if (!walletModel.isNull()) {
            names.append(name);
        }
    }
    return names;
}

QString OrganicLifeGUI::currentWalletName() const
{
    return currentWallet;
}

WalletModel* OrganicLifeGUI::getWallet(const QString& name) const
{
    const auto it = walletStack.find(name);
    return (it == walletStack.end() || it->second.isNull()) ? nullptr : it->second.data();
}

WalletModel* OrganicLifeGUI::currentWalletModel() const
{
    return getWallet(currentWallet);
}

QString OrganicLifeGUI::walletDisplayName(const QString& name) const
{
    QSettings settings;
    const QString displayNameKey = QStringLiteral("walletMetadata/displayName/%1").arg(walletSettingsId(name));
    const QString storedDisplayName = settings.value(displayNameKey).toString().trimmed();
    if (!storedDisplayName.isEmpty()) {
        return storedDisplayName;
    }
    return name.isEmpty() ? tr("Primary") : name;
}

void OrganicLifeGUI::setWalletDisplayName(const QString& name, const QString& displayName)
{
    QSettings settings;
    const QString displayNameKey = QStringLiteral("walletMetadata/displayName/%1").arg(walletSettingsId(name));
    const QString trimmedDisplayName = displayName.trimmed();
    if (trimmedDisplayName.isEmpty()) {
        settings.remove(displayNameKey);
    } else {
        settings.setValue(displayNameKey, trimmedDisplayName);
    }
    refreshWalletMetadata();
}

QString OrganicLifeGUI::primaryWalletName() const
{
    QSettings settings;
    if (settings.contains(QStringLiteral("walletMetadata/primaryWallet"))) {
        return settings.value(QStringLiteral("walletMetadata/primaryWallet")).toString();
    }
    return QString();
}

bool OrganicLifeGUI::isPrimaryWallet(const QString& name) const
{
    QSettings settings;
    if (settings.contains(QStringLiteral("walletMetadata/primaryWallet"))) {
        return settings.value(QStringLiteral("walletMetadata/primaryWallet")).toString() == name;
    }
    return name.isEmpty();
}

void OrganicLifeGUI::setPrimaryWalletName(const QString& name)
{
    QSettings settings;
    settings.setValue(QStringLiteral("walletMetadata/primaryWallet"), name);
    refreshWalletMetadata();
}

QStringList OrganicLifeGUI::autoloadWalletNames() const
{
    QSettings settings;
    return settings.value(QStringLiteral("walletMetadata/autoloadWallets")).toStringList();
}

void OrganicLifeGUI::addAutoloadWalletName(const QString& walletName)
{
    const QString trimmedWalletName = walletName.trimmed();
    if (trimmedWalletName.isEmpty()) {
        return;
    }

    QStringList walletNames = autoloadWalletNames();
    walletNames.append(trimmedWalletName);
    persistAutoloadWalletNames(walletNames);
}

void OrganicLifeGUI::removeAutoloadWalletName(const QString& walletName)
{
    const QString trimmedWalletName = walletName.trimmed();
    if (trimmedWalletName.isEmpty()) {
        return;
    }

    QStringList walletNames = autoloadWalletNames();
    walletNames.removeAll(trimmedWalletName);
    persistAutoloadWalletNames(walletNames);
}

void OrganicLifeGUI::persistAutoloadWalletNames(const QStringList& walletNames)
{
    QStringList dedupedWallets;
    for (const QString& walletName : walletNames) {
        const QString trimmedWalletName = walletName.trimmed();
        if (!trimmedWalletName.isEmpty()) {
            dedupedWallets.append(trimmedWalletName);
        }
    }
    dedupedWallets.removeDuplicates();
    dedupedWallets.sort();

    QSettings settings;
    settings.setValue(QStringLiteral("walletMetadata/autoloadWallets"), dedupedWallets);
}

void OrganicLifeGUI::incomingTransaction(const QString& date, int unit, const CAmount& amount, const QString& type, const QString& address)
{
    // Only send notifications when not disabled
    if (!bdisableSystemnotifications) {
        // On new transaction, make an info balloon
        message(amount < 0 ? tr("Sent transaction") : tr("Incoming transaction"),
            tr("Date: %1\n"
               "Amount: %2\n"
               "Type: %3\n"
               "Address: %4\n")
                .arg(date)
                .arg(BitcoinUnits::formatWithUnit(unit, amount, true))
                .arg(type)
                .arg(address),
            CClientUIInterface::MSG_INFORMATION);
    }
}

#endif // ENABLE_WALLET


static bool ThreadSafeMessageBox(OrganicLifeGUI* gui, const std::string& message, const std::string& caption, unsigned int style)
{
    bool modal = (style & CClientUIInterface::MODAL);
    // The SECURE flag has no effect in the Qt GUI.
    // bool secure = (style & CClientUIInterface::SECURE);
    style &= ~CClientUIInterface::SECURE;
    bool ret = false;
    std::cout << "thread safe box: " << message << std::endl;
    // In case of modal message, use blocking connection to wait for user to click a button
    QMetaObject::invokeMethod(gui, "message",
              modal ? GUIUtil::blockingGUIThreadConnection() : Qt::QueuedConnection,
              Q_ARG(QString, QString::fromStdString(caption)),
              Q_ARG(QString, QString::fromStdString(message)),
              Q_ARG(unsigned int, style),
              Q_ARG(bool*, &ret));
    return ret;
}


void OrganicLifeGUI::subscribeToCoreSignals()
{
    // Connect signals to client
    m_handler_message_box = interfaces::MakeHandler(uiInterface.ThreadSafeMessageBox.connect(std::bind(ThreadSafeMessageBox, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
}

void OrganicLifeGUI::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    m_handler_message_box->disconnect();
}
