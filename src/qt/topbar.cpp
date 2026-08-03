// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "topbar.h"
#include "ui_topbar.h"

#include "askpassphrasedialog.h"
#include "focuseddialog.h"
#include "loadingdialog.h"
#include "lockunlock.h"

#include "qtutils.h"
#include "receivedialog.h"
#include "progressutils.h"

#include "addresstablemodel.h"
#include "balancebubble.h"
#include "bitcoinunits.h"
#include "clientmodel.h"
#include "expandablebutton.h"
#include "optionsmodel.h"
#include "qt/guiutil.h"
#include "walletmodel.h"
#include "wallet/walletutil.h"

#include "masternode-sync.h" // for MASTERNODE_SYNC_THRESHOLD
#include "tiertwo/tiertwo_sync_state.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "validation.h"

#include <QPixmap>
#include <QImage>
#include <QCoreApplication>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

#define REQUEST_UPGRADE_WALLET 1

class ButtonHoverWatcher : public QObject
{
public:
    explicit ButtonHoverWatcher(QObject* parent = nullptr) :
            QObject(parent) {}
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        QPushButton* button = qobject_cast<QPushButton*>(watched);
        if (!button) return false;

        if (event->type() == QEvent::Enter) {
            button->setIcon(QIcon("://ic-information-hover"));
            return true;
        }

        if (event->type() == QEvent::Leave){
            button->setIcon(QIcon("://ic-information"));
            return true;
        }
        return false;
    }
};

namespace {
QIcon walletStateIcon(WalletModel* model)
{
    if (!model) {
        return QIcon("://ic-wallet-selector-unlocked");
    }

    switch (model->getEncryptionStatus()) {
    case WalletModel::Locked:
        return QIcon("://ic-wallet-selector-locked");
    case WalletModel::UnlockedForStaking:
        return QIcon("://ic-wallet-selector-staking");
    case WalletModel::Unlocked:
    case WalletModel::Unencrypted:
        return QIcon("://ic-wallet-selector-unlocked");
    }

    return QIcon("://ic-wallet-selector-unlocked");
}

QString walletListLabel(OrganicLifeGUI* window, const QString& walletName, const bool activeWallet)
{
    QStringList badges;
    if (window && window->isPrimaryWallet(walletName)) {
        badges << QObject::tr("Primary");
    }
    if (activeWallet) {
        badges << QObject::tr("Active");
    }

    const QString displayName = window ? window->walletDisplayName(walletName) : walletName;
    const bool hasDistinctFolderName = !walletName.isEmpty() && walletName != displayName;
    const QString baseLabel = hasDistinctFolderName ?
            QObject::tr("%1 [%2]").arg(displayName, walletName) :
            displayName;

    return badges.isEmpty() ? baseLabel : QObject::tr("%1 (%2)").arg(baseLabel, badges.join(", "));
}
} // namespace

class WalletSelectorDialog : public FocusedDialog
{
public:
    WalletSelectorDialog(OrganicLifeGUI* window, const QStringList& walletNames, const QString& currentWallet, QWidget* parent = nullptr) :
            FocusedDialog(parent ? parent : window),
            listWidget(new QListWidget(this))
    {
        applyParentOrAppStyleSheet(parent ? parent : window);
        setModal(true);
        setFixedSize(460, 340);

        auto* hostLayout = new QVBoxLayout(this);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(0);

        auto* titleLabel = new QLabel(tr("Select Wallet"), this);
        titleLabel->setProperty("cssClass", "text-title-dialog");
        auto* closeButton = new QPushButton(this);
        initDialogCloseButton(closeButton);

        auto* bodyFrame = new QFrame(this);
        auto* bodyLayout = new QVBoxLayout(bodyFrame);
        bodyLayout->setContentsMargins(20, 18, 20, 20);
        bodyLayout->setSpacing(14);

        auto* subtitleLabel = new QLabel(tr("Choose which loaded wallet is currently shown across the interface."), bodyFrame);
        subtitleLabel->setWordWrap(true);
        subtitleLabel->setProperty("cssClass", "text-main-grey");
        bodyLayout->addWidget(subtitleLabel);

        listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        listWidget->setAlternatingRowColors(false);
        listWidget->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        listWidget->setFrameShape(QFrame::NoFrame);
        listWidget->setProperty("cssClass", "list-menu");
        bodyLayout->addWidget(listWidget, 1);

        auto* buttonsLayout = new QHBoxLayout();
        buttonsLayout->setContentsMargins(0, 0, 0, 0);
        buttonsLayout->setSpacing(10);

        auto* manageButton = new QPushButton(tr("Manage Wallets"), bodyFrame);
        manageButton->setProperty("cssClass", "btn-dialog-cancel");
        auto* cancelButton = new QPushButton(tr("Cancel"), bodyFrame);
        cancelButton->setProperty("cssClass", "btn-dialog-cancel");
        auto* selectButton = new QPushButton(tr("Switch"), bodyFrame);
        selectButton->setProperty("cssClass", "btn-primary");
        selectButton->setEnabled(false);
        buttonsLayout->addWidget(manageButton);
        buttonsLayout->addStretch(1);
        buttonsLayout->addWidget(cancelButton);
        buttonsLayout->addWidget(selectButton);
        bodyLayout->addLayout(buttonsLayout);

        hostLayout->addWidget(titleLabel);
        hostLayout->addWidget(closeButton);
        hostLayout->addWidget(bodyFrame);

        initDraggableHeaderChrome(bodyFrame, titleLabel, closeButton, 16);

        for (const QString& walletName : walletNames) {
            QListWidgetItem* item = new QListWidgetItem(listWidget);
            WalletModel* model = window ? window->getWallet(walletName) : nullptr;
            item->setText(walletListLabel(window, walletName, walletName == currentWallet));
            item->setIcon(walletStateIcon(model));
            item->setData(Qt::UserRole, walletName);
            if (walletName == currentWallet) {
                listWidget->setCurrentItem(item);
            }
        }

        connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        connect(selectButton, &QPushButton::clicked, this, &QDialog::accept);
        connect(manageButton, &QPushButton::clicked, this, [this]() {
            manageWalletsRequested = true;
            accept();
        });
        connect(listWidget, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
            accept();
        });
        connect(listWidget, &QListWidget::currentRowChanged, this, [selectButton](int row) {
            selectButton->setEnabled(row >= 0);
        });
        selectButton->setEnabled(listWidget->currentRow() >= 0);
    }

    QString selectedWallet() const
    {
        QListWidgetItem* currentItem = listWidget->currentItem();
        return currentItem ? currentItem->data(Qt::UserRole).toString() : QString();
    }

    bool wantsManageWallets() const
    {
        return manageWalletsRequested;
    }

private:
    QListWidget* listWidget;
    bool manageWalletsRequested{false};
};

