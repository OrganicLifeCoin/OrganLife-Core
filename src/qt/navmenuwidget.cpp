// Copyright (c) 2019-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "navmenuwidget.h"
#include "ui_navmenuwidget.h"

#include "clientversion.h"
#include "optionsmodel.h"
#include "qtutils.h"

#include <QScrollBar>
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QIcon>
#include <QToolButton>
#include <algorithm>

NavMenuWidget::NavMenuWidget(OrganicLifeGUI *mainWindow, QWidget *parent) :
    PWidget(mainWindow, parent),
    ui(new Ui::NavMenuWidget)
{
    ui->setupUi(this);
    this->setFixedWidth(162);
    this->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(ui->navContainer_2, "container-nav");
    // The supplied logo now lives in OrganicLifeGUI's separate brand island.
    // Remove the legacy in-menu copy completely so the rail owns navigation only.
    for (int i = 0; i < ui->navContainer->count(); ++i) {
        if (ui->navContainer->itemAt(i)->widget() == ui->imgLogo) {
            ui->navContainer->takeAt(i);
            break;
        }
    }
    ui->imgLogo->hide();

    // App version (hidden on the slim rail; visible in About)
    ui->labelVersion->setText(QString(tr("v%1")).arg(QString::fromStdString(FormatVersionFriendly())));
    ui->labelVersion->setProperty("cssClass", "text-title-white");
    ui->labelVersion->setVisible(false);

    auto* utilityPanel = new QFrame(this);
    utilityPanel->setObjectName("navUtilityPanel");
    utilityPanel->setAttribute(Qt::WA_StyledBackground, true);
    setCssProperty(utilityPanel, "nav-utility-panel");
    auto* utilityLayout = new QVBoxLayout(utilityPanel);
    utilityLayout->setContentsMargins(6, 6, 6, 6);
    utilityLayout->setSpacing(2);
    const auto setupUtilityButton = [](QToolButton* button, const QString& text, const QIcon& icon) {
        button->setText(text);
        button->setIcon(icon);
        button->setIconSize(QSize(18, 18));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setFixedHeight(34);
        button->setCursor(Qt::PointingHandCursor);
        button->setAutoRaise(true);
        setCssProperty(button, "nav-utility-button");
    };
    walletSelectorButton = new QToolButton(utilityPanel);
    walletSelectorButton->setObjectName("navWalletSelector");
    setupUtilityButton(walletSelectorButton, tr("Wallet"),
                       QIcon(isLightTheme() ? ":/ic-check-wallet" : ":/ic-check-wallet-dark"));
    themeButton = new QToolButton(utilityPanel);
    themeButton->setCheckable(true);
    setupUtilityButton(themeButton, tr("Dark mode"), QIcon(":/ic-check-theme-dark"));
    setThemeState(isLightTheme());
    lockButton = new QToolButton(utilityPanel);
    setupUtilityButton(lockButton, tr("Lock wallet"),
                       QIcon(isLightTheme() ? ":/ic-check-locked-off" : ":/ic-check-locked"));
    utilityLayout->addWidget(walletSelectorButton);
    utilityLayout->addWidget(themeButton);
    utilityLayout->addWidget(lockButton);
    const int utilityInsertIndex = std::max(0, ui->navContainer->count() - 2);
    ui->navContainer->insertWidget(utilityInsertIndex, utilityPanel);
    connect(themeButton, &QToolButton::clicked, this, &NavMenuWidget::themeToggleRequested);
    connect(lockButton, &QToolButton::clicked, this, &NavMenuWidget::walletLockRequested);
    connect(walletSelectorButton, &QToolButton::clicked, this, &NavMenuWidget::walletSelectorRequested);

    // Buttons
    auto setupNavButton = [](QToolButton* btn, const QString& text) {
        btn->setText(text);
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setIconSize(QSize(22, 22));
        btn->setFixedSize(156, 44);
        btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        btn->setAutoRaise(true);
    };
    ui->btnDashboard->setProperty("name", "dash");
    setupNavButton(ui->btnDashboard, tr("Dashboard"));
    ui->btnSend->setProperty("name", "send");
    setupNavButton(ui->btnSend, tr("Send"));
    ui->btnReceive->setProperty("name", "receive");
    setupNavButton(ui->btnReceive, tr("Receive"));
    transactionsButton = new QToolButton(ui->scrollAreaNavVert);
    transactionsButton->setObjectName("btnTransactions");
    transactionsButton->setProperty("name", "transactions");
    transactionsButton->setCheckable(true);
    transactionsButton->setAutoExclusive(true);
    setupNavButton(transactionsButton, tr("Transactions"));
    ui->btnAddress->setProperty("name", "address");
    setupNavButton(ui->btnAddress, tr("Address Book"));
    ui->btnMaster->setProperty("name", "master");
    setupNavButton(ui->btnMaster, tr("Masternodes"));
    ui->btnColdStaking->setProperty("name", "cold-staking");
    setupNavButton(ui->btnColdStaking, tr("Cold Staking"));
    ui->btnSettings->setProperty("name", "settings");
    setupNavButton(ui->btnSettings, tr("Settings"));
    ui->btnGovernance->setProperty("name", "governance");
    setupNavButton(ui->btnGovernance, tr("Governance"));
    btns = {ui->btnDashboard, ui->btnSend, ui->btnReceive, transactionsButton, ui->btnAddress,
            ui->btnMaster, ui->btnColdStaking, ui->btnGovernance, ui->btnSettings};

    // Match the approved information architecture while retaining Cold Staking
    // when that wallet capability is enabled.
    for (QWidget* button : btns) ui->verticalLayout_3->removeWidget(button);
    for (int i = 0; i < btns.size(); ++i) {
        ui->verticalLayout_3->insertWidget(i, btns.at(i), 0, Qt::AlignHCenter);
    }
    onNavSelected(ui->btnDashboard, true);

    // Keep the labelled rail compact and aligned to the visual contract.
    ui->verticalLayout_3->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    ui->verticalLayout_3->setSpacing(4);

    ui->scrollAreaNav->setWidgetResizable(true);

    QSizePolicy scrollAreaPolicy = ui->scrollAreaNav->sizePolicy();
    scrollAreaPolicy.setVerticalStretch(1);
    ui->scrollAreaNav->setSizePolicy(scrollAreaPolicy);

    QSizePolicy scrollVertPolicy = ui->scrollAreaNavVert->sizePolicy();
    scrollVertPolicy.setVerticalStretch(1);
    ui->scrollAreaNavVert->setSizePolicy(scrollVertPolicy);

    connectActions();
}

