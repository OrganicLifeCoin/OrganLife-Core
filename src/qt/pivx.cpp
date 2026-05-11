// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/pivx-config.h"
#endif

#include "pivxgui.h"

#include "fs.h"
#include "guiinterface.h"
#include "init.h"
#include "interfaces/handler.h"
#include "net.h"
#include "qt/clientmodel.h"
#include "qt/guiconstants.h"
#include "qt/guiutil.h"
#include "qt/intro.h"
#include "qt/networkstyle.h"
#include "qt/optionsmodel.h"
#include "qt/winshutdownmonitor.h"
#include "rpc/server.h"
#include "shutdown.h"
#include "splash.h"
#include "util/system.h"
#include "random.h"
#include "utilstrencodings.h"
#include "utilitydialog.h"
#include "warnings.h"
#include "welcomecontentwidget.h"

#include <fstream>

#ifdef ENABLE_WALLET
#include "governancemodel.h"
#include "mnmodel.h"
#include "paymentserver.h"
#include "walletmodel.h"
#include "interfaces/wallet.h"
#include "wallet/walletutil.h"
#include "wallet/wallet.h"
#endif

#include <atomic>
#include <algorithm>

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QFont>
#include <QFontMetrics>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QTranslator>
#include <QProxyStyle>

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
Q_IMPORT_PLUGIN(QJpegPlugin);
Q_IMPORT_PLUGIN(QICOPlugin);
Q_IMPORT_PLUGIN(QSvgPlugin);
Q_IMPORT_PLUGIN(QSvgIconPlugin);
Q_IMPORT_PLUGIN(QGifPlugin);
#endif

// Declare meta types used for QMetaObject::invokeMethod
Q_DECLARE_METATYPE(bool*)
Q_DECLARE_METATYPE(CAmount)
Q_DECLARE_METATYPE(interfaces::WalletBalances);
Q_DECLARE_METATYPE(uint256)

static void InitMessage(const std::string& message)
{
    LogPrintf("init message: %s\n", message);
}

/*
   Translate string to current locale using Qt.
 */
static std::string Translate(const char* psz)
{
    return QCoreApplication::translate("pivx-core", psz).toStdString();
}

static QString GetLangTerritory(bool forceLangFromSetting = false)
{
    QSettings settings;
    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = QLocale::system().name();
    // 2) Language from QSettings
    QString lang_territory_qsettings = settings.value("language", "").toString();
    if (!lang_territory_qsettings.isEmpty())
        lang_territory = lang_territory_qsettings;
    // 3) -lang command line argument
    lang_territory = QString::fromStdString(gArgs.GetArg("-lang", lang_territory.toStdString()));
    return (forceLangFromSetting) ? lang_territory_qsettings : lang_territory;
}

static bool FontLooksBroken()
{
    // Heuristic: if the default application font can't render basic ASCII glyphs,
    // Qt will likely show "tofu" (squares) for most UI text.
    const QFont f = QApplication::font();
    const QFontMetrics fm(f);
    return !fm.inFont(QChar('A')) || !fm.inFont(QChar('a')) || !fm.inFont(QChar('0'));
}

static void EnsureReadableQtFonts()
{
#if defined(Q_OS_LINUX) && defined(__aarch64__)
    // On some aarch64 Linux environments with static Qt, font discovery can fail and result
    // in "tofu" (squares) for most text. Proactively load a common system font and set it
    // as the application default.
    //
    // Keep this scoped to aarch64 Linux to avoid overriding user theme/font choices on other
    // platforms.
    const QString arch = QSysInfo::currentCpuArchitecture().toLower();
    if (arch != "arm64" && arch != "aarch64") return;

    if (!FontLooksBroken()) return;

    const QStringList candidates = {
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };

    for (const QString& path : candidates) {
        if (!QFile::exists(path)) continue;
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) continue;
        const QStringList fams = QFontDatabase::applicationFontFamilies(id);
        if (fams.isEmpty()) continue;
        QApplication::setFont(QFont(fams.first()));
        break;
    }
#endif
}