class ManageWalletsDialog : public FocusedDialog
{
public:
    explicit ManageWalletsDialog(OrganicLifeGUI* window, QWidget* parent = nullptr) :
            FocusedDialog(parent ? parent : window),
            window(window),
            loadedList(new QListWidget(this))
    {
        applyParentOrAppStyleSheet(parent ? parent : window);
        setModal(true);
        resize(660, 460);

        auto* hostLayout = new QVBoxLayout(this);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        hostLayout->setSpacing(0);

        auto* titleLabel = new QLabel(tr("Manage Wallets"), this);
        titleLabel->setProperty("cssClass", "text-title-dialog");
        auto* closeButton = new QPushButton(this);
        initDialogCloseButton(closeButton);

        auto* bodyFrame = new QFrame(this);
        auto* bodyLayout = new QVBoxLayout(bodyFrame);
        bodyLayout->setContentsMargins(20, 18, 20, 20);
        bodyLayout->setSpacing(14);

        auto* subtitleLabel = new QLabel(tr("Create, unload, rename, and organize wallets managed automatically as wallet data files inside the chain data wallets directory."), bodyFrame);
        subtitleLabel->setWordWrap(true);
        subtitleLabel->setProperty("cssClass", "text-main-grey");
        bodyLayout->addWidget(subtitleLabel);

        auto* loadedColumn = new QVBoxLayout();
        loadedColumn->setSpacing(8);
        auto* loadedTitle = new QLabel(tr("Loaded wallets"), bodyFrame);
        loadedTitle->setProperty("cssClass", "text-title-dialog");
        loadedColumn->addWidget(loadedTitle);
        loadedList->setFrameShape(QFrame::NoFrame);
        loadedList->setProperty("cssClass", "list-menu");
        loadedList->setSelectionMode(QAbstractItemView::SingleSelection);
        loadedColumn->addWidget(loadedList, 1);
        bodyLayout->addLayout(loadedColumn, 1);

        auto* actionsLayout = new QHBoxLayout();
        actionsLayout->setSpacing(10);
        createButton = new QPushButton(tr("Create"), bodyFrame);
        unloadButton = new QPushButton(tr("Unload"), bodyFrame);
        renameButton = new QPushButton(tr("Rename"), bodyFrame);
        setPrimaryButton = new QPushButton(tr("Set Primary"), bodyFrame);
        setActiveButton = new QPushButton(tr("Set Active"), bodyFrame);
        for (QPushButton* button : {createButton, unloadButton, renameButton, setPrimaryButton, setActiveButton}) {
            button->setProperty("cssClass", "btn-dialog-cancel");
            actionsLayout->addWidget(button);
        }
        actionsLayout->addStretch(1);
        auto* doneButton = new QPushButton(tr("Done"), bodyFrame);
        doneButton->setProperty("cssClass", "btn-primary");
        actionsLayout->addWidget(doneButton);
        bodyLayout->addLayout(actionsLayout);

        hostLayout->addWidget(titleLabel);
        hostLayout->addWidget(closeButton);
        hostLayout->addWidget(bodyFrame);

        initDraggableHeaderChrome(bodyFrame, titleLabel, closeButton, 16);
        refreshLists();

        connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
        connect(doneButton, &QPushButton::clicked, this, &QDialog::accept);
        connect(createButton, &QPushButton::clicked, this, [this]() { createWallet(); });
        connect(unloadButton, &QPushButton::clicked, this, [this]() { unloadSelectedWallet(); });
        connect(renameButton, &QPushButton::clicked, this, [this]() { renameSelectedWallet(); });
        connect(setPrimaryButton, &QPushButton::clicked, this, [this]() { setSelectedPrimaryWallet(); });
        connect(setActiveButton, &QPushButton::clicked, this, [this]() { setSelectedActiveWallet(); });
        connect(loadedList, &QListWidget::itemSelectionChanged, this, [this]() { refreshButtons(); });
        connect(loadedList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { setSelectedActiveWallet(); });
    }

private:
    void refreshLists()
    {
        const QString loadedSelection = selectedLoadedWallet();

        loadedList->clear();
        for (const QString& walletName : window ? window->getWalletNames() : QStringList()) {
            WalletModel* model = window ? window->getWallet(walletName) : nullptr;
            auto* item = new QListWidgetItem(walletStateIcon(model), walletListLabel(window, walletName, window && walletName == window->currentWalletName()), loadedList);
            item->setData(Qt::UserRole, walletName);
            if (walletName == loadedSelection) {
                loadedList->setCurrentItem(item);
            }
        }

        refreshButtons();
    }

    void refreshButtons()
    {
        const bool hasLoadedSelection = !selectedLoadedWallet().isEmpty();
        unloadButton->setEnabled(hasLoadedSelection);
        renameButton->setEnabled(hasLoadedSelection);
        setPrimaryButton->setEnabled(hasLoadedSelection);
        setActiveButton->setEnabled(hasLoadedSelection && window && selectedLoadedWallet() != window->currentWalletName());
    }

    QString selectedLoadedWallet() const
    {
        QListWidgetItem* currentItem = loadedList->currentItem();
        return currentItem ? currentItem->data(Qt::UserRole).toString() : QString();
    }

    OperationResult runWalletOperation(const LoadingDialog::Content& content, const std::function<OperationResult()>& action)
    {
        OperationResult result(false);
        LoadingDialog* dialog = new LoadingDialog(window, content);
        QTimer::singleShot(0, dialog, [&result, action, dialog]() {
            result = action();
            dialog->accept();
        });
        openDialogWithOpaqueBackgroundFullScreen(dialog, window);
        QCoreApplication::processEvents();
        refreshLists();
        return result;
    }

    void createWallet()
    {
        bool ok = false;
        const QString walletName = QInputDialog::getText(this,
                                                         tr("Create Wallet"),
                                                         tr("Wallet name"),
                                                         QLineEdit::Normal,
                                                         QString(),
                                                         &ok).trimmed();
        if (!ok || walletName.isEmpty()) {
            return;
        }

        LoadingDialog::Content loadingContent;
        loadingContent.eyebrow = tr("Wallet");
        loadingContent.title = tr("Creating wallet");
        loadingContent.supportText = tr("Creating the new wallet data file inside the wallets data directory.");
        CWallet* loadedWallet = nullptr;
        const OperationResult result = runWalletOperation(loadingContent, [walletName, &loadedWallet]() {
            return CreateWalletInDir(walletName.toStdString(), loadedWallet);
        });

        if (!result) {
            QMessageBox::warning(this, tr("Create Wallet"), QString::fromStdString(result.getError()));
            return;
        }

        if (window) {
            window->setWalletDisplayName(walletName, walletName);
            window->addAutoloadWalletName(walletName);
        }
        QCoreApplication::processEvents();
        refreshLists();
    }

    void unloadSelectedWallet()
    {
        const QString walletName = selectedLoadedWallet();
        if (!window || walletName.isEmpty()) {
            return;
        }

        const QStringList loadedWalletNames = window->getWalletNames();
        if (loadedWalletNames.size() <= 1) {
            QMessageBox::warning(this, tr("Unload Wallet"), tr("At least one wallet must remain loaded."));
            return;
        }

        QString fallbackWallet = window->primaryWalletName();
        if (fallbackWallet == walletName || fallbackWallet.isEmpty() || !window->getWallet(fallbackWallet)) {
            for (const QString& loadedWalletName : loadedWalletNames) {
                if (loadedWalletName != walletName) {
                    fallbackWallet = loadedWalletName;
                    break;
                }
            }
        }

        if (fallbackWallet.isEmpty()) {
            QMessageBox::warning(this, tr("Unload Wallet"), tr("Select another wallet to keep loaded before unloading this one."));
            return;
        }

        if (window->isPrimaryWallet(walletName)) {
            window->setPrimaryWalletName(fallbackWallet);
        }
        if (window->currentWalletName() == walletName) {
            window->setCurrentWallet(fallbackWallet);
        }

        LoadingDialog::Content loadingContent;
        loadingContent.eyebrow = tr("Wallet");
        loadingContent.title = tr("Unloading wallet");
        loadingContent.supportText = tr("Stopping wallet activity and removing it from the current session.");
        const OperationResult result = runWalletOperation(loadingContent, [walletName]() {
            return UnloadWalletByName(walletName.toStdString());
        });

        if (!result) {
            QMessageBox::warning(this, tr("Unload Wallet"), QString::fromStdString(result.getError()));
            return;
        }
        window->removeAutoloadWalletName(walletName);
        QCoreApplication::processEvents();
        refreshLists();
    }

    void renameSelectedWallet()
    {
        if (!window) {
            return;
        }

        const QString walletName = selectedLoadedWallet();
        if (walletName.isEmpty()) {
            return;
        }

        bool ok = false;
        const QString displayName = QInputDialog::getText(this,
                                                          tr("Rename Wallet"),
                                                          tr("Display name"),
                                                          QLineEdit::Normal,
                                                          window->walletDisplayName(walletName),
                                                          &ok);
        if (!ok) {
            return;
        }

        window->setWalletDisplayName(walletName, displayName);
        refreshLists();
    }

    void setSelectedPrimaryWallet()
    {
        const QString walletName = selectedLoadedWallet();
        if (!window || walletName.isEmpty()) {
            return;
        }

        window->setPrimaryWalletName(walletName);
        refreshLists();
    }

    void setSelectedActiveWallet()
    {
        const QString walletName = selectedLoadedWallet();
        if (!window || walletName.isEmpty()) {
            return;
        }

        window->setCurrentWallet(walletName);
        refreshLists();
    }

    OrganicLifeGUI* window{nullptr};
    QListWidget* loadedList{nullptr};
    QPushButton* createButton{nullptr};
    QPushButton* unloadButton{nullptr};
    QPushButton* renameButton{nullptr};
    QPushButton* setPrimaryButton{nullptr};
    QPushButton* setActiveButton{nullptr};
};