void NavMenuWidget::loadWalletModel() {
    if (walletModel && walletModel->getOptionsModel()) {
        ui->btnColdStaking->setVisible(walletModel->getOptionsModel()->isColdStakingScreenEnabled());
    }
}

/**
 * Actions
 */
void NavMenuWidget::connectActions() {
    connect(ui->btnDashboard, &QPushButton::clicked, this, &NavMenuWidget::onDashboardClicked);
    connect(ui->btnSend, &QPushButton::clicked, this, &NavMenuWidget::onSendClicked);
    connect(ui->btnAddress, &QPushButton::clicked, this, &NavMenuWidget::onAddressClicked);
    connect(ui->btnMaster, &QPushButton::clicked, this, &NavMenuWidget::onMasterNodesClicked);
    connect(ui->btnSettings, &QPushButton::clicked, this, &NavMenuWidget::onSettingsClicked);
    connect(ui->btnReceive, &QPushButton::clicked, this, &NavMenuWidget::onReceiveClicked);
    connect(transactionsButton, &QToolButton::clicked, this, &NavMenuWidget::onTransactionsClicked);
    connect(ui->btnColdStaking, &QPushButton::clicked, this, &NavMenuWidget::onColdStakingClicked);
    connect(ui->btnGovernance, &QPushButton::clicked, this, &NavMenuWidget::onGovClicked);

    ui->btnDashboard->setShortcut(QKeySequence(SHORT_KEY | Qt::Key_1));
    ui->btnSend->setShortcut(QKeySequence(SHORT_KEY | Qt::Key_2));
    ui->btnReceive->setShortcut(QKeySequence(SHORT_KEY | Qt::Key_3));
    ui->btnAddress->setShortcut(QKeySequence(SHORT_KEY | Qt::Key_4));
    ui->btnMaster->setShortcut(QKeySequence(SHORT_KEY | Qt::Key_5));
    ui->btnColdStaking->setShortcut(QKeySequence(SHORT_KEY | Qt::Key_6));
    ui->btnSettings->setShortcut(QKeySequence(SHORT_KEY | Qt::Key_7));
}