class PasswordMaskProxyStyle final : public QProxyStyle
{
public:
    explicit PasswordMaskProxyStyle(const QString& key) : QProxyStyle(key) {}

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* returnData) const override
    {
        if (hint == QStyle::SH_LineEdit_PasswordCharacter) {
            return '*';
        }
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

static void EnsureReadablePasswordMask(QApplication& app)
{
#if defined(Q_OS_LINUX) && defined(__aarch64__)
    const QString arch = QSysInfo::currentCpuArchitecture().toLower();
    if (arch != "arm64" && arch != "aarch64") return;

    const int maskCodepoint = app.style()->styleHint(QStyle::SH_LineEdit_PasswordCharacter);
    const QChar passwordMask(static_cast<uint>(maskCodepoint));
    const QFontMetrics fm(QApplication::font());

    // If the current UI font cannot render the password mask glyph, force a safe ASCII mask.
    if (!passwordMask.isNull() && !fm.inFont(passwordMask)) {
        app.setStyle(new PasswordMaskProxyStyle(app.style()->name()));
    }
#endif
}

/** Set up translations */
static void initTranslations(QTranslator& qtTranslatorBase, QTranslator& qtTranslator, QTranslator& translatorBase, QTranslator& translator, bool forceLangFromSettings = false)
{
    // Remove old translators
    QApplication::removeTranslator(&qtTranslatorBase);
    QApplication::removeTranslator(&qtTranslator);
    QApplication::removeTranslator(&translatorBase);
    QApplication::removeTranslator(&translator);

    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = GetLangTerritory(forceLangFromSettings);

    // Convert to "de" only by truncating "_DE"
    QString lang = lang_territory;
    lang.truncate(lang_territory.lastIndexOf('_'));

    // Load language files for configured locale:
    // - First load the translator for the base language, without territory
    // - Then load the more specific locale translator

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    const QString translationsPath = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
#else
    const QString translationsPath = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
#endif

    // Load e.g. qt_de.qm
    if (qtTranslatorBase.load("qt_" + lang, translationsPath))
        QApplication::installTranslator(&qtTranslatorBase);

    // Load e.g. qt_de_DE.qm
    if (qtTranslator.load("qt_" + lang_territory, translationsPath))
        QApplication::installTranslator(&qtTranslator);

    // Load e.g. bitcoin_de.qm (shortcut "de" needs to be defined in pivx.qrc)
    if (translatorBase.load(lang, ":/translations/"))
        QApplication::installTranslator(&translatorBase);

    // Load e.g. bitcoin_de_DE.qm (shortcut "de_DE" needs to be defined in pivx.qrc)
    if (translator.load(lang_territory, ":/translations/"))
        QApplication::installTranslator(&translator);
}

/* qDebug() message handler --> debug.log */
void DebugMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    Q_UNUSED(context);
    if (type == QtDebugMsg) {
        LogPrint(BCLog::QT, "GUI: %s\n", msg.toStdString());
    } else {
        LogPrintf("GUI: %s\n", msg.toStdString());
    }
}

/** Class encapsulating CTEAM Core startup and shutdown.
 * Allows running startup and shutdown in a different thread from the UI thread.
 */
class BitcoinCore : public QObject
{
    Q_OBJECT
public:
    explicit BitcoinCore();

public Q_SLOTS:
    void initialize();
    void shutdown();
    bool shutdownFromThread(const QString& type = "Shutdown");
    void restart(const QStringList& args);

Q_SIGNALS:
    void initializeResult(int retval);
    void shutdownResult(int retval);
    void runawayException(const QString& message);

private:
    /// Pass fatal exception message to UI thread
    void handleRunawayException(const std::exception* e);
};

/** Main CTEAM application object */
class BitcoinApplication : public QApplication
{
    Q_OBJECT
public:
    explicit BitcoinApplication(int& argc, char** argv);
    ~BitcoinApplication();

#ifdef ENABLE_WALLET
    /// Create payment server
    void createPaymentServer();
#endif
    /// parameter interaction/setup based on rules
    void parameterSetup();
    /// Create options model
    void createOptionsModel();
    /// Create main window
    void createWindow(const NetworkStyle* networkStyle);
    /// Create splash screen
    void createSplashScreen(const NetworkStyle* networkStyle);

    /// Create tutorial screen
    bool createTutorialScreen();

    /// Request core initialization
    void requestInitialize();
    /// Request core shutdown
    void requestShutdown();

    /// Get process return value
    int getReturnValue() { return returnValue; }
    bool isCoreInitialized() const { return coreInitialized; }