TopBar::TopBar(OrganicLifeGUI* _mainWindow, QWidget *parent) :
    PWidget(_mainWindow, parent),
    ui(new Ui::TopBar)
{
    ui->setupUi(this);

    // Set parent stylesheet
    this->setStyleSheet(_mainWindow->styleSheet());
    /* Containers */
    ui->containerTop->setContentsMargins(0, 4, 0, 10);
    ui->containerTop->setProperty("cssClass", ui->bottom_container->isVisible() ? "container-top-home" : "container-top");

    std::initializer_list<QWidget*> lblTitles = {ui->labelTitle1, ui->labelTitle3, ui->labelTitle4, ui->labelTrans, ui->labelShield};
    setCssProperty(lblTitles, "text-title-topbar");
    QFont font;
    font.setWeight(QFont::Light);
    for (QWidget* w : lblTitles) { w->setFont(font); }

    // Amount information top
    ui->widgetTopAmount->setVisible(false);
    setCssProperty({ui->labelAmountTopPiv, ui->labelAmountTopShieldedPiv}, "amount-small-topbar");
    setCssProperty({ui->labelAmountPiv}, "amount-topbar");
    setCssProperty({ui->labelPendingPiv, ui->labelImmaturePiv}, "amount-small-topbar");

    ui->pushButtonFAQ->setButtonClassStyle("cssClass", "btn-check-faq");
    ui->pushButtonFAQ->setButtonText(tr("FAQ"));

    ui->pushButtonHDUpgrade->setButtonClassStyle("cssClass", "btn-check-hd-upgrade");
    ui->pushButtonHDUpgrade->setButtonText(tr("Upgrade to HD Wallet"));
    ui->pushButtonHDUpgrade->setNoIconText("HD");

    ui->pushButtonConnection->setButtonClassStyle("cssClass", "btn-check-connect-inactive");
    ui->pushButtonConnection->setButtonText(tr("No Connection"));

    ui->pushButtonTor->setButtonClassStyle("cssClass", "btn-check-tor-inactive");
    ui->pushButtonTor->setButtonText(tr("Tor Disabled"));
    ui->pushButtonTor->setChecked(false);

    ui->pushButtonStack->setButtonClassStyle("cssClass", "btn-check-stack-inactive");
    ui->pushButtonStack->setButtonText(tr("Staking Disabled"));

    ui->pushButtonColdStaking->setButtonClassStyle("cssClass", "btn-check-cold-staking-inactive");
    ui->pushButtonColdStaking->setButtonText(tr("Cold Staking Disabled"));

    ui->pushButtonSync->setButtonClassStyle("cssClass", "btn-check-sync");
    ui->pushButtonSync->setButtonText(tr("Synchronizing.."));
    ui->pushButtonSync->setExpandedWidth(360); // Wider for long sync text

    ui->pushButtonWallet->setButtonClassStyle("cssClass", "btn-check-wallet");
    ui->pushButtonWallet->setButtonText(tr("Primary"));
    ui->pushButtonWallet->setNoIconText(QString());
    ui->pushButtonWallet->setExpandedWidth(220);

    ui->pushButtonLock->setButtonClassStyle("cssClass", "btn-check-lock");

    updateThemeButtonState(isLightTheme(), false);
    // Ensure button starts in compact mode - force immediate resize
    ui->pushButtonTheme->setMinimumWidth(48);
    ui->pushButtonTheme->setMaximumWidth(48);

    setCssProperty(ui->qrContainer, "container-qr");
    // Hide the down arrow button - QR is now clickable directly
    ui->pushButtonQR->setVisible(false);
    setCssProperty(ui->pushButtonBalanceInfo, "btn-info");
    ButtonHoverWatcher * watcher = new ButtonHoverWatcher(this);
    ui->pushButtonBalanceInfo->installEventFilter(watcher);

    // QR image - update icon according to current theme.
    updateQrButtonIcon(isLightTheme());
    // Ensure the icon is centered in the button
    ui->btnQr->setLayoutDirection(Qt::LeftToRight);

    ui->pushButtonLock->setButtonText(tr("Locked"));
    ui->pushButtonLock->setButtonClassStyle("cssClass", "btn-check-status-lock");
    ui->pushButtonLock->setExpandedWidth(160); // Width for lock button


    // QR button is now hidden - clicking the QR code directly opens receive
    connect(ui->btnQr, &QPushButton::clicked, this, &TopBar::onBtnReceiveClicked);
    connect(ui->pushButtonBalanceInfo, &QPushButton::clicked, this, &TopBar::onBtnBalanceInfoClicked);
    connect(ui->pushButtonWallet, &ExpandableButton::Mouse_Pressed, this, &TopBar::onWalletButtonClicked);
    connect(ui->pushButtonLock, &ExpandableButton::Mouse_Pressed, this, &TopBar::onBtnLockClicked);
    connect(ui->pushButtonTheme, &ExpandableButton::Mouse_Pressed, this, &TopBar::onThemeClicked);
    connect(ui->pushButtonFAQ, &ExpandableButton::Mouse_Pressed, [this](){window->openFAQ();});
    connect(ui->pushButtonColdStaking, &ExpandableButton::Mouse_Pressed, this, &TopBar::onColdStakingClicked);
    connect(ui->pushButtonSync, &ExpandableButton::Mouse_Pressed, [this](){window->goToSettingsInfo();});
    connect(ui->pushButtonConnection, &ExpandableButton::Mouse_Pressed, [this](){window->openNetworkMonitor();});

    setupStatusButtonHoverBehavior();
    setupStatusHoverPill();

    hoverLeaveDebounceTimer = new QTimer(this);
    hoverLeaveDebounceTimer->setSingleShot(true);
    hoverLeaveDebounceTimer->setInterval(80);
    connect(hoverLeaveDebounceTimer, &QTimer::timeout, this, &TopBar::clearTopBarHoverState);

    // Force layout refresh after startup to ensure buttons are properly sized
    QTimer::singleShot(100, this, &TopBar::refreshButtonLayouts);
    QTimer::singleShot(300, this, &TopBar::refreshButtonLayouts);
    QTimer::singleShot(600, this, &TopBar::refreshButtonLayouts);

}

void TopBar::updateThemeButtonState(bool isLightTheme, bool refreshStyle)
{
    if (isLightTheme) {
        ui->pushButtonTheme->setButtonClassStyle("cssClass", "btn-check-theme-light", refreshStyle);
        ui->pushButtonTheme->setButtonText(tr("Light"));
        ui->pushButtonTheme->setNoIconText("L");
    } else {
        ui->pushButtonTheme->setButtonClassStyle("cssClass", "btn-check-theme-dark", refreshStyle);
        ui->pushButtonTheme->setButtonText(tr("Dark"));
        ui->pushButtonTheme->setNoIconText("D");
    }
    ui->pushButtonTheme->setSmall(false);
    if (refreshStyle) {
        updateStyle(ui->pushButtonTheme);
    }
}

void TopBar::updateQrButtonIcon(bool isLightTheme)
{
    QImage image(":/img-qr-test-big");
    if (image.isNull()) return;

    image = image.convertToFormat(QImage::Format_ARGB32);
    if (!isLightTheme) {
        image.invertPixels(QImage::InvertRgb);
    }

    const QPixmap iconPixmap = QPixmap::fromImage(image).scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    ui->btnQr->setIcon(QIcon(iconPixmap));
    ui->btnQr->setIconSize(iconPixmap.size());
}

void TopBar::onThemeClicked()
{
    // Store theme
    bool lightTheme = !isLightTheme();

    setTheme(lightTheme);

    updateThemeButtonState(lightTheme, true);
    updateQrButtonIcon(lightTheme);
    refreshButtonLayouts();

    update();

    Q_EMIT themeChanged(lightTheme);
}

void TopBar::changeTheme(bool isLightTheme, QString& theme)
{
    Q_UNUSED(theme);
    updateThemeButtonState(isLightTheme, true);
    updateQrButtonIcon(isLightTheme);
}

void TopBar::onBtnLockClicked()
{
    if (walletModel) {
        if (walletModel->getEncryptionStatus() == WalletModel::Unencrypted) {
            encryptWallet();
        } else {
            if (!lockUnlockWidget) {
                lockUnlockWidget = new LockUnlock(window);
                lockUnlockWidget->setStyleSheet("margin:0px; padding:0px;");
                connect(lockUnlockWidget, &LockUnlock::Mouse_Leave, this, &TopBar::lockDropdownMouseLeave);
                connect(ui->pushButtonLock, &ExpandableButton::Mouse_HoverLeave, [this]() {
                    QMetaObject::invokeMethod(this, "lockDropdownMouseLeave", Qt::QueuedConnection);
                });
                connect(lockUnlockWidget, &LockUnlock::lockClicked ,this, &TopBar::lockDropdownClicked);
            }

            lockUnlockWidget->updateStatus(walletModel->getEncryptionStatus());
            if (ui->pushButtonLock->width() <= 48) {
                ui->pushButtonLock->setExpanded();
            }
            // Keep it open
            ui->pushButtonLock->setKeepExpanded(true);
            QMetaObject::invokeMethod(this, "openLockUnlock", Qt::QueuedConnection);
        }
    }
}