void NavMenuWidget::onSendClicked(){
    window->goToSend();
    onNavSelected(ui->btnSend);
}

void NavMenuWidget::onDashboardClicked(){
    window->goToDashboard();
    onNavSelected(ui->btnDashboard);
}

void NavMenuWidget::onAddressClicked(){
    window->goToAddresses();
    onNavSelected(ui->btnAddress);
}

void NavMenuWidget::onMasterNodesClicked(){
    window->goToMasterNodes();
    onNavSelected(ui->btnMaster);
}

void NavMenuWidget::onColdStakingClicked() {
    window->goToColdStaking();
    onNavSelected(ui->btnColdStaking);
}

void NavMenuWidget::onGovClicked()
{
    window->goToGovernance();
    onNavSelected(ui->btnGovernance);
}

void NavMenuWidget::onSettingsClicked(){
    window->goToSettings();
    onNavSelected(ui->btnSettings);
}

void NavMenuWidget::onReceiveClicked(){
    window->goToReceive();
    onNavSelected(ui->btnReceive);
}

void NavMenuWidget::onTransactionsClicked()
{
    window->goToTransactions();
    onNavSelected(transactionsButton);
}

void NavMenuWidget::onNavSelected(QWidget* active, bool startup) {
    QString start = "btn-nav-";
    for (QWidget* w : btns) {
        QString clazz = start + w->property("name").toString();
        if (w == active) {
            clazz += "-active";
        }
        setCssProperty(w, clazz);

        auto* button = qobject_cast<QToolButton*>(w);
        if (!button) continue;
        const QString name = w->property("name").toString();
        QString iconName = name;
        if (name == QStringLiteral("dash")) iconName = QStringLiteral("dashboard");
        if (name == QStringLiteral("cold-staking")) iconName = QStringLiteral("cold-staking");
        const QString iconPath = name == QStringLiteral("transactions")
            ? QString(w == active ? ":/ic-transaction-staked" : ":/ic-transaction-staked-inactive")
            : QString(":/ic-rail-%1%2").arg(iconName, w == active ? QStringLiteral("-active") : QString());
        button->setIcon(QIcon(iconPath));
    }
    if (!startup) updateButtonStyles();
}

void NavMenuWidget::selectSettings() {
    onSettingsClicked();
}

void NavMenuWidget::setThemeState(bool isLight)
{
    if (!themeButton) return;
    themeButton->setChecked(!isLight);
    themeButton->setIcon(QIcon(isLight ? ":/ic-check-theme-light-dark" : ":/ic-check-theme-dark"));
    if (lockButton) lockButton->setIcon(QIcon(isLight ? ":/ic-check-locked-off" : ":/ic-check-locked"));
    if (walletSelectorButton) {
        walletSelectorButton->setIcon(QIcon(isLight ? ":/ic-check-wallet" : ":/ic-check-wallet-dark"));
    }
}

void NavMenuWidget::setWalletName(const QString& walletName)
{
    if (!walletSelectorButton) return;
    walletSelectorButton->setText(walletName.isEmpty() ? tr("Wallet") : walletName);
    walletSelectorButton->setToolTip(tr("Switch wallet"));
}

void NavMenuWidget::onShowHideColdStakingChanged(bool show) {
    ui->btnColdStaking->setVisible(show);
    if (show)
        ui->scrollAreaNav->verticalScrollBar()->setValue(ui->btnColdStaking->y());
}

void NavMenuWidget::showEvent(QShowEvent *event) {
    if (!init) {
        init = true;
        ui->scrollAreaNav->verticalScrollBar()->setValue(ui->btnDashboard->y());
    }
}

void NavMenuWidget::updateButtonStyles(){
    forceUpdateStyle({
         ui->btnDashboard,
         ui->btnSend,
         ui->btnAddress,
         ui->btnMaster,
         ui->btnSettings,
         ui->btnReceive,
         transactionsButton,
         ui->btnColdStaking,
         ui->btnGovernance
    });
}

NavMenuWidget::~NavMenuWidget(){
    delete ui;
}