    /// Get window identifier of QMainWindow (PIVXGUI)
    WId getMainWinId() const;

public Q_SLOTS:
    void initializeResult(int retval);
    void shutdownResult(int retval);
    /// Handle runaway exceptions. Shows a message box with the problem and quits the program.
    void handleRunawayException(const QString& message);
    void updateTranslation(bool forceLangFromSettings = false);

Q_SIGNALS:
    void requestedInitialize();
    void requestedRestart(QStringList args);
    void requestedShutdown();
    void stopThread();
    void splashFinished(QWidget* window);

private:
    QThread* coreThread{nullptr};
    OptionsModel* optionsModel{nullptr};
    ClientModel* clientModel{nullptr};
    PIVXGUI* window{nullptr};
    QTimer* pollShutdownTimer{nullptr};
    bool coreInitialized{false};
#ifdef ENABLE_WALLET
    PaymentServer* paymentServer{nullptr};
    WalletModel* walletModel{nullptr};
    std::vector<WalletModel*> walletModels;
    GovernanceModel* govModel{nullptr};
    MNModel* mnModel{nullptr};
    std::unique_ptr<interfaces::Handler> walletLoadHandler;
    std::unique_ptr<interfaces::Handler> walletUnloadHandler;
#endif
    int returnValue{0};
    QTranslator qtTranslatorBase, qtTranslator, translatorBase, translator;

    void startThread();
    void subscribeToWalletLifecycleNotifications();
    void unsubscribeFromWalletLifecycleNotifications();
};

#include "pivx.moc"

BitcoinCore::BitcoinCore() : QObject()
{
}

void BitcoinCore::handleRunawayException(const std::exception* e)
{
    PrintExceptionContinue(e, "Runaway exception");
    Q_EMIT runawayException(QString::fromStdString(GetWarnings("gui")));
}

void BitcoinCore::initialize()
{
    try {
        qDebug() << __func__ << ": Running AppInit2 in thread";
        if (!AppInitBasicSetup()) {
            Q_EMIT initializeResult(false);
            return;
        }
        if (!AppInitParameterInteraction()) {
            Q_EMIT initializeResult(false);
            return;
        }
        if (!AppInitSanityChecks()) {
            Q_EMIT initializeResult(false);
            return;
        }
        int rv = AppInitMain();
        Q_EMIT initializeResult(rv);
    } catch (const std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(nullptr);
    }
}

void BitcoinCore::restart(const QStringList& args)
{
    static std::atomic<bool> restartAvailable{true};
    if (restartAvailable.exchange(false)) {
        if (!shutdownFromThread("restart")) {
            qDebug() << __func__ << ": Restart failed...";
            return;
        }
        // Forced cleanup.
        CExplicitNetCleanup::callCleanup();
        ReleaseDirectoryLocks();
        QProcess::startDetached(QApplication::applicationFilePath(), args);
        qDebug() << __func__ << ": Restart initiated...";
        QApplication::quit();
    }
}

void BitcoinCore::shutdown()
{
    shutdownFromThread("Shutdown");
}

bool BitcoinCore::shutdownFromThread(const QString& type)
{
    try {
        qDebug() << __func__ << ": Running "+type+" in thread";
        Interrupt();
        Shutdown();
        qDebug() << __func__ << ": "+type+" finished";
        Q_EMIT shutdownResult(1);
        return true;
    } catch (const std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(nullptr);
    }
    return false;
}

BitcoinApplication::BitcoinApplication(int& argc, char** argv) : QApplication(argc, argv),
                                                                 coreThread(nullptr),
                                                                 optionsModel(nullptr),
                                                                 clientModel(nullptr),
                                                                 window(nullptr),
                                                                 pollShutdownTimer(nullptr),
#ifdef ENABLE_WALLET
                                                                 paymentServer(nullptr),
                                                                 walletModel(nullptr),
#endif
                                                                 returnValue(0)
{
    setQuitOnLastWindowClosed(false);
}

BitcoinApplication::~BitcoinApplication()
{
    if (coreThread) {
        qDebug() << __func__ << ": Stopping thread";
        Q_EMIT stopThread();
        coreThread->wait();
        qDebug() << __func__ << ": Stopped thread";
    }

    delete window;
    window = nullptr;
#ifdef ENABLE_WALLET
    delete paymentServer;
    paymentServer = nullptr;
#endif
    // Delete Qt-settings if user clicked on "Reset Options"
    QSettings settings;
    if (optionsModel && optionsModel->resetSettings) {
        settings.clear();
        settings.sync();
    }
    delete optionsModel;
    optionsModel = nullptr;
}