void TopBar::onWalletButtonClicked()
{
    if (!window) {
        return;
    }

    const QStringList walletNames = window->getWalletNames();
    WalletSelectorDialog dialog(window, walletNames, window->currentWalletName(), window);
    showHideOp(true);
    if (openDialogWithOpaqueBackgroundY(&dialog, window, 3, 6)) {
        if (dialog.wantsManageWallets()) {
            openManageWalletsDialog();
        } else {
            performWalletSwitch(dialog.selectedWallet());
        }
    }
}

void TopBar::performWalletSwitch(const QString& walletName)
{
    if (!window || walletName.isNull() || walletName == window->currentWalletName()) {
        return;
    }

    LoadingDialog::Content loadingContent;
    loadingContent.eyebrow = tr("Wallet");
    loadingContent.title = tr("Switching wallet");
    loadingContent.supportText = tr("Refreshing balances, addresses, and activity for the selected wallet.");
    LoadingDialog* dialog = new LoadingDialog(window, loadingContent);

    bool switched = false;
    showHideOp(true);
    QTimer::singleShot(0, dialog, [this, dialog, walletName, &switched]() {
        switched = window->setCurrentWallet(walletName);
        dialog->accept();
    });
    openDialogWithOpaqueBackgroundFullScreen(dialog, window);

    if (!switched) {
        warn(tr("Wallet Switch"), tr("Unable to switch to the selected wallet"));
    }
}

void TopBar::openManageWalletsDialog()
{
    if (!window) {
        return;
    }

    ManageWalletsDialog dialog(window, window);
    showHideOp(true);
    openDialogWithOpaqueBackgroundY(&dialog, window, 3, 5);
    refreshWalletSelector();
}

void TopBar::openLockUnlock()
{
    lockUnlockWidget->setFixedWidth(ui->pushButtonLock->width());
    lockUnlockWidget->adjustSize();

    lockUnlockWidget->move(
            ui->pushButtonLock->pos().rx() + window->getNavWidth() + 10,
            ui->pushButtonLock->y() + 36
    );

    lockUnlockWidget->raise();
    lockUnlockWidget->activateWindow();
    lockUnlockWidget->show();
}

void TopBar::openPassPhraseDialog(AskPassphraseDialog::Mode mode, AskPassphraseDialog::Context ctx)
{
    if (!walletModel)
        return;

    showHideOp(true);
    AskPassphraseDialog *dlg = new AskPassphraseDialog(mode, window, walletModel, ctx);
    dlg->adjustSize();
    openDialogWithOpaqueBackgroundY(dlg, window);

    refreshStatus();
    dlg->deleteLater();
}

void TopBar::encryptWallet()
{
    return openPassPhraseDialog(AskPassphraseDialog::Mode::Encrypt, AskPassphraseDialog::Context::Encrypt);
}

void TopBar::unlockWallet()
{
    if (!walletModel)
        return;
    // Unlock wallet when requested by wallet model (if unlocked or unlocked for staking only)
    if (walletModel->isWalletLocked(false))
        return openPassPhraseDialog(AskPassphraseDialog::Mode::Unlock, AskPassphraseDialog::Context::Unlock_Full);
}

static bool isExecuting = false;

void TopBar::lockDropdownClicked(const StateClicked& state)
{
    lockUnlockWidget->close();
    if (walletModel && !isExecuting) {
        isExecuting = true;

        switch (lockUnlockWidget->lock) {
            case 0: {
                if (walletModel->getEncryptionStatus() == WalletModel::Locked)
                    break;
                walletModel->setWalletLocked(true);
                ui->pushButtonLock->setButtonText(tr("Wallet Locked"));
                ui->pushButtonLock->setButtonClassStyle("cssClass", "btn-check-status-lock", true);
                // Directly update the staking status icon when the wallet is manually locked here
                // so the feedback is instant (no need to wait for the polling timeout)
                setStakingStatusActive(false);
                break;
            }
            case 1: {
                if (walletModel->getEncryptionStatus() == WalletModel::Unlocked)
                    break;
                showHideOp(true);
                AskPassphraseDialog *dlg = new AskPassphraseDialog(AskPassphraseDialog::Mode::Unlock, window, walletModel,
                                        AskPassphraseDialog::Context::ToggleLock);
                dlg->adjustSize();
                openDialogWithOpaqueBackgroundY(dlg, window);
                if (walletModel->getEncryptionStatus() == WalletModel::Unlocked) {
                    ui->pushButtonLock->setButtonText(tr("Wallet Unlocked"));
                    ui->pushButtonLock->setButtonClassStyle("cssClass", "btn-check-status-unlock", true);
                }
                dlg->deleteLater();
                break;
            }
            case 2: {
                WalletModel::EncryptionStatus status = walletModel->getEncryptionStatus();
                if (status == WalletModel::UnlockedForStaking)
                    break;

                if (status == WalletModel::Unlocked) {
                    walletModel->lockForStakingOnly();
                } else {
                    showHideOp(true);
                    AskPassphraseDialog *dlg = new AskPassphraseDialog(AskPassphraseDialog::Mode::UnlockAnonymize,
                                                                       window, walletModel,
                                                                       AskPassphraseDialog::Context::ToggleLock);
                    dlg->adjustSize();
                    openDialogWithOpaqueBackgroundY(dlg, window);
                    dlg->deleteLater();
                }
                if (walletModel->getEncryptionStatus() == WalletModel::UnlockedForStaking) {
                    ui->pushButtonLock->setButtonText(tr("Wallet Unlocked for staking"));
                    ui->pushButtonLock->setButtonClassStyle("cssClass", "btn-check-status-staking", true);
                }
                break;
            }
        }

        ui->pushButtonLock->setKeepExpanded(false);
        ui->pushButtonLock->setSmall();
        ui->pushButtonLock->update();

        isExecuting = false;
    }
}

void TopBar::lockDropdownMouseLeave()
{
    if (lockUnlockWidget->isVisible() && !lockUnlockWidget->isHovered()) {
        lockUnlockWidget->hide();
        ui->pushButtonLock->setKeepExpanded(false);
        ui->pushButtonLock->setSmall();
        ui->pushButtonLock->update();
    }
}

void TopBar::onBtnReceiveClicked()
{
    if (walletModel) {
        QString addressStr = walletModel->getAddressTableModel()->getAddressToShow();
        if (addressStr.isNull()) {
            inform(tr("Error generating address"));
            return;
        }
        showHideOp(true);
        ReceiveDialog *receiveDialog = new ReceiveDialog(window);
        receiveDialog->updateQr(addressStr);
        if (openDialogWithOpaqueBackgroundY(receiveDialog, window)) {
            inform(tr("Address Copied"));
        }
        receiveDialog->deleteLater();
    }
}

void TopBar::onBtnBalanceInfoClicked()
{
    if (!walletModel) return;
    if (balanceBubble) {
        if (balanceBubble->isVisible()) {
            balanceBubble->hide();
            return;
        }
    } else balanceBubble = new BalanceBubble(this);

    const auto& balances = walletModel->GetWalletBalances();
    balanceBubble->updateValues(balances.balance - balances.shielded_balance, balances.shielded_balance, nDisplayUnit);
    QPoint pos = this->pos();
    pos.setX(pos.x() + (ui->labelTitle1->width()) + 60);
    pos.setY(pos.y() + 20);
    balanceBubble->move(pos);
    balanceBubble->show();
}

void TopBar::showTop()
{
    if (ui->bottom_container->isVisible()) {
        if (balanceBubble && balanceBubble->isVisible()) balanceBubble->hide();
        ui->bottom_container->setVisible(false);
        ui->widgetTopAmount->setVisible(true);
        this->setFixedHeight(75);
    }
    compactTopMode = true;
    ui->containerTop->setProperty("cssClass", "container-top");
    updateStyle(ui->containerTop);
    for (ExpandableButton* button : statusButtons) {
        if (!button) continue;
        button->setHoverExpandEnabled(false);
        button->setKeepExpanded(false);
        button->setSmall(false);
    }
    refreshButtonLayouts();
}

void TopBar::showBottom()
{
    ui->widgetTopAmount->setVisible(false);
    ui->bottom_container->setVisible(true);
    this->setFixedHeight(200);
    this->adjustSize();
    compactTopMode = false;
    ui->containerTop->setProperty("cssClass", "container-top-home");
    updateStyle(ui->containerTop);
    for (ExpandableButton* button : statusButtons) {
        if (!button) continue;
        button->setHoverExpandEnabled(true);
    }
    clearTopBarHoverState();
    refreshButtonLayouts();
}

