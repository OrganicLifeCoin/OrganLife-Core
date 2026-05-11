// Copyright (c) 2019-2022 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "navmenuwidget.h"
#include "ui_navmenuwidget.h"

#include "clientversion.h"
#include "optionsmodel.h"
#include "qtutils.h"

#include <QScrollBar>
#include <QIcon>
#include <QToolButton>

NavMenuWidget::NavMenuWidget(PIVXGUI *mainWindow, QWidget *parent) :
    PWidget(mainWindow, parent),
    ui(new Ui::NavMenuWidget)
{
    ui->setupUi(this);
    this->setFixedWidth(160);
    setCssProperty(ui->navContainer_2, "container-nav");
    setCssProperty(ui->imgLogo, "img-nav-logo");
    // Ensure the nav logo actually renders at the desired size. Relying on
    // qproperty-icon via stylesheets can be flaky across Qt versions.
    constexpr int kLogoPx = 128;
    ui->imgLogo->setIcon(QIcon(":/img-logo-pivx"));
    ui->imgLogo->setIconSize(QSize(kLogoPx, kLogoPx));
    ui->imgLogo->setFixedHeight(kLogoPx + 20);
    ui->imgLogo->setFlat(true);

    // App version
    ui->labelVersion->setText(QString(tr("v%1")).arg(QString::fromStdString(FormatVersionFriendly())));
    ui->labelVersion->setProperty("cssClass", "text-title-white");

    // Buttons
    auto setupNavButton = [](QToolButton* btn) {
        btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setAutoRaise(true);
    };
    ui->btnDashboard->setProperty("name", "dash");
    setupNavButton(ui->btnDashboard);
    ui->btnSend->setProperty("name", "send");
    setupNavButton(ui->btnSend);
    ui->btnReceive->setProperty("name", "receive");
    setupNavButton(ui->btnReceive);
    ui->btnAddress->setProperty("name", "address");
    setupNavButton(ui->btnAddress);
    ui->btnMaster->setProperty("name", "master");
    setupNavButton(ui->btnMaster);
    ui->btnColdStaking->setProperty("name", "cold-staking");
    setupNavButton(ui->btnColdStaking);
    ui->btnSettings->setProperty("name", "settings");
    setupNavButton(ui->btnSettings);
    ui->btnGovernance->setProperty("name", "governance");
    setupNavButton(ui->btnGovernance);
    btns = {ui->btnDashboard, ui->btnSend, ui->btnReceive, ui->btnAddress, ui->btnMaster, ui->btnColdStaking, ui->btnSettings, ui->btnGovernance};
    onNavSelected(ui->btnDashboard, true);

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

void NavMenuWidget::onNavSelected(QWidget* active, bool startup) {
    QString start = "btn-nav-";
    for (QWidget* w : btns) {
        QString clazz = start + w->property("name").toString();
        if (w == active) {
            clazz += "-active";
        }
        setCssProperty(w, clazz);
    }
    if (!startup) updateButtonStyles();
}

void NavMenuWidget::selectSettings() {
    onSettingsClicked();
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
         ui->btnColdStaking,
         ui->btnGovernance
    });
}

NavMenuWidget::~NavMenuWidget(){
    delete ui;
}