#ifdef ENABLE_WALLET
void BitcoinApplication::createPaymentServer()
{
    paymentServer = new PaymentServer(this);
}
#endif

void BitcoinApplication::createOptionsModel()
{
    optionsModel = new OptionsModel();
}

void BitcoinApplication::createWindow(const NetworkStyle* networkStyle)
{
    window = new PIVXGUI(networkStyle, nullptr);

    pollShutdownTimer = new QTimer(window);
    connect(pollShutdownTimer, &QTimer::timeout, window, &PIVXGUI::detectShutdown);
}

void BitcoinApplication::createSplashScreen(const NetworkStyle* networkStyle)
{
    Splash* splash = new Splash(networkStyle);
    // We don't hold a direct pointer to the splash screen after creation, so use
    // Qt::WA_DeleteOnClose to make sure that the window will be deleted eventually.
    splash->setAttribute(Qt::WA_DeleteOnClose);
    splash->show();
    connect(this, &BitcoinApplication::splashFinished, splash, &Splash::slotFinish);
    connect(this, &BitcoinApplication::requestedShutdown, splash, &QWidget::close);
}

bool BitcoinApplication::createTutorialScreen()
{
    WelcomeContentWidget* widget = new WelcomeContentWidget();

    connect(widget, &WelcomeContentWidget::onLanguageSelected, [this](){
        updateTranslation(true);
    });

    widget->exec();
    bool ret = widget->isOk;
    widget->deleteLater();
    return ret;
}

void BitcoinApplication::updateTranslation(bool forceLangFromSettings){
    // Re-initialize translations after change them
    initTranslations(this->qtTranslatorBase, this->qtTranslator, this->translatorBase, this->translator, forceLangFromSettings);
}

void BitcoinApplication::startThread()
{
    if (coreThread)
        return;
    coreThread = new QThread(this);
    BitcoinCore* executor = new BitcoinCore();
    executor->moveToThread(coreThread);

    /*  communication to and from thread */
    connect(executor, &BitcoinCore::initializeResult, this, &BitcoinApplication::initializeResult);
    connect(executor, &BitcoinCore::shutdownResult, this, &BitcoinApplication::shutdownResult);
    connect(executor, &BitcoinCore::runawayException, this, &BitcoinApplication::handleRunawayException);
    connect(this, &BitcoinApplication::requestedInitialize, executor, &BitcoinCore::initialize);
    connect(this, &BitcoinApplication::requestedShutdown, executor, &BitcoinCore::shutdown);
    connect(window, &PIVXGUI::requestedRestart, executor, &BitcoinCore::restart);
    /*  make sure executor object is deleted in its own thread */
    connect(this, &BitcoinApplication::stopThread, executor, &QObject::deleteLater);
    connect(this, &BitcoinApplication::stopThread, coreThread, &QThread::quit);

    coreThread->start();
}

void BitcoinApplication::subscribeToWalletLifecycleNotifications()
{
#ifdef ENABLE_WALLET
    if (walletLoadHandler || walletUnloadHandler) {
        return;
    }

    walletLoadHandler = interfaces::MakeHandler(uiInterface.LoadWallet.connect([this](CWallet* wallet) {
        QMetaObject::invokeMethod(this, [this, wallet]() {
            if (!wallet || !window || !clientModel || !optionsModel) {
                return;
            }

            if (std::find(vpwallets.begin(), vpwallets.end(), wallet) == vpwallets.end()) {
                return;
            }

            const QString walletName = QString::fromStdString(wallet->GetName());
            if (window->getWallet(walletName)) {
                return;
            }

            WalletModel* runtimeWalletModel = new WalletModel(wallet, optionsModel);
            runtimeWalletModel->setClientModel(clientModel);
            runtimeWalletModel->init();
            walletModels.emplace_back(runtimeWalletModel);
            window->addWallet(walletName, runtimeWalletModel);
            window->persistAutoloadWalletNames(window->getWalletNames());

            if (!window->currentWalletModel()) {
                window->setCurrentWallet(walletName);
                walletModel = window->currentWalletModel();
                if (mnModel) mnModel->setWalletModel(walletModel);
                if (govModel) govModel->setWalletModel(walletModel);
            }
        }, Qt::QueuedConnection);
    }));

    walletUnloadHandler = interfaces::MakeHandler(uiInterface.UnloadWallet.connect([this](CWallet* wallet) {
        const QString walletName = wallet ? QString::fromStdString(wallet->GetName()) : QString();
        QMetaObject::invokeMethod(this, [this, walletName]() {
            if (!window || walletName.isNull()) {
                return;
            }

            if (window->currentWalletName() == walletName) {
                QString fallbackWallet = window->primaryWalletName();
                if (fallbackWallet == walletName || fallbackWallet.isEmpty() || !window->getWallet(fallbackWallet)) {
                    for (const QString& loadedWalletName : window->getWalletNames()) {
                        if (loadedWalletName != walletName) {
                            fallbackWallet = loadedWalletName;
                            break;
                        }
                    }
                }

                if (!fallbackWallet.isEmpty() && fallbackWallet != walletName) {
                    window->setCurrentWallet(fallbackWallet);
                }
            }

            WalletModel* removedWalletModel = window->removeWallet(walletName);
            if (!removedWalletModel) {
                return;
            }

            const auto it = std::find(walletModels.begin(), walletModels.end(), removedWalletModel);
            if (it != walletModels.end()) {
                walletModels.erase(it);
            }

            if (walletModel == removedWalletModel) {
                walletModel = window->currentWalletModel();
            }
            if (mnModel) mnModel->setWalletModel(walletModel);
            if (govModel) govModel->setWalletModel(walletModel);

            window->persistAutoloadWalletNames(window->getWalletNames());
            delete removedWalletModel;
        }, Qt::QueuedConnection);
    }));
#endif
}

