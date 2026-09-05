// Copyright (c) 2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_VOTEDIALOG_H
#define PIVX_QT_VOTEDIALOG_H

#include "containerdialog.h"
#include <QCheckBox>
#include <QProgressBar>
#include <QPoint>

#include "governancemodel.h"
#include <memory>

namespace Ui {
class VoteDialog;
}

struct ProposalInfo;
class MNModel;
class GovernanceModel;
class SnackBar;
class WalletModel;

class VoteDialog : public ContainerDialog
{
    Q_OBJECT

public:
    explicit VoteDialog(QWidget *parent, GovernanceModel* _govModel, MNModel* _mnModel, WalletModel* _walletModel = nullptr);
    ~VoteDialog();

    void showEvent(QShowEvent *event) override;
    void setProposal(const ProposalInfo& prop);

public Q_SLOTS:
    void onAcceptClicked();
    void onCheckBoxClicked(QCheckBox* checkBox, QProgressBar* progressBar, bool isVoteYes);
    void onVoteModeChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Ui::VoteDialog *ui;
    GovernanceModel* govModel{nullptr};
    WalletModel* walletModel{nullptr};
    SnackBar* snackBar{nullptr};

    QCheckBox* checkBoxNo{nullptr};
    QCheckBox* checkBoxYes{nullptr};
    QProgressBar* progressBarNo{nullptr};
    QProgressBar* progressBarYes{nullptr};

    std::unique_ptr<ProposalInfo> proposal;

    void initVoteCheck(QWidget* container, QCheckBox* checkBox, QProgressBar* progressBar,
                        const QString& text, Qt::LayoutDirection direction, bool isVoteYes);

    bool isCoinVoteMode() const;
    CAmount parseCoinLockAmount(bool& ok) const;
    CAmount getProposalCap() const;
    QString getCoinAmountLockedReason() const;
    OperationResult validateCoinLockAmount(CAmount* amountOut = nullptr) const;
    void updateCoinAmountValidationState();
    void applyCoinAmountInvalidState(bool invalid);
    uint32_t autoUnlockHeight() const;
    CAmount getCoinLockableBalance() const;
    void applyCoinAmountPreset(int basisPoints);
    void updateVoteModeUi();
    void updateCoinModeInfo();
    void updateCoinVoteStatusText();
    void inform(const QString& text);
    void setupStickyHeader();
    bool tryStartSystemMove();
    void applyAdaptiveLayoutForScreen();
    void applyScaleFactor(double scaleFactor);
    int scaledMetric(int baseValue, double scaleFactor, int minValue) const;

    bool headerDragging{false};
    QWidget* headerBar{nullptr};
    QWidget* shellContainer{nullptr};
    QPoint dragStartGlobal;
    QPoint dragStartDialogPos;
    double baseFontPointSize{0.0};
    int baseDialogWidth{680};
    int baseDialogHeight{760};
};

#endif // PIVX_QT_VOTEDIALOG_H
