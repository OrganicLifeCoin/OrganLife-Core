// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_TOPBAR_H
#define PIVX_QT_TOPBAR_H

#include "amount.h"
#include "lockunlock.h"
#include "pwidget.h"
#include "qt/askpassphrasedialog.h"

#include <QList>
#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>

class BalanceBubble;
class OrganicLifeGUI;
class WalletModel;
class ClientModel;
class ExpandableButton;
class QLabel;
class QGraphicsOpacityEffect;

namespace Ui {
class TopBar;
}

class TopBar : public PWidget
{
    Q_OBJECT

public:
    explicit TopBar(OrganicLifeGUI* _mainWindow, QWidget *parent = nullptr);
    ~TopBar();

    void showTop();
    void showBottom();

    void loadWalletModel() override;
    void clearWalletModel() override;
    void loadClientModel() override;

    void openPassPhraseDialog(AskPassphraseDialog::Mode mode, AskPassphraseDialog::Context ctx);
    void encryptWallet();

    void run(int type) override;
    void onError(QString error, int type) override;
    void unlockWallet();

public Q_SLOTS:
    void updateBalances(const interfaces::WalletBalances& newBalance);
    void updateDisplayUnit();

    void setNumConnections(int count);
    void setNumBlocks(int count);
    void setSyncProgress(const QString& title, int nProgress);
    void setNetworkActive(bool active);
    void setStakingStatusActive(bool fActive);
    void updateStakingStatus();
    void updateHDState(const bool upgraded, const QString& upgradeError);
    void showUpgradeDialog(const QString& message);

Q_SIGNALS:
    void themeChanged(bool isLight);
    void walletSynced(bool isSync);
    void tierTwoSynced(bool isSync);
    void onShowHideColdStakingChanged(bool show);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeTheme(bool isLightTheme, QString& theme) override;
private Q_SLOTS:
    void onBtnReceiveClicked();
    void onBtnBalanceInfoClicked();
    void onThemeClicked();
    void onBtnLockClicked();
    void onWalletButtonClicked();
    void lockDropdownMouseLeave();
    void lockDropdownClicked(const StateClicked&);
    void refreshStatus();
    void openLockUnlock();
    void onColdStakingClicked();
    void refreshButtonLayouts();
    void onStatusButtonHover();
    void onStatusButtonHoverLeave();
    void clearTopBarHoverState();
private:
    Ui::TopBar *ui;
    LockUnlock *lockUnlockWidget = nullptr;
    QWidget* statusHoverPill = nullptr;
    QLabel* statusHoverPillText = nullptr;
    QGraphicsOpacityEffect* statusHoverPillOpacity = nullptr;
    QPropertyAnimation* statusHoverPillFadeAnimation = nullptr;
    QPropertyAnimation* statusHoverPillGeometryAnimation = nullptr;
    QTimer* hoverLeaveDebounceTimer = nullptr;
    QList<ExpandableButton*> statusButtons;
    bool compactTopMode = false;
    int syncMaxKnownHeader = 0;
    double syncDisplayedProgress = -1.0;
    bool progressOverrideActive = false;
    QString progressOverrideTitle;
    int progressOverrideValue = 0;

    int nDisplayUnit = -1;
    QTimer* timerStakingIcon = nullptr;
    bool isInitializing = true;

    // info popup
    BalanceBubble* balanceBubble = nullptr;

    void updateTorIcon();
    void refreshWalletSelector();
    void performWalletSwitch(const QString& walletName);
    void openManageWalletsDialog();
    void connectUpgradeBtnAndDialogTimer(const QString& message);
    void setupStatusButtonHoverBehavior();
    void setupStatusHoverPill();
    void setTopBarHoverActive(bool active);
    void updateStatusButtonFocus(ExpandableButton* hovered);
    void applyStatusButtonFocus(ExpandableButton* button, bool focused);
    void showStatusHoverPill(ExpandableButton* button);
    void hideStatusHoverPill();
    QRect computeStatusHoverPillGeometry(ExpandableButton* button) const;
    void updateThemeButtonState(bool isLightTheme, bool refreshStyle);
    void updateQrButtonIcon(bool isLightTheme);
};

#endif // PIVX_QT_TOPBAR_H