void BitcoinApplication::unsubscribeFromWalletLifecycleNotifications()
{
#ifdef ENABLE_WALLET
    if (walletLoadHandler) {
        walletLoadHandler->disconnect();
        walletLoadHandler.reset();
    }
    if (walletUnloadHandler) {
        walletUnloadHandler->disconnect();
        walletUnloadHandler.reset();
    }
#endif
}

void BitcoinApplication::parameterSetup()
{
    // Default printtoconsole to false for the GUI. GUI programs should not
    // print to the console unnecessarily.
    gArgs.SoftSetBoolArg("-printtoconsole", false);

    InitLogging();
    InitParameterInteraction();
}

void BitcoinApplication::requestInitialize()
{
    qDebug() << __func__ << ": Requesting initialize";
    startThread();
    Q_EMIT requestedInitialize();
}

void BitcoinApplication::requestShutdown()
{
    qDebug() << __func__ << ": Requesting shutdown";
    startThread();
    unsubscribeFromWalletLifecycleNotifications();
    window->hide();
    if (govModel) {
        govModel->stop();
        govModel->setWalletModel(nullptr);
    }
    if (mnModel) {
        mnModel->setWalletModel(nullptr);
    }
    for (WalletModel* loadedWalletModel : walletModels) {
        if (loadedWalletModel) loadedWalletModel->stop();
    }
    window->setClientModel(nullptr);
    pollShutdownTimer->stop();

#ifdef ENABLE_WALLET
    window->persistAutoloadWalletNames(window->getWalletNames());
    window->removeAllWallets();
    for (WalletModel* loadedWalletModel : walletModels) {
        delete loadedWalletModel;
    }
    walletModels.clear();
    walletModel = nullptr;
#endif
    delete clientModel;
    clientModel = nullptr;

    // Show a simple window indicating shutdown status
    ShutdownWindow::showShutdownWindow(window);

    StartShutdown();

    // Request shutdown from core thread
    Q_EMIT requestedShutdown();
}