void TopBar::onColdStakingClicked()
{
    bool isColdStakingEnabled = walletModel->isColdStaking();
    ui->pushButtonColdStaking->setChecked(isColdStakingEnabled);

    bool show = (isInitializing) ? walletModel->getOptionsModel()->isColdStakingScreenEnabled() :
            walletModel->getOptionsModel()->invertColdStakingScreenStatus();
    QString className;
    QString text;

    if (isColdStakingEnabled) {
        text = "Cold Staking Active";
        className = (show) ? "btn-check-cold-staking-checked" : "btn-check-cold-staking-unchecked";
    } else if (show) {
        className = "btn-check-cold-staking";
        text = "Cold Staking Enabled";
    } else {
        className = "btn-check-cold-staking-inactive";
        text = "Cold Staking Disabled";
    }

    ui->pushButtonColdStaking->setButtonClassStyle("cssClass", className, true);
    ui->pushButtonColdStaking->setButtonText(text);
    updateStyle(ui->pushButtonColdStaking);

    Q_EMIT onShowHideColdStakingChanged(show);
}

TopBar::~TopBar()
{
    if (timerStakingIcon) {
        timerStakingIcon->stop();
    }
    delete ui;
}

void TopBar::loadClientModel()
{
    if (clientModel) {
        // Keep up to date with client
        setNumConnections(clientModel->getNumConnections());
        connect(clientModel, &ClientModel::numConnectionsChanged, this, &TopBar::setNumConnections);

        setNumBlocks(clientModel->getNumBlocks());
        connect(clientModel, &ClientModel::numBlocksChanged, this, &TopBar::setNumBlocks);
        connect(clientModel, &ClientModel::networkActiveChanged, this, &TopBar::setNetworkActive);

        timerStakingIcon = new QTimer(ui->pushButtonStack);
        connect(timerStakingIcon, &QTimer::timeout, this, &TopBar::updateStakingStatus);
        timerStakingIcon->start(10000);
        updateStakingStatus();
    }
}

void TopBar::setStakingStatusActive(bool fActive)
{
    if (ui->pushButtonStack->isChecked() != fActive) {
        ui->pushButtonStack->setButtonText(fActive ? tr("Staking active") : tr("Staking not active"));
        ui->pushButtonStack->setChecked(fActive);
        ui->pushButtonStack->setButtonClassStyle("cssClass", (fActive ?
                                                                "btn-check-stack" :
                                                                "btn-check-stack-inactive"), true);
    }
}
void TopBar::updateStakingStatus()
{
    if (walletModel && !walletModel->isShutdownRequested()) {
        setStakingStatusActive(!walletModel->isWalletLocked() &&
                               walletModel->isStakingStatusActive());

        // Taking advantage of this timer to update Tor status if needed.
        updateTorIcon();
    }
}

void TopBar::setNumConnections(int count)
{
    const bool hasConnections = count > 0;
    if (ui->pushButtonConnection->isChecked() != hasConnections) {
        ui->pushButtonConnection->setChecked(hasConnections);
    }
    ui->pushButtonConnection->setButtonClassStyle(
            "cssClass",
            hasConnections ? "btn-check-connect" : "btn-check-connect-inactive",
            true);

    ui->pushButtonConnection->setButtonText(tr("%n active connection(s)", "", count));
    const int textWidth = ui->pushButtonConnection->fontMetrics().horizontalAdvance(ui->pushButtonConnection->getText());
    ui->pushButtonConnection->setExpandedWidth(std::clamp(textWidth + 156, 260, 620));
    updateStakingStatus();
}

void TopBar::setNetworkActive(bool active)
{
    if (!active) {
        progressOverrideActive = false;
        progressOverrideTitle.clear();
        progressOverrideValue = 0;
    }

    if (!active) {
        ui->pushButtonSync->setButtonText(tr("Network activity disabled"));
        ui->pushButtonSync->setButtonClassStyle("cssClass", "btn-check-sync-inactive", true);
        const int textWidth = ui->pushButtonSync->fontMetrics().horizontalAdvance(ui->pushButtonSync->getText());
        ui->pushButtonSync->setExpandedWidth(std::clamp(textWidth + 156, 260, 620));
        ui->pushButtonSync->setProgress(0);
        syncDisplayedProgress = 0.0;
        syncMaxKnownHeader = 0;
    } else {
        if (!clientModel) return;
        setNumBlocks(clientModel->getLastBlockProcessedHeight());
    }
}

void TopBar::setSyncProgress(const QString& title, int nProgress)
{
    if (nProgress >= 100) {
        progressOverrideActive = false;
        progressOverrideTitle.clear();
        progressOverrideValue = 0;
        if (clientModel) {
            setNumBlocks(clientModel->getLastBlockProcessedHeight());
        }
        return;
    }

    progressOverrideActive = true;
    progressOverrideTitle = title;
    progressOverrideValue = ClampProgressPercent(nProgress);

    ui->pushButtonSync->setButtonClassStyle("cssClass", "btn-check-sync", true);
    ui->pushButtonSync->setButtonText(BuildProgressLabel(progressOverrideTitle, progressOverrideValue, tr("Processing...")));
    const int textWidth = ui->pushButtonSync->fontMetrics().horizontalAdvance(ui->pushButtonSync->getText());
    ui->pushButtonSync->setExpandedWidth(std::clamp(textWidth + 188, 300, 700));
    ui->pushButtonSync->setProgress(progressOverrideValue);
    syncDisplayedProgress = static_cast<double>(progressOverrideValue);
    syncMaxKnownHeader = 0;
    Q_EMIT walletSynced(false);
    Q_EMIT tierTwoSynced(false);
    updateStakingStatus();
}