void BitcoinApplication::initializeResult(int retval)
{
    qDebug() << __func__ << ": Initialization result: " << retval;
    // Set exit result: 0 if successful, 1 if failure
    returnValue = retval ? 0 : 1;
    if (retval) {
        coreInitialized = true;
#ifdef ENABLE_WALLET
        paymentServer->setOptionsModel(optionsModel);
#endif

        clientModel = new ClientModel(optionsModel);
        window->setClientModel(clientModel);

#ifdef ENABLE_WALLET
        mnModel = new MNModel(this);
        govModel = new GovernanceModel(clientModel, mnModel);
        window->setGovModel(govModel);
        window->setMNModel(mnModel);
        const std::vector<std::string> walletArgs = gArgs.GetArgs("-wallet");
        const bool hasOnlyImplicitPrimaryWalletArg = walletArgs.size() == 1 && walletArgs[0].empty();
        const bool hasExplicitWalletArgs = !walletArgs.empty() && !hasOnlyImplicitPrimaryWalletArg;
        if (!hasExplicitWalletArgs && window) {
            QStringList walletNamesToLoad = window->autoloadWalletNames();
            for (const std::string& discoveredWalletName : ListWalletDir()) {
                walletNamesToLoad.append(QString::fromStdString(discoveredWalletName));
            }
            walletNamesToLoad.removeDuplicates();
            walletNamesToLoad.sort();

            for (const QString& savedWalletName : walletNamesToLoad) {
                if (savedWalletName.isEmpty()) {
                    continue;
                }

                const auto loadedWalletIt = std::find_if(vpwallets.begin(), vpwallets.end(), [&savedWalletName](const CWalletRef loadedWallet) {
                    return loadedWallet && QString::fromStdString(loadedWallet->GetName()) == savedWalletName;
                });
                if (loadedWalletIt != vpwallets.end()) {
                    continue;
                }

                CWallet* loadedWallet = nullptr;
                const OperationResult loadResult = LoadWalletInDir(savedWalletName.toStdString(), loadedWallet);
                if (!loadResult) {
                    LogPrintf("Skipping autoload wallet '%s': %s\n",
                              savedWalletName.toStdString().c_str(),
                              loadResult.getError().c_str());
                }
            }
        }

        for (CWallet* wallet : vpwallets) {
            WalletModel* walletModel = new WalletModel(wallet, optionsModel);
            walletModel->setClientModel(clientModel);
            walletModel->init();
            walletModels.emplace_back(walletModel);
            window->addWallet(QString::fromStdString(wallet->GetName()), walletModel);
        }

        if (!vpwallets.empty()) {
            window->setCurrentWallet(QString::fromStdString(vpwallets[0]->GetName()));
            const QString savedPrimaryWallet = window->primaryWalletName();
            if (window->getWallet(savedPrimaryWallet)) {
                window->setCurrentWallet(savedPrimaryWallet);
            }
            walletModel = window->currentWalletModel();
            mnModel->setWalletModel(walletModel);
            govModel->setWalletModel(walletModel);
            mnModel->init();
        }
        window->persistAutoloadWalletNames(window->getWalletNames());
        subscribeToWalletLifecycleNotifications();
#endif

        // If -min option passed, start window minimized.
        if (gArgs.GetBoolArg("-min", false)) {
            window->showMinimized();
        } else {
            window->show();
        }
        Q_EMIT splashFinished(window);

#ifdef ENABLE_WALLET
        // Now that initialization/startup is done, process any command-line
        // CTEAM: URIs or payment requests:
        //connect(paymentServer, &PaymentServer::receivedPaymentRequest, window, &PIVXGUI::handlePaymentRequest);
        connect(window, &PIVXGUI::receivedURI, paymentServer, &PaymentServer::handleURIOrFile);
        connect(paymentServer, &PaymentServer::message, [this](const QString& title, const QString& message, unsigned int style) {
          window->message(title, message, style);
        });
        QTimer::singleShot(100, paymentServer, &PaymentServer::uiReady);
#endif
        pollShutdownTimer->start(200);
    } else {
        Q_EMIT splashFinished(window);
        QMessageBox::critical(nullptr, PACKAGE_NAME,
            QObject::tr("%1 failed to initialize. This is most likely because another instance is already running or the data directory cannot be locked.").arg(PACKAGE_NAME));
        quit(); // Exit main loop
    }
}

void BitcoinApplication::shutdownResult(int retval)
{
    qDebug() << __func__ << ": Shutdown result: " << retval;
    // Close shutdown window explicitly before quitting to prevent hang in Qt6
    ShutdownWindow::closeShutdownWindow();
    // Process events to ensure the shutdown window is hidden/deleted
    QCoreApplication::processEvents();
    quit(); // Exit main loop after shutdown finished
}

void BitcoinApplication::handleRunawayException(const QString& message)
{
    QMessageBox::critical(nullptr, "Runaway exception", QObject::tr("A fatal error occurred. CTEAM can no longer continue safely and will quit.") + QString("\n\n") + message);
    ::exit(EXIT_FAILURE);
}

WId BitcoinApplication::getMainWinId() const
{
    if (!window)
        return 0;

    return window->winId();
}