void TopBar::setNumBlocks(int count)
{
    if (!clientModel)
        return;

    if (progressOverrideActive) {
        ui->pushButtonSync->setButtonClassStyle("cssClass", "btn-check-sync", true);
        ui->pushButtonSync->setButtonText(BuildProgressLabel(progressOverrideTitle, progressOverrideValue, tr("Processing...")));
        const int textWidth = ui->pushButtonSync->fontMetrics().horizontalAdvance(ui->pushButtonSync->getText());
        ui->pushButtonSync->setExpandedWidth(std::clamp(textWidth + 188, 300, 700));
        ui->pushButtonSync->setProgress(progressOverrideValue);
        updateStakingStatus();
        return;
    }

    std::string text;
    bool needState = true;
    const bool isPrePoS = !Params().IsRegTestNet() &&
            !Params().GetConsensus().NetworkUpgradeActive(
                    WITH_LOCK(cs_main, return chainActive.Height();),
                    Consensus::UPGRADE_POS);
    // On low-hashrate / low-activity networks (e.g. testnet), the "time behind" heuristic can
    // incorrectly mark a fully-synced node as "behind" simply because no new blocks were mined
    // recently. Prefer the core-provided initial-sync flag to decide whether we're actually
    // still syncing the chain.
    const bool chainSynced = g_tiertwo_sync_state.IsBlockchainSynced() || !clientModel->inInitialBlockDownload();
    if (chainSynced) {
        // chain synced
        Q_EMIT walletSynced(true);
        // During the PoW-only phase, tier-two features are not yet active, so
        // masternode/budget sync cannot meaningfully complete. Avoid confusing the user with
        // endless "Synchronizing masternodes..." warnings until PoS activates.
        if (g_tiertwo_sync_state.IsSynced() || isPrePoS) {
            // Node synced
            ui->pushButtonSync->setButtonClassStyle("cssClass", "btn-check-synced", true);
            ui->pushButtonSync->setButtonText(tr("Synchronized - Block: %1").arg(QString::number(count)));
            const int syncedTextWidth = ui->pushButtonSync->fontMetrics().horizontalAdvance(ui->pushButtonSync->getText());
            ui->pushButtonSync->setExpandedWidth(std::clamp(syncedTextWidth + 188, 300, 700));
            ui->pushButtonSync->setProgress(-1);
            syncDisplayedProgress = -1.0;
            syncMaxKnownHeader = 0;
            Q_EMIT tierTwoSynced(true);
            return;
        } else {

            // TODO: Show out of sync warning
            int RequestedMasternodeAssets = g_tiertwo_sync_state.GetSyncPhase();
            int nAttempt = masternodeSync.RequestedMasternodeAttempt < MASTERNODE_SYNC_THRESHOLD ?
                           masternodeSync.RequestedMasternodeAttempt + 1 :
                           MASTERNODE_SYNC_THRESHOLD;
            int progress = nAttempt + (RequestedMasternodeAssets - 1) * MASTERNODE_SYNC_THRESHOLD;
            if (progress >= 0) {
                // todo: MN progress..
                text = strprintf("%s - Block: %d", masternodeSync.GetSyncStatus(), count);
                needState = false;
            }
        }
    } else {
        Q_EMIT walletSynced(false);
    }

    if (needState && clientModel->isTipCached() && clientModel->inInitialBlockDownload()) {
        // Represent time from last generated block in human readable text
        QDateTime lastBlockDate = clientModel->getLastBlockDate();
        QDateTime currentDate = QDateTime::currentDateTime();
        int secs = lastBlockDate.secsTo(currentDate);

        QString timeBehindText;
        const int HOUR_IN_SECONDS = 60 * 60;
        const int DAY_IN_SECONDS = 24 * 60 * 60;
        const int WEEK_IN_SECONDS = 7 * 24 * 60 * 60;
        const int YEAR_IN_SECONDS = 31556952; // Average length of year in Gregorian calendar
        if (secs < 2 * DAY_IN_SECONDS) {
            timeBehindText = tr("%n hour(s)", "", secs / HOUR_IN_SECONDS);
        } else if (secs < 2 * WEEK_IN_SECONDS) {
            timeBehindText = tr("%n day(s)", "", secs / DAY_IN_SECONDS);
        } else if (secs < YEAR_IN_SECONDS) {
            timeBehindText = tr("%n week(s)", "", secs / WEEK_IN_SECONDS);
        } else {
            int years = secs / YEAR_IN_SECONDS;
            int remainder = secs % YEAR_IN_SECONDS;
            timeBehindText = tr("%1 and %2").arg(tr("%n year(s)", "", years)).arg(
                    tr("%n week(s)", "", remainder / WEEK_IN_SECONDS));
        }
        QString timeBehind(" behind. Scanning block ");
        QString str = timeBehindText + timeBehind + QString::number(count);
        text = str.toStdString();
        
        // Calculate sync progress using checkpoints for better accuracy
        const int currentHeight = std::max(0, clientModel->getLastBlockProcessedHeight());
        const int bestKnownHeaderHeight = WITH_LOCK(cs_main, return pindexBestHeader ? pindexBestHeader->nHeight : currentHeight);
        const int checkpointHeight = WITH_LOCK(cs_main, return Checkpoints::GetTotalBlocksEstimate());
        syncMaxKnownHeader = std::max(syncMaxKnownHeader, std::max(checkpointHeight, bestKnownHeaderHeight));
        const int maxKnownHeight = std::max({1, checkpointHeight, bestKnownHeaderHeight, syncMaxKnownHeader});
        
        // Use direct percentage based on current vs max known height
        const double targetRatio = static_cast<double>(currentHeight) / static_cast<double>(maxKnownHeight);
        double targetProgress = targetRatio * 100.0;
        if (currentHeight > 0 && targetProgress < 1.0) {
            targetProgress = 1.0;
        }

        if (syncDisplayedProgress < 0.0) {
            syncDisplayedProgress = targetProgress;
        } else {
            // Keep progress monotonic while syncing to avoid visible backtracking jitter.
            const double monotonicTarget = std::max(targetProgress, syncDisplayedProgress);
            const double delta = monotonicTarget - syncDisplayedProgress;
            const double absDelta = std::abs(delta);
            const double alpha = (delta >= 0.0)
                    ? std::clamp(0.08 + absDelta / 220.0, 0.08, 0.20)
                    : std::clamp(0.06 + absDelta / 220.0, 0.06, 0.14);
            syncDisplayedProgress += delta * alpha;
        }

        int fillProgress = static_cast<int>(std::lround(syncDisplayedProgress));
        // Never show fully complete while still in IBD.
        if (clientModel->inInitialBlockDownload()) {
            fillProgress = std::clamp(fillProgress, 0, 99);
        } else {
            fillProgress = std::clamp(fillProgress, 0, 100);
            syncDisplayedProgress = static_cast<double>(fillProgress);
        }
        ui->pushButtonSync->setProgress(fillProgress);
    }

    if (text.empty()) {
        text = "No block source available..";
        ui->pushButtonSync->setButtonClassStyle("cssClass", "btn-check-sync", true);
        ui->pushButtonSync->setProgress(0);
        syncDisplayedProgress = 0.0;
        syncMaxKnownHeader = 0;
    }

    if (ui->pushButtonSync->getText() != tr("Network activity disabled")) {
        ui->pushButtonSync->setButtonClassStyle("cssClass", "btn-check-sync", true);
    }
    ui->pushButtonSync->setButtonText(tr(text.data()));
    const int syncTextWidth = ui->pushButtonSync->fontMetrics().horizontalAdvance(ui->pushButtonSync->getText());
    ui->pushButtonSync->setExpandedWidth(std::clamp(syncTextWidth + 188, 300, 700));
    updateStakingStatus();
}

void TopBar::showUpgradeDialog(const QString& message)
{
    QString title = tr("Wallet Upgrade");
    if (ask(title, message)) {
        std::unique_ptr<WalletModel::UnlockContext> pctx = std::make_unique<WalletModel::UnlockContext>(walletModel->requestUnlock());
        if (!pctx->isValid()) {
            warn(tr("Upgrade Wallet"), tr("Wallet unlock cancelled"));
            return;
        }
        // Action performed on a separate thread, it's locking cs_main and cs_wallet.
        LoadingDialog::Content loadingContent;
        loadingContent.eyebrow = tr("Wallet");
        loadingContent.title = tr("Upgrading wallet");
        loadingContent.supportText = tr("Applying wallet changes securely. This can take a moment.");
        LoadingDialog *dialog = new LoadingDialog(window, loadingContent);
        dialog->execute(this, REQUEST_UPGRADE_WALLET, std::move(pctx));
        openDialogWithOpaqueBackgroundFullScreen(dialog, window);
    }
}

void TopBar::loadWalletModel()
{
    if (!walletModel) {
        return;
    }

    disconnect(ui->pushButtonHDUpgrade, nullptr, nullptr, nullptr);
    refreshWalletSelector();

    // Upgrade wallet.
    if (walletModel->isHDEnabled()) {
        if (walletModel->isSaplingWalletEnabled()) {
            // hide upgrade
            ui->pushButtonHDUpgrade->setVisible(false);
        } else {
            // show upgrade to Sapling
            ui->pushButtonHDUpgrade->setButtonText(tr("Upgrade to Sapling Wallet"));
            ui->pushButtonHDUpgrade->setNoIconText("SHIELD UPGRADE");
            connectUpgradeBtnAndDialogTimer(tr("Upgrading to Sapling wallet will enable\nall of the privacy features!\n\n\n"
                                               "NOTE: after the upgrade, a new\nbackup will be created.\n"));
        }
    } else {
        connectUpgradeBtnAndDialogTimer(tr("Upgrading to HD wallet will improve\nthe wallet's reliability and security.\n\n\n"
                                           "NOTE: after the upgrade, a new\nbackup will be created.\n"));
    }

    connect(walletModel, &WalletModel::balanceChanged, this, &TopBar::updateBalances);
    connect(walletModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &TopBar::updateDisplayUnit);
    connect(walletModel, &WalletModel::encryptionStatusChanged, this, &TopBar::refreshStatus);
    // Ask for passphrase if needed
    connect(walletModel, &WalletModel::requireUnlock, this, &TopBar::unlockWallet);
    // update the display unit, to not use the default ("OrganicLife")
    updateDisplayUnit();

    refreshStatus();
    onColdStakingClicked();

    isInitializing = false;
}

void TopBar::clearWalletModel()
{
    PWidget::clearWalletModel();
    ui->pushButtonWallet->setButtonText(tr("Wallet"));
    ui->pushButtonWallet->setNoIconText(QString());
    ui->pushButtonWallet->setEnabled(window != nullptr);
}

void TopBar::refreshWalletSelector()
{
    const QString walletLabel = walletModel ? walletModel->getDisplayName() : tr("Wallet");
    ui->pushButtonWallet->setButtonText(walletLabel);
    ui->pushButtonWallet->setNoIconText(QString());
    ui->pushButtonWallet->setEnabled(window != nullptr);

    const int textWidth = ui->pushButtonWallet->fontMetrics().horizontalAdvance(walletLabel);
    ui->pushButtonWallet->setExpandedWidth(std::clamp(textWidth + 156, 220, 420));
}

void TopBar::connectUpgradeBtnAndDialogTimer(const QString& message)
{
    const auto& func = [this, message]() { showUpgradeDialog(message); };
    connect(ui->pushButtonHDUpgrade, &ExpandableButton::Mouse_Pressed, func);

    // Upgrade wallet timer, only once. launched 4 seconds after the wallet started.
    QTimer::singleShot(4000, func);
}

void TopBar::updateTorIcon()
{
    std::string ip_port;
    bool torEnabled = clientModel->getTorInfo(ip_port);

    if (torEnabled) {
        if (!ui->pushButtonTor->isChecked()) {
            ui->pushButtonTor->setChecked(true);
            ui->pushButtonTor->setButtonClassStyle("cssClass", "btn-check-tor", true);
        }
        ui->pushButtonTor->setButtonText(tr("Tor Active"));
        ui->pushButtonTor->setToolTip("Address: " + QString::fromStdString(ip_port));
    } else {
        if (ui->pushButtonTor->isChecked()) {
            ui->pushButtonTor->setChecked(false);
            ui->pushButtonTor->setButtonClassStyle("cssClass", "btn-check-tor-inactive", true);
            ui->pushButtonTor->setButtonText(tr("Tor Disabled"));
        }
    }
}

void TopBar::refreshStatus()
{
    // Check lock status
    if (!walletModel || !walletModel->hasWallet())
        return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    switch (encStatus) {
        case WalletModel::EncryptionStatus::Unencrypted:
            ui->pushButtonLock->setButtonText(tr("Wallet Unencrypted"));
            ui->pushButtonLock->setButtonClassStyle("cssClass", "btn-check-status-unlock", true);
            break;
        case WalletModel::EncryptionStatus::Locked:
            ui->pushButtonLock->setButtonText(tr("Wallet Locked"));
            ui->pushButtonLock->setButtonClassStyle("cssClass", "btn-check-status-lock", true);
            break;
        case WalletModel::EncryptionStatus::UnlockedForStaking:
            ui->pushButtonLock->setButtonText(tr("Wallet Unlocked for staking"));
            ui->pushButtonLock->setButtonClassStyle("cssClass", "btn-check-status-staking", true);
            break;
        case WalletModel::EncryptionStatus::Unlocked:
            ui->pushButtonLock->setButtonText(tr("Wallet Unlocked"));
            ui->pushButtonLock->setButtonClassStyle("cssClass", "btn-check-status-unlock", true);
            break;
    }
    updateStyle(ui->pushButtonLock);
    updateStakingStatus();
}

void TopBar::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        int displayUnitPrev = nDisplayUnit;
        nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
        if (displayUnitPrev != nDisplayUnit)
            updateBalances(walletModel->GetWalletBalances());
    }
}

void TopBar::updateBalances(const interfaces::WalletBalances& newBalance)
{
    // Locked balance. //TODO move this to the signal properly in the future..
    CAmount nLockedBalance = 0;
    if (walletModel) {
        nLockedBalance = walletModel->getLockedBalance(true) + walletModel->getLockedBalance(false);
    }
    ui->labelTitle1->setText(nLockedBalance > 0 ? tr("Available (Locked included)") : tr("Available"));

    // PIV Total
    QString totalPiv = GUIUtil::formatBalance(newBalance.balance, nDisplayUnit);
    QString totalTransparent = GUIUtil::formatBalance(newBalance.balance - newBalance.shielded_balance);
    QString totalShielded = GUIUtil::formatBalance(newBalance.shielded_balance);

    // PIV
    // Top
    ui->labelAmountTopPiv->setText(totalTransparent);
    ui->labelAmountTopShieldedPiv->setText(totalShielded);
    // Expanded
    ui->labelAmountPiv->setText(totalPiv);
    ui->labelPendingPiv->setText(GUIUtil::formatBalance(newBalance.unconfirmed_balance + newBalance.unconfirmed_shielded_balance, nDisplayUnit));
    ui->labelImmaturePiv->setText(GUIUtil::formatBalance(newBalance.immature_balance, nDisplayUnit));
}

void TopBar::resizeEvent(QResizeEvent *event)
{
    if (lockUnlockWidget && lockUnlockWidget->isVisible()) lockDropdownMouseLeave();
    QWidget::resizeEvent(event);
}

void TopBar::updateHDState(const bool upgraded, const QString& upgradeError)
{
    if (upgraded) {
        ui->pushButtonHDUpgrade->setVisible(false);
        if (ask("HD Upgrade Complete", tr("The wallet has been successfully upgraded to HD.") + "\n" +
                tr("It is advised to make a backup.") + "\n\n" + tr("Do you wish to backup now?") + "\n\n")) {
            // backup wallet
            QString filename = GUIUtil::getSaveFileName(this,
                                                tr("Backup Wallet"), QString(),
                                                tr("Wallet Data (*.dat)"), nullptr);
            if (!filename.isEmpty()) {
                inform(walletModel->backupWallet(filename) ? tr("Backup created") : tr("Backup creation failed"));
            } else {
                warn(tr("Backup creation failed"), tr("no file selected"));
            }
        } else {
            inform(tr("Wallet upgraded successfully, but no backup created.") + "\n" +
                    tr("WARNING: remember to make a copy of your wallet file!"));
        }
    } else {
        warn(tr("Upgrade Wallet Error"), upgradeError);
    }
}

void TopBar::run(int type)
{
    if (type == REQUEST_UPGRADE_WALLET) {
        std::string upgradeError;
        bool ret = this->walletModel->upgradeWallet(upgradeError);
        QMetaObject::invokeMethod(this,
                "updateHDState",
                Qt::QueuedConnection,
                Q_ARG(bool, ret),
                Q_ARG(QString, QString::fromStdString(upgradeError))
        );
    }
}

void TopBar::onError(QString error, int type)
{
    if (type == REQUEST_UPGRADE_WALLET) {
        warn(tr("Upgrade Wallet Error"), error);
    }
}

void TopBar::refreshButtonLayouts()
{
    // Force all expandable buttons to their proper compact state
    // This fixes any half-expanded states that can occur on startup
    ui->pushButtonTheme->setSmall(false);
    ui->pushButtonSync->setSmall(false);
    ui->pushButtonConnection->setSmall(false);
    ui->pushButtonTor->setSmall(false);
    ui->pushButtonStack->setSmall(false);
    ui->pushButtonColdStaking->setSmall(false);
    ui->pushButtonWallet->setSmall(false);
    ui->pushButtonLock->setSmall(false);
    ui->pushButtonFAQ->setSmall(false);
    ui->pushButtonHDUpgrade->setSmall(false);
    
    // Keep compact width consistent with UI geometry to avoid subtle resize jitter.
    constexpr int compactWidth = 48;
    ui->pushButtonTheme->setMinimumWidth(compactWidth);
    ui->pushButtonTheme->setMaximumWidth(compactWidth);
    ui->pushButtonSync->setMinimumWidth(compactWidth);
    ui->pushButtonSync->setMaximumWidth(compactWidth);
    ui->pushButtonConnection->setMinimumWidth(compactWidth);
    ui->pushButtonConnection->setMaximumWidth(compactWidth);
    ui->pushButtonTor->setMinimumWidth(compactWidth);
    ui->pushButtonTor->setMaximumWidth(compactWidth);
    ui->pushButtonStack->setMinimumWidth(compactWidth);
    ui->pushButtonStack->setMaximumWidth(compactWidth);
    ui->pushButtonColdStaking->setMinimumWidth(compactWidth);
    ui->pushButtonColdStaking->setMaximumWidth(compactWidth);
    ui->pushButtonWallet->setMinimumWidth(compactWidth);
    ui->pushButtonWallet->setMaximumWidth(compactWidth);
    ui->pushButtonLock->setMinimumWidth(compactWidth);
    ui->pushButtonLock->setMaximumWidth(compactWidth);
    ui->pushButtonFAQ->setMinimumWidth(compactWidth);
    ui->pushButtonFAQ->setMaximumWidth(compactWidth);
    ui->pushButtonHDUpgrade->setMinimumWidth(compactWidth);
    ui->pushButtonHDUpgrade->setMaximumWidth(compactWidth);
    
    // Force layout update
    ui->containerTop->layout()->activate();
    updateGeometry();
    update();
}