#ifndef BITCOIN_QT_TEST
int main(int argc, char* argv[])
{
#ifdef WIN32
    util::WinCmdLineArgs winArgs;
    std::tie(argc, argv) = winArgs.get();
#endif
    SetupEnvironment();

    /// 1. Parse command-line options. These take precedence over anything else.
    // Command-line options take precedence:
    gArgs.ParseParameters(argc, argv);

    // `-daemon` is a daemon-only option, but historically it has been passed
    // to the Qt binary by scripts. In GUI mode it can disable helpful recovery
    // behavior (e.g. auto-reindex) and may lead to confusing behavior.
    //
    // For compatibility treat it as a no-op in cteam-qt and guide users to
    // cteamd if they truly want background mode.
    if (gArgs.IsArgSet("-daemon")) {
        fprintf(stderr, "Warning: -daemon is ignored by cteam-qt.\n");
        fprintf(stderr, "         Use cteamd -daemon instead (e.g. `cteamd -testnet -daemon`).\n");
        gArgs.ForceSetArg("-daemon", "0");
    }

// Do not refer to data directory yet, this can be overridden by Intro::pickDataDirectory

/// 2. Basic Qt initialization (not dependent on parameters or configuration)
    Q_INIT_RESOURCE(pivx_locale);
    Q_INIT_RESOURCE(pivx);

    // High-DPI support:
    // - Qt5: opt-in via app attributes
    // - Qt6: always enabled; the attributes are deprecated/no-ops
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= 0x050600
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
#endif
#ifdef Q_OS_MAC
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
#endif
    BitcoinApplication app(argc, argv);
    EnsureReadableQtFonts();
    EnsureReadablePasswordMask(app);

    // Register meta types used for QMetaObject::invokeMethod
    qRegisterMetaType<bool*>();
    //   Need to pass name here as CAmount is a typedef (see http://qt-project.org/doc/qt-5/qmetatype.html#qRegisterMetaType)
    //   IMPORTANT if it is no longer a typedef use the normal variant above
    qRegisterMetaType<CAmount>("CAmount");
    qRegisterMetaType<CAmount>("interfaces::WalletBalances");
    qRegisterMetaType<size_t>("size_t");

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are loaded,
    // as it is used to locate QSettings
    QApplication::setOrganizationName(QAPP_ORG_NAME);
    QApplication::setOrganizationDomain(QAPP_ORG_DOMAIN);
    QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);

    /// 4. Initialization of translations, so that intro dialog is in user's language
    // Now that QSettings are accessible, initialize translations
    //initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);
    app.updateTranslation();
    translationInterface.Translate.connect(Translate);

    // Show help message immediately after parsing command-line options (for "-lang") and setting locale,
    // but before showing splash screen.
    if (gArgs.IsArgSet("-?") || gArgs.IsArgSet("-h") || gArgs.IsArgSet("-help") || gArgs.IsArgSet("-version")) {
        HelpMessageDialog help(nullptr, gArgs.IsArgSet("-version"));
        help.showOrPrint();
        return EXIT_SUCCESS;
    }

    /// 5. Now that settings and translations are available, ask user for data directory
    // User language is set up: pick a data directory
    if (!Intro::pickDataDirectory())
        return EXIT_SUCCESS;

    /// 6. Determine availability of data directory and parse pivx.conf
    /// - Do not call GetDataDir(true) before this step finishes
    if (!CheckDataDirOption()) {
        QMessageBox::critical(nullptr, PACKAGE_NAME,
            QObject::tr("Error: Specified data directory \"%1\" does not exist.").arg(QString::fromStdString(gArgs.GetArg("-datadir", ""))));
        return EXIT_FAILURE;
    }
    try {
        // First run: create a default config (mainnet by default).
        // If the user later edits the file or passes -testnet/-regtest on the CLI, those will still take precedence.
        const fs::path conf_path = GetConfigFile(gArgs.GetArg("-conf", PIVX_CONF_FILENAME));
        if (!fs::exists(conf_path)) {
            try {
                fs::create_directories(conf_path.parent_path());
            } catch (const fs::filesystem_error&) {
                // We'll still attempt to write; if that fails, ReadConfigFile will behave as usual.
            }
            std::ofstream conf_stream(conf_path.string(), std::ios::out | std::ios::binary);
            if (conf_stream.is_open()) {
                // Generate a random rpcpassword
                unsigned char rand_bytes[32];
                GetRandBytes(rand_bytes, sizeof(rand_bytes));
                std::string rpcpassword = HexStr(Span<const uint8_t>(rand_bytes, 32));

                conf_stream <<
                    "# CTEAM configuration\n"
                    "# Testnet by default.\n"
                    "\n"
                    "# RPC credentials (rpcpassword was randomly generated):\n"
                    "rpcuser=cteamrpc\n"
                    "rpcpassword=" << rpcpassword << "\n"
                    "rpcallowip=127.0.0.1\n"
                    "\n"
                    "# Enable staking by default on testnet.\n"
                    "staking=1\n"
                    "\n"
                    "# Listen for connections:\n"
                    "testnet=1\n"
                    "\n"
                    "# Listen for connections:\n"
                    "listen=1\n"
                    "server=1\n"
                    "\n";
            }
        }
        gArgs.ReadConfigFile(gArgs.GetArg("-conf", PIVX_CONF_FILENAME));
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, PACKAGE_NAME,
            QObject::tr("Error: Cannot parse configuration file: %1. Only use key=value syntax.").arg(e.what()));
        return EXIT_FAILURE;
    }

    /// 7. Determine network (and switch to network specific options)
    // - Do not call Params() before this step
    // - Do this after parsing the configuration file, as the network can be switched there
    // - QSettings() will use the new application name after this, resulting in network-specific settings
    // - Needs to be done before createOptionsModel

    // Check for -testnet parameter (Params() calls are only valid after this clause)
    try {
        SelectParams(gArgs.GetChainName());
    } catch(const std::exception& e) {
        QMessageBox::critical(nullptr, PACKAGE_NAME, QObject::tr("Error: %1").arg(e.what()));
        return EXIT_FAILURE;
    }