void TopBar::setupStatusButtonHoverBehavior()
{
    statusButtons = {
        ui->pushButtonSync,
        ui->pushButtonHDUpgrade,
        ui->pushButtonStack,
        ui->pushButtonColdStaking,
        ui->pushButtonConnection,
        ui->pushButtonTor,
        ui->pushButtonWallet,
        ui->pushButtonLock,
        ui->pushButtonTheme,
        ui->pushButtonFAQ
    };

    for (ExpandableButton* button : statusButtons) {
        if (!button) continue;
        const int textWidth = button->fontMetrics().horizontalAdvance(button->getText());
        const int expandedWidth = std::clamp(textWidth + 156, 260, 620);
        button->setExpandedWidth(expandedWidth);
        button->setHoverExpandEnabled(!compactTopMode);
        connect(button, &ExpandableButton::Mouse_Hover, this, &TopBar::onStatusButtonHover);
        connect(button, &ExpandableButton::Mouse_HoverLeave, this, &TopBar::onStatusButtonHoverLeave);
    }
}

void TopBar::onStatusButtonHover()
{
    if (!compactTopMode) return;

    if (hoverLeaveDebounceTimer) hoverLeaveDebounceTimer->stop();

    ExpandableButton* hovered = qobject_cast<ExpandableButton*>(sender());
    if (!hovered) return;

    setTopBarHoverActive(true);
    hovered->raise();
    showStatusHoverPill(hovered);
    updateStatusButtonFocus(hovered);
}

void TopBar::onStatusButtonHoverLeave()
{
    if (!compactTopMode || !hoverLeaveDebounceTimer) return;

    // Debounce leave events so moving between status buttons doesn't flicker.
    hoverLeaveDebounceTimer->start();
}

void TopBar::clearTopBarHoverState()
{
    if (!compactTopMode) {
        setTopBarHoverActive(false);
        hideStatusHoverPill();
        updateStatusButtonFocus(nullptr);
        return;
    }

    const QPoint cursorPos = QCursor::pos();
    for (ExpandableButton* button : statusButtons) {
        if (button && button->containsGlobalPos(cursorPos)) {
            return;
        }
    }

    setTopBarHoverActive(false);
    hideStatusHoverPill();
    updateStatusButtonFocus(nullptr);
}

void TopBar::setTopBarHoverActive(bool active)
{
    Q_UNUSED(active);
}

void TopBar::updateStatusButtonFocus(ExpandableButton* hovered)
{
    for (ExpandableButton* button : statusButtons) {
        applyStatusButtonFocus(button, button == hovered);
    }
}

void TopBar::applyStatusButtonFocus(ExpandableButton* button, bool focused)
{
    if (!button) return;

    if (compactTopMode) {
        button->setKeepExpanded(false);
        if (button->width() > 50) {
            button->setSmall(false);
        }
        return;
    }

    if (focused) {
        button->setKeepExpanded(true);
        button->setExpanded(true);
        button->raise();
        return;
    }

    // Keep lock control expanded while its menu is open.
    if (button == ui->pushButtonLock && lockUnlockWidget && lockUnlockWidget->isVisible()) {
        return;
    }

    button->setKeepExpanded(false);
    if (button->width() > 50) {
        button->setSmall(true);
    }
}

void TopBar::setupStatusHoverPill()
{
    statusHoverPill = new QWidget(this);
    statusHoverPill->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    statusHoverPill->setObjectName("statusHoverPill");
    statusHoverPill->hide();

    auto* layout = new QHBoxLayout(statusHoverPill);
    layout->setContentsMargins(14, 7, 14, 7);
    layout->setSpacing(0);

    statusHoverPillText = new QLabel(statusHoverPill);
    statusHoverPillText->setAlignment(Qt::AlignCenter);
    statusHoverPillText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(statusHoverPillText, 1, Qt::AlignCenter);

    statusHoverPillOpacity = new QGraphicsOpacityEffect(statusHoverPill);
    statusHoverPillOpacity->setOpacity(0.0);
    statusHoverPill->setGraphicsEffect(statusHoverPillOpacity);

    statusHoverPillFadeAnimation = new QPropertyAnimation(statusHoverPillOpacity, "opacity", this);
    statusHoverPillFadeAnimation->setDuration(110);
    statusHoverPillFadeAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(statusHoverPillFadeAnimation, &QPropertyAnimation::finished, this, [this]() {
        if (statusHoverPill && statusHoverPillOpacity && statusHoverPillOpacity->opacity() <= 0.01) {
            statusHoverPill->hide();
        }
    });

    statusHoverPillGeometryAnimation = new QPropertyAnimation(statusHoverPill, "geometry", this);
    statusHoverPillGeometryAnimation->setDuration(120);
    statusHoverPillGeometryAnimation->setEasingCurve(QEasingCurve::OutCubic);
}

void TopBar::showStatusHoverPill(ExpandableButton* button)
{
    if (!compactTopMode || !button || !statusHoverPill || !statusHoverPillText) return;

    const QString text = button->getText().trimmed();
    if (text.isEmpty()) {
        hideStatusHoverPill();
        return;
    }

    statusHoverPillText->setText(text);
    statusHoverPillText->setStyleSheet(isLightTheme() ? "color:#3A2418; font-weight:500;" : "color:#F0E3D4; font-weight:500;");
    statusHoverPill->setStyleSheet(isLightTheme()
        ? "#statusHoverPill { background-color: rgba(255,255,255,236); border: 1px solid #D7C2AE; border-radius: 10px; }"
        : "#statusHoverPill { background-color: rgba(19,11,8,236); border: 1px solid #6D4F3B; border-radius: 10px; }");

    const QRect endRect = computeStatusHoverPillGeometry(button);
    if (!endRect.isValid()) return;

    const bool wasVisible = statusHoverPill->isVisible();
    QRect startRect = endRect;
    if (!wasVisible) {
        startRect.setWidth(std::max(100, endRect.width() / 2));
        startRect.moveCenter(endRect.center());
        startRect.translate(0, 4);
        statusHoverPill->setGeometry(startRect);
        statusHoverPill->show();
        statusHoverPill->raise();
        statusHoverPillOpacity->setOpacity(0.0);
    }

    if (statusHoverPillFadeAnimation) {
        statusHoverPillFadeAnimation->stop();
        statusHoverPillFadeAnimation->setDuration(110);
        statusHoverPillFadeAnimation->setEasingCurve(QEasingCurve::OutCubic);
        statusHoverPillFadeAnimation->setStartValue(statusHoverPillOpacity->opacity());
        statusHoverPillFadeAnimation->setEndValue(1.0);
        statusHoverPillFadeAnimation->start();
    }

    if (statusHoverPillGeometryAnimation) {
        statusHoverPillGeometryAnimation->stop();
        statusHoverPillGeometryAnimation->setDuration(wasVisible ? 95 : 120);
        statusHoverPillGeometryAnimation->setEasingCurve(QEasingCurve::OutCubic);
        statusHoverPillGeometryAnimation->setStartValue(statusHoverPill->geometry());
        statusHoverPillGeometryAnimation->setEndValue(endRect);
        statusHoverPillGeometryAnimation->start();
    } else {
        statusHoverPill->setGeometry(endRect);
    }
}

void TopBar::hideStatusHoverPill()
{
    if (!statusHoverPill || !statusHoverPill->isVisible()) return;

    if (statusHoverPillGeometryAnimation) {
        statusHoverPillGeometryAnimation->stop();
    }

    if (!statusHoverPillFadeAnimation || !statusHoverPillOpacity) {
        statusHoverPill->hide();
        return;
    }

    statusHoverPillFadeAnimation->stop();
    statusHoverPillFadeAnimation->setStartValue(statusHoverPillOpacity->opacity());
    statusHoverPillFadeAnimation->setEndValue(0.0);
    statusHoverPillFadeAnimation->setDuration(95);
    statusHoverPillFadeAnimation->setEasingCurve(QEasingCurve::InCubic);
    statusHoverPillFadeAnimation->start();
}

QRect TopBar::computeStatusHoverPillGeometry(ExpandableButton* button) const
{
    if (!button || !statusHoverPillText || !ui || !ui->containerTop) return QRect();

    const int textWidth = statusHoverPillText->fontMetrics().horizontalAdvance(statusHoverPillText->text());
    const int width = std::clamp(textWidth + 42, 150, 440);
    const int height = 40;

    const QPoint buttonPos = button->mapTo(const_cast<TopBar*>(this), QPoint(0, 0));
    int x = buttonPos.x() + button->width() - width;
    const int minX = ui->containerTop->x() + 12;
    const int maxX = ui->containerTop->x() + ui->containerTop->width() - width - 12;
    x = std::clamp(x, minX, std::max(minX, maxX));
    const int y = ui->containerTop->y() + 8;
    return QRect(x, y, width, height);
}