#ifdef ENABLE_WALLET
    // Parse URIs on command line -- this can affect Params()
    PaymentServer::ipcParseCommandLine(argc, argv);
#endif

    QScopedPointer<const NetworkStyle> networkStyle(NetworkStyle::instantiate(QString::fromStdString(Params().NetworkIDString())));
    assert(!networkStyle.isNull());
    // Allow for separate UI settings for testnets
    QApplication::setApplicationName(networkStyle->getAppName());
    // Re-initialize translations after changing application name (language in network-specific settings can be different)
    app.updateTranslation();

#ifdef ENABLE_WALLET
    /// 8. URI IPC sending
    // - Do this early as we don't want to bother initializing if we are just calling IPC
    // - Do this *after* setting up the data directory, as the data directory hash is used in the name
    // of the server.
    // - Do this after creating app and setting up translations, so errors are
    // translated properly.
    if (PaymentServer::ipcSendCommandLine())
        exit(EXIT_SUCCESS);

    // Start up the payment server early, too, so impatient users that click on
    // pivx: links repeatedly have their payment requests routed to this process:
    app.createPaymentServer();
#endif

    /// 9. Main GUI initialization
    // Install global event filter that makes sure that long tooltips can be word-wrapped
    app.installEventFilter(new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));
#if defined(Q_OS_WIN)
    // Install global event filter for processing Windows session related Windows messages (WM_QUERYENDSESSION and WM_ENDSESSION)
    qApp->installNativeEventFilter(new WinShutdownMonitor());
#endif
    // Install qDebug() message handler to route to debug.log
    qInstallMessageHandler(DebugMessageHandler);
    // Allow parameter interaction before we create the options model
    app.parameterSetup();
    // Load GUI settings from QSettings
    app.createOptionsModel();

    // Subscribe to global signals from core
    uiInterface.InitMessage.connect(InitMessage);

    bool ret = true;
#ifdef ENABLE_WALLET

#endif
    if(!ret){
        // wallet not loaded.
        return 0;
    }

    if (gArgs.GetBoolArg("-splash", true) && !gArgs.GetBoolArg("-min", false))
        app.createSplashScreen(networkStyle.data());

    try {
        app.createWindow(networkStyle.data());
        app.requestInitialize();
#if defined(Q_OS_WIN)
        WinShutdownMonitor::registerShutdownBlockReason(QObject::tr("%1 didn't yet exit safely...").arg(PACKAGE_NAME), (HWND)app.getMainWinId());
#endif
        app.exec();
        // If core initialization never succeeded (e.g. datadir is already locked by
        // another instance), do not enter the shutdown event loop. Just exit.
        if (app.isCoreInitialized()) {
            app.requestShutdown();
            app.exec();
        }
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(GetWarnings("gui")));
    } catch (...) {
        PrintExceptionContinue(nullptr, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(GetWarnings("gui")));
    }
    return app.getReturnValue();
}
#endif // BITCOIN_QT_TEST
