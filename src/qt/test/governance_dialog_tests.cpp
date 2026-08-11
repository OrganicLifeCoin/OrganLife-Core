// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "governance_dialog_tests.h"

#include "askpassphrasedialog.h"
#include "addresseswidget.h"
#include "bitcoinunits.h"
#include "budget/budgetproposal.h"
#include "clientmodel.h"
#include "coldstakingwidget.h"
#include "createproposaldialog.h"
#include "dashboardwidget.h"
#include "expandablebutton.h"
#include "governancemodel.h"
#include "governancewidget.h"
#include "guiutil.h"
#include "loadingdialog.h"
#include "masternodeswidget.h"
#include "mnmodel.h"
#include "networkstyle.h"
#include "optionbutton.h"
#include "organiclifegui.h"
#include "proposalcard.h"
#include "proposalinfodialog.h"
#include "receivedialog.h"
#include "receivewidget.h"
#include "send.h"
#include "sendconfirmdialog.h"
#include "topbar.h"
#include "transactionrecord.h"
#include "txrow.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "streams.h"
#include "qtutils.h"
#include "utilstrencodings.h"
#include "votedialog.h"

#include <QCheckBox>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyle>
#include <QtTest/qtest_widgets.h>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <cstdlib>
#include <memory>
#include <set>
#include <vector>

namespace {
class FakeGovernanceModel : public GovernanceModel
{
public:
    explicit FakeGovernanceModel(MNModel* mnModel) : GovernanceModel(nullptr, mnModel) {}

    OperationResult createVoteLockAndCast(const ProposalInfo& prop,
                                          bool isVotePositive,
                                          CAmount lockAmount,
                                          uint32_t unlockHeight) override
    {
        (void)prop;
        calledCoinVote = true;
        lastVoteDirectionYes = isVotePositive;
        lastLockAmount = lockAmount;
        lastUnlockHeight = unlockHeight;
        return {true};
    }

    CAmount getCoinLockableBalance() const override
    {
        return lockableBalance;
    }

    CAmount lockableBalance{8 * COIN};
    bool calledCoinVote{false};
    bool lastVoteDirectionYes{false};
    CAmount lastLockAmount{0};
    uint32_t lastUnlockHeight{0};
};

static ProposalInfo BuildTestProposal(int remainingPayments = 1)
{
    return ProposalInfo(uint256S("01"),
                        "qt-test-proposal",
                        "https://example.com",
                        0,
                        0,
                        "",
                        10 * COIN,
                        remainingPayments,
                        remainingPayments,
                        ProposalInfo::PASSING,
                        100,
                        200);
}

static void AssertRoundedWindowChrome(QWidget& widget, bool expectFrameless)
{
    QVERIFY(widget.testAttribute(Qt::WA_TranslucentBackground));
    if (expectFrameless) {
        QVERIFY(widget.windowFlags().testFlag(Qt::FramelessWindowHint));
    } else {
        QVERIFY(!widget.windowFlags().testFlag(Qt::FramelessWindowHint));
    }
    QVERIFY(widget.mask().isEmpty());
}

QString resolveQtSourceFile(const QString& fileName)
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QDir cwd(QDir::currentPath());
    const QStringList candidates = {
        appDir.absoluteFilePath("../../../../src/qt/" + fileName),
        appDir.absoluteFilePath("../../../src/qt/" + fileName),
        appDir.absoluteFilePath("../../src/qt/" + fileName),
        appDir.absoluteFilePath("../src/qt/" + fileName),
        appDir.absoluteFilePath("src/qt/" + fileName),
        cwd.absoluteFilePath("src/qt/" + fileName),
        cwd.absoluteFilePath("qt/" + fileName),
        cwd.absoluteFilePath("../src/qt/" + fileName),
    };

    for (const QString& candidate : candidates) {
        const QFileInfo fi(candidate);
        if (fi.exists() && fi.isFile()) return fi.absoluteFilePath();
    }
    return QString();
}
} // namespace

void GovernanceDialogTests::coinModeRequiresAmountDirectionAndAutoUnlock()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    QVERIFY(coinModeRadio != nullptr);
    coinModeRadio->setChecked(true);

    auto* amountEdit = dialog.findChild<QLineEdit*>("lineEditCoinAmount");
    QVERIFY(amountEdit != nullptr);

    auto* yesCheck = dialog.findChild<QCheckBox*>("checkVoteYes");
    QVERIFY(yesCheck != nullptr);

    // Missing direction should reject.
    amountEdit->setText("5");
    dialog.onAcceptClicked();
    QVERIFY(!model.calledCoinVote);

    // Missing amount should reject.
    yesCheck->setChecked(true);
    amountEdit->setText("");
    dialog.onAcceptClicked();
    QVERIFY(!model.calledCoinVote);

    // Complete input should call coin vote path.
    amountEdit->setText("5");
    dialog.onAcceptClicked();
    QVERIFY(model.calledCoinVote);
    QVERIFY(model.lastVoteDirectionYes);
    QCOMPARE(model.lastLockAmount, CAmount(5 * COIN));
    QCOMPARE(model.lastUnlockHeight, uint32_t(200));
}

void GovernanceDialogTests::coinModeUiHasQuickPickButtonsAndNoManualUnlock()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    QVERIFY(dialog.findChild<QSpinBox*>("spinCoinUnlockHeight") == nullptr);
    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    QVERIFY(coinModeRadio != nullptr);
    coinModeRadio->setChecked(true);

    auto* btnQuarter = dialog.findChild<QPushButton*>("btnCoinAmount25");
    auto* btnHalf = dialog.findChild<QPushButton*>("btnCoinAmount50");
    auto* btnThreeQuarters = dialog.findChild<QPushButton*>("btnCoinAmount75");
    auto* btnMax = dialog.findChild<QPushButton*>("btnCoinAmountMax");
    auto* amountEdit = dialog.findChild<QLineEdit*>("lineEditCoinAmount");
    QVERIFY(btnQuarter != nullptr);
    QVERIFY(btnHalf != nullptr);
    QVERIFY(btnThreeQuarters != nullptr);
    QVERIFY(btnMax != nullptr);
    QVERIFY(amountEdit != nullptr);
    if (!btnQuarter || !btnHalf || !btnThreeQuarters || !btnMax || !amountEdit) {
        QFAIL("Missing coin quick-pick controls");
        return;
    }

    btnMax->click();
    QCOMPARE(GUIUtil::parseValue(amountEdit->text(), BitcoinUnits::PIV), CAmount(8 * COIN));

    auto* lockSafetyLabel = dialog.findChild<QLabel*>("labelCoinLockSafety");
    QVERIFY(lockSafetyLabel != nullptr);
    if (!lockSafetyLabel) {
        QFAIL("Missing lock safety label");
        return;
    }
    QVERIFY(lockSafetyLabel->wordWrap());

    dialog.show();
    QTest::qWait(50);
    QCoreApplication::processEvents();
    if (!btnQuarter || !amountEdit) {
        QFAIL("Missing quick-pick/button geometry controls");
        return;
    }
    QVERIFY(btnQuarter->geometry().top() >= amountEdit->geometry().bottom());
}

void GovernanceDialogTests::coinModeQuickPicksUseProposalCapAndClampToLockable()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* btnQuarter = dialog.findChild<QPushButton*>("btnCoinAmount25");
    auto* btnHalf = dialog.findChild<QPushButton*>("btnCoinAmount50");
    auto* btnThreeQuarters = dialog.findChild<QPushButton*>("btnCoinAmount75");
    auto* btnMax = dialog.findChild<QPushButton*>("btnCoinAmountMax");
    auto* amountEdit = dialog.findChild<QLineEdit*>("lineEditCoinAmount");
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(btnQuarter != nullptr);
    QVERIFY(btnHalf != nullptr);
    QVERIFY(btnThreeQuarters != nullptr);
    QVERIFY(btnMax != nullptr);
    QVERIFY(amountEdit != nullptr);

    coinModeRadio->setChecked(true);

    // proposalCap = 10 PIV (amount 10 * remainingPayments 1), lockable = 8 PIV.
    btnQuarter->click();
    QCOMPARE(GUIUtil::parseValue(amountEdit->text(), BitcoinUnits::PIV), CAmount((25 * COIN) / 10));

    btnHalf->click();
    QCOMPARE(GUIUtil::parseValue(amountEdit->text(), BitcoinUnits::PIV), CAmount(5 * COIN));

    btnThreeQuarters->click();
    QCOMPARE(GUIUtil::parseValue(amountEdit->text(), BitcoinUnits::PIV), CAmount((75 * COIN) / 10));

    // Max uses proposal cap then clamps to lockable.
    btnMax->click();
    QCOMPARE(GUIUtil::parseValue(amountEdit->text(), BitcoinUnits::PIV), CAmount(8 * COIN));
}

void GovernanceDialogTests::coinModeInvalidAmountDisablesVoteAndBlocksSubmission()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    model.lockableBalance = 20 * COIN;
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal()); // proposalCap = 10 PIV

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* amountEdit = dialog.findChild<QLineEdit*>("lineEditCoinAmount");
    auto* yesCheck = dialog.findChild<QCheckBox*>("checkVoteYes");
    auto* voteButton = dialog.findChild<QPushButton*>("btnSave");
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(amountEdit != nullptr);
    QVERIFY(yesCheck != nullptr);
    QVERIFY(voteButton != nullptr);
    if (!coinModeRadio || !amountEdit || !yesCheck || !voteButton) {
        QFAIL("Missing controls for invalid-amount test");
        return;
    }

    coinModeRadio->setChecked(true);
    yesCheck->setChecked(true);

    amountEdit->setText("11");
    QCoreApplication::processEvents();
    QVERIFY(!voteButton->isEnabled());

    dialog.onAcceptClicked();
    QVERIFY(!model.calledCoinVote);
}

void GovernanceDialogTests::coinModeAmountAboveLockableDisablesVoteAndBlocksSubmission()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    model.lockableBalance = 20 * COIN;
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(3)); // proposalCap = 30 PIV

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* amountEdit = dialog.findChild<QLineEdit*>("lineEditCoinAmount");
    auto* yesCheck = dialog.findChild<QCheckBox*>("checkVoteYes");
    auto* voteButton = dialog.findChild<QPushButton*>("btnSave");
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(amountEdit != nullptr);
    QVERIFY(yesCheck != nullptr);
    QVERIFY(voteButton != nullptr);
    if (!coinModeRadio || !amountEdit || !yesCheck || !voteButton) {
        QFAIL("Missing controls for lockable-amount test");
        return;
    }

    coinModeRadio->setChecked(true);
    yesCheck->setChecked(true);

    amountEdit->setText("21");
    QCoreApplication::processEvents();
    QVERIFY(!voteButton->isEnabled());

    dialog.onAcceptClicked();
    QVERIFY(!model.calledCoinVote);
}

void GovernanceDialogTests::coinModeDisabledAmountEntryShowsReason()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);

    ProposalInfo finishedProposal = BuildTestProposal(0);
    finishedProposal.status = ProposalInfo::FINISHED;
    dialog.setProposal(finishedProposal);

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* amountEdit = dialog.findChild<QLineEdit*>("lineEditCoinAmount");
    auto* hintLabel = dialog.findChild<QLabel*>("labelCoinValidationHint");
    auto* voteButton = dialog.findChild<QPushButton*>("btnSave");
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(amountEdit != nullptr);
    QVERIFY(hintLabel != nullptr);
    QVERIFY(voteButton != nullptr);
    if (!coinModeRadio || !amountEdit || !hintLabel || !voteButton) {
        QFAIL("Missing controls for disabled-amount hint test");
        return;
    }

    coinModeRadio->setChecked(true);
    QCoreApplication::processEvents();

    QVERIFY(!amountEdit->isEnabled());
    QVERIFY(hintLabel->text().contains("finished", Qt::CaseInsensitive));
    QVERIFY(!voteButton->isEnabled());
}

void GovernanceDialogTests::backendRejectsAmountAboveProposalCapBeforeWalletChecks()
{
    MNModel mnModel(nullptr);
    GovernanceModel model(nullptr, &mnModel);
    const ProposalInfo proposal = BuildTestProposal(); // proposalCap = 10 PIV

    const auto overCap = model.createVoteLockAndCast(proposal, true, 11 * COIN, 200);
    QVERIFY(!overCap);
    QCOMPARE(QString::fromStdString(overCap.getError()), QString("Lock amount exceeds proposal cap"));

    const auto atCap = model.createVoteLockAndCast(proposal, true, 10 * COIN, 200);
    QVERIFY(!atCap);
    QCOMPARE(QString::fromStdString(atCap.getError()), QString("Wallet not loaded"));
}

void GovernanceDialogTests::governanceModelRequiresFullTierTwoSyncForVotingGate()
{
    MNModel mnModel(nullptr);
    GovernanceModel model(nullptr, &mnModel);

    struct SyncStateGuard {
        bool chainSynced;
        int syncPhase;
        ~SyncStateGuard()
        {
            g_tiertwo_sync_state.SetCurrentSyncPhase(syncPhase);
            g_tiertwo_sync_state.SetBlockchainSync(chainSynced, 0);
        }
    } guard{g_tiertwo_sync_state.IsBlockchainSynced(), g_tiertwo_sync_state.GetSyncPhase()};

    g_tiertwo_sync_state.SetBlockchainSync(true, 0);
    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_BUDGET);
    QVERIFY(!model.isTierTwoSync());

    g_tiertwo_sync_state.SetCurrentSyncPhase(MASTERNODE_SYNC_FINISHED);
    QVERIFY(model.isTierTwoSync());
}

void GovernanceDialogTests::clientModelConnectionRefreshRules()
{
    QVERIFY(ClientModel::shouldEmitNumConnectionsChanged(-1, 0));
    QVERIFY(ClientModel::shouldEmitNumConnectionsChanged(0, 3));
    QVERIFY(!ClientModel::shouldEmitNumConnectionsChanged(4, 4));
}

void GovernanceDialogTests::topBarConnectionStyleTracksCurrentCount()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    TopBar topBar(&mainWindow);

    auto* connectionButton = topBar.findChild<ExpandableButton*>("pushButtonConnection");
    QVERIFY(connectionButton != nullptr);
    if (!connectionButton) {
        QFAIL("Missing topbar connection button");
        return;
    }
    auto* innerButton = connectionButton->findChild<QPushButton*>("pushButton");
    QVERIFY(innerButton != nullptr);
    if (!innerButton) {
        QFAIL("Missing inner QPushButton on connection control");
        return;
    }

    // Simulate stale UI state observed in production: inactive icon style while checked state is true.
    connectionButton->setChecked(true);
    connectionButton->setButtonClassStyle("cssClass", "btn-check-connect-inactive", true);
    QCOMPARE(innerButton->property("cssClass").toString(), QString("btn-check-connect-inactive"));

    topBar.setNumConnections(7);
    QCOMPARE(innerButton->property("cssClass").toString(), QString("btn-check-connect"));
    QVERIFY(connectionButton->isChecked());

    topBar.setNumConnections(0);
    QCOMPARE(innerButton->property("cssClass").toString(), QString("btn-check-connect-inactive"));
    QVERIFY(!connectionButton->isChecked());
}

void GovernanceDialogTests::lightThemeUtilitiesHaveVisibleChrome()
{
    QFile file(resolveQtSourceFile(QStringLiteral("res/css/style_light.css")));
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString css = QString::fromUtf8(file.readAll());
    const int start = css.indexOf(QStringLiteral("*[cssClass=\"topbar-utility-strip\"]"));
    QVERIFY2(start >= 0, "Light mode needs a dedicated visible utility-strip surface");
    const QString block = start >= 0 ? css.mid(start, 1800) : QString();
    QVERIFY2(block.contains(QStringLiteral("background-color: #FFFFFF")),
             "Light-mode utility controls must not disappear into the page background");
    QVERIFY2(block.contains(QStringLiteral("border: 1px solid #D4DDD2")),
             "Light-mode theme and lock controls need visible boundary chrome");
}

void GovernanceDialogTests::transactionRowsUseColorCodedIconBadges()
{
    TxRow row;
    row.init(true);
    auto* icon = row.findChild<QPushButton*>(QStringLiteral("icon"));
    QVERIFY(icon != nullptr);
    if (!icon) return;

    row.setType(true, TransactionRecord::RecvWithAddress, true);
    QCOMPARE(icon->property("cssClass").toString(), QStringLiteral("dashboard-tx-icon-received"));
    row.setType(true, TransactionRecord::SendToAddress, true);
    QCOMPARE(icon->property("cssClass").toString(), QStringLiteral("dashboard-tx-icon-sent"));
    row.setType(true, TransactionRecord::StakeMint, true);
    QCOMPARE(icon->property("cssClass").toString(), QStringLiteral("dashboard-tx-icon-reward"));
    const auto hasLightGlyph = [](const QIcon& sourceIcon) {
        const QImage image = sourceIcon.pixmap(24, 24).toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > 40 && pixel.red() > 220 && pixel.green() > 220 && pixel.blue() > 220) {
                    return true;
                }
            }
        }
        return false;
    };
    QVERIFY2(hasLightGlyph(icon->icon()), "Color badges need a high-contrast light glyph in light mode");
    row.setType(true, TransactionRecord::StakeMint, false);
    QCOMPARE(icon->property("cssClass").toString(), QStringLiteral("dashboard-tx-icon-reward"));
    QVERIFY2(hasLightGlyph(icon->icon()), "Pending rewards should keep the same clear semantic glyph");

    const auto assertSoftSemanticSurfaces = [](const QString& relativePath) {
        QFile file(resolveQtSourceFile(relativePath));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(relativePath));
        const QString css = QString::fromUtf8(file.readAll());
        for (const QString& role : {
                 QStringLiteral("dashboard-tx-icon-received"),
                 QStringLiteral("dashboard-tx-icon-sent"),
                 QStringLiteral("dashboard-tx-icon-reward"),
                 QStringLiteral("dashboard-tx-icon-transfer")}) {
            const int selector = css.lastIndexOf(QStringLiteral("QPushButton[cssClass=\"%1\"]").arg(role));
            QVERIFY2(selector >= 0, qPrintable(QString("Missing %1 in %2").arg(role, relativePath)));
            const int blockEnd = selector >= 0 ? css.indexOf(QLatin1Char('}'), selector) : -1;
            const QString block = blockEnd > selector ? css.mid(selector, blockEnd - selector) : QString();
            QVERIFY2(block.contains(QStringLiteral("rgba(")),
                     qPrintable(QString("%1 must use a soft translucent surface in %2").arg(role, relativePath)));
        }
    };
    assertSoftSemanticSurfaces(QStringLiteral("res/css/style_light.css"));
    assertSoftSemanticSurfaces(QStringLiteral("res/css/style_dark.css"));
}

void GovernanceDialogTests::secondaryScreensUseOneFintechShell()
{
    QFile file(resolveQtSourceFile(QStringLiteral("settings/settingswidget.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll());
    QVERIFY2(source.contains(QStringLiteral("setCssProperty(ui->left, \"screen-main-surface\")")),
             "Settings must use the same main content surface as Send and the other wallet screens");
    QVERIFY2(source.contains(QStringLiteral("setCssProperty(ui->right, \"screen-side-rail\")")),
             "Settings must use the shared premium side rail");
    QVERIFY2(source.contains(QStringLiteral("setCssProperty(ui->labelTitle, \"screen-header-title\")")),
             "Settings must use the shared screen title typography");
}

void GovernanceDialogTests::expandableButtonExpandsWhenInnerPushButtonReceivesHoverEntry()
{
    QWidget host;
    host.resize(320, 96);

    ExpandableButton button(&host);
    button.setButtonText(QStringLiteral("No Connection"));
    button.setNoIconText(QStringLiteral("NC"));
    button.setExpandedWidth(260);
    button.move(12, 12);
    button.show();
    host.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    auto* innerButton = button.findChild<QPushButton*>("pushButton");
    QVERIFY(innerButton != nullptr);
    if (!innerButton) {
        QFAIL("Missing inner QPushButton on expandable button");
        return;
    }

    QCOMPARE(button.width(), 48);

    QEvent enterEvent(QEvent::Enter);
    QCoreApplication::sendEvent(innerButton, &enterEvent);
    QTRY_VERIFY_WITH_TIMEOUT(button.width() > 48, 400);
}

void GovernanceDialogTests::expandableButtonCollapsesWhenMouseMovesOutsideWithoutLeaveEvent()
{
    QWidget host;
    host.resize(420, 120);

    ExpandableButton button(&host);
    button.setButtonText(QStringLiteral("No Connection"));
    button.setNoIconText(QStringLiteral("NC"));
    button.setExpandedWidth(260);
    button.move(12, 12);

    QWidget outsideArea(&host);
    outsideArea.setGeometry(320, 12, 72, 48);

    button.show();
    outsideArea.show();
    host.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    auto* innerButton = button.findChild<QPushButton*>("pushButton");
    QVERIFY(innerButton != nullptr);
    if (!innerButton) {
        QFAIL("Missing inner QPushButton on expandable button");
        return;
    }

    QCOMPARE(button.width(), 48);

    QEvent enterEvent(QEvent::Enter);
    QCoreApplication::sendEvent(innerButton, &enterEvent);
    QTRY_VERIFY_WITH_TIMEOUT(button.width() > 48, 400);

    const QPoint outsideLocalPos = outsideArea.rect().center();
    button.syncHoverStateToGlobalPos(outsideArea.mapToGlobal(outsideLocalPos));
    QTRY_VERIFY_WITH_TIMEOUT(button.width() < (button.getExpandedWidth() / 4), 400);
}

void GovernanceDialogTests::loadingDialogFullScreenHelperKeepsDialogInsideWalletWindow()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(920, 620);
    mainWindow.move(210, 130);
    mainWindow.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    LoadingDialog dialog(&mainWindow, QStringLiteral("Preparing transaction"));

    bool checkerExecuted = false;
    QRect observedGeometry;
    QTimer::singleShot(120, &dialog, [&]() {
        checkerExecuted = true;
        observedGeometry = dialog.geometry();
        dialog.reject();
    });

    (void)openDialogWithOpaqueBackgroundFullScreen(&dialog, &mainWindow);

    QVERIFY(checkerExecuted);
    const QRect parentGlobal(mainWindow.mapToGlobal(QPoint(0, 0)), mainWindow.size());
    QCOMPARE(observedGeometry.size(), parentGlobal.size());
    QVERIFY2(std::abs(observedGeometry.x() - parentGlobal.x()) <= 2,
             qPrintable(QString("Fullscreen helper drifted horizontally outside wallet bounds (dialog=%1 parent=%2)")
                                .arg(observedGeometry.x())
                                .arg(parentGlobal.x())));
    QVERIFY2(std::abs(observedGeometry.y() - parentGlobal.y()) <= 2,
             qPrintable(QString("Fullscreen helper drifted vertically outside wallet bounds (dialog=%1 parent=%2)")
                                .arg(observedGeometry.y())
                                .arg(parentGlobal.y())));

    const QString qtUtilsSourcePath = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSourcePath.isEmpty(), "Unable to resolve qtutils.cpp");
    QFile qtUtilsSourceFile(qtUtilsSourcePath);
    QVERIFY2(qtUtilsSourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read qtutils.cpp");
    const QString qtUtilsSource = QString::fromUtf8(qtUtilsSourceFile.readAll());

    const QString startToken = QStringLiteral("bool openDialogWithOpaqueBackgroundFullScreen(QDialog* widget, OrganicLifeGUI* gui)");
    const QString endToken = QStringLiteral("bool openDialogCentered(");
    const int start = qtUtilsSource.indexOf(startToken);
    const int end = qtUtilsSource.indexOf(endToken, start);
    QVERIFY(start >= 0);
    QVERIFY(end > start);
    if (start < 0 || end <= start) {
        QFAIL("openDialogWithOpaqueBackgroundFullScreen function not found");
        return;
    }

    const QString functionSlice = qtUtilsSource.mid(start, end - start);
    QVERIFY2(functionSlice.contains(QStringLiteral("gui->mapToGlobal(QPoint(0, 0))")),
             "Fullscreen opaque helper must anchor dialogs to the wallet window origin");
    QVERIFY2(functionSlice.contains(QStringLiteral("widget->resize(gui->width(), gui->height());")),
             "Fullscreen opaque helper must size dialogs to the wallet window");
    QVERIFY2(!functionSlice.contains(QStringLiteral("QPropertyAnimation")),
             "Fullscreen opaque helper must not animate dialogs from outside the wallet window");
}

void GovernanceDialogTests::loadingDialogUsesCompactProcessingCard()
{
    LoadingDialog dialog(nullptr, QStringLiteral("Preparing transaction"));
    dialog.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    auto* overlayFrame = dialog.findChild<QFrame*>("frame");
    auto* cardFrame = dialog.findChild<QFrame*>("cardFrame");
    auto* movieLabel = dialog.findChild<QLabel*>("labelMovie");
    auto* titleLabel = dialog.findChild<QLabel*>("labelTitle");
    auto* supportLabel = dialog.findChild<QLabel*>("labelSupport");
    QVERIFY(overlayFrame != nullptr);
    QVERIFY(cardFrame != nullptr);
    QVERIFY(movieLabel != nullptr);
    QVERIFY(titleLabel != nullptr);
    QVERIFY(supportLabel != nullptr);
    if (!overlayFrame || !cardFrame || !movieLabel || !titleLabel || !supportLabel) {
        QFAIL("Loading dialog premium card widgets are missing");
        return;
    }

    QCOMPARE(overlayFrame->property("cssClass").toString(), QString("loading-card-overlay"));
    QCOMPARE(cardFrame->property("cssClass").toString(), QString("loading-card-shell"));
    QVERIFY(cardFrame->maximumWidth() <= 520);
    QVERIFY(movieLabel->maximumWidth() <= 140);
    QVERIFY(movieLabel->maximumHeight() <= 140);
    QVERIFY(titleLabel->wordWrap());
    QVERIFY(supportLabel->wordWrap());
}

void GovernanceDialogTests::loadingDialogBadgePreservesVerticalBreathingRoom()
{
    LoadingDialog::Content content;
    content.eyebrow = QStringLiteral("Security");
    content.title = QStringLiteral("Encrypting wallet");
    content.supportText = QStringLiteral("Applying encryption and securing your wallet data.");

    LoadingDialog dialog(nullptr, content);
    dialog.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    auto* badgeFrame = dialog.findChild<QFrame*>("badgeFrame");
    auto* movieLabel = dialog.findChild<QLabel*>("labelMovie");
    QVERIFY(badgeFrame != nullptr);
    QVERIFY(movieLabel != nullptr);
    if (!badgeFrame || !movieLabel) {
        QFAIL("Loading dialog badge widgets are missing");
        return;
    }

    const int verticalClearance = badgeFrame->height() - movieLabel->height();
    const int horizontalClearance = badgeFrame->width() - movieLabel->width();
    QVERIFY2(verticalClearance >= 24,
             qPrintable(QString("Loading badge vertical clearance too tight (%1px)").arg(verticalClearance)));
    QVERIFY2(horizontalClearance >= 24,
             qPrintable(QString("Loading badge horizontal clearance too tight (%1px)").arg(horizontalClearance)));
}

void GovernanceDialogTests::loadingDialogTitlePreservesVerticalBreathingRoom()
{
    LoadingDialog::Content content;
    content.eyebrow = QStringLiteral("Security");
    content.title = QStringLiteral("Encrypting wallet");
    content.supportText = QStringLiteral("Applying encryption and securing your wallet data.");

    LoadingDialog dialog(nullptr, content);
    dialog.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    auto* titleLabel = dialog.findChild<QLabel*>("labelTitle");
    QVERIFY(titleLabel != nullptr);
    if (!titleLabel) {
        QFAIL("Loading dialog title label is missing");
        return;
    }

    const QMargins titleMargins = titleLabel->contentsMargins();
    const QRect textBounds = titleLabel->fontMetrics().boundingRect(
            titleLabel->contentsRect(),
            titleLabel->alignment() | Qt::TextWordWrap,
            titleLabel->text());
    const int verticalClearance = titleLabel->height() - textBounds.height();

    QVERIFY2(titleLabel->width() >= 400,
             qPrintable(QString("Loading title width too narrow (%1px)").arg(titleLabel->width())));
    QVERIFY2(titleLabel->minimumHeight() >= 60,
             qPrintable(QString("Loading title minimum height too small (%1px)").arg(titleLabel->minimumHeight())));
    QVERIFY2(titleMargins.top() >= 6,
             qPrintable(QString("Loading title top margin too small (%1px)").arg(titleMargins.top())));
    QVERIFY2(titleMargins.bottom() >= 6,
             qPrintable(QString("Loading title bottom margin too small (%1px)").arg(titleMargins.bottom())));
    QVERIFY2(verticalClearance >= 12,
             qPrintable(QString("Loading title vertical clearance too tight (%1px)").arg(verticalClearance)));
}

void GovernanceDialogTests::loadingDialogThemesDefinePremiumCardStyles()
{
    const QString lightCssPath = resolveQtSourceFile("res/css/style_light.css");
    const QString darkCssPath = resolveQtSourceFile("res/css/style_dark.css");
    QVERIFY2(!lightCssPath.isEmpty(), "Unable to resolve style_light.css");
    QVERIFY2(!darkCssPath.isEmpty(), "Unable to resolve style_dark.css");

    const auto readSource = [](const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
        return QString::fromUtf8(file.readAll());
    };

    const QString lightCss = readSource(lightCssPath);
    const QString darkCss = readSource(darkCssPath);
    QVERIFY2(!lightCss.isEmpty(), "Unable to read style_light.css");
    QVERIFY2(!darkCss.isEmpty(), "Unable to read style_dark.css");

    for (const QString& token : {
             QStringLiteral("loading-card-shell"),
             QStringLiteral("loading-card-title"),
             QStringLiteral("loading-card-support"),
             QStringLiteral("loading-card-meta-pill")
         }) {
        QVERIFY2(lightCss.contains(token), qPrintable(QString("Light theme missing %1").arg(token)));
        QVERIFY2(darkCss.contains(token), qPrintable(QString("Dark theme missing %1").arg(token)));
    }
}

void GovernanceDialogTests::sendFlowBuildsStructuredLoadingDialogContent()
{
    const QString sendSourcePath = resolveQtSourceFile("send.cpp");
    QVERIFY2(!sendSourcePath.isEmpty(), "Unable to resolve send.cpp");

    QFile sendSourceFile(sendSourcePath);
    QVERIFY2(sendSourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read send.cpp");
    const QString sendSource = QString::fromUtf8(sendSourceFile.readAll());

    QVERIFY2(sendSource.contains(QStringLiteral("LoadingDialog::Content")),
             "Send flow must build structured LoadingDialog content");
    QVERIFY2(sendSource.contains(QStringLiteral("Selecting inputs, calculating fees, and validating outputs.")),
             "Prepare transaction flow must provide supporting copy");
    QVERIFY2(sendSource.contains(QStringLiteral("Broadcasting transaction")),
             "Broadcast flow must still expose a broadcast title");
}

void GovernanceDialogTests::topBarHoverAnimationsUseSmoothCubicEasing()
{
    const QString topBarSourcePath = resolveQtSourceFile("topbar.cpp");
    QVERIFY2(!topBarSourcePath.isEmpty(), "Unable to resolve src/qt/topbar.cpp");
    QFile topBarSourceFile(topBarSourcePath);
    QVERIFY2(topBarSourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read topbar.cpp");
    const QString topBarSource = QString::fromUtf8(topBarSourceFile.readAll());

    const int setupStart = topBarSource.indexOf(QStringLiteral("void TopBar::setupStatusHoverPill()"));
    const int setupEnd = topBarSource.indexOf(QStringLiteral("void TopBar::showStatusHoverPill"), setupStart);
    QVERIFY2(setupStart >= 0 && setupEnd > setupStart, "Unable to isolate TopBar::setupStatusHoverPill()");
    const QString setupSlice = topBarSource.mid(setupStart, setupEnd - setupStart);
    QVERIFY2(setupSlice.contains(QStringLiteral("statusHoverPillGeometryAnimation->setEasingCurve(QEasingCurve::OutCubic);")),
             "Top bar hover pill geometry animation should use smooth cubic easing");
    QVERIFY2(!setupSlice.contains(QStringLiteral("statusHoverPillGeometryAnimation->setEasingCurve(QEasingCurve::OutBack);")),
             "Top bar hover pill geometry animation must not use overshoot/bounce easing");

    const int showStart = topBarSource.indexOf(QStringLiteral("void TopBar::showStatusHoverPill(ExpandableButton* button)"));
    const int showEnd = topBarSource.indexOf(QStringLiteral("void TopBar::hideStatusHoverPill()"), showStart);
    QVERIFY2(showStart >= 0 && showEnd > showStart, "Unable to isolate TopBar::showStatusHoverPill()");
    const QString showSlice = topBarSource.mid(showStart, showEnd - showStart);
    QVERIFY2(showSlice.contains(QStringLiteral("statusHoverPillFadeAnimation->setDuration(110);")),
             "Top bar hover pill show animation should reset fade duration for consistent smoothness");
    QVERIFY2(showSlice.contains(QStringLiteral("statusHoverPillFadeAnimation->setEasingCurve(QEasingCurve::OutCubic);")),
             "Top bar hover pill show animation should reset fade easing after hide transitions");

    const QString expandableSourcePath = resolveQtSourceFile("expandablebutton.cpp");
    QVERIFY2(!expandableSourcePath.isEmpty(), "Unable to resolve src/qt/expandablebutton.cpp");
    QFile expandableSourceFile(expandableSourcePath);
    QVERIFY2(expandableSourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read expandablebutton.cpp");
    const QString expandableSource = QString::fromUtf8(expandableSourceFile.readAll());

    const int animateStart = expandableSource.indexOf(QStringLiteral("void ExpandableButton::animateWidth(int startWidth, int endWidth)"));
    const int animateEnd = expandableSource.indexOf(QStringLiteral("void ExpandableButton::setSmall"), animateStart);
    QVERIFY2(animateStart >= 0 && animateEnd > animateStart, "Unable to isolate ExpandableButton::animateWidth()");
    const QString animateSlice = expandableSource.mid(animateStart, animateEnd - animateStart);
    QVERIFY2(animateSlice.contains(QStringLiteral("const QEasingCurve easing = expanding ? QEasingCurve(QEasingCurve::OutCubic) : QEasingCurve::InOutCubic;")),
             "Top bar expandable buttons should use cubic easing instead of overshoot bounce on expand");
    QVERIFY2(!animateSlice.contains(QStringLiteral("QEasingCurve::OutBack")),
             "Top bar expandable button width animation must not use OutBack overshoot");
}

void GovernanceDialogTests::topBarReceiveDialogUsesOpaqueBackgroundHelper()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sourcePath = appDir.absoluteFilePath("../../../../src/qt/topbar.cpp");
    QFile sourceFile(sourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read topbar.cpp");

    const QString source = QString::fromUtf8(sourceFile.readAll());
    const int methodStart = source.indexOf(QStringLiteral("void TopBar::onBtnReceiveClicked()"));
    QVERIFY2(methodStart >= 0, "Missing TopBar::onBtnReceiveClicked implementation");
    const int methodEnd = source.indexOf(QStringLiteral("void TopBar::onBtnBalanceInfoClicked()"), methodStart);
    QVERIFY2(methodEnd > methodStart, "Unable to isolate TopBar::onBtnReceiveClicked implementation");

    const QString methodBody = source.mid(methodStart, methodEnd - methodStart);
    const QRegularExpression openHelperRegex(R"(openDialogWithOpaqueBackgroundY\s*\(\s*receiveDialog\s*,\s*window\b)");
    QVERIFY2(openHelperRegex.match(methodBody).hasMatch(),
             "TopBar receive flow must use openDialogWithOpaqueBackgroundY for container-dialog centering");

    const QRegularExpression legacyCenterRegex(R"(openDialogCentered\s*\(\s*receiveDialog\s*,\s*window\b)");
    QVERIFY2(!legacyCenterRegex.match(methodBody).hasMatch(),
             "TopBar receive flow must not use legacy openDialogCentered helper");
}

void GovernanceDialogTests::multiWalletStartupBuildsWalletModelsForAllLoadedWallets()
{
    const QString pivxSourcePath = resolveQtSourceFile("organiclife.cpp");
    QVERIFY2(!pivxSourcePath.isEmpty(), "Unable to resolve src/qt/organiclife.cpp");
    QFile sourceFile(pivxSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read organiclife.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    const int methodStart = source.indexOf(QStringLiteral("void BitcoinApplication::initializeResult(int retval)"));
    QVERIFY2(methodStart >= 0, "Missing BitcoinApplication::initializeResult implementation");
    const int methodEnd = source.indexOf(QStringLiteral("void BitcoinApplication::handleRunawayException"), methodStart);
    QVERIFY2(methodEnd > methodStart, "Unable to isolate BitcoinApplication::initializeResult implementation");
    const QString methodBody = source.mid(methodStart, methodEnd - methodStart);

    QVERIFY2(methodBody.contains(QStringLiteral("for (CWallet* wallet : vpwallets)")),
             "Qt startup must build a WalletModel for every loaded wallet");
    QVERIFY2(methodBody.contains(QStringLiteral("window->addWallet(QString::fromStdString(wallet->GetName()), walletModel)")),
             "Qt startup must register each loaded wallet in the main window");
    QVERIFY2(methodBody.contains(QStringLiteral("window->setCurrentWallet(QString::fromStdString(vpwallets[0]->GetName()))")),
             "Qt startup must select the first loaded wallet as the initial active wallet");
}

void GovernanceDialogTests::qtStartupSetsGovernanceModelBeforeInitialWalletBinding()
{
    const QString pivxSourcePath = resolveQtSourceFile("organiclife.cpp");
    QVERIFY2(!pivxSourcePath.isEmpty(), "Unable to resolve src/qt/organiclife.cpp");
    QFile sourceFile(pivxSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read organiclife.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    const int methodStart = source.indexOf(QStringLiteral("void BitcoinApplication::initializeResult(int retval)"));
    QVERIFY2(methodStart >= 0, "Missing BitcoinApplication::initializeResult implementation");
    const int methodEnd = source.indexOf(QStringLiteral("void BitcoinApplication::handleRunawayException"), methodStart);
    QVERIFY2(methodEnd > methodStart, "Unable to isolate BitcoinApplication::initializeResult implementation");
    const QString methodBody = source.mid(methodStart, methodEnd - methodStart);

    const int setGovModelIndex = methodBody.indexOf(QStringLiteral("window->setGovModel(govModel);"));
    QVERIFY2(setGovModelIndex >= 0, "Qt startup must assign the governance model to the main window");
    const int initialWalletIndex = methodBody.indexOf(QStringLiteral("window->setCurrentWallet(QString::fromStdString(vpwallets[0]->GetName()))"));
    QVERIFY2(initialWalletIndex >= 0, "Qt startup must bind an initial active wallet");
    QVERIFY2(setGovModelIndex < initialWalletIndex,
             "Qt startup must set the governance model before the initial wallet binding to avoid null governance dereferences");
}

void GovernanceDialogTests::guiTracksWalletRegistryAndSupportsCurrentWalletSwitching()
{
    const QString guiSourcePath = resolveQtSourceFile("organiclifegui.cpp");
    QVERIFY2(!guiSourcePath.isEmpty(), "Unable to resolve src/qt/organiclifegui.cpp");
    QFile sourceFile(guiSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read organiclifegui.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    const int addWalletStart = source.indexOf(QStringLiteral("bool OrganicLifeGUI::addWallet(const QString& name, WalletModel* walletModel)"));
    QVERIFY2(addWalletStart >= 0, "Missing OrganicLifeGUI::addWallet implementation");
    const int setWalletStart = source.indexOf(QStringLiteral("bool OrganicLifeGUI::setCurrentWallet(const QString& name)"), addWalletStart);
    QVERIFY2(setWalletStart > addWalletStart, "Unable to isolate OrganicLifeGUI::addWallet implementation");
    const QString addWalletBody = source.mid(addWalletStart, setWalletStart - addWalletStart);
    QVERIFY2(addWalletBody.contains(QStringLiteral("walletStack.emplace(name, walletModel);")),
             "OrganicLifeGUI::addWallet must persist wallet models in a registry");
    QVERIFY2(!addWalletBody.contains(QStringLiteral("Single wallet supported for now")),
             "OrganicLifeGUI::addWallet must no longer advertise single-wallet-only behavior");

    const int removeWalletsStart = source.indexOf(QStringLiteral("void OrganicLifeGUI::removeAllWallets()"), setWalletStart);
    QVERIFY2(removeWalletsStart > setWalletStart, "Unable to isolate OrganicLifeGUI::setCurrentWallet implementation");
    const QString setWalletBody = source.mid(setWalletStart, removeWalletsStart - setWalletStart);
    QVERIFY2(setWalletBody.contains(QStringLiteral("currentWallet = name;")),
             "OrganicLifeGUI::setCurrentWallet must update the active wallet key");
    QVERIFY2(setWalletBody.contains(QStringLiteral("topBar->setWalletModel(walletModel)")),
             "OrganicLifeGUI::setCurrentWallet must rebind the top bar to the selected wallet");
    QVERIFY2(setWalletBody.contains(QStringLiteral("rpcConsole->setWalletModel(walletModel)")),
             "OrganicLifeGUI::setCurrentWallet must retarget the RPC console to the selected wallet");
}

void GovernanceDialogTests::multiWalletStakingStartsOneThreadPerLoadedWallet()
{
    const QString initSourcePath = resolveQtSourceFile("../init.cpp");
    QVERIFY2(!initSourcePath.isEmpty(), "Unable to resolve src/init.cpp");
    QFile initSourceFile(initSourcePath);
    QVERIFY2(initSourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read init.cpp");
    const QString initSource = QString::fromUtf8(initSourceFile.readAll());

    const int stakingStart = initSource.indexOf(QStringLiteral("if (!vpwallets.empty() && gArgs.GetBoolArg(\"-staking\", !Params().IsRegTestNet() && DEFAULT_STAKING)) {"));
    QVERIFY2(stakingStart >= 0, "Missing staking startup block in init.cpp");
    const int stakingEnd = initSource.indexOf(QStringLiteral("#endif"), stakingStart);
    QVERIFY2(stakingEnd > stakingStart, "Unable to isolate staking startup block in init.cpp");
    const QString stakingBody = initSource.mid(stakingStart, stakingEnd - stakingStart);

    QVERIFY2(stakingBody.contains(QStringLiteral("for (CWalletRef pwallet : vpwallets)")),
             "Staking startup must iterate over every loaded wallet");
    QVERIFY2(stakingBody.contains(QStringLiteral("StartWalletStakingThread(pwallet);")),
             "Staking startup must launch staking through the per-wallet thread helper");
    QVERIFY2(initSource.contains(QStringLiteral("threadGroup.create_thread(std::bind(&ThreadStakeMinter, pwallet));")),
             "Per-wallet staking helper must still create a dedicated staking thread for each wallet");

    const QString minerSourcePath = resolveQtSourceFile("../miner.cpp");
    QVERIFY2(!minerSourcePath.isEmpty(), "Unable to resolve src/miner.cpp");
    QFile minerSourceFile(minerSourcePath);
    QVERIFY2(minerSourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read miner.cpp");
    const QString minerSource = QString::fromUtf8(minerSourceFile.readAll());

    QVERIFY2(minerSource.contains(QStringLiteral("void ThreadStakeMinter(CWallet* pwallet)")),
             "ThreadStakeMinter must accept a wallet argument");
    QVERIFY2(!minerSource.contains(QStringLiteral("CWallet* pwallet = vpwallets[0];")),
             "ThreadStakeMinter must not hardcode wallet 0");
}

void GovernanceDialogTests::rpcExecutorRoutesCommandsToConfigurableWalletName()
{
    const QString executorSourcePath = resolveQtSourceFile("rpcexecutor.cpp");
    QVERIFY2(!executorSourcePath.isEmpty(), "Unable to resolve src/qt/rpcexecutor.cpp");
    QFile sourceFile(executorSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read rpcexecutor.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("void RPCExecutor::setWalletName(const QString& name, bool configured)")),
             "RPCExecutor must expose a setter for the active wallet name");
    QVERIFY2(source.contains(QStringLiteral("if (walletNameConfigured)")),
             "RPCExecutor must only use wallet routing when an active wallet name is configured");
    QVERIFY2(source.contains(QStringLiteral("req.URI = \"/wallet/\"+std::string(encodedName.constData(), encodedName.length());")),
             "RPCExecutor must derive the wallet RPC URI from the configured active wallet name");
    QVERIFY2(!source.contains(QStringLiteral("use always the wallet with index 0 when running with multiple wallets")),
             "RPCExecutor must not hardcode wallet 0 once wallet switching is available");
}

void GovernanceDialogTests::rpcExecutorSupportsExplicitPerCommandWalletOverride()
{
    const QString executorSourcePath = resolveQtSourceFile("rpcexecutor.cpp");
    QVERIFY2(!executorSourcePath.isEmpty(), "Unable to resolve src/qt/rpcexecutor.cpp");
    QFile sourceFile(executorSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read rpcexecutor.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("ParseConsoleWalletOverride")),
             "RPCExecutor must parse a console-only wallet override suffix before dispatch");
    QVERIFY2(source.contains(QStringLiteral("wallet=\\\"DeveloperWallet\\\"")),
             "RPCExecutor help must document the explicit per-command wallet override syntax");
    QVERIFY2(source.contains(QStringLiteral("ParseConsoleWalletOverride(resolvedCommand, requestWalletName, requestWalletConfigured);")),
             "RPCExecutor must resolve per-command wallet overrides before executing the parsed RPC command");
    QVERIFY2(source.contains(QStringLiteral("ExecuteCommandLine(result, executableCommand, requestWalletName, requestWalletConfigured)")),
             "RPCExecutor must execute commands against the per-command wallet override when one is supplied");
}

void GovernanceDialogTests::rpcConsoleDefaultsToPrimaryWalletWhenMultipleWalletsAreLoaded()
{
    const QString consoleSourcePath = resolveQtSourceFile("rpcconsole.cpp");
    QVERIFY2(!consoleSourcePath.isEmpty(), "Unable to resolve src/qt/rpcconsole.cpp");
    QFile sourceFile(consoleSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read rpcconsole.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("qobject_cast<OrganicLifeGUI*>(parentWidget())")),
             "RPCConsole must inspect its owning main window when choosing the implicit wallet context");
    QVERIFY2(source.contains(QStringLiteral("window->getWallet(window->primaryWalletName())")),
             "RPCConsole must prefer the configured primary wallet for implicit wallet-scoped RPC calls");
    QVERIFY2(source.contains(QStringLiteral("window->getWallet(QString())")),
             "RPCConsole must fall back to the implicit primary wallet key when no stored primary wallet is available");
}

void GovernanceDialogTests::settingsConsoleReceivesWalletModelLifecycleFromSettingsWidget()
{
    const QString settingsSourcePath = resolveQtSourceFile("settings/settingswidget.cpp");
    QVERIFY2(!settingsSourcePath.isEmpty(), "Unable to resolve src/qt/settings/settingswidget.cpp");
    QFile sourceFile(settingsSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read settingswidget.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("this->settingsConsoleWidget->setWalletModel(this->walletModel);")),
             "SettingsWidget must forward wallet model changes into the embedded settings console");
    QVERIFY2(source.contains(QStringLiteral("this->settingsConsoleWidget->clearWalletModel();")),
             "SettingsWidget must clear the embedded settings console wallet model when the active wallet changes");
}

void GovernanceDialogTests::setgenerateHelpDocumentsQtConsoleWalletOverride()
{
    const QString miningSourcePath = resolveQtSourceFile("../rpc/mining.cpp");
    QVERIFY2(!miningSourcePath.isEmpty(), "Unable to resolve src/rpc/mining.cpp");
    QFile sourceFile(miningSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read mining.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("In the Qt RPC console, append wallet=<name> to target a specific loaded wallet.")),
             "setgenerate help must document the Qt console wallet override suffix");
    QVERIFY2(source.contains(QStringLiteral("setgenerate true 1 wallet=\\\"DeveloperWallet\\\"")),
             "setgenerate help must include an example for a named wallet in the Qt console");
    QVERIFY2(source.contains(QStringLiteral("setgenerate true 1 wallet=\\\"\\\"")),
             "setgenerate help must include an example for the primary wallet in the Qt console");
}

void GovernanceDialogTests::topBarExposesDedicatedWalletSelectorControl()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    TopBar topBar(&mainWindow);
    topBar.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    auto* walletButton = topBar.findChild<ExpandableButton*>(QStringLiteral("pushButtonWallet"));
    QVERIFY2(walletButton != nullptr, "TopBar must expose a dedicated wallet selector control");
}

void GovernanceDialogTests::topBarCompactWalletSelectorUsesIconOnlyState()
{
    const QString topbarSourcePath = resolveQtSourceFile("topbar.cpp");
    QVERIFY2(!topbarSourcePath.isEmpty(), "Unable to resolve src/qt/topbar.cpp");
    QFile sourceFile(topbarSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read topbar.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("ui->pushButtonWallet->setNoIconText(QString());")),
             "Wallet selector compact state must remain icon-only instead of showing a one-letter fallback");
    QVERIFY2(!source.contains(QStringLiteral("ui->pushButtonWallet->setNoIconText(walletLabel.left(1).toUpper());")),
             "Wallet selector compact state must not show the first wallet character");
}

void GovernanceDialogTests::walletSelectorDialogShowsPerWalletLockState()
{
    const QString topbarSourcePath = resolveQtSourceFile("topbar.cpp");
    QVERIFY2(!topbarSourcePath.isEmpty(), "Unable to resolve src/qt/topbar.cpp");
    QFile sourceFile(topbarSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read topbar.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("model->getEncryptionStatus()")),
             "Wallet selector dialog must inspect each wallet encryption status");
    QVERIFY2(source.contains(QStringLiteral("://ic-wallet-selector-locked")),
             "Wallet selector dialog must expose a locked-wallet icon");
    QVERIFY2(source.contains(QStringLiteral("://ic-wallet-selector-staking")),
             "Wallet selector dialog must expose a staking-unlock icon");
    QVERIFY2(source.contains(QStringLiteral("://ic-wallet-selector-unlocked")),
             "Wallet selector dialog must expose an unlocked-wallet icon");
}

void GovernanceDialogTests::walletSelectorUsesInvertedThemeIcons()
{
    const QString lightCssPath = resolveQtSourceFile("res/css/style_light.css");
    QVERIFY2(!lightCssPath.isEmpty(), "Unable to resolve src/qt/res/css/style_light.css");
    QFile lightCssFile(lightCssPath);
    QVERIFY2(lightCssFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read style_light.css");
    const QString lightCss = QString::fromUtf8(lightCssFile.readAll());

    const QString darkCssPath = resolveQtSourceFile("res/css/style_dark.css");
    QVERIFY2(!darkCssPath.isEmpty(), "Unable to resolve src/qt/res/css/style_dark.css");
    QFile darkCssFile(darkCssPath);
    QVERIFY2(darkCssFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read style_dark.css");
    const QString darkCss = QString::fromUtf8(darkCssFile.readAll());

    QVERIFY2(lightCss.contains(QStringLiteral("qproperty-icon: url(\"://ic-check-wallet\");")),
             "Light theme wallet button must use the lighter wallet icon");
    QVERIFY2(darkCss.contains(QStringLiteral("qproperty-icon: url(\"://ic-check-wallet-dark\");")),
             "Dark theme wallet button must use the darker wallet icon");
}

void GovernanceDialogTests::guiInterfaceExposesWalletUnloadSignal()
{
    const QString guiInterfacePath = resolveQtSourceFile("../guiinterface.h");
    QVERIFY2(!guiInterfacePath.isEmpty(), "Unable to resolve src/guiinterface.h");
    QFile sourceFile(guiInterfacePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read guiinterface.h");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("boost::signals2::signal<void(CWallet* wallet)> UnloadWallet;")),
             "CClientUIInterface must expose an unload-wallet signal for runtime wallet removal");
}

void GovernanceDialogTests::qtRuntimeWalletLifecycleWiresLoadAndUnloadSignals()
{
    const QString pivxSourcePath = resolveQtSourceFile("organiclife.cpp");
    QVERIFY2(!pivxSourcePath.isEmpty(), "Unable to resolve src/qt/organiclife.cpp");
    QFile sourceFile(pivxSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read organiclife.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("interfaces::MakeHandler(uiInterface.LoadWallet.connect")),
             "Qt wallet runtime wiring must subscribe to wallet-load notifications");
    QVERIFY2(source.contains(QStringLiteral("interfaces::MakeHandler(uiInterface.UnloadWallet.connect")),
             "Qt wallet runtime wiring must subscribe to wallet-unload notifications");
    QVERIFY2(source.contains(QStringLiteral("QMetaObject::invokeMethod(this, [this, wallet]")),
             "Runtime wallet notifications must marshal wallet add/remove work back onto the GUI thread");
}

void GovernanceDialogTests::walletSelectorDialogExposesManageWalletsEntry()
{
    const QString topbarSourcePath = resolveQtSourceFile("topbar.cpp");
    QVERIFY2(!topbarSourcePath.isEmpty(), "Unable to resolve src/qt/topbar.cpp");
    QFile sourceFile(topbarSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read topbar.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("Manage Wallets")),
             "Wallet selector dialog must expose a Manage Wallets entrypoint");
    QVERIFY2(source.contains(QStringLiteral("openManageWalletsDialog")),
             "TopBar must provide a dedicated manage-wallets launch path");
}

void GovernanceDialogTests::walletMetadataUsesPersistentQSettingsKeys()
{
    const QString guiSourcePath = resolveQtSourceFile("organiclifegui.cpp");
    QVERIFY2(!guiSourcePath.isEmpty(), "Unable to resolve src/qt/organiclifegui.cpp");
    QFile sourceFile(guiSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read organiclifegui.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("QSettings settings;")),
             "Wallet metadata helpers must persist through QSettings");
    QVERIFY2(source.contains(QStringLiteral("walletMetadata/displayName/")),
             "Wallet display names must be stored under a dedicated QSettings key");
    QVERIFY2(source.contains(QStringLiteral("walletMetadata/primaryWallet")),
             "Primary wallet selection must be stored in QSettings");
    QVERIFY2(source.contains(QStringLiteral("walletMetadata/autoloadWallets")),
             "Autoload wallet names must be stored in QSettings");
}

void GovernanceDialogTests::walletMetadataExposesAutoloadMutationHelpers()
{
    const QString guiHeaderPath = resolveQtSourceFile("organiclifegui.h");
    QVERIFY2(!guiHeaderPath.isEmpty(), "Unable to resolve src/qt/organiclifegui.h");
    QFile guiHeaderFile(guiHeaderPath);
    QVERIFY2(guiHeaderFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read organiclifegui.h");
    const QString headerSource = QString::fromUtf8(guiHeaderFile.readAll());

    const QString guiSourcePath = resolveQtSourceFile("organiclifegui.cpp");
    QVERIFY2(!guiSourcePath.isEmpty(), "Unable to resolve src/qt/organiclifegui.cpp");
    QFile guiSourceFile(guiSourcePath);
    QVERIFY2(guiSourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read organiclifegui.cpp");
    const QString source = QString::fromUtf8(guiSourceFile.readAll());

    QVERIFY2(headerSource.contains(QStringLiteral("void addAutoloadWalletName(const QString& walletName);")),
             "OrganicLifeGUI must expose an explicit helper to add a wallet name to autoload state");
    QVERIFY2(headerSource.contains(QStringLiteral("void removeAutoloadWalletName(const QString& walletName);")),
             "OrganicLifeGUI must expose an explicit helper to remove a wallet name from autoload state");
    QVERIFY2(source.contains(QStringLiteral("void OrganicLifeGUI::addAutoloadWalletName(const QString& walletName)")),
             "OrganicLifeGUI must implement explicit autoload addition");
    QVERIFY2(source.contains(QStringLiteral("void OrganicLifeGUI::removeAutoloadWalletName(const QString& walletName)")),
             "OrganicLifeGUI must implement explicit autoload removal");
}

void GovernanceDialogTests::manageWalletsDialogOnlyShowsLoadedWallets()
{
    const QString topbarSourcePath = resolveQtSourceFile("topbar.cpp");
    QVERIFY2(!topbarSourcePath.isEmpty(), "Unable to resolve src/qt/topbar.cpp");
    QFile sourceFile(topbarSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read topbar.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(!source.contains(QStringLiteral("Available in data/wallets")),
             "Manage Wallets should no longer expose a separate available-wallet list");
    QVERIFY2(!source.contains(QStringLiteral("loadButton = new QPushButton(tr(\"Load\")")),
             "Manage Wallets should not require a separate Load action for managed wallets");
    QVERIFY2(!source.contains(QStringLiteral("availableList(new QListWidget(this))")),
             "Manage Wallets should only render the loaded-wallet list");
    QVERIFY2(!source.contains(QStringLiteral("selectedAvailableWallet() const")),
             "Manage Wallets should not maintain available-wallet selection state");
    QVERIFY2(!source.contains(QStringLiteral("ListWalletDir()")),
             "Manage Wallets should not browse arbitrary available wallet folders");
}

void GovernanceDialogTests::createWalletPersistsAutoloadNameImmediately()
{
    const QString topbarSourcePath = resolveQtSourceFile("topbar.cpp");
    QVERIFY2(!topbarSourcePath.isEmpty(), "Unable to resolve src/qt/topbar.cpp");
    QFile sourceFile(topbarSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read topbar.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    const int createStart = source.indexOf(QStringLiteral("void createWallet()"));
    QVERIFY2(createStart >= 0, "Missing createWallet implementation in Manage Wallets dialog");
    const int createEnd = source.indexOf(QStringLiteral("void unloadSelectedWallet()"), createStart);
    QVERIFY2(createEnd > createStart, "Unable to isolate createWallet implementation");
    const QString createBody = source.mid(createStart, createEnd - createStart);

    QVERIFY2(createBody.contains(QStringLiteral("window->addAutoloadWalletName(walletName);")),
             "Create wallet must immediately persist the new wallet name into the autoload set");
}

void GovernanceDialogTests::unloadWalletRemovesAutoloadNameImmediately()
{
    const QString topbarSourcePath = resolveQtSourceFile("topbar.cpp");
    QVERIFY2(!topbarSourcePath.isEmpty(), "Unable to resolve src/qt/topbar.cpp");
    QFile sourceFile(topbarSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read topbar.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    const int unloadStart = source.indexOf(QStringLiteral("void unloadSelectedWallet()"));
    QVERIFY2(unloadStart >= 0, "Missing unloadSelectedWallet implementation in Manage Wallets dialog");
    const int unloadEnd = source.indexOf(QStringLiteral("void renameSelectedWallet()"), unloadStart);
    QVERIFY2(unloadEnd > unloadStart, "Unable to isolate unloadSelectedWallet implementation");
    const QString unloadBody = source.mid(unloadStart, unloadEnd - unloadStart);

    QVERIFY2(unloadBody.contains(QStringLiteral("window->removeAutoloadWalletName(walletName);")),
             "Unload wallet must immediately remove the wallet name from the autoload set");
}

void GovernanceDialogTests::walletVerifyMigratesLegacyManagedWalletLayoutBeforeVerification()
{
    const QString initSourcePath = resolveQtSourceFile("../wallet/init.cpp");
    QVERIFY2(!initSourcePath.isEmpty(), "Unable to resolve src/wallet/init.cpp");
    QFile sourceFile(initSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read wallet/init.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    const int verifyStart = source.indexOf(QStringLiteral("bool WalletVerify()"));
    QVERIFY2(verifyStart >= 0, "Missing WalletVerify implementation");
    const int verifyEnd = source.indexOf(QStringLiteral("bool InitLoadWallet()"), verifyStart);
    QVERIFY2(verifyEnd > verifyStart, "Unable to isolate WalletVerify implementation");
    const QString verifyBody = source.mid(verifyStart, verifyEnd - verifyStart);

    const int migrateIndex = verifyBody.indexOf(QStringLiteral("MigrateLegacyManagedWalletLayout("));
    QVERIFY2(migrateIndex >= 0,
             "WalletVerify must migrate legacy wallet locations before verifying the new managed-wallet layout");
    const int initMessageIndex = verifyBody.indexOf(QStringLiteral("uiInterface.InitMessage(_("));
    QVERIFY2(initMessageIndex >= 0, "WalletVerify must still show the wallet verification startup message");
    QVERIFY2(migrateIndex < initMessageIndex,
             "Legacy wallet migration must happen before WalletVerify opens or verifies wallet paths");
}

void GovernanceDialogTests::qtStartupDiscoversManagedWalletFilesForAutoload()
{
    const QString pivxSourcePath = resolveQtSourceFile("organiclife.cpp");
    QVERIFY2(!pivxSourcePath.isEmpty(), "Unable to resolve src/qt/organiclife.cpp");
    QFile sourceFile(pivxSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read organiclife.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    const int methodStart = source.indexOf(QStringLiteral("void BitcoinApplication::initializeResult(int retval)"));
    QVERIFY2(methodStart >= 0, "Missing BitcoinApplication::initializeResult implementation");
    const int methodEnd = source.indexOf(QStringLiteral("void BitcoinApplication::handleRunawayException"), methodStart);
    QVERIFY2(methodEnd > methodStart, "Unable to isolate BitcoinApplication::initializeResult implementation");
    const QString methodBody = source.mid(methodStart, methodEnd - methodStart);

    QVERIFY2(methodBody.contains(QStringLiteral("ListWalletDir()")),
             "Qt startup must discover managed wallet files from the wallets directory so migrated wallets appear automatically");
    QVERIFY2(methodBody.contains(QStringLiteral("walletNamesToLoad.removeDuplicates();")),
             "Qt startup must deduplicate discovered and persisted wallet names before loading");
    QVERIFY2(!methodBody.contains(QStringLiteral("const bool hasExplicitWalletArgs = !gArgs.GetArgs(\"-wallet\").empty();")),
             "Qt startup must not treat the implicit default -wallet=\"\" value as an explicit wallet selection");
    QVERIFY2(methodBody.contains(QStringLiteral("walletArgs.size() == 1 && walletArgs[0].empty()")),
             "Qt startup must detect the implicit single empty wallet arg and still allow managed-wallet discovery");
}

void GovernanceDialogTests::qtShutdownPersistsLoadedWalletNamesBeforeWalletTeardown()
{
    const QString pivxSourcePath = resolveQtSourceFile("organiclife.cpp");
    QVERIFY2(!pivxSourcePath.isEmpty(), "Unable to resolve src/qt/organiclife.cpp");
    QFile sourceFile(pivxSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read organiclife.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    const int shutdownStart = source.indexOf(QStringLiteral("void BitcoinApplication::requestShutdown()"));
    QVERIFY2(shutdownStart >= 0, "Missing BitcoinApplication::requestShutdown implementation");
    const int shutdownEnd = source.indexOf(QStringLiteral("void BitcoinApplication::initializeResult(int retval)"), shutdownStart);
    QVERIFY2(shutdownEnd > shutdownStart, "Unable to isolate BitcoinApplication::requestShutdown implementation");
    const QString shutdownBody = source.mid(shutdownStart, shutdownEnd - shutdownStart);

    const int persistIndex = shutdownBody.indexOf(QStringLiteral("window->persistAutoloadWalletNames(window->getWalletNames());"));
    QVERIFY2(persistIndex >= 0,
             "Qt shutdown must persist the currently loaded wallet names before removing the wallet models");
    const int removeIndex = shutdownBody.indexOf(QStringLiteral("window->removeAllWallets();"));
    QVERIFY2(removeIndex >= 0, "Qt shutdown must still tear down loaded wallet models");
    QVERIFY2(persistIndex < removeIndex,
             "Qt shutdown must persist autoload state before the loaded-wallet registry is cleared");
}

void GovernanceDialogTests::aboutDialogUsesFriendlyVersionWhileCliHelpKeepsFullBuildString()
{
    const QString utilityDialogSourcePath = resolveQtSourceFile("utilitydialog.cpp");
    QVERIFY2(!utilityDialogSourcePath.isEmpty(), "Unable to resolve src/qt/utilitydialog.cpp");
    QFile sourceFile(utilityDialogSourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to read utilitydialog.cpp");
    const QString source = QString::fromUtf8(sourceFile.readAll());

    QVERIFY2(source.contains(QStringLiteral("const QString aboutVersion = QString{PACKAGE_NAME} + \" \" + tr(\"version\") + \" \" + QString::fromStdString(FormatVersionFriendly());")),
             "About dialog should use the friendly UI version string");
    QVERIFY2(source.contains(QStringLiteral("const QString cliVersion = QString{PACKAGE_NAME} + \" \" + tr(\"version\") + \" \" + QString::fromStdString(FormatFullVersion());")),
             "CLI help/version output should keep the full build string");
    QVERIFY2(source.contains(QStringLiteral("text = aboutVersion + \"\\n\" + licenseInfo;")),
             "About dialog text should render the friendly version string");
    QVERIFY2(source.contains(QStringLiteral("consoleText = cliVersion + \"\\n\" + licenseInfo;")),
             "Console --version output should keep the full build string even when the GUI About dialog uses the friendly version");
    QVERIFY2(source.contains(QStringLiteral("cursor.insertText(cliVersion);")),
             "Command-line help should still print the full build version");
    QVERIFY2(source.contains(QStringLiteral("fprintf(stdout, \"%s\\n\", qPrintable(consoleText.isEmpty() ? text : consoleText));")),
             "printToConsole should prefer the dedicated console text when one is provided");
}

void GovernanceDialogTests::toastNotificationsDoNotUseFocusableToolWindows()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    mainWindow.messageInfo("toast focus regression check");
    QTest::qWait(20);
    QCoreApplication::processEvents();

    auto* toast = mainWindow.findChild<SnackBar*>();
    QVERIFY(toast != nullptr);
    if (!toast) {
        QFAIL("Missing snackbar instance");
        return;
    }

    const Qt::WindowFlags flags = toast->windowFlags();
    QVERIFY(!flags.testFlag(Qt::Tool));
    QVERIFY(!flags.testFlag(Qt::Dialog));
    QCOMPARE(toast->windowType(), Qt::Widget);
    QVERIFY(flags.testFlag(Qt::FramelessWindowHint));
    QVERIFY(flags.testFlag(Qt::NoDropShadowWindowHint));
    QCOMPARE(toast->focusPolicy(), Qt::NoFocus);
    QVERIFY(toast->testAttribute(Qt::WA_ShowWithoutActivating));
    QVERIFY(!mainWindow.isMinimized());
}

void GovernanceDialogTests::coinModeLayoutHasNoVerticalOverlapWithLargerFonts()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* amountEdit = dialog.findChild<QLineEdit*>("lineEditCoinAmount");
    auto* btnQuarter = dialog.findChild<QPushButton*>("btnCoinAmount25");
    auto* lockableTitle = dialog.findChild<QLabel*>("labelCoinLockableBalanceTitle");
    auto* unlockTitle = dialog.findChild<QLabel*>("labelCoinAutoUnlockTitle");
    auto* lockBadge = dialog.findChild<QLabel*>("labelCoinLockSafety");
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(amountEdit != nullptr);
    QVERIFY(btnQuarter != nullptr);
    QVERIFY(lockableTitle != nullptr);
    QVERIFY(unlockTitle != nullptr);
    QVERIFY(lockBadge != nullptr);
    if (!coinModeRadio || !amountEdit || !btnQuarter || !lockableTitle || !unlockTitle || !lockBadge) {
        QFAIL("Missing controls for overlap regression test");
        return;
    }

    coinModeRadio->setChecked(true);

    QFont font = dialog.font();
    font.setPointSize(font.pointSize() + 6);
    dialog.setFont(font);
    dialog.resize(680, 700);
    dialog.show();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    if (!btnQuarter || !amountEdit || !lockableTitle || !unlockTitle || !lockBadge) {
        QFAIL("Missing controls for overlap geometry assertions");
        return;
    }
    QVERIFY(btnQuarter->geometry().top() >= amountEdit->geometry().bottom());
    QVERIFY(lockableTitle->geometry().top() >= btnQuarter->geometry().bottom());
    QVERIFY(unlockTitle->geometry().top() >= lockableTitle->geometry().bottom());
    QVERIFY(lockBadge->geometry().top() >= unlockTitle->geometry().bottom());
}

void GovernanceDialogTests::coinModeHasComfortableSpacingBeforeActionButtons()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* lockBadge = dialog.findChild<QLabel*>("labelCoinLockSafety");
    auto* btnCancel = dialog.findChild<QPushButton*>("btnCancel");
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(lockBadge != nullptr);
    QVERIFY(btnCancel != nullptr);

    coinModeRadio->setChecked(true);
    dialog.show();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    const int verticalGap = btnCancel->geometry().top() - lockBadge->geometry().bottom();
    QVERIFY(verticalGap >= 16);
}

void GovernanceDialogTests::voteDialogDoesNotForceOversizedMinimumHeight()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    QVERIFY(dialog.minimumHeight() <= 760);
}

void GovernanceDialogTests::voteDialogUsesWholeDialogScrollOnSmallViewport()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));

    auto* scrollArea = dialog.findChild<QScrollArea*>("scrollVoteContent");
    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* btnSave = dialog.findChild<QPushButton*>("btnSave");
    QVERIFY(scrollArea != nullptr);
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(btnSave != nullptr);
    if (!scrollArea || !coinModeRadio || !btnSave) {
        QFAIL("Missing scroll or action controls on vote dialog");
        return;
    }

    coinModeRadio->setChecked(true);

    QFont largeFont = dialog.font();
    largeFont.setPointSize(largeFont.pointSize() + 16);
    dialog.setFont(largeFont);
    dialog.resize(620, 420);
    dialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();
    dialog.resize(620, 320);
    QCoreApplication::processEvents();

    QVERIFY(scrollArea->widgetResizable());
    QVERIFY(scrollArea->widget() != nullptr);
    QVERIFY(scrollArea->widget()->isAncestorOf(btnSave));
    QVERIFY(scrollArea->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded);
    QVERIFY(scrollArea->widget()->sizeHint().height() > scrollArea->viewport()->height());
}

void GovernanceDialogTests::voteDialogFitsInsideSmallParentViewport()
{
    QWidget parent;
    parent.resize(640, 420);
    parent.show();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    QVERIFY(coinModeRadio != nullptr);
    if (!coinModeRadio) {
        QFAIL("Missing coin mode toggle");
        return;
    }
    coinModeRadio->setChecked(true);

    dialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    // Dialog must fit the available parent viewport so footer actions remain reachable.
    QVERIFY(dialog.height() <= parent.height() - 20);
}

void GovernanceDialogTests::voteDialogUsesTrueRoundedWindowMask()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));

    dialog.show();
    QTest::qWait(60);
    QCoreApplication::processEvents();

    AssertRoundedWindowChrome(dialog, true);
}

void GovernanceDialogTests::roundedContainerDialogsUseFramelessChromeAppWide()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);

    VoteDialog voteDialog(&parent, &model, &mnModel);
    voteDialog.show();
    QCoreApplication::processEvents();
    QVERIFY(voteDialog.windowFlags().testFlag(Qt::FramelessWindowHint));

    ProposalInfoDialog infoDialog(&parent);
    infoDialog.setProposal(BuildTestProposal(1));
    infoDialog.show();
    QCoreApplication::processEvents();
    QVERIFY(infoDialog.windowFlags().testFlag(Qt::FramelessWindowHint));

    ReceiveDialog receiveDialog(&parent);
    receiveDialog.show();
    QCoreApplication::processEvents();
    QVERIFY(receiveDialog.windowFlags().testFlag(Qt::FramelessWindowHint));
}

void GovernanceDialogTests::voteDialogHeaderRemainsFixedWhileBodyScrolls()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(560, 340);
    mainWindow.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&mainWindow, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    auto* header = dialog.findChild<QWidget*>("voteDialogHeader");
    auto* scrollArea = dialog.findChild<QScrollArea*>("scrollVoteContent");
    QVERIFY(header != nullptr);
    QVERIFY(scrollArea != nullptr);
    if (!header || !scrollArea) {
        QFAIL("Missing header or scroll area");
        return;
    }

    if (QScrollBar* sb = scrollArea->verticalScrollBar()) {
        const QPoint headerPosBefore = header->mapToGlobal(QPoint(0, 0));
        sb->setValue(sb->maximum());
        QCoreApplication::processEvents();
        const QPoint headerPosAfter = header->mapToGlobal(QPoint(0, 0));
        QCOMPARE(headerPosAfter, headerPosBefore);
    } else {
        QFAIL("Missing vertical scrollbar");
    }
}

void GovernanceDialogTests::voteDialogHeaderDragMovesDialogButBodyDragDoesNot()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(900, 700);
    mainWindow.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&mainWindow, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    auto* header = dialog.findChild<QWidget*>("voteDialogHeader");
    auto* scrollArea = dialog.findChild<QScrollArea*>("scrollVoteContent");
    QVERIFY(header != nullptr);
    QVERIFY(scrollArea != nullptr);
    if (!header || !scrollArea) {
        QFAIL("Missing header or scroll area");
        return;
    }

    const QPoint beforeHeaderDrag = dialog.pos();
    QTest::mousePress(header, Qt::LeftButton, Qt::NoModifier, QPoint(24, std::max(2, header->height() / 2)));
    QTest::mouseMove(header, QPoint(120, std::max(2, header->height() / 2)), 25);
    QTest::mouseRelease(header, Qt::LeftButton, Qt::NoModifier, QPoint(120, std::max(2, header->height() / 2)));
    QCoreApplication::processEvents();
    QVERIFY(dialog.pos() != beforeHeaderDrag);

    const QPoint beforeBodyDrag = dialog.pos();
    QWidget* viewport = scrollArea->viewport();
    QVERIFY(viewport != nullptr);
    if (!viewport) {
        QFAIL("Missing scroll viewport");
        return;
    }
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QTest::mouseMove(viewport, QPoint(120, 20), 25);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, QPoint(120, 20));
    QCoreApplication::processEvents();
    QCOMPARE(dialog.pos(), beforeBodyDrag);
}

void GovernanceDialogTests::voteDialogCloseButtonVisibleAndClickableAfterScroll()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    auto* btnEsc = dialog.findChild<QPushButton*>("btnEsc");
    auto* scrollArea = dialog.findChild<QScrollArea*>("scrollVoteContent");
    auto* header = dialog.findChild<QWidget*>("voteDialogHeader");
    QVERIFY(btnEsc != nullptr);
    QVERIFY(scrollArea != nullptr);
    QVERIFY(header != nullptr);
    if (!btnEsc || !scrollArea || !header) {
        QFAIL("Missing close button/header/scroll area");
        return;
    }

    if (QScrollBar* sb = scrollArea->verticalScrollBar()) {
        sb->setValue(sb->maximum());
    }
    QCoreApplication::processEvents();

    QVERIFY(btnEsc->isVisible());
    QVERIFY(btnEsc->isEnabled());
    QVERIFY(header->rect().contains(btnEsc->geometry().center()));

    QTest::mouseClick(btnEsc, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 1000);
}

void GovernanceDialogTests::voteDialogCloseButtonIconHasVisibleSize()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.show();
    QTest::qWait(60);
    QCoreApplication::processEvents();

    auto* btnEsc = dialog.findChild<QPushButton*>("btnEsc");
    QVERIFY(btnEsc != nullptr);
    if (!btnEsc) {
        QFAIL("Missing close button");
        return;
    }

    QVERIFY(btnEsc->minimumWidth() >= 28);
    QVERIFY(btnEsc->minimumHeight() >= 28);
    QVERIFY(btnEsc->isVisible());
}

void GovernanceDialogTests::voteDialogCloseButtonIconIsResolvedAtRuntime()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    auto* btnEsc = dialog.findChild<QPushButton*>("btnEsc");
    QVERIFY(btnEsc != nullptr);
    if (!btnEsc) {
        QFAIL("Missing close button");
        return;
    }

    QVERIFY(!btnEsc->icon().isNull());
    QVERIFY(btnEsc->iconSize().width() >= 18);
    QVERIFY(btnEsc->iconSize().height() >= 18);
}

void GovernanceDialogTests::proposalInfoDialogUsesTrueRoundedWindowMask()
{
    QWidget parent;
    ProposalInfoDialog dialog(&parent);
    dialog.setProposal(BuildTestProposal(1));

    dialog.show();
    QTest::qWait(60);
    QCoreApplication::processEvents();

    AssertRoundedWindowChrome(dialog, true);
}

void GovernanceDialogTests::receiveDialogUsesTrueRoundedWindowMask()
{
    QWidget parent;
    ReceiveDialog dialog(&parent);

    dialog.show();
    QTest::qWait(60);
    QCoreApplication::processEvents();

    AssertRoundedWindowChrome(dialog, true);
}

void GovernanceDialogTests::containerDialogCssRadiusIs16InBothThemes()
{
    const auto cssHas16 = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression re(
            R"(\*\[cssClass=\"container-dialog\"\]\s*\{[^\}]*border-radius\s*:\s*16px\s*;)",
            QRegularExpression::DotMatchesEverythingOption
        );
        return re.match(css).hasMatch();
    };
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY(cssHas16(":/css/default") || cssHas16(lightSource));
    QVERIFY(cssHas16(":/css/default-dark") || cssHas16(darkSource));
}

void GovernanceDialogTests::containerDialogsEnableTranslucentBackgroundForRoundedCorners()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(1));

    dialog.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    AssertRoundedWindowChrome(dialog, true);
}

void GovernanceDialogTests::allDialogsUseContainerDialogBaseClass()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString qtSourceDir = appDir.absoluteFilePath("../../../../src/qt");

    const auto readSource = [](const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
        return QString::fromUtf8(file.readAll());
    };

    const QString focusedHeader = readSource(qtSourceDir + "/focuseddialog.h");
    QVERIFY2(!focusedHeader.isEmpty(), "Unable to read focuseddialog.h");
    QVERIFY2(QRegularExpression(R"(class\s+FocusedDialog\s*:\s*public\s+ContainerDialog)").match(focusedHeader).hasMatch(),
             "FocusedDialog must inherit ContainerDialog");

    const QStringList dialogHeaders = QDir(qtSourceDir).entryList(QStringList() << "*dialog.h", QDir::Files);
    QVERIFY2(!dialogHeaders.isEmpty(), "No dialog headers found");

    const QRegularExpression disallowedDirectBase(R"(class\s+\w+\s*:\s*public\s+QDialog\b)");
    for (const QString& header : dialogHeaders) {
        if (header == "containerdialog.h") continue;
        const QString content = readSource(qtSourceDir + "/" + header);
        QVERIFY2(!content.isEmpty(), qPrintable(QString("Unable to read %1").arg(header)));
        QVERIFY2(!disallowedDirectBase.match(content).hasMatch(),
                 qPrintable(QString("Dialog header still inherits QDialog directly: %1").arg(header)));
    }
}

void GovernanceDialogTests::containerDialogsUseSharedDraggableHeaderChrome()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString qtSourceDir = appDir.absoluteFilePath("../../../../src/qt");

    const auto readSource = [](const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
        return QString::fromUtf8(file.readAll());
    };

    const QStringList expectedHeaderChromeFiles{
        "addnewcontactdialog.cpp",
        "askpassphrasedialog.cpp",
        "createproposaldialog.cpp",
        "defaultdialog.cpp",
        "masternodewizarddialog.cpp",
        "mninfodialog.cpp",
        "mnselectiondialog.cpp",
        "openuridialog.cpp",
        "proposalinfodialog.cpp",
        "receivedialog.cpp",
        "requestdialog.cpp",
        "sendchangeaddressdialog.cpp",
        "sendconfirmdialog.cpp",
        "sendcustomfeedialog.cpp",
        "sendmemodialog.cpp"
    };

    for (const QString& sourceFile : expectedHeaderChromeFiles) {
        const QString source = readSource(qtSourceDir + "/" + sourceFile);
        QVERIFY2(!source.isEmpty(), qPrintable(QString("Unable to read %1").arg(sourceFile)));
        QVERIFY2(source.contains("initDraggableHeaderChrome("),
                 qPrintable(QString("Missing shared header chrome in %1").arg(sourceFile)));
    }

    const QStringList dialogSources = QDir(qtSourceDir).entryList(QStringList() << "*dialog.cpp", QDir::Files);
    QVERIFY2(!dialogSources.isEmpty(), "No dialog cpp files found");

    for (const QString& sourceFile : dialogSources) {
        if (sourceFile == "votedialog.cpp") continue;
        const QString source = readSource(qtSourceDir + "/" + sourceFile);
        QVERIFY2(!source.isEmpty(), qPrintable(QString("Unable to read %1").arg(sourceFile)));
        QVERIFY2(!source.contains("initRoundedContainerFrame("),
                 qPrintable(QString("Dialog still uses legacy rounded-frame path: %1").arg(sourceFile)));
    }
}

void GovernanceDialogTests::dialogIconClassesResolveToExistingQrcAliases()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString qtSourceDir = appDir.absoluteFilePath("../../../../src/qt");

    const auto readSource = [](const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
        return QString::fromUtf8(file.readAll());
    };

    const QString qrcSource = readSource(qtSourceDir + "/organiclife.qrc");
    QVERIFY2(!qrcSource.isEmpty(), "Unable to read organiclife.qrc");

    std::set<QString> qrcAliases;
    QRegularExpressionMatchIterator aliasIt =
            QRegularExpression(R"(alias=\"([^\"]+)\")").globalMatch(qrcSource);
    while (aliasIt.hasNext()) {
        qrcAliases.insert(aliasIt.next().captured(1));
    }
    QVERIFY2(!qrcAliases.empty(), "No qrc aliases found");

    const QString allCss = readSource(qtSourceDir + "/res/css/default.css") +
                           "\n" + readSource(qtSourceDir + "/res/css/style_light.css") +
                           "\n" + readSource(qtSourceDir + "/res/css/style_dark.css");
    QVERIFY2(!allCss.isEmpty(), "Unable to read CSS files");

    std::set<QString> iconClasses;
    const QStringList dialogSources = QDir(qtSourceDir).entryList(QStringList() << "*dialog.cpp", QDir::Files);
    QVERIFY2(!dialogSources.isEmpty(), "No dialog cpp files found");

    const QRegularExpression iconClassLiteral(R"(\"(ic-[A-Za-z0-9\-]+)\")");
    for (const QString& sourceFile : dialogSources) {
        const QString source = readSource(qtSourceDir + "/" + sourceFile);
        QVERIFY2(!source.isEmpty(), qPrintable(QString("Unable to read %1").arg(sourceFile)));
        QRegularExpressionMatchIterator matchIt = iconClassLiteral.globalMatch(source);
        while (matchIt.hasNext()) {
            iconClasses.insert(matchIt.next().captured(1));
        }
    }

    QVERIFY2(!iconClasses.empty(), "No icon css classes discovered in dialog sources");

    for (const QString& iconClass : iconClasses) {
        const QRegularExpression cssIconRef(
            QString(R"(\[cssClass=\"%1\"\][^\{]*\{[^\}]*(?:qproperty-icon|background-image)\s*:\s*url\(\"(:/{1,2}[^\"]+)\"\))")
                    .arg(QRegularExpression::escape(iconClass)),
            QRegularExpression::DotMatchesEverythingOption
        );

        QRegularExpressionMatch match = cssIconRef.match(allCss);
        QVERIFY2(match.hasMatch(),
                 qPrintable(QString("Missing icon CSS mapping for class %1").arg(iconClass)));
        if (!match.hasMatch()) continue;

        QString iconPath = match.captured(1);
        while (iconPath.startsWith(":/")) {
            iconPath.remove(0, 2);
        }
        while (iconPath.startsWith('/')) {
            iconPath.remove(0, 1);
        }
        QVERIFY2(qrcAliases.count(iconPath) == 1,
                 qPrintable(QString("CSS icon path for class %1 resolves to missing qrc alias %2")
                                    .arg(iconClass)
                                    .arg(iconPath)));
    }
}

void GovernanceDialogTests::roundedContainerDialogsAutoSizeToVisibleContent()
{
    QWidget parent;

    AskPassphraseDialog unlockDialog(
            AskPassphraseDialog::Mode::Unlock,
            &parent,
            nullptr,
            AskPassphraseDialog::Context::Unlock_Menu);
    unlockDialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();
    const int unlockHeight = unlockDialog.height();
    const int unlockWidth = unlockDialog.width();
    unlockDialog.close();

    AskPassphraseDialog changeDialog(
            AskPassphraseDialog::Mode::ChangePass,
            &parent,
            nullptr,
            AskPassphraseDialog::Context::ChangePass);
    changeDialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();
    const int changeHeight = changeDialog.height();
    const int changeWidth = changeDialog.width();
    changeDialog.close();

    QVERIFY2(unlockHeight + 80 < changeHeight,
             qPrintable(QString("Unlock dialog should auto-size smaller than change-pass dialog (unlock=%1, change=%2)")
                                .arg(unlockHeight)
                                .arg(changeHeight)));
    QVERIFY2(unlockWidth + 40 < changeWidth,
             qPrintable(QString("Unlock dialog width should shrink for smaller content (unlock=%1, change=%2)")
                                .arg(unlockWidth)
                                .arg(changeWidth)));
}

void GovernanceDialogTests::unlockWalletDialogHeaderShowsExpectedTitleAndClass()
{
    QWidget parent;
    AskPassphraseDialog unlockDialog(
            AskPassphraseDialog::Mode::Unlock,
            &parent,
            nullptr,
            AskPassphraseDialog::Context::Unlock_Menu);
    unlockDialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    auto* titleLabel = unlockDialog.findChild<QLabel*>("labelTitle");
    auto* header = unlockDialog.findChild<QWidget*>("containerDialogHeader");
    QVERIFY(titleLabel != nullptr);
    QVERIFY(header != nullptr);
    if (!titleLabel || !header) {
        QFAIL("Unlock dialog header/title widgets are missing");
        return;
    }

    QCOMPARE(titleLabel->text(), QString("Unlock wallet"));
    QCOMPARE(titleLabel->property("cssClass").toString(), QString("text-title-dialog"));
    QCOMPARE(titleLabel->parentWidget(), header);
}

void GovernanceDialogTests::unlockWalletDialogWatchToggleIsFullyVisible()
{
    QWidget parent;
    AskPassphraseDialog unlockDialog(
            AskPassphraseDialog::Mode::Unlock,
            &parent,
            nullptr,
            AskPassphraseDialog::Context::Unlock_Menu);
    unlockDialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    auto* watchToggle = unlockDialog.findChild<QCheckBox*>("btnWatchPassword");
    QVERIFY(watchToggle != nullptr);
    if (!watchToggle) {
        QFAIL("Unlock dialog watch toggle not found");
        return;
    }

    const QRect toggleInDialog(
            watchToggle->mapTo(&unlockDialog, QPoint(0, 0)),
            watchToggle->size());
    QVERIFY2(unlockDialog.rect().contains(toggleInDialog.center()),
             qPrintable(QString("Watch toggle is not fully anchored inside dialog bounds (%1x%2 at %3,%4 in %5x%6)")
                                .arg(toggleInDialog.width())
                                .arg(toggleInDialog.height())
                                .arg(toggleInDialog.x())
                                .arg(toggleInDialog.y())
                                .arg(unlockDialog.width())
                                .arg(unlockDialog.height())));
    QVERIFY2(unlockDialog.width() >= 460,
             qPrintable(QString("Unlock dialog width is too narrow for watch toggle visibility (%1)")
                                .arg(unlockDialog.width())));
}

void GovernanceDialogTests::roundedContainerDialogsUsePopAnimationCurve()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString qtUtilsSource = appDir.absoluteFilePath("../../../../src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    const int bindStart = source.indexOf(QStringLiteral("void bindRoundedDialogMask("));
    const int bindEnd = source.indexOf(QStringLiteral("QPixmap encodeToQr("), bindStart);
    QVERIFY(bindStart >= 0);
    QVERIFY(bindEnd > bindStart);
    if (bindStart < 0 || bindEnd <= bindStart) {
        QFAIL("bindRoundedDialogMask function slice not found");
        return;
    }

    const QString bindSlice = source.mid(bindStart, bindEnd - bindStart);
    QVERIFY(bindSlice.contains(QStringLiteral("setDialogRoundedFramelessMode(dialog, true);")));

    QVERIFY2(source.contains(QStringLiteral("QEasingCurve::OutBack")),
             "Rounded dialog pop animation must use an OutBack bounce easing curve");
}

void GovernanceDialogTests::roundedContainerDialogsStartPopAnimationWithoutQueuedShowDelay()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    const int showCaseStart = source.indexOf(QStringLiteral("case QEvent::Show:"));
    const int showCaseBreak = source.indexOf(QStringLiteral("break;"), showCaseStart);
    QVERIFY(showCaseStart >= 0);
    QVERIFY(showCaseBreak > showCaseStart);
    if (showCaseStart < 0 || showCaseBreak <= showCaseStart) {
        QFAIL("RoundedMaskBinder show-event case not found");
        return;
    }

    const QString showCase = source.mid(showCaseStart, showCaseBreak - showCaseStart);
    QVERIFY2(showCase.contains(QStringLiteral("fitDialogToContents(m_dialog);")),
             "Show-event path should size dialog immediately");
    QVERIFY2(showCase.contains(QStringLiteral("applyRoundedDialogMask(m_dialog, m_contentFrame, m_radiusPx);")),
             "Show-event path should apply rounded mask immediately");
    QVERIFY2(showCase.contains(QStringLiteral("runDialogPopAnimation(m_dialog, m_contentFrame);")),
             "Show-event path should start pop animation immediately");
    QVERIFY2(!showCase.contains(QStringLiteral("Qt::QueuedConnection")),
             "Show-event pop startup must not be queued to next event-loop turn");
}

void GovernanceDialogTests::roundedContainerDialogsPreparePolishedGeometryBeforeShow()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    const QString startToken = QStringLiteral("void prepareDialogBeforeShow(QDialog* dialog)");
    const QString endToken = QStringLiteral("} // namespace");
    const int start = source.indexOf(startToken);
    const int end = source.indexOf(endToken, start);
    QVERIFY(start >= 0);
    QVERIFY(end > start);
    if (start < 0 || end <= start) {
        QFAIL("prepareDialogBeforeShow function not found");
        return;
    }

    const QString functionSlice = source.mid(start, end - start);
    QVERIFY2(functionSlice.contains(QStringLiteral("dialog->ensurePolished();")),
             "prepareDialogBeforeShow must ensure polishing before sizing");
    QVERIFY2(functionSlice.contains(QStringLiteral("fitDialogToContents(dialog);")),
             "prepareDialogBeforeShow must fit dialog geometry before show");
}

void GovernanceDialogTests::roundedContainerDialogsUseContentOpacityFadeInsteadOfTopLevelWindowOpacity()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    const QString startToken = QStringLiteral("void runDialogPopAnimation(QDialog* dialog, QWidget* fallbackTarget)");
    const QString endToken = QStringLiteral("Qt::WindowFlags dialogFlagsFor(const QDialog* dialog)");
    const int start = source.indexOf(startToken);
    const int end = source.indexOf(endToken, start);
    QVERIFY(start >= 0);
    QVERIFY(end > start);
    if (start < 0 || end <= start) {
        QFAIL("runDialogPopAnimation function not found");
        return;
    }

    const QString functionSlice = source.mid(start, end - start);
    QVERIFY2(functionSlice.contains(QStringLiteral("DialogScaleFadeEffect")),
             "Pop path should use a single shared scale/fade effect for whole-dialog animation");
    QVERIFY2(!functionSlice.contains(QStringLiteral("QPropertyAnimation(dialog, \"windowOpacity\"")),
             "Pop animation must not animate top-level windowOpacity");
    QVERIFY2(!functionSlice.contains(QStringLiteral("dialog->setWindowOpacity(0.0)")),
             "Pop animation must not force top-level dialog opacity to zero");
}

void GovernanceDialogTests::roundedContainerDialogsAvoidMoveTriggeredMaskReapply()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    const int switchStart = source.indexOf(QStringLiteral("switch (event->type())"));
    const int switchEnd = source.indexOf(QStringLiteral("return QObject::eventFilter(watched, event);"), switchStart);
    QVERIFY(switchStart >= 0);
    QVERIFY(switchEnd > switchStart);
    if (switchStart < 0 || switchEnd <= switchStart) {
        QFAIL("RoundedMaskBinder event switch not found");
        return;
    }

    const QString switchSlice = source.mid(switchStart, switchEnd - switchStart);
    QVERIFY2(!switchSlice.contains(QStringLiteral("case QEvent::Move:")),
             "Rounded dialog mask should not be reapplied on move events");
}

void GovernanceDialogTests::roundedContainerDialogsExposePerformanceModeSwitchInQtUtils()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    QVERIFY2(source.contains(QStringLiteral("kDialogPerformanceModeProperty")),
             "qtutils should define a dialog performance-mode property for smoothness tuning");
    QVERIFY2(source.contains(QStringLiteral("bool dialogPerformanceModeEnabled(const QDialog* dialog)")),
             "qtutils should expose runtime performance-mode evaluation for dialog animations");
    QVERIFY2(source.contains(QStringLiteral("void setDialogPerformanceMode(QDialog* dialog, bool enabled)")),
             "qtutils should expose a setter for per-dialog performance-mode override");
}

void GovernanceDialogTests::roundedContainerDialogsSkipOpacityEffectInPerformanceMode()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    const QString startToken = QStringLiteral("void runDialogPopAnimation(QDialog* dialog, QWidget* fallbackTarget)");
    const QString endToken = QStringLiteral("Qt::WindowFlags dialogFlagsFor(const QDialog* dialog)");
    const int start = source.indexOf(startToken);
    const int end = source.indexOf(endToken, start);
    QVERIFY(start >= 0);
    QVERIFY(end > start);
    if (start < 0 || end <= start) {
        QFAIL("runDialogPopAnimation function not found");
        return;
    }

    const QString functionSlice = source.mid(start, end - start);
    QVERIFY2(functionSlice.contains(QStringLiteral("const bool performanceMode = dialogPerformanceModeEnabled(dialog);")),
             "runDialogPopAnimation should evaluate performance mode once per run");
    QVERIFY2(functionSlice.contains(QStringLiteral("ensureDialogScaleFadeEffect(target)")),
             "runDialogPopAnimation should apply unified scale/fade effect on one animation host");
}

void GovernanceDialogTests::roundedContainerDialogsEmitFramePacingDiagnostics()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    QVERIFY2(source.contains(QStringLiteral("PIVX_QT_DIALOG_ANIM_TRACE")),
             "qtutils should provide env toggle for dialog frame pacing diagnostics");
    QVERIFY2(source.contains(QStringLiteral("ui.anim.dialog.stall")),
             "qtutils should emit frame-stall warnings for dialog animation pacing");
    QVERIFY2(source.contains(QStringLiteral("ui.anim.dialog.summary")),
             "qtutils should emit frame pacing summary after dialog pop animation");
}

void GovernanceDialogTests::roundedContainerDialogsKeepCenterLockedThroughoutPopFrames()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    QVERIFY2(!source.contains(QStringLiteral("void lockDialogWindowCenter(QDialog* dialog, const QPoint& lockedCenter)")),
             "Pop/close animation should not force per-frame dialog move recentering");
    const QString startToken = QStringLiteral("void runDialogPopAnimation(QDialog* dialog, QWidget* fallbackTarget)");
    const QString endToken = QStringLiteral("bool runDialogCloseAnimation(QDialog* dialog, QWidget* fallbackTarget)");
    const int start = source.indexOf(startToken);
    const int end = source.indexOf(endToken, start);
    QVERIFY(start >= 0);
    QVERIFY(end > start);
    if (start < 0 || end <= start) {
        QFAIL("runDialogPopAnimation function not found");
        return;
    }

    const QString functionSlice = source.mid(start, end - start);
    QVERIFY2(!functionSlice.contains(QStringLiteral("dialog->move(")),
             "runDialogPopAnimation should not reposition dialog while animating");
}

void GovernanceDialogTests::roundedContainerDialogsPerformanceModeAnimatesWholeDialogAsSingleUnit()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    const QString startToken = QStringLiteral("void runDialogPopAnimation(QDialog* dialog, QWidget* fallbackTarget)");
    const QString endToken = QStringLiteral("Qt::WindowFlags dialogFlagsFor(const QDialog* dialog)");
    const int start = source.indexOf(startToken);
    const int end = source.indexOf(endToken, start);
    QVERIFY(start >= 0);
    QVERIFY(end > start);
    if (start < 0 || end <= start) {
        QFAIL("runDialogPopAnimation function not found");
        return;
    }

    const QString functionSlice = source.mid(start, end - start);
    QVERIFY2(functionSlice.contains(QStringLiteral("const auto timing = dialogAnimationTiming(performanceMode);")),
             "Performance mode should use shared timing presets for open/close consistency");
    QVERIFY2(functionSlice.contains(QStringLiteral("DialogScaleFadeEffect* effect = ensureDialogScaleFadeEffect(target);")),
             "Performance mode should animate one top-level host via shared effect");
    QVERIFY2(functionSlice.contains(QStringLiteral("QEasingCurve popCurve(QEasingCurve::OutBack);")),
             "Performance mode pop should use an overshooting back easing curve");
    QVERIFY2(functionSlice.contains(QStringLiteral("popCurve.setOvershoot(timing.overshoot);")),
             "Performance mode pop should use configured overshoot value from shared timing");
    QVERIFY2(!functionSlice.contains(QStringLiteral("QPropertyAnimation(dialog, \"geometry\"")),
             "Performance mode should not resize dialog geometry during pop");
    QVERIFY2(!functionSlice.contains(QStringLiteral("target->setGeometry(")),
             "Performance mode should avoid host geometry writes during animation");
}

void GovernanceDialogTests::roundedContainerDialogsNonPerformanceModeAnimatesWholeDialogAndUsesLongerDurations()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    const QString startToken = QStringLiteral("void runDialogPopAnimation(QDialog* dialog, QWidget* fallbackTarget)");
    const QString endToken = QStringLiteral("Qt::WindowFlags dialogFlagsFor(const QDialog* dialog)");
    const int start = source.indexOf(startToken);
    const int end = source.indexOf(endToken, start);
    QVERIFY(start >= 0);
    QVERIFY(end > start);
    if (start < 0 || end <= start) {
        QFAIL("runDialogPopAnimation function not found");
        return;
    }

    const QString functionSlice = source.mid(start, end - start);
    QVERIFY2(functionSlice.contains(QStringLiteral("const auto timing = dialogAnimationTiming(performanceMode);")),
             "Non-performance mode should run the longer scale duration");
    QVERIFY2(source.contains(QStringLiteral("const DialogAnimationTiming kDefaultDialogAnimationTiming")),
             "Shared animation timing table should be declared once for consistency");
    QVERIFY2(source.contains(QStringLiteral("{370, 300, 0.75}")),
             "Performance timing preset should include reduced overshoot");
    QVERIFY2(source.contains(QStringLiteral("{440, 340, 0.75}")),
             "Standard timing preset should include reduced overshoot");
    QVERIFY2(functionSlice.contains(QStringLiteral("timing.opacityMs")),
             "Non-performance mode should run the longer opacity duration");
    QVERIFY2(functionSlice.contains(QStringLiteral("QVariantAnimation")),
             "Non-performance mode should use smooth property animations instead of geometry resize");
    QVERIFY2(!functionSlice.contains(QStringLiteral("QPropertyAnimation(target, \"geometry\"")),
             "Non-performance mode should avoid geometry-based pop");
}

void GovernanceDialogTests::roundedContainerDialogsAnimateCloseAsReverseWholeDialogPop()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    QVERIFY2(source.contains(QStringLiteral("case QEvent::Close:")),
             "Rounded dialog binder should intercept close events to run reverse animation");
    QVERIFY2(source.contains(QStringLiteral("QEasingCurve::InBack")),
             "Close animation should use reverse back easing for polished pop-out");
    QVERIFY2(source.contains(QStringLiteral("dialog->done(pendingResult);")),
             "Close animation should preserve and apply dialog result after reverse animation");
}

void GovernanceDialogTests::opaqueBackgroundDialogOpenDoesNotAnimateSlideUp()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString qtUtilsSource = appDir.absoluteFilePath("../../../../src/qt/qtutils.cpp");
    QFile f(qtUtilsSource);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString source = QString::fromUtf8(f.readAll());

    const QString startToken = QStringLiteral("bool openDialogWithOpaqueBackgroundY(");
    const QString endToken = QStringLiteral("bool openDialogWithOpaqueBackground(");
    const int start = source.indexOf(startToken);
    QVERIFY(start >= 0);
    if (start < 0) {
        QFAIL("openDialogWithOpaqueBackgroundY function not found");
        return;
    }
    const int end = source.indexOf(endToken, start + startToken.size());
    QVERIFY(end > start);
    if (end <= start) {
        QFAIL("openDialogWithOpaqueBackgroundY function end token not found");
        return;
    }

    const QString functionSlice = source.mid(start, end - start);
    QVERIFY(!functionSlice.contains("QPropertyAnimation"));
}

void GovernanceDialogTests::containerDialogsAvoidQueuedRecenteringAfterShow()
{
    const QString qtUtilsSource = resolveQtSourceFile("qtutils.cpp");
    QVERIFY2(!qtUtilsSource.isEmpty(), "Unable to resolve src/qt/qtutils.cpp");
    QFile qtUtilsFile(qtUtilsSource);
    QVERIFY2(qtUtilsFile.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(QString("Unable to read %1").arg(qtUtilsSource)));
    const QString qtUtils = QString::fromUtf8(qtUtilsFile.readAll());

    const QString helperStartToken = QStringLiteral("bool openDialogWithOpaqueBackgroundY(");
    const QString helperEndToken = QStringLiteral("bool openDialogWithOpaqueBackground(");
    const int helperStart = qtUtils.indexOf(helperStartToken);
    QVERIFY(helperStart >= 0);
    if (helperStart < 0) {
        QFAIL("openDialogWithOpaqueBackgroundY function not found");
        return;
    }
    const int helperEnd = qtUtils.indexOf(helperEndToken, helperStart + helperStartToken.size());
    QVERIFY(helperEnd > helperStart);
    if (helperEnd <= helperStart) {
        QFAIL("openDialogWithOpaqueBackgroundY function end token not found");
        return;
    }

    const QString helperSlice = qtUtils.mid(helperStart, helperEnd - helperStart);
    QVERIFY2(!helperSlice.contains(QStringLiteral("Qt::QueuedConnection")),
             "openDialogWithOpaqueBackgroundY should not queue position corrections after show");
    QVERIFY2(!helperSlice.contains(QStringLiteral("QMetaObject::invokeMethod")),
             "openDialogWithOpaqueBackgroundY should not asynchronously reposition dialogs");
    QVERIFY2(helperSlice.contains(QStringLiteral("if (dialogOwnsOpenPosition(widget))")),
             "openDialogWithOpaqueBackgroundY should have a dedicated own-position centering path");
    QVERIFY2(helperSlice.contains(QStringLiteral("widget->move(anchorRect.center() - QPoint(widget->width() / 2, widget->height() / 2));")),
             "openDialogWithOpaqueBackgroundY should center own-position dialogs before show");

    const QString binderStartToken = QStringLiteral("class RoundedMaskBinder final : public QObject");
    const QString binderEndToken = QStringLiteral("void applyRoundedDialogMask(QDialog* dialog, QWidget* contentFrame, int radiusPx)");
    const int binderStart = qtUtils.indexOf(binderStartToken);
    const int binderEnd = qtUtils.indexOf(binderEndToken, binderStart);
    QVERIFY(binderStart >= 0);
    QVERIFY(binderEnd > binderStart);
    if (binderStart < 0 || binderEnd <= binderStart) {
        QFAIL("RoundedMaskBinder class not found");
        return;
    }
    const QString binderSlice = qtUtils.mid(binderStart, binderEnd - binderStart);
    QVERIFY2(!binderSlice.contains(QStringLiteral("m_dialog->move(anchorRect.center() - QPoint(m_dialog->width() / 2, m_dialog->height() / 2));")),
             "RoundedMaskBinder should not recenter in show event; ContainerDialog owns centering");

    const QString containerSourcePath = resolveQtSourceFile("containerdialog.h");
    QVERIFY2(!containerSourcePath.isEmpty(), "Unable to resolve src/qt/containerdialog.h");
    QFile containerFile(containerSourcePath);
    QVERIFY2(containerFile.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(QString("Unable to read %1").arg(containerSourcePath)));
    const QString containerSource = QString::fromUtf8(containerFile.readAll());

    const QString showStartToken = QStringLiteral("void showEvent(QShowEvent* event) override");
    const QString hideStartToken = QStringLiteral("void hideEvent(QHideEvent* event) override");
    const int showStart = containerSource.indexOf(showStartToken);
    const int hideStart = containerSource.indexOf(hideStartToken, showStart);
    QVERIFY(showStart >= 0);
    QVERIFY(hideStart > showStart);
    if (showStart < 0 || hideStart <= showStart) {
        QFAIL("ContainerDialog::showEvent function not found");
        return;
    }

    const QString showSlice = containerSource.mid(showStart, hideStart - showStart);
    QVERIFY2(showSlice.contains(QStringLiteral("centerOnParentOrScreen();")),
             "ContainerDialog::showEvent should center immediately");
    QVERIFY2(!showSlice.contains(QStringLiteral("Qt::QueuedConnection")),
             "ContainerDialog::showEvent should not queue recentering");
    QVERIFY2(!showSlice.contains(QStringLiteral("QMetaObject::invokeMethod")),
             "ContainerDialog::showEvent should not asynchronously reposition");
}

void GovernanceDialogTests::voteDialogOpensFullyInsideSmallParentViewport()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(640, 420);
    mainWindow.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&mainWindow, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));

    auto* btnSave = dialog.findChild<QPushButton*>("btnSave");
    auto* btnCancel = dialog.findChild<QPushButton*>("btnCancel");
    auto* scrollArea = dialog.findChild<QScrollArea*>("scrollVoteContent");
    QVERIFY(btnSave != nullptr);
    QVERIFY(btnCancel != nullptr);
    QVERIFY(scrollArea != nullptr);
    if (!btnSave || !btnCancel || !scrollArea) {
        QFAIL("Missing vote dialog footer controls");
        return;
    }

    bool footerReachable = false;
    bool checkerExecuted = false;
    QTimer::singleShot(120, &dialog, [&]() {
        checkerExecuted = true;
        if (scrollArea->verticalScrollBar()) {
            scrollArea->verticalScrollBar()->setValue(scrollArea->verticalScrollBar()->maximum());
            QCoreApplication::processEvents();
        }

        const QRect saveRectInViewport(btnSave->mapTo(scrollArea->viewport(), QPoint(0, 0)), btnSave->size());
        const QRect cancelRectInViewport(btnCancel->mapTo(scrollArea->viewport(), QPoint(0, 0)), btnCancel->size());
        const QRect viewportRect = scrollArea->viewport()->rect();
        footerReachable = viewportRect.intersects(saveRectInViewport) || viewportRect.intersects(cancelRectInViewport);
        dialog.reject();
    });

    (void)openDialogWithOpaqueBackgroundY(&dialog, &mainWindow, 3, 5, false);
    QVERIFY(checkerExecuted);
    QVERIFY(footerReachable);
}

void GovernanceDialogTests::voteDialogFooterReachableWithDpiScaledFonts()
{
    const QFont originalAppFont = QApplication::font();
    struct FontRestorer {
        QFont font;
        ~FontRestorer() { QApplication::setFont(font); }
    } restorer{originalAppFont};

    QFont scaledFont = originalAppFont;
    const qreal basePointSize = originalAppFont.pointSizeF() > 0.0 ? originalAppFont.pointSizeF() : 10.0;
    scaledFont.setPointSizeF(basePointSize * 1.25);
    QApplication::setFont(scaledFont);

    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(560, 340);
    mainWindow.show();
    QTest::qWait(30);
    QCoreApplication::processEvents();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&mainWindow, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* btnSave = dialog.findChild<QPushButton*>("btnSave");
    auto* btnCancel = dialog.findChild<QPushButton*>("btnCancel");
    auto* scrollArea = dialog.findChild<QScrollArea*>("scrollVoteContent");
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(btnSave != nullptr);
    QVERIFY(btnCancel != nullptr);
    QVERIFY(scrollArea != nullptr);
    if (!coinModeRadio || !btnSave || !btnCancel || !scrollArea) {
        QFAIL("Missing vote dialog controls for DPI footer reachability test");
        return;
    }

    coinModeRadio->setChecked(true);

    bool checkerExecuted = false;
    bool footerReachable = false;
    QTimer::singleShot(160, &dialog, [&]() {
        checkerExecuted = true;
        if (QScrollBar* sb = scrollArea->verticalScrollBar()) {
            sb->setValue(sb->maximum());
            QCoreApplication::processEvents();
        }

        const QRect saveRectInViewport(btnSave->mapTo(scrollArea->viewport(), QPoint(0, 0)), btnSave->size());
        const QRect cancelRectInViewport(btnCancel->mapTo(scrollArea->viewport(), QPoint(0, 0)), btnCancel->size());
        const QRect viewportRect = scrollArea->viewport()->rect();
        footerReachable = viewportRect.intersects(saveRectInViewport) || viewportRect.intersects(cancelRectInViewport);
        dialog.reject();
    });

    (void)openDialogWithOpaqueBackgroundY(&dialog, &mainWindow, 3, 5, false);
    QVERIFY(checkerExecuted);
    QVERIFY(footerReachable);
}

void GovernanceDialogTests::voteDialogHeaderAndBodyDoNotOverlapAtScaledFonts()
{
    const QFont originalAppFont = QApplication::font();
    struct FontRestorer {
        QFont font;
        ~FontRestorer() { QApplication::setFont(font); }
    } restorer{originalAppFont};

    QFont scaledFont = originalAppFont;
    const qreal basePointSize = originalAppFont.pointSizeF() > 0.0 ? originalAppFont.pointSizeF() : 10.0;
    scaledFont.setPointSizeF(basePointSize * 1.25);
    QApplication::setFont(scaledFont);

    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(520, 300);
    mainWindow.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&mainWindow, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));

    auto* header = dialog.findChild<QWidget*>("voteDialogHeader");
    auto* scrollArea = dialog.findChild<QScrollArea*>("scrollVoteContent");
    auto* btnSave = dialog.findChild<QPushButton*>("btnSave");
    QVERIFY(header != nullptr);
    QVERIFY(scrollArea != nullptr);
    QVERIFY(btnSave != nullptr);
    if (!header || !scrollArea || !btnSave) {
        QFAIL("Missing header, scroll area, or save button");
        return;
    }

    bool checkerExecuted = false;
    bool footerReachable = false;
    QRect headerRect;
    QRect scrollRect;
    QTimer::singleShot(160, &dialog, [&]() {
        checkerExecuted = true;
        headerRect = header->geometry();
        scrollRect = scrollArea->geometry();
        if (QScrollBar* sb = scrollArea->verticalScrollBar()) {
            sb->setValue(sb->maximum());
            QCoreApplication::processEvents();
        }

        const QRect saveRectInViewport(btnSave->mapTo(scrollArea->viewport(), QPoint(0, 0)), btnSave->size());
        footerReachable = scrollArea->viewport()->rect().intersects(saveRectInViewport);
        dialog.reject();
    });

    (void)openDialogWithOpaqueBackgroundY(&dialog, &mainWindow, 3, 5, false);
    QVERIFY(checkerExecuted);
    QVERIFY(headerRect.bottom() <= scrollRect.top());
    QVERIFY(footerReachable);
}

void GovernanceDialogTests::voteDialogUsesWiderLayoutAndNoHorizontalListScrollbar()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(1280, 900);
    mainWindow.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&mainWindow, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    auto* list = dialog.findChild<QListWidget*>("listMasternodesInline");
    QVERIFY(list != nullptr);
    if (!list) {
        QFAIL("Missing masternodes list");
        return;
    }

    QVERIFY(dialog.width() >= 800);
    QCOMPARE(list->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
}

void GovernanceDialogTests::voteDialogAutoExpandsToEliminateBodyHorizontalOverflow()
{
    const QFont originalFont = QApplication::font();
    QFont scaledFont = originalFont;
    scaledFont.setPointSizeF((originalFont.pointSizeF() > 0.0 ? originalFont.pointSizeF() : 10.0) * 1.25);
    QApplication::setFont(scaledFont);

    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QApplication::setFont(originalFont);
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(980, 720);
    mainWindow.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&mainWindow, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.resize(680, 620);
    dialog.show();
    QTest::qWait(120);
    QCoreApplication::processEvents();

    auto* scrollArea = dialog.findChild<QScrollArea*>("scrollVoteContent");
    auto* bodyFrame = dialog.findChild<QFrame*>("frame");
    auto* shell = dialog.findChild<QWidget*>("voteDialogShell");
    auto* header = dialog.findChild<QWidget*>("voteDialogHeader");
    QVERIFY(scrollArea != nullptr);
    QVERIFY(bodyFrame != nullptr);
    QVERIFY(shell != nullptr);
    QVERIFY(header != nullptr);
    if (!scrollArea || !bodyFrame || !shell || !header) {
        QApplication::setFont(originalFont);
        QFAIL("Missing vote dialog widgets for horizontal overflow check");
        return;
    }

    QScrollBar* hBar = scrollArea->horizontalScrollBar();
    QVERIFY(hBar != nullptr);
    if (!hBar) {
        QApplication::setFont(originalFont);
        QFAIL("Missing horizontal scrollbar");
        return;
    }

    const int hMax = hBar->maximum();
    const int bodyRightInViewport = bodyFrame->mapTo(scrollArea->viewport(), QPoint(bodyFrame->width() - 1, 0)).x();
    const int viewportRight = scrollArea->viewport()->rect().right();

    QVERIFY2(hMax == 0, qPrintable(QString("Horizontal overflow remains (hMax=%1, dialogWidth=%2)")
                                            .arg(hMax)
                                            .arg(dialog.width())));
    QVERIFY2(bodyRightInViewport <= viewportRight,
             qPrintable(QString("Body overflows viewport (bodyRight=%1, viewportRight=%2)")
                                .arg(bodyRightInViewport)
                                .arg(viewportRight)));
    QVERIFY2(header->width() >= shell->width() - 2,
             qPrintable(QString("Header width does not track shell width (header=%1, shell=%2)")
                                .arg(header->width())
                                .arg(shell->width())));

    QApplication::setFont(originalFont);
}

void GovernanceDialogTests::voteDialogCentersOnParentWindowWithoutParentClamping()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(420, 300);
    mainWindow.move(180, 120);
    mainWindow.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&mainWindow, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.show();
    QTest::qWait(100);
    QCoreApplication::processEvents();

    const QRect parentGlobal(mainWindow.mapToGlobal(QPoint(0, 0)), mainWindow.size());
    const QRect dialogGlobal = dialog.frameGeometry();

    QVERIFY2(std::abs(dialogGlobal.center().x() - parentGlobal.center().x()) <= 4,
             qPrintable(QString("Dialog not centered horizontally (dialog=%1, parent=%2)")
                                .arg(dialogGlobal.center().x())
                                .arg(parentGlobal.center().x())));
    QVERIFY2(std::abs(dialogGlobal.center().y() - parentGlobal.center().y()) <= 4,
             qPrintable(QString("Dialog not centered vertically (dialog=%1, parent=%2)")
                                .arg(dialogGlobal.center().y())
                                .arg(parentGlobal.center().y())));

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sourcePath = appDir.absoluteFilePath("../../../../src/qt/votedialog.cpp");
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(sourceFile.readAll());
    const int showEventStart = source.indexOf(QStringLiteral("void VoteDialog::showEvent("));
    QVERIFY(showEventStart >= 0);
    const int showEventEnd = source.indexOf(QStringLiteral("void VoteDialog::onMnSelectionClicked"), showEventStart);
    QVERIFY(showEventEnd > showEventStart);
    const QString showEventSlice = source.mid(showEventStart, showEventEnd - showEventStart);
    QVERIFY(!showEventSlice.contains("kViewportMargin"));
    QVERIFY2(showEventSlice.contains(QStringLiteral("ContainerDialog::showEvent(event);")),
             "VoteDialog::showEvent should delegate centering flow to ContainerDialog");
    QVERIFY2(!showEventSlice.contains(QStringLiteral("move(centeredPos);")),
             "VoteDialog::showEvent should avoid manual second centering move");
}

void GovernanceDialogTests::voteDialogRemainsCenteredWhenOpenedViaOpaqueBackgroundHelper()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(460, 320);
    mainWindow.move(220, 140);
    mainWindow.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&mainWindow, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));

    bool checkerExecuted = false;
    QPoint finalCenter;
    QTimer::singleShot(180, &dialog, [&]() {
        checkerExecuted = true;
        finalCenter = dialog.frameGeometry().center();
        dialog.reject();
    });

    (void)openDialogWithOpaqueBackgroundY(&dialog, &mainWindow, 4.5, 5, false);

    QVERIFY(checkerExecuted);
    const QPoint parentCenter = mainWindow.frameGeometry().center();
    QVERIFY2(std::abs(finalCenter.x() - parentCenter.x()) <= 4,
             qPrintable(QString("Dialog not centered horizontally after helper open (dialog=%1, parent=%2)")
                                .arg(finalCenter.x())
                                .arg(parentCenter.x())));
    QVERIFY2(std::abs(finalCenter.y() - parentCenter.y()) <= 4,
             qPrintable(QString("Dialog not centered vertically after helper open (dialog=%1, parent=%2)")
                                .arg(finalCenter.y())
                                .arg(parentCenter.y())));
}

void GovernanceDialogTests::opaqueBackgroundHelperCentersOwnPositionDialogs()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(460, 320);
    mainWindow.move(240, 140);
    mainWindow.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    QDialog dialog(&mainWindow);
    dialog.resize(260, 180);
    setDialogOwnsOpenPosition(&dialog, true);

    bool checkerExecuted = false;
    QPoint finalCenter;
    QTimer::singleShot(120, &dialog, [&]() {
        checkerExecuted = true;
        finalCenter = dialog.frameGeometry().center();
        dialog.reject();
    });

    (void)openDialogWithOpaqueBackgroundY(&dialog, &mainWindow, 3, 5, false);

    QVERIFY(checkerExecuted);
    const QPoint parentCenter = mainWindow.frameGeometry().center();
    QVERIFY2(std::abs(finalCenter.x() - parentCenter.x()) <= 4,
             qPrintable(QString("Dialog not centered horizontally for owns-position helper path (dialog=%1, parent=%2)")
                                .arg(finalCenter.x())
                                .arg(parentCenter.x())));
    QVERIFY2(std::abs(finalCenter.y() - parentCenter.y()) <= 4,
             qPrintable(QString("Dialog not centered vertically for owns-position helper path (dialog=%1, parent=%2)")
                                .arg(finalCenter.y())
                                .arg(parentCenter.y())));
}

void GovernanceDialogTests::voteDialogModeSwitchUsesButtonLikeToggles()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);

    auto* mnModeRadio = dialog.findChild<QRadioButton*>("radioModeMasternode");
    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    QVERIFY(mnModeRadio != nullptr);
    QVERIFY(coinModeRadio != nullptr);
    if (!mnModeRadio || !coinModeRadio) {
        QFAIL("Missing vote mode toggles");
        return;
    }

    QCOMPARE(mnModeRadio->property("cssClass").toString(), QString("vote-mode-toggle"));
    QCOMPARE(coinModeRadio->property("cssClass").toString(), QString("vote-mode-toggle"));

    const auto cssHasButtonLikeVoteModeToggle = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression indicatorHidden(
            R"(QRadioButton\[cssClass=\"vote-mode-toggle\"\]::indicator\s*\{[^\}]*width\s*:\s*0px\s*;[^\}]*height\s*:\s*0px\s*;)",
            QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpression checkedPill(
            R"(QRadioButton\[cssClass=\"vote-mode-toggle\"\]:checked\s*\{[^\}]*background-color\s*:[^\}]*border-radius\s*:[^\}]*\})",
            QRegularExpression::DotMatchesEverythingOption);
        return indicatorHidden.match(css).hasMatch() && checkedPill.match(css).hasMatch();
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY(cssHasButtonLikeVoteModeToggle(lightSource));
    QVERIFY(cssHasButtonLikeVoteModeToggle(darkSource));
}

void GovernanceDialogTests::voteDialogCoinModeHidesLegacyInfoLines()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));

    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* validationHint = dialog.findChild<QLabel*>("labelCoinValidationHint");
    auto* message = dialog.findChild<QLabel*>("labelMessage");
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(validationHint != nullptr);
    QVERIFY(message != nullptr);
    if (!coinModeRadio || !validationHint || !message) {
        QFAIL("Missing coin-mode labels");
        return;
    }

    coinModeRadio->setChecked(true);
    dialog.show();
    QTest::qWait(70);
    QCoreApplication::processEvents();

    QVERIFY(!validationHint->isVisible());
    QVERIFY(!message->isVisible());
    QVERIFY(!validationHint->text().contains("Pick a quick amount", Qt::CaseInsensitive));
    QVERIFY(!message->text().contains("Coins stay locked", Qt::CaseInsensitive));
}

void GovernanceDialogTests::voteDialogLinkButtonIconIsResolvedAtRuntime()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.show();
    QTest::qWait(60);
    QCoreApplication::processEvents();

    auto* btnLink = dialog.findChild<QPushButton*>("btnLink");
    QVERIFY(btnLink != nullptr);
    if (!btnLink) {
        QFAIL("Missing proposal link button");
        return;
    }

    QVERIFY(!btnLink->icon().isNull());
    QVERIFY(btnLink->iconSize().width() >= 16);
    QVERIFY(btnLink->iconSize().height() >= 16);
}

void GovernanceDialogTests::voteDialogActionButtonsUseDedicatedModernClasses()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);

    auto* btnCancel = dialog.findChild<QPushButton*>("btnCancel");
    auto* btnSave = dialog.findChild<QPushButton*>("btnSave");
    QVERIFY(btnCancel != nullptr);
    QVERIFY(btnSave != nullptr);
    if (!btnCancel || !btnSave) {
        QFAIL("Missing action buttons");
        return;
    }

    QCOMPARE(btnCancel->property("cssClass").toString(), QString("vote-action-cancel"));
    QCOMPARE(btnSave->property("cssClass").toString(), QString("vote-action-primary"));
}

void GovernanceDialogTests::voteDialogBodyUsesModernPanelClasses()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);

    auto* coinModeContainer = dialog.findChild<QWidget*>("containerCoinMode");
    auto* mnModeContainer = dialog.findChild<QWidget*>("containerMnMode");
    auto* btnSelectMasternodes = dialog.findChild<QPushButton*>("btnSelectMasternodes");
    auto* subtitle = dialog.findChild<QLabel*>("labelSubtitle");
    auto* voteModeLabel = dialog.findChild<QLabel*>("labelVoteMode");
    auto* coinAmountLabel = dialog.findChild<QLabel*>("labelCoinAmount");
    QVERIFY(coinModeContainer != nullptr);
    QVERIFY(mnModeContainer != nullptr);
    QVERIFY(btnSelectMasternodes != nullptr);
    QVERIFY(subtitle != nullptr);
    QVERIFY(voteModeLabel != nullptr);
    QVERIFY(coinAmountLabel != nullptr);
    if (!coinModeContainer || !mnModeContainer || !btnSelectMasternodes || !subtitle || !voteModeLabel || !coinAmountLabel) {
        QFAIL("Missing vote dialog body widgets");
        return;
    }

    QCOMPARE(coinModeContainer->property("cssClass").toString(), QString("vote-body-panel"));
    QCOMPARE(mnModeContainer->property("cssClass").toString(), QString("vote-body-panel"));
    QCOMPARE(btnSelectMasternodes->property("cssClass").toString(), QString("vote-inline-selector"));
    QCOMPARE(subtitle->property("cssClass").toString(), QString("vote-dialog-subtitle"));
    QCOMPARE(voteModeLabel->property("cssClass").toString(), QString("vote-section-label"));
    QCOMPARE(coinAmountLabel->property("cssClass").toString(), QString("vote-section-label"));
}

void GovernanceDialogTests::voteDialogModernBodyCssExistsInBothThemes()
{
    const auto cssHasModernVoteBodyStyles = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression bodyPanel(
            R"(\*\[cssClass=\"vote-body-panel\"\]\s*\{(?=[^\}]*border-radius\s*:\s*12px\s*;)(?=[^\}]*border\s*:\s*1px\s+solid)[^\}]*\})",
            QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpression inlineSelector(
            R"(QPushButton\[cssClass=\"vote-inline-selector\"\]\s*\{[^\}]*border-radius\s*:\s*12px\s*;[^\}]*padding\s*:[^\}]*\})",
            QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpression subtitle(
            R"(\*\[cssClass=\"vote-dialog-subtitle\"\]\s*\{[^\}]*font-size\s*:\s*16px\s*;[^\}]*\})",
            QRegularExpression::DotMatchesEverythingOption);
        return bodyPanel.match(css).hasMatch() &&
               inlineSelector.match(css).hasMatch() &&
               subtitle.match(css).hasMatch();
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY(cssHasModernVoteBodyStyles(lightSource));
    QVERIFY(cssHasModernVoteBodyStyles(darkSource));
}

void GovernanceDialogTests::voteDialogHeaderUsesOrganicLifeAccentToneInBothThemes()
{
    const auto cssHasAccentHeader = [](const QString& cssPath, const QString& expectedColor) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression re(
            QString(R"(\*\[cssClass=\"vote-dialog-header\"\]\s*\{[^\}]*background-color\s*:\s*%1\s*;)")
                .arg(QRegularExpression::escape(expectedColor)),
            QRegularExpression::DotMatchesEverythingOption);
        return re.match(css).hasMatch();
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY(cssHasAccentHeader(lightSource, "#F2D4B8"));
    QVERIFY(cssHasAccentHeader(darkSource, "#85a653"));
}

void GovernanceDialogTests::voteDialogCloseButtonUsesThemeAwareIconClass()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal(4));
    dialog.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    auto* btnEsc = dialog.findChild<QPushButton*>("btnEsc");
    QVERIFY(btnEsc != nullptr);
    if (!btnEsc) {
        QFAIL("Missing close button");
        return;
    }

    QCOMPARE(btnEsc->property("cssClass").toString(), QString("ic-close"));

    const auto cssHasCloseIcon = [](const QString& cssPath, const QString& expectedIconToken) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression re(
            QString(R"(\*\[cssClass=\"ic-close\"\]\s*\{[^\}]*qproperty-icon\s*:\s*url\(\"%1\"\))")
                .arg(QRegularExpression::escape(expectedIconToken)),
            QRegularExpression::DotMatchesEverythingOption);
        return re.match(css).hasMatch();
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY(cssHasCloseIcon(lightSource, "://ic-close"));
    QVERIFY(cssHasCloseIcon(darkSource, "://ic-close-white"));
}

void GovernanceDialogTests::voteDialogHeaderTitleUsesThemeAwareReadableColor()
{
    const auto cssHasTitleSpec = [](const QString& cssPath, const QString& expectedColor) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression re(
            QString(R"(QWidget#voteDialogHeader\s+QLabel\[cssClass=\"vote-dialog-title\"\]\s*\{[^\}]*color\s*:\s*%1\s*;[^\}]*font-size\s*:\s*24px\s*;)")
                .arg(QRegularExpression::escape(expectedColor)),
            QRegularExpression::DotMatchesEverythingOption
        );
        return re.match(css).hasMatch();
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY(cssHasTitleSpec(lightSource, "#2d3020"));
    QVERIFY(cssHasTitleSpec(darkSource, "#FFFFFF"));
}

void GovernanceDialogTests::containerDialogHeaderTitleUsesReadableDarkThemeColor()
{
    const auto cssHasContainerHeaderTitleSpec = [](const QString& cssPath, const QString& expectedColor) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression re(
            QString(R"(QWidget#containerDialogHeader\s+QLabel\[cssClass=\"text-title-dialog\"\]\s*\{[^\}]*color\s*:\s*%1\s*;)")
                .arg(QRegularExpression::escape(expectedColor)),
            QRegularExpression::DotMatchesEverythingOption);
        return re.match(css).hasMatch();
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");
    QVERIFY(cssHasContainerHeaderTitleSpec(darkSource, "#FFFFFF"));
}

void GovernanceDialogTests::voteDialogHeaderTitleCssClassIsForceUpdatedAtRuntime()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sourcePath = appDir.absoluteFilePath("../../../../src/qt/votedialog.cpp");
    QFile f(sourcePath);
    QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), "Unable to open votedialog.cpp");
    const QString source = QString::fromUtf8(f.readAll());

    const QRegularExpression re(
        R"(setCssProperty\(\s*ui->labelTitle\s*,\s*\"vote-dialog-title\"\s*,\s*true\s*\)\s*;)");
    QVERIFY2(re.match(source).hasMatch(),
             "VoteDialog title class assignment should force style refresh so theme-aware title color is always applied");
}

void GovernanceDialogTests::voteDialogHeaderUsesScopedThemeAwareSelectors()
{
    const auto cssHasScopedCloseButton = [](const QString& cssPath, const QString& expectedIconToken) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression re(
            QString(R"(QWidget#voteDialogHeader\s+QPushButton\[cssClass=\"ic-close\"\]\s*\{[^\}]*qproperty-icon\s*:\s*url\(\"%1\"\)[^\}]*qproperty-iconSize\s*:\s*20px\s+20px\s*;)")
                .arg(QRegularExpression::escape(expectedIconToken)),
            QRegularExpression::DotMatchesEverythingOption
        );
        return re.match(css).hasMatch();
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY(cssHasScopedCloseButton(lightSource, "://ic-close"));
    QVERIFY(cssHasScopedCloseButton(darkSource, "://ic-close-white"));
}

void GovernanceDialogTests::voteDialogBodyHasScopedSquareTopCornersInCss()
{
    const auto cssHasScopedBodyCorners = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression re(
            R"(QFrame#voteDialogShell\s+QFrame\[cssClass=\"vote-dialog-body\"\]\s*\{[^\}]*border-top-left-radius\s*:\s*0px\s*;[^\}]*border-top-right-radius\s*:\s*0px\s*;)",
            QRegularExpression::DotMatchesEverythingOption
        );
        return re.match(css).hasMatch();
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY(cssHasScopedBodyCorners(lightSource));
    QVERIFY(cssHasScopedBodyCorners(darkSource));
}

void GovernanceDialogTests::voteDialogBodyClassSwitchForcesStyleRefresh()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sourcePath = appDir.absoluteFilePath("../../../../src/qt/votedialog.cpp");
    QFile file(sourcePath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    if (!file.isOpen()) {
        QFAIL("Unable to open votedialog.cpp");
        return;
    }
    const QString source = QString::fromUtf8(file.readAll());
    const QRegularExpression re(
        R"(setCssProperty\(\s*ui->frame\s*,\s*\"vote-dialog-body\"\s*,\s*true\s*\)\s*;)");
    QVERIFY2(re.match(source).hasMatch(), "VoteDialog body cssClass switch must force a style refresh");
}

void GovernanceDialogTests::voteDialogBodyWidgetsForceStyleRefreshAfterClassAssignment()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sourcePath = appDir.absoluteFilePath("../../../../src/qt/votedialog.cpp");
    QFile file(sourcePath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    if (!file.isOpen()) {
        QFAIL("Unable to open votedialog.cpp");
        return;
    }
    const QString source = QString::fromUtf8(file.readAll());

    const std::vector<QRegularExpression> requiredForceUpdates = {
        QRegularExpression(R"(setCssProperty\(\s*ui->radioModeMasternode\s*,\s*\"vote-mode-toggle\"\s*,\s*true\s*\)\s*;)"),
        QRegularExpression(R"(setCssProperty\(\s*ui->radioModeCoinLock\s*,\s*\"vote-mode-toggle\"\s*,\s*true\s*\)\s*;)"),
        QRegularExpression(R"(setCssProperty\(\s*ui->btnCancel\s*,\s*\"vote-action-cancel\"\s*,\s*true\s*\)\s*;)"),
        QRegularExpression(R"(setCssProperty\(\s*ui->btnSave\s*,\s*\"vote-action-primary\"\s*,\s*true\s*\)\s*;)"),
        QRegularExpression(R"(setCssProperty\(\s*ui->containerCoinMode\s*,\s*\"vote-body-panel\"\s*,\s*true\s*\)\s*;)"),
        QRegularExpression(R"(setCssProperty\(\s*ui->containerMnMode\s*,\s*\"vote-body-panel\"\s*,\s*true\s*\)\s*;)"),
    };

    for (const auto& re : requiredForceUpdates) {
        QVERIFY2(re.match(source).hasMatch(), qPrintable(QString("Missing forced style refresh for pattern: %1").arg(re.pattern())));
    }
}

void GovernanceDialogTests::voteDialogBodyDoesNotUseLegacyContainerDialogClass()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sourcePath = appDir.absoluteFilePath("../../../../src/qt/votedialog.cpp");
    QFile file(sourcePath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    if (!file.isOpen()) {
        QFAIL("Unable to open votedialog.cpp");
        return;
    }
    const QString source = QString::fromUtf8(file.readAll());
    const QRegularExpression legacyRe(
        R"(setCssProperty\(\s*ui->frame\s*,\s*\"container-dialog\"\s*\)\s*;)");
    QVERIFY2(!legacyRe.match(source).hasMatch(),
             "VoteDialog body frame must not use legacy container-dialog class");
}

void GovernanceDialogTests::voteDialogVoteModeCardUsesSquareTopCornersInCss()
{
    const auto cssHasSquareTopModeCard = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QRegularExpression re(
            R"(\*\[cssClass=\"vote-mode-card\"\]\s*\{[^\}]*border-top-left-radius\s*:\s*0px\s*;[^\}]*border-top-right-radius\s*:\s*0px\s*;)",
            QRegularExpression::DotMatchesEverythingOption);
        return re.match(css).hasMatch();
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY(cssHasSquareTopModeCard(lightSource));
    QVERIFY(cssHasSquareTopModeCard(darkSource));
}

void GovernanceDialogTests::voteDialogModernSelectorsOverrideLateGenericDialogRules()
{
    const auto hasLateScopedVoteOverrides = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());

        const int genericRadioIndicatorIdx = css.indexOf(QStringLiteral("QRadioButton::indicator"));
        const int scopedToggleIndicatorIdx = css.indexOf(
            QStringLiteral("QFrame#voteDialogShell QRadioButton[cssClass=\"vote-mode-toggle\"]::indicator"));
        const int genericDialogButtonIdx = css.indexOf(QStringLiteral("QDialog QPushButton {"));
        const int scopedPrimaryIdx = css.indexOf(
            QStringLiteral("QFrame#voteDialogShell QPushButton[cssClass=\"vote-action-primary\"]"));
        const int scopedCancelIdx = css.indexOf(
            QStringLiteral("QFrame#voteDialogShell QPushButton[cssClass=\"vote-action-cancel\"]"));

        return genericRadioIndicatorIdx >= 0 &&
               scopedToggleIndicatorIdx > genericRadioIndicatorIdx &&
               genericDialogButtonIdx >= 0 &&
               scopedPrimaryIdx > genericDialogButtonIdx &&
               scopedCancelIdx > genericDialogButtonIdx;
    };

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString lightSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_light.css");
    const QString darkSource = appDir.absoluteFilePath("../../../../src/qt/res/css/style_dark.css");

    QVERIFY2(hasLateScopedVoteOverrides(lightSource),
             "Light theme must define scoped vote overrides after generic QDialog/QRadioButton rules");
    QVERIFY2(hasLateScopedVoteOverrides(darkSource),
             "Dark theme must define scoped vote overrides after generic QDialog/QRadioButton rules");
}

void GovernanceDialogTests::voteDialogSetsDirectionalVoteProperties()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    auto* yesCheck = dialog.findChild<QCheckBox*>("checkVoteYes");
    auto* noCheck = dialog.findChild<QCheckBox*>("checkVoteNo");
    QVERIFY(yesCheck != nullptr);
    QVERIFY(noCheck != nullptr);
    if (!yesCheck || !noCheck) {
        QFAIL("Missing vote direction controls");
        return;
    }

    QCOMPARE(yesCheck->property("voteChoice").toString(), QString("yes"));
    QCOMPARE(noCheck->property("voteChoice").toString(), QString("no"));
}

void GovernanceDialogTests::voteDialogVoteTilesToggleFromBackgroundClick()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());
    dialog.show();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    auto* yesCheck = dialog.findChild<QCheckBox*>("checkVoteYes");
    auto* noCheck = dialog.findChild<QCheckBox*>("checkVoteNo");
    auto* yesTile = dialog.findChild<QWidget*>("containerYes");
    auto* noTile = dialog.findChild<QWidget*>("containerNo");
    QVERIFY(yesCheck != nullptr);
    QVERIFY(noCheck != nullptr);
    QVERIFY(yesTile != nullptr);
    QVERIFY(noTile != nullptr);
    if (!yesCheck || !noCheck || !yesTile || !noTile) {
        QFAIL("Missing vote tile controls");
        return;
    }

    QVERIFY(!yesCheck->isChecked());
    QVERIFY(!noCheck->isChecked());

    const QPoint yesBackgroundPoint(6, std::max(1, yesTile->height() / 2));
    const QPoint noBackgroundPoint(6, std::max(1, noTile->height() / 2));
    QTest::mouseClick(yesTile, Qt::LeftButton, Qt::NoModifier, yesBackgroundPoint);
    QVERIFY(yesCheck->isChecked());
    QVERIFY(!noCheck->isChecked());

    QTest::mouseClick(noTile, Qt::LeftButton, Qt::NoModifier, noBackgroundPoint);
    QVERIFY(!yesCheck->isChecked());
    QVERIFY(noCheck->isChecked());
}

void GovernanceDialogTests::voteDialogVoteModeLabelsFitWithoutClipping()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    auto* frameMode = dialog.findChild<QFrame*>("frameVoteMode");
    auto* mnModeRadio = dialog.findChild<QRadioButton*>("radioModeMasternode");
    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    QVERIFY(frameMode != nullptr);
    QVERIFY(mnModeRadio != nullptr);
    QVERIFY(coinModeRadio != nullptr);
    if (!frameMode || !mnModeRadio || !coinModeRadio) {
        QFAIL("Missing vote mode controls");
        return;
    }

    dialog.show();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    const auto minRequiredWidth = [](QRadioButton* radio) {
        const int indicator = radio->style()->pixelMetric(QStyle::PM_ExclusiveIndicatorWidth, nullptr, radio);
        const int spacing = radio->style()->pixelMetric(QStyle::PM_RadioButtonLabelSpacing, nullptr, radio);
        const int textWidth = radio->fontMetrics().horizontalAdvance(radio->text());
        return textWidth + indicator + spacing + 16;
    };

    QVERIFY(mnModeRadio->width() >= minRequiredWidth(mnModeRadio));
    QVERIFY(coinModeRadio->width() >= minRequiredWidth(coinModeRadio));

    const QRect frameRect = frameMode->contentsRect();
    const QRect coinRect = coinModeRadio->geometry();
    QVERIFY(coinRect.right() <= frameRect.right());
}

void GovernanceDialogTests::voteDialogModesReserveEqualBodyHeight()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    auto* mnModeRadio = dialog.findChild<QRadioButton*>("radioModeMasternode");
    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* coinModeContainer = dialog.findChild<QWidget*>("containerCoinMode");
    auto* mnModeContainer = dialog.findChild<QWidget*>("containerMnMode");
    auto* btnCancel = dialog.findChild<QPushButton*>("btnCancel");
    QVERIFY(mnModeRadio != nullptr);
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(coinModeContainer != nullptr);
    QVERIFY(mnModeContainer != nullptr);
    QVERIFY(btnCancel != nullptr);
    if (!mnModeRadio || !coinModeRadio || !coinModeContainer || !mnModeContainer || !btnCancel) {
        QFAIL("Missing controls for stable body height test");
        return;
    }

    dialog.show();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    QVERIFY(mnModeRadio->isChecked());
    QVERIFY(coinModeContainer->minimumHeight() >= 220);
    QCOMPARE(mnModeContainer->minimumHeight(), coinModeContainer->minimumHeight());

    const int footerTopInMnMode = btnCancel->geometry().top();
    coinModeRadio->setChecked(true);
    QTest::qWait(50);
    QCoreApplication::processEvents();

    QCOMPARE(btnCancel->geometry().top(), footerTopInMnMode);
}

void GovernanceDialogTests::voteDialogFooterAvoidsLegacyCenteringSpacers()
{
    QFile file(resolveQtSourceFile("forms/votedialog.ui"));
    QVERIFY2(file.fileName().size() > 0, "Unable to resolve forms/votedialog.ui");
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Unable to read %1").arg(file.fileName())));
    const QString source = QString::fromUtf8(file.readAll());

    const int layoutStart = source.indexOf(QStringLiteral("<layout class=\"QHBoxLayout\" name=\"horizontalLayout_4\""));
    QVERIFY(layoutStart >= 0);
    if (layoutStart < 0) {
        QFAIL("Vote dialog footer layout not found");
        return;
    }

    const QString layoutSlice = source.mid(layoutStart, 1600);
    QVERIFY2(!layoutSlice.contains(QStringLiteral("horizontalSpacer_6")),
             "Vote dialog footer should not rely on a leading centering spacer");
    QVERIFY2(!layoutSlice.contains(QStringLiteral("horizontalSpacer_7")),
             "Vote dialog footer should not rely on a trailing centering spacer");
}

void GovernanceDialogTests::masternodeModeShowsInlineSelectionList()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    auto* mnModeRadio = dialog.findChild<QRadioButton*>("radioModeMasternode");
    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    auto* inlineList = dialog.findChild<QListWidget*>("listMasternodesInline");
    auto* noContainer = dialog.findChild<QWidget*>("containerNo");
    auto* yesContainer = dialog.findChild<QWidget*>("containerYes");
    auto* yesCheck = dialog.findChild<QCheckBox*>("checkVoteYes");
    auto* noCheck = dialog.findChild<QCheckBox*>("checkVoteNo");
    QVERIFY(mnModeRadio != nullptr);
    QVERIFY(coinModeRadio != nullptr);
    QVERIFY(inlineList != nullptr);
    QVERIFY(noContainer != nullptr);
    QVERIFY(yesContainer != nullptr);
    QVERIFY(yesCheck != nullptr);
    QVERIFY(noCheck != nullptr);
    if (!mnModeRadio || !coinModeRadio || !inlineList || !noContainer || !yesContainer || !yesCheck || !noCheck) {
        QFAIL("Missing controls for masternode mode test");
        return;
    }

    dialog.show();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    if (!mnModeRadio || !inlineList || !yesCheck || !noCheck || !noContainer || !yesContainer) {
        QFAIL("Missing controls for masternode geometry assertions");
        return;
    }
    QVERIFY(mnModeRadio->isChecked());
    QVERIFY(inlineList->isVisible());
    QVERIFY(inlineList->minimumHeight() >= 100);
    QVERIFY(yesCheck->height() <= 44);
    QVERIFY(noCheck->height() <= 44);
    QVERIFY(std::abs(noContainer->height() - noCheck->height()) <= 6);
    QVERIFY(std::abs(yesContainer->height() - yesCheck->height()) <= 6);

    coinModeRadio->setChecked(true);
    QCoreApplication::processEvents();
    QVERIFY(!inlineList->isVisible());
}

void GovernanceDialogTests::finishedProposalCardHidesVoteButton()
{
    QWidget parent;
    ProposalCard card(&parent);

    ProposalInfo finished = BuildTestProposal(0);
    finished.status = ProposalInfo::FINISHED;
    card.setProposal(finished);

    auto* voteButton = card.findChild<QPushButton*>("btnVote");
    QVERIFY(voteButton != nullptr);
    if (!voteButton) {
        QFAIL("Missing vote button on proposal card");
        return;
    }
    QVERIFY(voteButton->isHidden());

    card.setProposal(BuildTestProposal());
    QVERIFY(!voteButton->isHidden());
}

void GovernanceDialogTests::expiredProposalCardShowsExpiredStatus()
{
    QWidget parent;
    ProposalCard card(&parent);

    ProposalInfo expired = BuildTestProposal(0);
    expired.status = ProposalInfo::FINISHED;
    expired.isValid = false;
    card.setProposal(expired);

    auto* statusLabel = card.findChild<QLabel*>("labelStatus");
    QVERIFY(statusLabel != nullptr);
    if (!statusLabel) {
        QFAIL("Missing status label on proposal card");
        return;
    }
    QVERIFY(statusLabel->text().contains("Expired"));
}

void GovernanceDialogTests::proposalCardShowsCycleBasedRemainingLabel()
{
    QWidget parent;
    ProposalCard card(&parent);

    ProposalInfo proposal = BuildTestProposal(2);
    proposal.totalPayments = 4;
    proposal.remainingPayments = 2;
    card.setProposal(proposal);

    auto* timeLabel = card.findChild<QLabel*>("labelPropMonths");
    QVERIFY(timeLabel != nullptr);
    if (!timeLabel) {
        QFAIL("Missing remaining cycles label on proposal card");
        return;
    }
    QVERIFY(timeLabel->text().contains("cycles", Qt::CaseInsensitive));
}

void GovernanceDialogTests::voteDialogShowsCycleBasedRemainingLabel()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);

    ProposalInfo proposal = BuildTestProposal(2);
    proposal.totalPayments = 4;
    proposal.remainingPayments = 2;
    dialog.setProposal(proposal);

    auto* timeLabel = dialog.findChild<QLabel*>("labelTime");
    QVERIFY(timeLabel != nullptr);
    if (!timeLabel) {
        QFAIL("Missing remaining cycles label on vote dialog");
        return;
    }
    QVERIFY(timeLabel->text().contains("cycles", Qt::CaseInsensitive));
}

void GovernanceDialogTests::proposalCardShowsVoteCounts()
{
    QWidget parent;
    ProposalCard card(&parent);

    ProposalInfo proposal = BuildTestProposal();
    proposal.votesYes = 3;
    proposal.votesNo = 1;
    proposal.coinVotesYes = 150;
    proposal.coinVotesNo = 25;
    card.setProposal(proposal);

    auto* labelNo = card.findChild<QLabel*>("labelNo");
    auto* labelYes = card.findChild<QLabel*>("labelYes");
    QVERIFY(labelNo != nullptr);
    QVERIFY(labelYes != nullptr);
    if (!labelNo || !labelYes) {
        QFAIL("Missing vote labels on proposal card");
        return;
    }

    QVERIFY(labelNo->text().contains("No 1"));
    QVERIFY(labelNo->text().contains("coin 25"));
    QVERIFY(labelYes->text().contains("Yes 3"));
    QVERIFY(labelYes->text().contains("coin 150"));
}

void GovernanceDialogTests::proposalCardRendersContainedYesNoProgressBars()
{
    QWidget parent;
    ProposalCard card(&parent);

    ProposalInfo proposal = BuildTestProposal();
    proposal.votesYes = 3;
    proposal.votesNo = 1;
    card.setProposal(proposal);
    card.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    auto* containerVotes = card.findChild<QWidget*>("containerVotes");
    auto* containerText = card.findChild<QWidget*>("containerText");
    auto* votesNoBar = card.findChild<QProgressBar*>("votesNoBar");
    auto* votesYesBar = card.findChild<QProgressBar*>("votesYesBar");
    QVERIFY(containerVotes != nullptr);
    QVERIFY(containerText != nullptr);
    QVERIFY(votesNoBar != nullptr);
    QVERIFY(votesYesBar != nullptr);
    if (!containerVotes || !containerText || !votesNoBar || !votesYesBar) {
        QFAIL("Missing yes/no progress bars in proposal card");
        return;
    }

    QCOMPARE(votesNoBar->value(), 25);
    QCOMPARE(votesYesBar->value(), 75);
    QVERIFY(votesNoBar->geometry().height() == containerVotes->height());
    QVERIFY(votesYesBar->geometry().height() == containerVotes->height());
    QVERIFY(containerText->geometry().height() == containerVotes->height());
    QVERIFY(votesNoBar->geometry().top() == 0);
    QVERIFY(votesYesBar->geometry().top() == 0);
}

void GovernanceDialogTests::proposalCardBarsBlendMnAndCoinVotes()
{
    QWidget parent;
    ProposalCard card(&parent);

    ProposalInfo proposal = BuildTestProposal();
    proposal.votesYes = 9;
    proposal.votesNo = 1;
    proposal.coinVotesYes = 1;
    proposal.coinVotesNo = 9;
    card.setProposal(proposal);

    auto* votesNoBar = card.findChild<QProgressBar*>("votesNoBar");
    auto* votesYesBar = card.findChild<QProgressBar*>("votesYesBar");
    QVERIFY(votesNoBar != nullptr);
    QVERIFY(votesYesBar != nullptr);
    if (!votesNoBar || !votesYesBar) {
        QFAIL("Missing yes/no progress bars in proposal card");
        return;
    }

    // MN ratio is 10/90 and coin ratio is 90/10. Card bars must reflect both channels.
    QCOMPARE(votesNoBar->value(), 50);
    QCOMPARE(votesYesBar->value(), 50);
}

void GovernanceDialogTests::proposalCardUsesPremiumDropShadowEffect()
{
    QWidget parent;
    ProposalCard card(&parent);

    auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(card.graphicsEffect());
    QVERIFY(effect != nullptr);
    if (!effect) {
        QFAIL("Expected a QGraphicsDropShadowEffect on proposal card");
        return;
    }

    QVERIFY(effect->blurRadius() >= 28.0);
    QVERIFY(effect->xOffset() == 0.0);
    QVERIFY(effect->yOffset() >= 8.0);
}

void GovernanceDialogTests::proposalCardUsesRoundedVoteRailsInBothThemes()
{
    const QStringList paths = {
        resolveQtSourceFile("res/css/style_light.css"),
        resolveQtSourceFile("res/css/style_dark.css")
    };

    for (const QString& path : paths) {
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(QString("Failed to open %1").arg(path)));
        const QString source = QString::fromUtf8(file.readAll());

        const int railStart = source.indexOf(QStringLiteral("*[cssClass=\"card-progress-box\"]"));
        QVERIFY(railStart >= 0);
        const int railEnd = source.indexOf(QStringLiteral("*[cssClass=\"btn-link\"]"), railStart);
        QVERIFY(railEnd > railStart);
        const QString railSlice = source.mid(railStart, railEnd - railStart);

        QVERIFY2(railSlice.contains(QStringLiteral("border-radius: 10px;")),
                 qPrintable(QString("Rounded rail radius missing in %1").arg(path)));
        QVERIFY2(railSlice.contains(QStringLiteral("*[cssClass=\"card-progress-no\"]::chunk:horizontal")),
                 qPrintable(QString("No-vote chunk selector missing in %1").arg(path)));
        QVERIFY2(railSlice.contains(QStringLiteral("*[cssClass=\"card-progress-yes\"]::chunk:horizontal")),
                 qPrintable(QString("Yes-vote chunk selector missing in %1").arg(path)));
        QVERIFY2(railSlice.contains(QStringLiteral("border-radius: 9px;")),
                 qPrintable(QString("Rounded chunk radius missing in %1").arg(path)));
    }
}

void GovernanceDialogTests::governanceHeaderSubtitleStaysCompactWithoutLegacyHeightCap()
{
    QFile uiFile(resolveQtSourceFile("forms/governancewidget.ui"));
    QVERIFY2(uiFile.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.ui");
    const QString source = QString::fromUtf8(uiFile.readAll());

    const int headerStart = source.indexOf(QStringLiteral("<widget class=\"QWidget\" name=\"containerTitles\""));
    QVERIFY(headerStart >= 0);
    const int headerEnd = source.indexOf(QStringLiteral("<widget class=\"QWidget\" name=\"layoutWarning\""), headerStart);
    QVERIFY(headerEnd > headerStart);
    const QString headerSlice = source.mid(headerStart, headerEnd - headerStart);

    QVERIFY2(headerSlice.contains(QStringLiteral("<widget class=\"QLabel\" name=\"labelSubtitle1\">")),
             "Subtitle label not found in header slice");
    QVERIFY2(headerSlice.contains(QStringLiteral("<height>64</height>")),
             "Governance header should use the compact 64px height");
    QVERIFY2(!headerSlice.contains(QStringLiteral("<height>80</height>")),
             "Intermediate 80px governance header height still present");
    QVERIFY2(!headerSlice.contains(QStringLiteral("<height>60</height>")),
             "Legacy 60px header height cap still present");

    QFile cppFile(resolveQtSourceFile("governancewidget.cpp"));
    QVERIFY2(cppFile.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.cpp");
    const QString cppSource = QString::fromUtf8(cppFile.readAll());
    QVERIFY2(cppSource.contains(QStringLiteral("ui->labelSubtitle1->setWordWrap(false);")),
             "Governance header subtitle should stay single-line to keep the header compact");
}

void GovernanceDialogTests::governanceWidgetUsesDedicatedDashboardClasses()
{
    QFile file(resolveQtSourceFile("governancewidget.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.cpp");
    const QString source = QString::fromUtf8(file.readAll());

    const QStringList requiredSnippets = {
        QStringLiteral("setCssProperty(ui->left, \"governance-dashboard-shell\")"),
        QStringLiteral("setCssProperty(ui->containerTitles, \"governance-dashboard-band\")"),
        QStringLiteral("setCssProperty(ui->containerFilter, \"governance-dashboard-filter-shell\")"),
        QStringLiteral("setCssProperty(ui->comboBoxFilter, \"governance-dashboard-filter\""),
        QStringLiteral("setCssProperty(ui->layoutWarning, \"governance-dashboard-warning\")"),
        QStringLiteral("setCssProperty(ui->mainContainer, \"governance-dashboard-surface\")"),
        QStringLiteral("setCssProperty(ui->scrollArea, \"governance-dashboard-surface\")"),
        QStringLiteral("setCssProperty(ui->labelSubtitle1, \"governance-subtitle\")")
    };

    for (const QString& snippet : requiredSnippets) {
        QVERIFY2(source.contains(snippet),
                 qPrintable(QString("Missing governance dashboard class assignment: %1").arg(snippet)));
    }
}

void GovernanceDialogTests::governanceDashboardStylesExistInBothThemes()
{
    const auto cssHasGovernanceDashboardStyles = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const QStringList selectors = {
            QStringLiteral("*[cssClass=\"governance-dashboard-shell\"]"),
            QStringLiteral("*[cssClass=\"governance-dashboard-band\"]"),
            QStringLiteral("*[cssClass=\"governance-dashboard-surface\"]"),
            QStringLiteral("*[cssClass=\"governance-dashboard-warning\"]"),
            QStringLiteral("QComboBox[cssClass=\"governance-dashboard-filter\"]"),
            QStringLiteral("*[cssClass=\"governance-subtitle\"]"),
            QStringLiteral("QFrame#voteDialogShell QLabel[cssClass=\"vote-dialog-subtitle\"]")
        };
        for (const QString& selector : selectors) {
            if (!css.contains(selector)) return false;
        }
        return true;
    };

    QVERIFY(cssHasGovernanceDashboardStyles(resolveQtSourceFile("res/css/style_light.css")));
    QVERIFY(cssHasGovernanceDashboardStyles(resolveQtSourceFile("res/css/style_dark.css")));
}

void GovernanceDialogTests::governanceHeaderCtaUsesDedicatedClasses()
{
    QFile file(resolveQtSourceFile("governancewidget.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.cpp");
    const QString source = QString::fromUtf8(file.readAll());

    const QStringList requiredSnippets = {
        QStringLiteral("setCssProperty(ui->btnCreateProposal, \"governance-header-cta\""),
        QStringLiteral("ui->btnCreateProposal->setTitleClassAndText(\"governance-cta-title\""),
        QStringLiteral("ui->btnCreateProposal->setSubTitleClassAndText(\"governance-cta-subtitle\""),
        QStringLiteral("ui->btnCreateProposal->setRightIconClass(\"governance-cta-arrow\", true)")
    };

    for (const QString& snippet : requiredSnippets) {
        QVERIFY2(source.contains(snippet),
                 qPrintable(QString("Missing governance CTA setup: %1").arg(snippet)));
    }
}

void GovernanceDialogTests::governanceSideRailContainsCreateProposalCta()
{
    QFile file(resolveQtSourceFile("forms/governancewidget.ui"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.ui");
    const QString source = QString::fromUtf8(file.readAll());

    const int headerStart = source.indexOf(QStringLiteral("<widget class=\"QWidget\" name=\"containerTitles\""));
    QVERIFY(headerStart >= 0);
    const int headerEnd = source.indexOf(QStringLiteral("<widget class=\"QWidget\" name=\"layoutWarning\""), headerStart);
    QVERIFY(headerEnd > headerStart);
    const QString headerSlice = source.mid(headerStart, headerEnd - headerStart);
    QVERIFY2(!headerSlice.contains(QStringLiteral("<widget class=\"OptionButton\" name=\"btnCreateProposal\"")),
             "Create Proposal CTA should no longer live inside the governance header band");

    const int rightStart = source.indexOf(QStringLiteral("<widget class=\"QWidget\" name=\"right\""));
    QVERIFY(rightStart >= 0);
    const int ctaPos = source.indexOf(QStringLiteral("<widget class=\"OptionButton\" name=\"btnCreateProposal\""), rightStart);
    const int budgetPos = source.indexOf(QStringLiteral("<widget class=\"QWidget\" name=\"containerBudget\""), rightStart);
    QVERIFY2(ctaPos > rightStart, "Create Proposal CTA must live in the right governance rail");
    QVERIFY2(budgetPos > ctaPos, "Create Proposal CTA must stay above Budget Distribution");
}

void GovernanceDialogTests::governanceHeaderCtaUsesInsetContentPadding()
{
    QFile file(resolveQtSourceFile("governancewidget.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.cpp");
    const QString source = QString::fromUtf8(file.readAll());

    const QStringList requiredSnippets = {
        QStringLiteral("findChild<QWidget*>(\"layoutOptions2\")"),
        QStringLiteral("setContentsMargins(12, 6, 8, 6)")
    };

    for (const QString& snippet : requiredSnippets) {
        QVERIFY2(source.contains(snippet),
                 qPrintable(QString("Missing governance CTA inset padding setup: %1").arg(snippet)));
    }
}

void GovernanceDialogTests::governanceFilterLineEditOpensPopupOnPress()
{
    QFile file(resolveQtSourceFile("governancewidget.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.cpp");
    const QString source = QString::fromUtf8(file.readAll());

    const QString snippet =
            QStringLiteral("connect(lineEditFilter, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxFilter->showPopup();});");
    QVERIFY2(source.contains(snippet),
             "Governance filter must open the dropdown when the read-only filter field is clicked");
}

void GovernanceDialogTests::governanceWarningBandUsesInsetRow()
{
    QFile file(resolveQtSourceFile("forms/governancewidget.ui"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.ui");
    const QString source = QString::fromUtf8(file.readAll());

    const int warningLayoutStart = source.indexOf(QStringLiteral("<layout class=\"QHBoxLayout\" name=\"horizontalLayoutWarningInset\""));
    QVERIFY(warningLayoutStart >= 0);
    if (warningLayoutStart < 0) {
        QFAIL("Governance warning inset layout not found");
        return;
    }

    const QString warningSlice = source.mid(warningLayoutStart, 1200);
    const QRegularExpression leftMarginRe(
            QStringLiteral(R"(<property name=\"leftMargin\">\s*<number>8</number>)"),
            QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression rightMarginRe(
            QStringLiteral(R"(<property name=\"rightMargin\">\s*<number>8</number>)"),
            QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(leftMarginRe.match(warningSlice).hasMatch(),
             "Governance warning inset row must add 8px left padding");
    QVERIFY2(rightMarginRe.match(warningSlice).hasMatch(),
             "Governance warning inset row must add 8px right padding");
    QVERIFY2(warningSlice.contains(QStringLiteral("<widget class=\"QWidget\" name=\"layoutWarning\"")),
             "Inset warning row must contain the sync warning widget");
}

void GovernanceDialogTests::governanceSideRailUsesFivePixelInset()
{
    QFile file(resolveQtSourceFile("forms/governancewidget.ui"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.ui");
    const QString source = QString::fromUtf8(file.readAll());

    const int layoutStart = source.indexOf(QStringLiteral("<layout class=\"QVBoxLayout\" name=\"verticalLayout_9\""));
    QVERIFY(layoutStart >= 0);
    if (layoutStart < 0) {
        QFAIL("Governance right-rail content layout not found");
        return;
    }

    const QString slice = source.mid(layoutStart, 900);
    const QRegularExpression leftMarginRe(
            QStringLiteral(R"(<property name=\"leftMargin\">\s*<number>5</number>)"),
            QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(leftMarginRe.match(slice).hasMatch(),
             "Governance right-rail CTA and budget stack should use a 5px left inset");
}

void GovernanceDialogTests::governanceFilterUsesLeftAlignedIconSubcontrol()
{
    const auto cssHasLeftAlignedIconSubcontrol = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());

        const int filterStart = css.indexOf(QStringLiteral("QComboBox[cssClass=\"governance-dashboard-filter\"]"));
        if (filterStart < 0) return false;
        const int filterEnd = css.indexOf(QStringLiteral("QComboBox[cssClass=\"governance-dashboard-filter\"]::drop-down"), filterStart);
        if (filterEnd <= filterStart) return false;
        const QString filterSlice = css.mid(filterStart, filterEnd - filterStart);

        const int dropDownStart = css.indexOf(QStringLiteral("QComboBox[cssClass=\"governance-dashboard-filter\"]::drop-down"), filterEnd);
        if (dropDownStart < 0) return false;
        const int arrowStart = css.indexOf(QStringLiteral("QComboBox[cssClass=\"governance-dashboard-filter\"]::down-arrow"), dropDownStart);
        if (arrowStart < dropDownStart) return false;
        const QString dropDownSlice = css.mid(dropDownStart, arrowStart - dropDownStart);
        const QString arrowSlice = css.mid(arrowStart, 220);

        return !filterSlice.contains(QStringLiteral("background-image: url(\"://ic-filter\");")) &&
               filterSlice.contains(QStringLiteral("padding: 8px 14px 8px 38px;")) &&
               dropDownSlice.contains(QStringLiteral("subcontrol-position: left center;")) &&
               dropDownSlice.contains(QStringLiteral("left: 10px;")) &&
               arrowSlice.contains(QStringLiteral("image: url(\"://ic-filter\");"));
    };

    QVERIFY2(cssHasLeftAlignedIconSubcontrol(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme governance filter should use a left-aligned icon subcontrol");
    QVERIFY2(cssHasLeftAlignedIconSubcontrol(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme governance filter should use a left-aligned icon subcontrol");
}

void GovernanceDialogTests::governanceFilterLineEditUsesLeftVCenterAlignment()
{
    QFile file(resolveQtSourceFile("governancewidget.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.cpp");
    const QString source = QString::fromUtf8(file.readAll());

    QVERIFY2(source.contains(QStringLiteral("lineEditFilter->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);")),
             "Governance filter line edit should explicitly use left + vertical-center alignment");
}

void GovernanceDialogTests::governanceHeaderCtaHasHoverTreatmentInBothThemes()
{
    const auto cssHasGovernanceCtaHover = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        return css.contains(QStringLiteral("OptionButton[cssClass=\"governance-header-cta\"] QWidget#layoutOptions2[hovered=\"true\"]"));
    };

    QVERIFY2(cssHasGovernanceCtaHover(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme governance CTA should define a hover treatment");
    QVERIFY2(cssHasGovernanceCtaHover(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme governance CTA should define a hover treatment");
}

void GovernanceDialogTests::governanceHeaderCtaHoverStateTogglesInnerPanelProperty()
{
    OptionButton button;
    QWidget* panel = button.findChild<QWidget*>("layoutOptions2");
    QVERIFY(panel != nullptr);
    if (!panel) {
        QFAIL("OptionButton inner panel not found");
        return;
    }

    QVERIFY(!panel->property("hovered").toBool());

    QEvent enterEvent(QEvent::Enter);
    QCoreApplication::sendEvent(&button, &enterEvent);
    QVERIFY(panel->property("hovered").toBool());

    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(&button, &leaveEvent);
    QVERIFY(!panel->property("hovered").toBool());
}

void GovernanceDialogTests::governanceHeaderBandAvoidsSeparateWarningFillAndReachesLeftEdge()
{
    QFile uiFile(resolveQtSourceFile("forms/governancewidget.ui"));
    QVERIFY2(uiFile.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.ui");
    const QString uiSource = QString::fromUtf8(uiFile.readAll());
    QVERIFY2(uiSource.contains(QStringLiteral("<widget class=\"QWidget\" name=\"warningInsetShell\"")),
             "Governance warning row should use a dedicated background shell widget");

    const int rootLayoutStart = uiSource.indexOf(QStringLiteral("<layout class=\"QHBoxLayout\" name=\"horizontalLayout_1\""));
    QVERIFY(rootLayoutStart >= 0);
    const QString rootLayoutSlice = uiSource.mid(rootLayoutStart, 700);
    const QRegularExpression zeroRootLeftMarginRe(
            QStringLiteral(R"(<property name=\"leftMargin\">\s*<number>0</number>)"),
            QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(zeroRootLeftMarginRe.match(rootLayoutSlice).hasMatch(),
             "Governance root layout should not inset the full dashboard from the left edge");

    const int leftLayoutStart = uiSource.indexOf(QStringLiteral("<layout class=\"QVBoxLayout\" name=\"verticalLayout_1\""));
    QVERIFY(leftLayoutStart >= 0);
    const QString leftLayoutSlice = uiSource.mid(leftLayoutStart, 700);
    const QRegularExpression zeroLeftColumnMarginRe(
            QStringLiteral(R"(<property name=\"leftMargin\">\s*<number>0</number>)"),
            QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(zeroLeftColumnMarginRe.match(leftLayoutSlice).hasMatch(),
             "Governance left column should let the header band reach the shell border");

    QFile cppFile(resolveQtSourceFile("governancewidget.cpp"));
    QVERIFY2(cppFile.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.cpp");
    const QString cppSource = QString::fromUtf8(cppFile.readAll());
    QVERIFY2(!cppSource.contains(QStringLiteral("ui->warningInsetShell->setAttribute(Qt::WA_StyledBackground, true);")),
             "Governance warning inset shell should not paint its own background bar");
    QVERIFY2(!cppSource.contains(QStringLiteral("setCssProperty(ui->warningInsetShell, \"governance-dashboard-band-fill\")")),
             "Governance warning row should not use a separate full-width band-fill surface");

    const auto cssHasBandFill = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        return css.contains(QStringLiteral("*[cssClass=\"governance-dashboard-band-fill\"]"));
    };

    QVERIFY2(!cssHasBandFill(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme should not define a separate governance band-fill background");
    QVERIFY2(!cssHasBandFill(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme should not define a separate governance band-fill background");
}

void GovernanceDialogTests::governanceHeaderCtaMatchesHeaderHeight()
{
    QFile uiFile(resolveQtSourceFile("forms/governancewidget.ui"));
    QVERIFY2(uiFile.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open governancewidget.ui");
    const QString uiSource = QString::fromUtf8(uiFile.readAll());

    const int headerStart = uiSource.indexOf(QStringLiteral("<widget class=\"QWidget\" name=\"containerTitles\""));
    QVERIFY(headerStart >= 0);
    if (headerStart < 0) {
        QFAIL("Governance header band widget not found");
        return;
    }
    const QString headerSlice = uiSource.mid(headerStart, 900);
    const QRegularExpression headerMinHeightRe(
            QStringLiteral(R"(<property name=\"minimumSize\">\s*<size>\s*<width>0</width>\s*<height>64</height>)"),
            QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(headerMinHeightRe.match(headerSlice).hasMatch(),
             "Governance header band should keep its compact 64px minimum height");

    const int ctaStart = uiSource.indexOf(QStringLiteral("<widget class=\"OptionButton\" name=\"btnCreateProposal\""));
    QVERIFY(ctaStart >= 0);
    if (ctaStart < 0) {
        QFAIL("Create Proposal CTA widget not found");
        return;
    }

    const QString ctaSlice = uiSource.mid(ctaStart, 900);
    const QRegularExpression minHeightRe(
            QStringLiteral(R"(<property name=\"minimumSize\">\s*<size>\s*<width>0</width>\s*<height>64</height>)"),
            QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(minHeightRe.match(ctaSlice).hasMatch(),
             "Create Proposal CTA should match the compact 64px governance header height");
}

void GovernanceDialogTests::governanceWarningBandUsesRoundedBorderInBothThemes()
{
    const auto cssHasRoundedWarningPill = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());

        const int start = css.indexOf(QStringLiteral("*[cssClass=\"governance-dashboard-warning\"]"));
        if (start < 0) return false;
        const int end = css.indexOf(QStringLiteral("*[cssClass=\"governance-dashboard-surface\"]"), start);
        if (end <= start) return false;
        const QString slice = css.mid(start, end - start);

        return slice.contains(QStringLiteral("border: 1px solid")) &&
               slice.contains(QStringLiteral("border-radius:")) &&
               !slice.contains(QStringLiteral("border-top:")) &&
               !slice.contains(QStringLiteral("border-bottom:"));
    };

    QVERIFY2(cssHasRoundedWarningPill(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme governance warning should use a full rounded border");
    QVERIFY2(cssHasRoundedWarningPill(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme governance warning should use a full rounded border");
}

void GovernanceDialogTests::governanceDashboardShellUsesSubtleDarkBorder()
{
    QFile file(resolveQtSourceFile("res/css/style_dark.css"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open style_dark.css");
    const QString source = QString::fromUtf8(file.readAll());

    const int shellStart = source.indexOf(QStringLiteral("*[cssClass=\"governance-dashboard-shell\"]"));
    QVERIFY(shellStart >= 0);
    const int shellEnd = source.indexOf(QStringLiteral("*[cssClass=\"governance-dashboard-band\"]"), shellStart);
    QVERIFY(shellEnd > shellStart);
    const QString shellSlice = source.mid(shellStart, shellEnd - shellStart);

    QVERIFY2(!shellSlice.contains(QStringLiteral("border: 1px solid #1D4ED8;")),
             "Dark governance shell should not use the old bright electric-blue border");
    QVERIFY2(shellSlice.contains(QStringLiteral("border: 1px solid rgba(")),
             "Dark governance shell should use a softer translucent border");
}

void GovernanceDialogTests::mainScreenUiFormsUsePremiumHeaderContainers()
{
    const struct UiExpectation {
        QString path;
        QString widgetName;
    } expectations[] = {
        {QStringLiteral("forms/send.ui"), QStringLiteral("containerHeader")},
        {QStringLiteral("forms/receivewidget.ui"), QStringLiteral("containerHeader")},
        {QStringLiteral("forms/addresseswidget.ui"), QStringLiteral("containerHeader")},
        {QStringLiteral("forms/masternodeswidget.ui"), QStringLiteral("containerHeader")},
        {QStringLiteral("forms/coldstakingwidget.ui"), QStringLiteral("containerTitle")}
    };

    for (const auto& expectation : expectations) {
        QFile file(resolveQtSourceFile(expectation.path));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(QString("Failed to open %1").arg(expectation.path)));
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY2(source.contains(QStringLiteral("<widget class=\"QWidget\" name=\"%1\"").arg(expectation.widgetName)),
                 qPrintable(QString("%1 should define premium header widget %2")
                            .arg(expectation.path, expectation.widgetName)));
    }
}

void GovernanceDialogTests::mainScreenWidgetsUseSharedPremiumHeaderClasses()
{
    const struct CppExpectation {
        QString path;
        QString headerWidget;
    } expectations[] = {
        {QStringLiteral("send.cpp"), QStringLiteral("containerHeader")},
        {QStringLiteral("receivewidget.cpp"), QStringLiteral("containerHeader")},
        {QStringLiteral("addresseswidget.cpp"), QStringLiteral("containerHeader")},
        {QStringLiteral("masternodeswidget.cpp"), QStringLiteral("containerHeader")},
        {QStringLiteral("coldstakingwidget.cpp"), QStringLiteral("containerTitle")}
    };

    for (const auto& expectation : expectations) {
        QFile file(resolveQtSourceFile(expectation.path));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(QString("Failed to open %1").arg(expectation.path)));
        const QString source = QString::fromUtf8(file.readAll());

        QVERIFY2(source.contains(QStringLiteral("ui->%1->setAttribute(Qt::WA_StyledBackground, true);").arg(expectation.headerWidget)),
                 qPrintable(QString("%1 should enable styled background on %2")
                            .arg(expectation.path, expectation.headerWidget)));
        QVERIFY2(source.contains(QStringLiteral("setCssProperty(ui->%1, \"screen-header-band\"").arg(expectation.headerWidget)),
                 qPrintable(QString("%1 should assign the shared premium header band class")
                            .arg(expectation.path)));
        QVERIFY2(source.contains(QStringLiteral("setCssProperty(ui->labelTitle, \"screen-header-title\"")),
                 qPrintable(QString("%1 should assign the shared premium header title class")
                            .arg(expectation.path)));
        QVERIFY2(source.contains(QStringLiteral("setCssProperty(ui->labelSubtitle1, \"screen-header-subtitle\"")),
                 qPrintable(QString("%1 should assign the shared premium header subtitle class")
                            .arg(expectation.path)));
    }
}

void GovernanceDialogTests::mainScreenPremiumHeaderStylesExistInBothThemes()
{
    const auto cssHasScreenHeaderStyles = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        return css.contains(QStringLiteral("*[cssClass=\"screen-header-band\"]")) &&
               css.contains(QStringLiteral("*[cssClass=\"screen-header-title\"]")) &&
               css.contains(QStringLiteral("*[cssClass=\"screen-header-subtitle\"]"));
    };

    QVERIFY2(cssHasScreenHeaderStyles(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme should define shared premium screen header classes");
    QVERIFY2(cssHasScreenHeaderStyles(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme should define shared premium screen header classes");
}

void GovernanceDialogTests::mainScreenWidgetsUseSharedPremiumRailClasses()
{
    const struct CppExpectation {
        QString path;
        QStringList snippets;
    } expectations[] = {
        {QStringLiteral("send.cpp"), {
             QStringLiteral("setCssProperty(ui->right, \"screen-side-rail\")"),
             QStringLiteral("setCssProperty(ui->btnCoinControl, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->btnChangeAddress, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->btnUri, \"screen-side-option\", true)")
         }},
        {QStringLiteral("receivewidget.cpp"), {
             QStringLiteral("setCssProperty(ui->right, \"screen-side-rail\")"),
             QStringLiteral("setCssProperty(ui->btnRequest, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->btnMyAddresses, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->sortWidget, \"screen-side-card\")"),
             QStringLiteral("setCssProperty(ui->listViewAddress, \"screen-side-card\")")
         }},
        {QStringLiteral("addresseswidget.cpp"), {
             QStringLiteral("setCssProperty(ui->right, \"screen-side-rail\")"),
             QStringLiteral("setCssProperty(ui->btnAddContact, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->layoutNewContact, \"screen-side-card\")"),
             QStringLiteral("setCssProperty(ui->labelName, \"screen-side-section-title\")"),
             QStringLiteral("setCssProperty(ui->labelAddress, \"screen-side-section-title\")")
         }},
        {QStringLiteral("masternodeswidget.cpp"), {
             QStringLiteral("setCssProperty(ui->right, \"screen-side-rail\")"),
             QStringLiteral("setCssProperty(ui->btnCoinControl, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->btnAbout, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->btnAboutController, \"screen-side-option\", true)")
         }},
        {QStringLiteral("coldstakingwidget.cpp"), {
             QStringLiteral("setCssProperty(ui->right, \"screen-side-rail\")"),
             QStringLiteral("setCssProperty(ui->btnCoinControl, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->btnColdStaking, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->btnMyStakingAddresses, \"screen-side-option\", true)"),
             QStringLiteral("setCssProperty(ui->sortWidget, \"screen-side-card\")"),
             QStringLiteral("setCssProperty(ui->listViewStakingAddress, \"screen-side-card\")")
         }}
    };

    for (const auto& expectation : expectations) {
        QFile file(resolveQtSourceFile(expectation.path));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(QString("Failed to open %1").arg(expectation.path)));
        const QString source = QString::fromUtf8(file.readAll());

        for (const QString& snippet : expectation.snippets) {
            QVERIFY2(source.contains(snippet),
                     qPrintable(QString("%1 should contain: %2").arg(expectation.path, snippet)));
        }
    }
}

void GovernanceDialogTests::mainScreenPremiumRailStylesExistInBothThemes()
{
    const auto cssHasScreenRailStyles = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        return css.contains(QStringLiteral("*[cssClass=\"screen-side-rail\"]")) &&
               css.contains(QStringLiteral("*[cssClass=\"screen-side-card\"]")) &&
               css.contains(QStringLiteral("*[cssClass=\"screen-side-section-title\"]")) &&
               css.contains(QStringLiteral("OptionButton[cssClass=\"screen-side-option\"] QWidget#layoutOptions2"));
    };

    QVERIFY2(cssHasScreenRailStyles(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme should define shared premium main-screen side-rail classes");
    QVERIFY2(cssHasScreenRailStyles(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme should define shared premium main-screen side-rail classes");
}

void GovernanceDialogTests::mainScreenWidgetsNormalizePremiumRailGutters()
{
    const struct CppExpectation {
        QString path;
        QStringList snippets;
    } expectations[] = {
        {QStringLiteral("send.cpp"), {
             QStringLiteral("ui->container_right->setContentsMargins(0, 0, 0, 0);"),
             QStringLiteral("ui->verticalLayout_4->removeItem(ui->verticalSpacer_8);")
         }},
        {QStringLiteral("receivewidget.cpp"), {
             QStringLiteral("ui->container_right->setContentsMargins(0, 0, 0, 0);"),
             QStringLiteral("ui->verticalLayout->removeItem(ui->verticalSpacer_8);")
         }},
        {QStringLiteral("addresseswidget.cpp"), {
             QStringLiteral("ui->container_right->setContentsMargins(0, 0, 0, 0);"),
             QStringLiteral("ui->verticalLayout_4->removeItem(ui->verticalSpacer_8);")
         }},
        {QStringLiteral("masternodeswidget.cpp"), {
             QStringLiteral("ui->container_right->setContentsMargins(0, 0, 0, 0);"),
             QStringLiteral("ui->verticalLayout_4->removeItem(ui->verticalSpacer_8);")
        }},
        {QStringLiteral("coldstakingwidget.cpp"), {
             QStringLiteral("ui->rightContainer->setContentsMargins(0, 0, 0, 0);"),
             QStringLiteral("ui->verticalLayout_4->removeItem(ui->verticalSpacer_8);")
         }}
    };

    for (const auto& expectation : expectations) {
        QFile file(resolveQtSourceFile(expectation.path));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(QString("Failed to open %1").arg(expectation.path)));
        const QString source = QString::fromUtf8(file.readAll());

        for (const QString& snippet : expectation.snippets) {
            QVERIFY2(source.contains(snippet),
                     qPrintable(QString("%1 should contain: %2").arg(expectation.path, snippet)));
        }
    }
}

void GovernanceDialogTests::premiumRailOptionTilesUseTallerSizingContract()
{
    QFile uiFile(resolveQtSourceFile("forms/optionbutton.ui"));
    QVERIFY2(uiFile.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open optionbutton.ui");
    const QString uiSource = QString::fromUtf8(uiFile.readAll());

    QVERIFY2(uiSource.contains(QStringLiteral("<height>96</height>")),
             "OptionButton should use a taller base geometry height");
    QVERIFY2(uiSource.contains(QStringLiteral("<width>28</width>")) &&
             uiSource.contains(QStringLiteral("<height>28</height>")),
             "OptionButton should use a larger arrow affordance size");

    const auto cssHasTallerPremiumRailTiles = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        const int start = css.indexOf(QStringLiteral("OptionButton[cssClass=\"screen-side-option\"] {"));
        if (start < 0) return false;
        const int end = css.indexOf(QStringLiteral("OptionButton[cssClass=\"screen-side-option\"] QWidget#layoutOptions2"), start);
        if (end <= start) return false;
        const QString slice = css.mid(start, end - start);
        return slice.contains(QStringLiteral("min-height: 96px;"));
    };

    QVERIFY2(cssHasTallerPremiumRailTiles(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme should define a taller premium right-rail tile height");
    QVERIFY2(cssHasTallerPremiumRailTiles(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme should define a taller premium right-rail tile height");
}

void GovernanceDialogTests::mainScreenWidgetsRemoveOuterHeaderInsets()
{
    const struct CppExpectation {
        QString path;
        QString snippet;
    } expectations[] = {
        {QStringLiteral("send.cpp"), QStringLiteral("ui->left->setContentsMargins(0,0,0,20);")},
        {QStringLiteral("receivewidget.cpp"), QStringLiteral("ui->left->setContentsMargins(0,0,0,20);")},
        {QStringLiteral("addresseswidget.cpp"), QStringLiteral("ui->left->setContentsMargins(0,0,0,20);")},
        {QStringLiteral("masternodeswidget.cpp"), QStringLiteral("ui->left->setContentsMargins(0,0,0,20);")},
        {QStringLiteral("coldstakingwidget.cpp"), QStringLiteral("ui->left->setContentsMargins(0,0,0,20);")}
    };

    for (const auto& expectation : expectations) {
        QFile file(resolveQtSourceFile(expectation.path));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(QString("Failed to open %1").arg(expectation.path)));
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY2(source.contains(expectation.snippet),
                 qPrintable(QString("%1 should contain: %2").arg(expectation.path, expectation.snippet)));
    }
}

void GovernanceDialogTests::premiumRailOptionTilesWrapTextAndUseTighterStackSpacing()
{
    QFile file(resolveQtSourceFile("optionbutton.cpp"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "Failed to open optionbutton.cpp");
    const QString source = QString::fromUtf8(file.readAll());

    QVERIFY2(source.contains(QStringLiteral("ui->labelTitleChange->setWordWrap(true);")),
             "OptionButton titles should wrap so the arrow remains visible");
    QVERIFY2(source.contains(QStringLiteral("ui->labelSubtitleChange->setWordWrap(true);")),
             "OptionButton subtitles should wrap so the arrow remains visible");

    const struct CppExpectation {
        QString path;
        QStringList snippets;
    } expectations[] = {
        {QStringLiteral("send.cpp"), {
             QStringLiteral("ui->container_right->setSpacing(6);"),
             QStringLiteral("ui->verticalLayout_4->setSpacing(6);")
         }},
        {QStringLiteral("receivewidget.cpp"), {
             QStringLiteral("ui->container_right->setSpacing(6);"),
             QStringLiteral("ui->verticalLayout->setSpacing(6);")
         }},
        {QStringLiteral("addresseswidget.cpp"), {
             QStringLiteral("ui->container_right->setSpacing(6);"),
             QStringLiteral("ui->verticalLayout_4->setSpacing(6);")
         }},
        {QStringLiteral("masternodeswidget.cpp"), {
             QStringLiteral("ui->container_right->setSpacing(6);"),
             QStringLiteral("ui->verticalLayout_4->setSpacing(6);")
        }},
        {QStringLiteral("coldstakingwidget.cpp"), {
             QStringLiteral("ui->rightContainer->setSpacing(6);"),
             QStringLiteral("ui->verticalLayout_4->setSpacing(6);")
         }}
    };

    for (const auto& expectation : expectations) {
        QFile f(resolveQtSourceFile(expectation.path));
        QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(QString("Failed to open %1").arg(expectation.path)));
        const QString cpp = QString::fromUtf8(f.readAll());
        for (const QString& snippet : expectation.snippets) {
            QVERIFY2(cpp.contains(snippet),
                     qPrintable(QString("%1 should contain: %2").arg(expectation.path, snippet)));
        }
    }
}

void GovernanceDialogTests::premiumRailOptionTilesUseCtaStyleArrowChrome()
{
    const auto cssHasCtaArrowChrome = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());

        const int start = css.indexOf(QStringLiteral("OptionButton[cssClass=\"screen-side-option\"] QPushButton#labelArrow3"));
        if (start < 0) return false;
        const int end = css.indexOf(QStringLiteral("OptionButton[cssClass=\"screen-side-option\"] QPushButton#labelArrow3:checked"), start);
        if (end <= start) return false;
        const QString slice = css.mid(start, end - start);

        return slice.contains(QStringLiteral("border-radius: 12px;")) &&
               slice.contains(QStringLiteral("background-color: rgba(")) &&
               slice.contains(QStringLiteral("border: 1px solid")) &&
               slice.contains(QStringLiteral("min-width: 24px;")) &&
               slice.contains(QStringLiteral("max-width: 24px;")) &&
               slice.contains(QStringLiteral("min-height: 24px;")) &&
               slice.contains(QStringLiteral("max-height: 24px;"));
    };

    const auto cssHasCtaArrowCheckedState = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        return css.contains(QStringLiteral("OptionButton[cssClass=\"screen-side-option\"] QPushButton#labelArrow3:checked"));
    };

    QVERIFY2(cssHasCtaArrowChrome(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme premium side-option arrows should use the CTA-style circular chrome");
    QVERIFY2(cssHasCtaArrowChrome(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme premium side-option arrows should use the CTA-style circular chrome");
    QVERIFY2(cssHasCtaArrowCheckedState(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme premium side-option arrows should define a checked-state icon");
    QVERIFY2(cssHasCtaArrowCheckedState(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme premium side-option arrows should define a checked-state icon");
}

void GovernanceDialogTests::qtStartupKeepsQt5HighDpiAttributes()
{
    QFile f(resolveQtSourceFile("organiclife.cpp"));
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    if (!f.isOpen()) {
        QFAIL("Unable to read organiclife.cpp");
        return;
    }

    const QString source = QString::fromUtf8(f.readAll());
    QVERIFY(source.contains(QStringLiteral("#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)")));
    QVERIFY(source.contains(QStringLiteral("QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);")));
    QVERIFY(source.contains(QStringLiteral("QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);")));
}

void GovernanceDialogTests::premiumHeadersRemainExpandableWithScaledFonts()
{
    const QFont originalAppFont = QApplication::font();
    struct FontRestorer {
        QFont font;
        ~FontRestorer() { QApplication::setFont(font); }
    } restorer{originalAppFont};

    QFont scaledFont = originalAppFont;
    const qreal basePointSize = originalAppFont.pointSizeF() > 0.0 ? originalAppFont.pointSizeF() : 10.0;
    scaledFont.setPointSizeF(basePointSize * 1.25);
    QApplication::setFont(scaledFont);

    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);

    SendWidget sendWidget(&mainWindow);
    ReceiveWidget receiveWidget(&mainWindow);
    AddressesWidget addressesWidget(&mainWindow);
    MasterNodesWidget masternodesWidget(&mainWindow);
    ColdStakingWidget coldStakingWidget(&mainWindow);
    GovernanceWidget governanceWidget(&mainWindow);

    const auto assertHeaderGeometry = [](QWidget& page, const QString& headerName, bool allowCompactCap = false) {
        page.resize(1280, 720);
        page.show();
        QTest::qWait(20);
        QCoreApplication::processEvents();

        QWidget* header = page.findChild<QWidget*>(headerName);
        QLabel* title = page.findChild<QLabel*>("labelTitle");
        QLabel* subtitle = page.findChild<QLabel*>("labelSubtitle1");

        QVERIFY2(header != nullptr, qPrintable(QString("Missing %1").arg(headerName)));
        QVERIFY(title != nullptr);
        QVERIFY(subtitle != nullptr);
        if (!header || !title || !subtitle) {
            return;
        }

        if (!allowCompactCap) {
            QVERIFY2(header->maximumHeight() >= 100000,
                     qPrintable(QString("%1 should not cap height for high-DPI font growth").arg(headerName)));
        } else {
            QVERIFY2(header->maximumHeight() <= 64,
                     qPrintable(QString("%1 should keep the compact governance height cap").arg(headerName)));
        }

        const QRect headerRect(QPoint(0, 0), header->size());
        const QRect titleRect(title->mapTo(header, QPoint(0, 0)), title->size());
        const QRect subtitleRect(subtitle->mapTo(header, QPoint(0, 0)), subtitle->size());

        QVERIFY2(headerRect.intersects(titleRect),
                 qPrintable(QString("%1 title should remain inside the header bounds").arg(headerName)));
        QVERIFY2(headerRect.intersects(subtitleRect),
                 qPrintable(QString("%1 subtitle should remain inside the header bounds").arg(headerName)));
        QVERIFY2(titleRect.bottom() <= headerRect.bottom(),
                 qPrintable(QString("%1 title should not clip vertically").arg(headerName)));
        QVERIFY2(subtitleRect.bottom() <= headerRect.bottom(),
                 qPrintable(QString("%1 subtitle should not clip vertically").arg(headerName)));
    };

    assertHeaderGeometry(sendWidget, QStringLiteral("containerHeader"));
    assertHeaderGeometry(receiveWidget, QStringLiteral("containerHeader"));
    assertHeaderGeometry(addressesWidget, QStringLiteral("containerHeader"));
    assertHeaderGeometry(masternodesWidget, QStringLiteral("containerHeader"));
    assertHeaderGeometry(coldStakingWidget, QStringLiteral("containerTitle"));
    assertHeaderGeometry(governanceWidget, QStringLiteral("containerTitles"), true);
}

void GovernanceDialogTests::dashboardWidgetUsesPremiumDashboardClasses()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 720);
    mainWindow.show();   // child visibility requires visible ancestors
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QWidget* leftShell = widget.findChild<QWidget*>("left");
    QWidget* rightShell = widget.findChild<QWidget*>("right");
    QWidget* headerBand = widget.findChild<QWidget*>("left_top_container");
    QWidget* warningPill = widget.findChild<QWidget*>("containerWarning");
    QListView* txList = widget.findChild<QListView*>("listTransactions");
    QWidget* analyticsModule = widget.findChild<QWidget*>("analyticsModule");
    QWidget* chartCanvas = widget.findChild<QWidget*>("chartContainer");

    QVERIFY(leftShell != nullptr);
    QVERIFY(rightShell != nullptr);
    QVERIFY(headerBand != nullptr);
    QVERIFY(warningPill != nullptr);
    QVERIFY(txList != nullptr);
    QVERIFY(analyticsModule != nullptr);
    QVERIFY(chartCanvas != nullptr);
    if (!leftShell || !rightShell || !headerBand || !warningPill || !txList || !analyticsModule || !chartCanvas) {
        return;
    }

    QCOMPARE(leftShell->property("cssClass").toString(), QStringLiteral("dashboard-shell-left"));
    QCOMPARE(rightShell->property("cssClass").toString(), QStringLiteral("dashboard-shell-right"));
    QCOMPARE(headerBand->property("cssClass").toString(), QStringLiteral("dashboard-header-band"));
    QCOMPARE(warningPill->property("cssClass").toString(), QStringLiteral("dashboard-warning-pill"));
    QCOMPARE(txList->property("cssClass").toString(), QStringLiteral("dashboard-transactions-list"));
    QCOMPARE(analyticsModule->property("cssClass").toString(), QStringLiteral("dashboard-analytics-card"));
    QCOMPARE(chartCanvas->property("cssClass").toString(), QStringLiteral("dashboard-chart-canvas"));
}

void GovernanceDialogTests::walletShellKeepsBrandIslandOutsideNavigation()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    const bool originalLightTheme = isLightTheme();
    const QString captureTheme = qEnvironmentVariable("ORGANICLIFE_CAPTURE_THEME").trimmed().toLower();
    if (captureTheme == QStringLiteral("light")) {
        setTheme(true);
    } else if (captureTheme == QStringLiteral("dark")) {
        setTheme(false);
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    bool widthOk = false;
    bool heightOk = false;
    const int captureWidth = qEnvironmentVariableIntValue("ORGANICLIFE_CAPTURE_WIDTH", &widthOk);
    const int captureHeight = qEnvironmentVariableIntValue("ORGANICLIFE_CAPTURE_HEIGHT", &heightOk);
    mainWindow.resize(widthOk ? captureWidth : 1280, heightOk ? captureHeight : 800);
    mainWindow.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QWidget* brandIsland = mainWindow.findChild<QWidget*>("brandIsland");
    QLabel* brandLogo = mainWindow.findChild<QLabel*>("brandIslandLogo");
    QWidget* navigation = mainWindow.findChild<QWidget*>("NavMenuWidget");
    QToolButton* transactionsButton = mainWindow.findChild<QToolButton*>("btnTransactions");
    QWidget* utilityPanel = mainWindow.findChild<QWidget*>("navUtilityPanel");
    TopBar* topBar = mainWindow.findChild<TopBar*>();

    QVERIFY(brandIsland != nullptr);
    QVERIFY(brandLogo != nullptr);
    QVERIFY(navigation != nullptr);
    QVERIFY(transactionsButton != nullptr);
    QVERIFY(utilityPanel != nullptr);
    QVERIFY(topBar != nullptr);
    if (!brandIsland || !brandLogo || !navigation || !transactionsButton || !utilityPanel || !topBar) return;

    QVERIFY2(!navigation->isAncestorOf(brandIsland), "The logo island must not belong to the navigation widget");
    QVERIFY2(!navigation->isAncestorOf(brandLogo), "The logo image must not be painted from inside the navigation widget");
    QVERIFY(!brandLogo->hasScaledContents());
    QVERIFY(!brandLogo->pixmap().isNull());

    QWidget* leftShell = brandIsland->parentWidget();
    QVERIFY(leftShell != nullptr);
    QCOMPARE(navigation->parentWidget(), leftShell);
    QVERIFY2(navigation->geometry().top() > brandIsland->geometry().bottom(),
             "Navigation must begin below the complete logo island footprint");
    QVERIFY(navigation->isAncestorOf(utilityPanel));
    QVERIFY(navigation->isAncestorOf(transactionsButton));
    QCOMPARE(transactionsButton->text(), QStringLiteral("Transactions"));
    QVERIFY2(!topBar->isVisible(), "The rejected legacy top utility bar must not consume visible wallet space");
    QCOMPARE(topBar->height(), 0);
    auto* walletUtilityButton = utilityPanel->findChild<QToolButton*>("navWalletSelector");
    QVERIFY2(walletUtilityButton != nullptr && walletUtilityButton->isVisible(),
             "The lower-left utility panel must expose the multi-wallet selector");

    const QString capturePath = qEnvironmentVariable("ORGANICLIFE_CAPTURE_PATH");
    if (!capturePath.isEmpty()) {
        if (qEnvironmentVariable("ORGANICLIFE_CAPTURE_SCREEN").trimmed().toLower() == QStringLiteral("transactions")) {
            mainWindow.goToTransactions();
            QCoreApplication::processEvents();
        }
        QVERIFY2(mainWindow.grab().save(capturePath), qPrintable(QString("Unable to save visual capture to %1").arg(capturePath)));
    }
    setTheme(originalLightTheme);
}

void GovernanceDialogTests::walletShellRedistributesLegacyTopBarUtilities()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(1280, 800);
    mainWindow.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    auto* topBar = mainWindow.findChild<TopBar*>();
    auto* walletSelector = mainWindow.findChild<QToolButton*>("navWalletSelector");
    auto* dashboard = mainWindow.findChild<DashboardWidget*>();
    auto* connections = mainWindow.findChild<QLabel*>("dashboardConnectionStatus");
    auto* staking = mainWindow.findChild<QLabel*>("dashboardStakingStatus");
    QVERIFY(topBar != nullptr);
    QVERIFY(walletSelector != nullptr);
    QVERIFY(dashboard != nullptr);
    QVERIFY(connections != nullptr);
    QVERIFY(staking != nullptr);
    if (!topBar || !walletSelector || !dashboard || !connections || !staking) return;

    QVERIFY(!topBar->isVisible());
    QVERIFY(walletSelector->isVisible());
    topBar->setNumConnections(4);
    topBar->setStakingStatusActive(true);
    QCOMPARE(connections->text(), QStringLiteral("4 connections"));
    QCOMPARE(staking->text(), QStringLiteral("Staking active"));
}

void GovernanceDialogTests::walletThemesUseApprovedGreenSemanticRoles()
{
    const auto assertRoles = [](const QString& relativePath, const QStringList& roles) {
        QFile file(resolveQtSourceFile(relativePath));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(relativePath));
        const QString css = QString::fromUtf8(file.readAll());
        for (const QString& role : roles) {
            QVERIFY2(css.contains(role, Qt::CaseInsensitive), qPrintable(QString("Missing approved role %1 in %2").arg(role, relativePath)));
        }
    };

    assertRoles(QStringLiteral("res/css/style_dark.css"), {
        QStringLiteral("#0D1512"), QStringLiteral("#101B16"), QStringLiteral("#121F1A"),
        QStringLiteral("#2B3D34"), QStringLiteral("#6EA449"), QStringLiteral("#F3F1E8")
    });
    assertRoles(QStringLiteral("res/css/style_light.css"), {
        QStringLiteral("#F3F6F1"), QStringLiteral("#E9EFE8"), QStringLiteral("#FFFFFF"),
        QStringLiteral("#D4DDD2"), QStringLiteral("#56863B"), QStringLiteral("#19221D")
    });
}

void GovernanceDialogTests::dashboardWidgetExposesApprovedFintechModules()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 800);
    mainWindow.show();
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    const QStringList requiredModules = {
        QStringLiteral("dashboardHeader"),
        QStringLiteral("headerAvailableBalance"),
        QStringLiteral("balanceCard"),
        QStringLiteral("analyticsModule"),
        QStringLiteral("analyticsStatRow"),
        QStringLiteral("recentTransactionsCard")
    };
    for (const QString& objectName : requiredModules) {
        QVERIFY2(widget.findChild<QWidget*>(objectName) != nullptr,
                 qPrintable(QString("Missing dashboard module: %1").arg(objectName)));
    }
}

void GovernanceDialogTests::dashboardBalanceCardOmitsMiniActivityFeed()
{
    QFile file(resolveQtSourceFile(QStringLiteral("dashboardwidget.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll());
    QVERIFY2(!source.contains(QStringLiteral("latestActivityCard")),
             "The balance card must stay clean; activity belongs only in the full-width transactions card");
    QVERIFY2(!source.contains(QStringLiteral("Latest activity")),
             "The dashboard must not duplicate a clipped mini transaction feed");
}

void GovernanceDialogTests::dashboardCardsUseEqualHeightContract()
{
    QFile file(resolveQtSourceFile(QStringLiteral("dashboardwidget.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll());
    QVERIFY2(source.contains(QStringLiteral("balanceCard->setProperty(\"dashboardCardRole\", \"primary\")")),
             "Balance card must opt into the shared primary-card height contract");
    QVERIFY2(source.contains(QStringLiteral("analyticsModule->setProperty(\"dashboardCardRole\", \"primary\")")),
             "Analytics card must opt into the shared primary-card height contract");
    QVERIFY2(source.contains(QStringLiteral("primaryCardHeight")),
             "Both top cards must share one explicit compact height contract");
}

void GovernanceDialogTests::dashboardPrimaryCardsAlignAndContainRewards()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 800);
    mainWindow.show();
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QWidget* topCards = widget.findChild<QWidget*>("dashboardTopCards");
    QWidget* balance = widget.findChild<QWidget*>("balanceCard");
    QWidget* analytics = widget.findChild<QWidget*>("analyticsModule");
    QWidget* chartContent = widget.findChild<QWidget*>("dashboardChartContent");
    QVERIFY(topCards != nullptr);
    QVERIFY(balance != nullptr);
    QVERIFY(analytics != nullptr);
    QVERIFY(chartContent != nullptr);
    if (!topCards || !balance || !analytics || !chartContent) return;

    QCOMPARE(balance->parentWidget(), topCards);
    QCOMPARE(analytics->parentWidget(), topCards);
    QCOMPARE(balance->geometry().top(), analytics->geometry().top());
    QCOMPARE(balance->height(), analytics->height());
    const QRect contentInCard(chartContent->mapTo(analytics, QPoint(0, 0)), chartContent->size());
    QVERIFY2(analytics->rect().contains(contentInCard), "The complete rewards content must remain inside its card");
}

void GovernanceDialogTests::dashboardHeaderExposesCompactLiveStatusCluster()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 800);
    mainWindow.show();
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QWidget* cluster = widget.findChild<QWidget*>("dashboardStatusCluster");
    QLabel* sync = widget.findChild<QLabel*>("dashboardSyncIcon");
    QLabel* connections = widget.findChild<QLabel*>("dashboardConnectionIcon");
    QLabel* staking = widget.findChild<QLabel*>("dashboardStakingIcon");
    QVERIFY(cluster != nullptr);
    QVERIFY(sync != nullptr);
    QVERIFY(connections != nullptr);
    QVERIFY(staking != nullptr);
    if (!cluster || !sync || !connections || !staking) return;
    QVERIFY(cluster->maximumHeight() <= 30);
    QVERIFY(!sync->pixmap().isNull());
    QVERIFY(!connections->pixmap().isNull());
    QVERIFY(!staking->pixmap().isNull());
}

void GovernanceDialogTests::dashboardStatusBadgesExposeRestrainedSemanticStates()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    QWidget* connections = widget.findChild<QWidget*>("dashboardConnectionBadge");
    QWidget* staking = widget.findChild<QWidget*>("dashboardStakingBadge");
    QLabel* stakingIcon = widget.findChild<QLabel*>("dashboardStakingIcon");
    QVERIFY(connections != nullptr);
    QVERIFY(staking != nullptr);
    QVERIFY(stakingIcon != nullptr);
    if (!connections || !staking || !stakingIcon) return;

    widget.setStakingStatusActive(false);
    QCOMPARE(staking->property("statusState").toString(), QStringLiteral("inactive"));
    QCOMPARE(stakingIcon->property("statusIcon").toString(), QStringLiteral(":/ic-check-staking-off"));
    QVERIFY(staking->graphicsEffect() == nullptr);

    widget.setStakingStatusActive(true);
    QCOMPARE(staking->property("statusState").toString(), QStringLiteral("active"));
    QCOMPARE(stakingIcon->property("statusIcon").toString(), QStringLiteral(":/ic-check-staking"));
    auto* stakingGlow = qobject_cast<QGraphicsDropShadowEffect*>(staking->graphicsEffect());
    QVERIFY(stakingGlow != nullptr);
    if (stakingGlow) QVERIFY(stakingGlow->blurRadius() <= 16.0);

    widget.setNumConnections(0);
    QCOMPARE(connections->property("statusState").toString(), QStringLiteral("danger"));
    QVERIFY(qobject_cast<QGraphicsDropShadowEffect*>(connections->graphicsEffect()) != nullptr);
    widget.setNumConnections(1);
    QCOMPARE(connections->property("statusState").toString(), QStringLiteral("warning"));
    widget.setNumConnections(5);
    QCOMPARE(connections->property("statusState").toString(), QStringLiteral("warning"));
    widget.setNumConnections(6);
    QCOMPARE(connections->property("statusState").toString(), QStringLiteral("connected"));
    QVERIFY(connections->graphicsEffect() == nullptr);
}

void GovernanceDialogTests::dashboardSyncBadgeDisplaysFlexibleBlockHeight()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    QWidget* syncBadge = widget.findChild<QWidget*>("dashboardSyncBadge");
    QFrame* divider = widget.findChild<QFrame*>("dashboardSyncDivider");
    QLabel* blockHeight = widget.findChild<QLabel*>("dashboardBlockHeight");
    QVERIFY(syncBadge != nullptr);
    QVERIFY(divider != nullptr);
    QVERIFY(blockHeight != nullptr);
    if (!syncBadge || !divider || !blockHeight) return;

    QVERIFY(QMetaObject::invokeMethod(&widget, "setBlockHeight", Q_ARG(int, 1234567)));
    QCOMPARE(blockHeight->text(), QStringLiteral("Block 1,234,567"));
    QVERIFY(syncBadge->maximumWidth() > syncBadge->minimumSizeHint().width());
    QVERIFY(divider->isVisibleTo(syncBadge));
}

void GovernanceDialogTests::walletBrandIslandUsesThemeAwareLogoSurface()
{
    QFile lightFile(resolveQtSourceFile(QStringLiteral("res/css/style_light.css")));
    QFile darkFile(resolveQtSourceFile(QStringLiteral("res/css/style_dark.css")));
    QVERIFY(lightFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(darkFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString lightCss = QString::fromUtf8(lightFile.readAll());
    const QString darkCss = QString::fromUtf8(darkFile.readAll());
    const QRegularExpression lightSurfaceRule(
        QStringLiteral("\\*\\[cssClass=\\\"brand-island\\\"\\]\\s*\\{[^}]*background(?:-color)?\\s*:\\s*#FFFFFF"),
        QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpression darkSurfaceRule(
        QStringLiteral("\\*\\[cssClass=\\\"brand-island\\\"\\]\\s*\\{[^}]*background\\s*:\\s*qlineargradient"),
        QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(lightSurfaceRule.match(lightCss).hasMatch(), "Light mode must retain the clean white logo surface");
    QVERIFY2(darkSurfaceRule.match(darkCss).hasMatch(), "Dark mode must use a restrained dark logo surface");
}

void GovernanceDialogTests::walletBrandIslandUsesOriginalTransparentLogoAsset()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    QLabel* floatingLogo = mainWindow.findChild<QLabel*>("brandIslandLogo");
    QLabel* cardLogo = widget.findChild<QLabel*>("dashboardBrandLogo");
    QVERIFY(floatingLogo != nullptr);
    QVERIFY(cardLogo != nullptr);
    if (!floatingLogo || !cardLogo) return;

    QCOMPARE(floatingLogo->property("sourceAsset").toString(), QStringLiteral(":/img-logo-pivx"));
    QCOMPARE(cardLogo->property("sourceAsset").toString(), QStringLiteral(":/img-logo-pivx"));
    QVERIFY(!floatingLogo->pixmap().isNull());
    QVERIFY(!cardLogo->pixmap().isNull());
    QVERIFY2(floatingLogo->pixmap().width() >= 120, "The supplied logo must fill more of its floating island");
}

void GovernanceDialogTests::dashboardChartShowsValuesOnlyOnHover()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.initChart();
    widget.resize(1280, 800);
    mainWindow.show();
    widget.show();
    QCoreApplication::processEvents();

    QLabel* tooltip = widget.findChild<QLabel*>("dashboardChartValueTooltip");
    QVERIFY(tooltip != nullptr);
    if (!tooltip) return;
    QVERIFY(!tooltip->isVisible());
    QVERIFY2(QMetaObject::invokeMethod(&widget, "onChartPointHovered",
                                      Q_ARG(QPointF, QPointF(2.0, 1234.5)),
                                      Q_ARG(bool, true)),
             "The chart must expose a hover-only value handler");
    // The standalone test widget is not mounted in OrganicLifeGUI's page stack,
    // so verify the handler's explicit show state rather than ancestor visibility.
    QVERIFY(!tooltip->isHidden());
    QCOMPARE(tooltip->text(), QStringLiteral("1,234.50 OLC"));
    QVERIFY2(QMetaObject::invokeMethod(&widget, "onChartPointHovered",
                                      Q_ARG(QPointF, QPointF(2.0, 1234.5)),
                                      Q_ARG(bool, false)),
             "The chart must hide the value when hover ends");
    QVERIFY(!tooltip->isVisible());
}

void GovernanceDialogTests::dashboardTransactionsHeaderUsesCompactContract()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 800);
    mainWindow.show();
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QWidget* header = widget.findChild<QWidget*>("left_top_container");
    QLabel* subtitle = widget.findChild<QLabel*>("labelSubtitle");
    QVERIFY(header != nullptr);
    QVERIFY(subtitle != nullptr);
    if (!header || !subtitle) return;
    QVERIFY2(header->minimumHeight() >= 56, "The activity header must still contain its filters comfortably");
    QVERIFY2(header->maximumHeight() <= 62, "The activity header must leave more room for transaction rows");
    QVERIFY2(!subtitle->isVisible(), "The compact activity header must not clip a redundant subtitle");
}

void GovernanceDialogTests::transactionsNavigationShowsFocusedActivityView()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    mainWindow.resize(1280, 800);
    mainWindow.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    auto* dashboard = mainWindow.findChild<DashboardWidget*>();
    auto* transactions = mainWindow.findChild<QToolButton*>("btnTransactions");
    auto* topCards = mainWindow.findChild<QWidget*>("dashboardTopCards");
    auto* recent = mainWindow.findChild<QWidget*>("recentTransactionsCard");
    auto* dashboardButton = mainWindow.findChild<QToolButton*>("btnDashboard");
    QVERIFY(dashboard != nullptr);
    QVERIFY(transactions != nullptr);
    QVERIFY(topCards != nullptr);
    QVERIFY(recent != nullptr);
    QVERIFY(dashboardButton != nullptr);
    if (!dashboard || !transactions || !topCards || !recent || !dashboardButton) return;

    transactions->click();
    QCoreApplication::processEvents();
    QVERIFY(dashboard->property("transactionsOnly").toBool());
    QVERIFY(!topCards->isVisible());
    QVERIFY(recent->isVisible());

    dashboardButton->click();
    QCoreApplication::processEvents();
    QVERIFY(!dashboard->property("transactionsOnly").toBool());
    QVERIFY(topCards->isVisible());
}

void GovernanceDialogTests::dashboardChartFilterIncludesGeneratedRegtestRewards()
{
    QFile file(resolveQtSourceFile(QStringLiteral("dashboardwidget.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll());
    const QRegularExpression chartFilter(
        R"(stakesFilter->setTypeFilter\([^;]*TransactionRecord::Generated[^;]*\);)",
        QRegularExpression::DotMatchesEverythingOption);
    QVERIFY2(chartFilter.match(source).hasMatch(),
             "Generated coinbase rewards must reach the chart filter on regtest and v6 reward chains");
}

void GovernanceDialogTests::dashboardWidgetThemesDefineRoundedScrollBarChrome()
{
    const auto cssHasDashboardChrome = [](const QString& cssPath) {
        QFile f(cssPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        const QString css = QString::fromUtf8(f.readAll());
        return css.contains(QStringLiteral("*[cssClass=\"dashboard-header-band\"]")) &&
               css.contains(QStringLiteral("*[cssClass=\"dashboard-shell-left\"]")) &&
               css.contains(QStringLiteral("*[cssClass=\"dashboard-shell-right\"]")) &&
               css.contains(QStringLiteral("*[cssClass=\"dashboard-analytics-card\"]")) &&
               css.contains(QStringLiteral("QListView[cssClass=\"dashboard-transactions-list\"] QScrollBar:vertical")) &&
               css.contains(QStringLiteral("QListView[cssClass=\"dashboard-transactions-list\"] QScrollBar::handle:vertical"));
    };

    QVERIFY2(cssHasDashboardChrome(resolveQtSourceFile("res/css/style_light.css")),
             "Light theme should define premium dashboard shells and rounded transaction list scrollbars");
    QVERIFY2(cssHasDashboardChrome(resolveQtSourceFile("res/css/style_dark.css")),
             "Dark theme should define premium dashboard shells and rounded transaction list scrollbars");
}

void GovernanceDialogTests::dashboardWidgetPreservesTransactionAnimationHooks()
{
    QFile f(resolveQtSourceFile("dashboardwidget.cpp"));
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    if (!f.isOpen()) {
        QFAIL("Unable to read dashboardwidget.cpp");
        return;
    }

    const QString source = QString::fromUtf8(f.readAll());
    QVERIFY(source.contains(QStringLiteral("connect(txModel, &TransactionTableModel::rowsAboutToBeInserted, this, &DashboardWidget::prepareTransactionInsertionAnimation);")));
    QVERIFY(source.contains(QStringLiteral("connect(txModel, &TransactionTableModel::rowsInserted, this, &DashboardWidget::processNewTransaction);")));
    QVERIFY(source.contains(QStringLiteral("void DashboardWidget::startInsertedRowAnimations(const QModelIndex& proxyIndex)")));
}

void GovernanceDialogTests::dashboardWidgetHeaderRemainsExpandableWithScaledFonts()
{
    const QFont originalAppFont = QApplication::font();
    struct FontRestorer {
        QFont font;
        ~FontRestorer() { QApplication::setFont(font); }
    } restorer{originalAppFont};

    QFont scaledFont = originalAppFont;
    const qreal basePointSize = originalAppFont.pointSizeF() > 0.0 ? originalAppFont.pointSizeF() : 10.0;
    scaledFont.setPointSizeF(basePointSize * 1.25);
    QApplication::setFont(scaledFont);

    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 720);
    mainWindow.show();   // child visibility requires visible ancestors
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QWidget* headerBand = widget.findChild<QWidget*>("left_top_container");
    QLabel* title = widget.findChild<QLabel*>("labelTitle");
    QLabel* subtitle = widget.findChild<QLabel*>("labelSubtitle");
    QComboBox* sortType = widget.findChild<QComboBox*>("comboBoxSortType");
    QComboBox* sort = widget.findChild<QComboBox*>("comboBoxSort");

    QVERIFY(headerBand != nullptr);
    QVERIFY(title != nullptr);
    QVERIFY(subtitle != nullptr);
    QVERIFY(sortType != nullptr);
    QVERIFY(sort != nullptr);
    if (!headerBand || !title || !subtitle || !sortType || !sort) {
        return;
    }

    QCOMPARE(headerBand->property("cssClass").toString(), QStringLiteral("dashboard-header-band"));

    const QRect headerRect(QPoint(0, 0), headerBand->size());
    const QRect titleRect(title->mapTo(headerBand, QPoint(0, 0)), title->size());
    const QRect subtitleRect(subtitle->mapTo(headerBand, QPoint(0, 0)), subtitle->size());
    const QRect sortTypeRect(sortType->mapTo(headerBand, QPoint(0, 0)), sortType->size());
    const QRect sortRect(sort->mapTo(headerBand, QPoint(0, 0)), sort->size());

    QVERIFY(titleRect.bottom() <= headerRect.bottom());
    QVERIFY(subtitleRect.bottom() <= headerRect.bottom());
    QVERIFY(sortTypeRect.bottom() <= headerRect.bottom());
    QVERIFY(sortRect.bottom() <= headerRect.bottom());
}

void GovernanceDialogTests::dashboardWidgetMergesRewardStatsAndChartIntoSidebarModule()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 720);
    mainWindow.show();   // child visibility requires visible ancestors
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QWidget* analyticsModule = widget.findChild<QWidget*>("analyticsModule");
    QLabel* amountPiv = widget.findChild<QLabel*>("labelAmountPiv");
    QLabel* amountMn = widget.findChild<QLabel*>("labelAmountMN");
    QWidget* liveChart = widget.findChild<QWidget*>("layoutChart");
    QFrame* emptyChart = widget.findChild<QFrame*>("emptyContainerChart");
    QVBoxLayout* analyticsLayout = qobject_cast<QVBoxLayout*>(analyticsModule ? analyticsModule->layout() : nullptr);

    QVERIFY(analyticsModule != nullptr);
    QVERIFY(amountPiv != nullptr);
    QVERIFY(amountMn != nullptr);
    QVERIFY(liveChart != nullptr);
    QVERIFY(emptyChart != nullptr);
    QVERIFY(analyticsLayout != nullptr);
    if (!analyticsModule || !amountPiv || !amountMn || !liveChart || !emptyChart || !analyticsLayout) {
        return;
    }

    QCOMPARE(analyticsModule->property("cssClass").toString(), QStringLiteral("dashboard-analytics-card"));
    QVERIFY(analyticsLayout->spacing() <= 6); // compact reference-card spacing
    QVERIFY(analyticsModule->isAncestorOf(amountPiv));
    QVERIFY(analyticsModule->isAncestorOf(amountMn));
    QVERIFY(analyticsModule->isAncestorOf(liveChart));
    QVERIFY(analyticsModule->isAncestorOf(emptyChart));
}

void GovernanceDialogTests::dashboardWidgetRestoresTransactionListVisibilityAfterWalletSwitch()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 720);
    mainWindow.show();   // child visibility requires visible ancestors
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QFrame* emptyState = widget.findChild<QFrame*>("emptyContainer");
    QListView* txList = widget.findChild<QListView*>("listTransactions");
    QComboBox* sortType = widget.findChild<QComboBox*>("comboBoxSortType");
    QComboBox* sort = widget.findChild<QComboBox*>("comboBoxSort");

    QVERIFY(emptyState != nullptr);
    QVERIFY(txList != nullptr);
    QVERIFY(sortType != nullptr);
    QVERIFY(sort != nullptr);
    if (!emptyState || !txList || !sortType || !sort) {
        return;
    }

    widget.updateTransactionViewState(false, false);
    QVERIFY(emptyState->isVisible());
    QVERIFY(!txList->isVisible());
    QVERIFY(!sortType->isVisible());
    QVERIFY(!sort->isVisible());

    widget.updateTransactionViewState(true, true);
    QVERIFY(!emptyState->isVisible());
    QVERIFY(txList->isVisible());
    QVERIFY(sortType->isVisible());
    QVERIFY(sort->isVisible());
}

void GovernanceDialogTests::masternodeWidgetRefreshesWhenModelLoadsConfiguredMasternodes()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    struct MasternodeConfigRestorer {
        MasternodeConfigRestorer() { masternodeConfig.clear(); }
        ~MasternodeConfigRestorer() { masternodeConfig.clear(); }
    } restorer;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    MasterNodesWidget widget(&mainWindow);
    MNModel mnModel(nullptr);
    widget.setMNModel(&mnModel);
    widget.resize(1280, 720);
    mainWindow.show();   // child visibility requires visible ancestors
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QListView* masternodeList = widget.findChild<QListView*>("listMn");
    QWidget* emptyState = widget.findChild<QWidget*>("emptyContainer");

    QVERIFY(masternodeList != nullptr);
    QVERIFY(emptyState != nullptr);
    if (!masternodeList || !emptyState) {
        QFAIL("Missing masternode widget state controls");
        return;
    }

    QVERIFY(emptyState->isVisible());
    QVERIFY(!masternodeList->isVisible());
    QCOMPARE(mnModel.rowCount(), 0);

    masternodeConfig.add(
            "mn-test",
            "127.0.0.2:31616",
            "93HaYBVUCYjEMeeH1Y4sBGLALQZE1Yc1K64xiqgX37tGBDQL8Xg",
            "1111111111111111111111111111111111111111111111111111111111111111",
            "0");

    mnModel.updateMNList();
    QCoreApplication::processEvents();

    QCOMPARE(mnModel.rowCount(), 1);
    QVERIFY2(masternodeList->isVisible(), "Configured masternodes should become visible after the model refreshes");
    QVERIFY2(!emptyState->isVisible(), "Empty state should hide once configured masternodes are loaded");
}

void GovernanceDialogTests::dashboardWidgetUsesSelfDescribingRewardTiles()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 720);
    mainWindow.show();   // child visibility requires visible ancestors
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QWidget* stakingTile = widget.findChild<QWidget*>("stakingRewardTile");
    QWidget* masternodeTile = widget.findChild<QWidget*>("masternodeRewardTile");
    QLabel* chartLabel = widget.findChild<QLabel*>("labelChart");
    QLabel* legacyStakingLabel = widget.findChild<QLabel*>("labelPiv");
    QLabel* legacyMasternodeLabel = widget.findChild<QLabel*>("labelMN");
    QLabel* stakingMarker = widget.findChild<QLabel*>("labelSquarePiv");
    QLabel* masternodeMarker = widget.findChild<QLabel*>("labelSquareMN");

    QVERIFY(stakingTile != nullptr);
    QVERIFY(masternodeTile != nullptr);
    QVERIFY(chartLabel != nullptr);
    QVERIFY(legacyStakingLabel != nullptr);
    QVERIFY(legacyMasternodeLabel != nullptr);
    QVERIFY(stakingMarker != nullptr);
    QVERIFY(masternodeMarker != nullptr);
    if (!stakingTile || !masternodeTile || !chartLabel || !legacyStakingLabel || !legacyMasternodeLabel ||
        !stakingMarker || !masternodeMarker) {
        return;
    }

    const auto tileHasLabel = [](QWidget* tile, const QString& expected) {
        const auto labels = tile->findChildren<QLabel*>();
        for (QLabel* label : labels) {
            if (label && label->text() == expected) {
                return true;
            }
        }
        return false;
    };

    QVERIFY(tileHasLabel(stakingTile, QStringLiteral("Staking")));
    QVERIFY(tileHasLabel(masternodeTile, QStringLiteral("Masternodes")));
    QVERIFY(stakingTile->isAncestorOf(stakingMarker));
    QVERIFY(masternodeTile->isAncestorOf(masternodeMarker));
    QVERIFY(stakingMarker->styleSheet().contains(QStringLiteral("qlineargradient")));
    QVERIFY(masternodeMarker->styleSheet().contains(QStringLiteral("qlineargradient")));
    QVERIFY(chartLabel->isHidden());
    QVERIFY(legacyStakingLabel->isHidden());
    QVERIFY(legacyMasternodeLabel->isHidden());
}

void GovernanceDialogTests::dashboardWidgetPrioritizesChartHeightWithProminentRewardMarkers()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) {
        QFAIL("Failed to create network style");
        return;
    }

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 720);
    mainWindow.show();   // child visibility requires visible ancestors
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QLabel* stakingMarker = widget.findChild<QLabel*>("labelSquarePiv");
    QLabel* masternodeMarker = widget.findChild<QLabel*>("labelSquareMN");
    QWidget* rewardSummaryRow = widget.findChild<QWidget*>("rewardSummaryRow");
    QWidget* chartBody = widget.findChild<QWidget*>("verticalWidgetChart");

    QVERIFY(stakingMarker != nullptr);
    QVERIFY(masternodeMarker != nullptr);
    QVERIFY(rewardSummaryRow != nullptr);
    QVERIFY(chartBody != nullptr);
    if (!stakingMarker || !masternodeMarker || !rewardSummaryRow || !chartBody) {
        return;
    }

    QVERIFY(stakingMarker->width() >= 14);
    QVERIFY(stakingMarker->height() >= 14);
    QVERIFY(masternodeMarker->width() >= 14);
    QVERIFY(masternodeMarker->height() >= 14);
    QVERIFY(rewardSummaryRow->maximumHeight() <= 96);
    QVERIFY(chartBody->minimumHeight() >= 96);
    QVERIFY(chartBody->minimumHeight() <= 120);
}

void GovernanceDialogTests::dashboardChartPeriodControlsStayInsideAnalyticsCard()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.resize(1280, 800);
    mainWindow.show();
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();

    QWidget* analytics = widget.findChild<QWidget*>("analyticsModule");
    QWidget* period = widget.findChild<QWidget*>("dashboardPeriodFilter");
    QComboBox* years = widget.findChild<QComboBox*>("comboBoxYears");
    QComboBox* months = widget.findChild<QComboBox*>("comboBoxMonths");
    QVERIFY(analytics != nullptr);
    QVERIFY(period != nullptr);
    QVERIFY(years != nullptr);
    QVERIFY(months != nullptr);
    if (!analytics || !period || !years || !months) return;

    QVERIFY2(analytics->isAncestorOf(period), "Period selectors must stay inside the analytics card");
    QVERIFY2(period->maximumHeight() <= 42, "Period selectors must use a compact single-row treatment");
    QCOMPARE(years->property("cssClass").toString(), QStringLiteral("dashboard-period-combo"));
    QCOMPARE(months->property("cssClass").toString(), QStringLiteral("dashboard-period-combo"));
}

void GovernanceDialogTests::dashboardChartKeepsReferenceCleanAxisTreatment()
{
    QFile file(resolveQtSourceFile(QStringLiteral("dashboardwidget.cpp")));
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString source = QString::fromUtf8(file.readAll());
    QVERIFY2(source.contains(QStringLiteral("axisY->setLabelsVisible(false)")),
             "The rewards chart should not show clipped numeric labels on its compact right edge");
    QVERIFY2(source.contains(QStringLiteral("axisY->setLineVisible(false)")),
             "The rewards chart should use the mockup's clean grid-only Y axis treatment");
}

void GovernanceDialogTests::dashboardChartUsesVisibleCompactTimelineLabels()
{
    std::unique_ptr<const NetworkStyle> networkStyle(NetworkStyle::instantiate("main"));
    QVERIFY(networkStyle != nullptr);
    if (!networkStyle) return;

    OrganicLifeGUI mainWindow(networkStyle.get(), nullptr);
    DashboardWidget widget(&mainWindow);
    widget.initChart();
    widget.resize(1280, 800);
    mainWindow.show();
    widget.show();
    QTest::qWait(20);
    QCoreApplication::processEvents();
    widget.updateChartTimelineGeometry();

    int timelineLabelCount = 0;
    for (QLabel* label : widget.findChildren<QLabel*>()) {
        if (label->property("cssClass").toString() == QStringLiteral("dashboard-chart-timeline-label")) {
            ++timelineLabelCount;
        }
    }
    QCOMPARE(timelineLabelCount, 5);
    QWidget* timeline = widget.findChild<QWidget*>(QStringLiteral("dashboardChartTimeline"));
    QVERIFY(timeline != nullptr);
    if (timeline) {
        QCOMPARE(timeline->parentWidget(), widget.chartView->viewport());
        QWidget* visibleAnalyticsCard = widget.findChild<QWidget*>(QStringLiteral("analyticsModule"));
        QVERIFY(visibleAnalyticsCard != nullptr);
        if (visibleAnalyticsCard) {
            const QPoint cardBottomGlobal = visibleAnalyticsCard->mapToGlobal(
                QPoint(0, visibleAnalyticsCard->height()));
            const int visibleCardBottom = widget.chartView->viewport()->mapFromGlobal(cardBottomGlobal).y();
            QVERIFY2(timeline->geometry().bottom() <= visibleCardBottom - 4,
                     "The chart timeline must stay above the visible analytics-card edge");
        }
    }
    QVERIFY(widget.axisX != nullptr);
    if (widget.axisX) {
        QVERIFY2(!widget.axisX->labelsVisible(), "The compact timeline should replace clipped chart-native labels");
    }
    QVERIFY(widget.chart != nullptr);
    if (widget.chart) {
        QVERIFY2(widget.chart->margins().bottom() >= 20, "The plot must reserve in-card room for its timeline");
    }
}

void GovernanceDialogTests::voteDialogSubtitleKeepsStablePaddingAcrossModes()
{
    QWidget parent;
    MNModel mnModel(nullptr);
    FakeGovernanceModel model(&mnModel);
    VoteDialog dialog(&parent, &model, &mnModel);
    dialog.setProposal(BuildTestProposal());

    auto* subtitle = dialog.findChild<QLabel*>("labelSubtitle");
    auto* mnModeRadio = dialog.findChild<QRadioButton*>("radioModeMasternode");
    auto* coinModeRadio = dialog.findChild<QRadioButton*>("radioModeCoinLock");
    QVERIFY(subtitle != nullptr);
    QVERIFY(mnModeRadio != nullptr);
    QVERIFY(coinModeRadio != nullptr);
    if (!subtitle || !mnModeRadio || !coinModeRadio) {
        QFAIL("Missing VoteDialog subtitle controls");
        return;
    }

    dialog.show();
    QTest::qWait(50);
    QCoreApplication::processEvents();

    QVERIFY(subtitle->wordWrap());
    QVERIFY(subtitle->minimumHeight() >= 38);

    const QRect mnRect = subtitle->geometry();
    coinModeRadio->setChecked(true);
    QTest::qWait(50);
    QCoreApplication::processEvents();
    const QRect coinRect = subtitle->geometry();

    QCOMPARE(coinRect.top(), mnRect.top());
    QCOMPARE(coinRect.height(), mnRect.height());
}

void GovernanceDialogTests::proposalInfoDialogShowsCoinVoteTotals()
{
    QWidget parent;
    ProposalInfoDialog dialog(&parent);

    ProposalInfo proposal = BuildTestProposal();
    proposal.votesYes = 7;
    proposal.votesNo = 2;
    proposal.coinVotesYes = 250;
    proposal.coinVotesNo = 40;
    dialog.setProposal(proposal);

    auto* textPosVotes = dialog.findChild<QLabel*>("textPosVotes");
    auto* textNegVotes = dialog.findChild<QLabel*>("textNegVotes");
    auto* textPosCoinVotes = dialog.findChild<QLabel*>("textPosCoinVotes");
    auto* textNegCoinVotes = dialog.findChild<QLabel*>("textNegCoinVotes");
    QVERIFY(textPosVotes != nullptr);
    QVERIFY(textNegVotes != nullptr);
    QVERIFY(textPosCoinVotes != nullptr);
    QVERIFY(textNegCoinVotes != nullptr);
    if (!textPosVotes || !textNegVotes || !textPosCoinVotes || !textNegCoinVotes) {
        QFAIL("Missing vote labels on proposal info dialog");
        return;
    }

    QCOMPARE(textPosVotes->text(), QString("7"));
    QCOMPARE(textNegVotes->text(), QString("2"));
    QCOMPARE(textPosCoinVotes->text(), QString("250"));
    QCOMPARE(textNegCoinVotes->text(), QString("40"));
}

void GovernanceDialogTests::transactionRecordHandlesMalformedProposalMetadata()
{
    std::string proposalName;

    bool threw = false;
    bool ok = false;
    try {
        ok = TransactionRecord::TryExtractProposalName("00", proposalName);
    } catch (const std::exception&) {
        threw = true;
    }
    QVERIFY(!threw);
    QVERIFY(!ok);
    QVERIFY(proposalName.empty());

    const CBudgetProposal proposal("qt-test-proposal", "https://example.com", 1, CScript(), 10 * COIN, 144, UINT256_ZERO);
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << proposal;
    const std::string proposalHex = HexStr(MakeUCharSpan(ss));
    proposalName.clear();
    QVERIFY(TransactionRecord::TryExtractProposalName(proposalHex, proposalName));
    QCOMPARE(QString::fromStdString(proposalName), QString("qt-test-proposal"));
}

void GovernanceDialogTests::createProposalDialogHasComfortableMinimumWidthForStepHeader()
{
    CreateProposalDialog dialog(nullptr, nullptr, nullptr);
    dialog.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    auto* stepHeaderRow = dialog.findChild<QWidget*>("widget_2");
    QVERIFY(stepHeaderRow != nullptr);
    if (!stepHeaderRow) {
        QFAIL("Missing create proposal top step header row");
        return;
    }

    constexpr int kExpectedMinimumDialogWidth = 760;
    QVERIFY2(dialog.minimumWidth() >= kExpectedMinimumDialogWidth,
             "Create proposal dialog minimum width is too small for top step header");
    QVERIFY2(dialog.width() >= kExpectedMinimumDialogWidth,
             "Create proposal dialog should open wide enough to avoid clipping top step header");
}

void GovernanceDialogTests::txDetailDialogCentersOnParentWindow()
{
    QWidget parent;
    parent.resize(900, 680);
    parent.move(170, 110);
    parent.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    TxDetailDialog detailDialog(&parent, false);
    detailDialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    const QRect parentGlobal(parent.mapToGlobal(QPoint(0, 0)), parent.size());
    const QRect dialogGlobal = detailDialog.frameGeometry();
    QVERIFY2(std::abs(dialogGlobal.center().x() - parentGlobal.center().x()) <= 4,
             qPrintable(QString("TxDetailDialog not centered horizontally (dialog=%1 parent=%2)")
                                .arg(dialogGlobal.center().x())
                                .arg(parentGlobal.center().x())));
    QVERIFY2(std::abs(dialogGlobal.center().y() - parentGlobal.center().y()) <= 4,
             qPrintable(QString("TxDetailDialog not centered vertically (dialog=%1 parent=%2)")
                                .arg(dialogGlobal.center().y())
                                .arg(parentGlobal.center().y())));
}

void GovernanceDialogTests::txDetailDialogUsesSharedDraggableHeaderChrome()
{
    QWidget parent;
    parent.resize(940, 720);
    parent.move(100, 120);
    parent.show();
    QTest::qWait(40);
    QCoreApplication::processEvents();

    TxDetailDialog detailDialog(&parent, false);
    detailDialog.show();
    QTest::qWait(80);
    QCoreApplication::processEvents();

    auto* header = detailDialog.findChild<QWidget*>("containerDialogHeader");
    auto* body = detailDialog.findChild<QWidget*>("frame");
    auto* title = detailDialog.findChild<QLabel*>("labelTitle");
    auto* btnEsc = detailDialog.findChild<QPushButton*>("btnEsc");
    QVERIFY(header != nullptr);
    QVERIFY(body != nullptr);
    QVERIFY(title != nullptr);
    QVERIFY(btnEsc != nullptr);
    if (!header || !body || !title || !btnEsc) {
        QFAIL("TxDetailDialog is missing shared header chrome widgets");
        return;
    }

    QCOMPARE(header->property("cssClass").toString(), QString("container-dialog-header"));
    QCOMPARE(body->property("cssClass").toString(), QString("container-dialog-body"));
    QCOMPARE(title->parentWidget(), header);
    QCOMPARE(btnEsc->parentWidget(), header);
    QVERIFY(detailDialog.windowFlags().testFlag(Qt::FramelessWindowHint));

    const QPoint beforeDrag = detailDialog.pos();
    const QPoint pressPos(std::max(8, header->width() / 4), std::max(8, header->height() / 2));
    const QPoint movePos(std::min(header->width() - 8, pressPos.x() + 70), pressPos.y());
    QTest::mousePress(header, Qt::LeftButton, Qt::NoModifier, pressPos);
    QTest::mouseMove(header, movePos, 20);
    QTest::mouseRelease(header, Qt::LeftButton, Qt::NoModifier, movePos);
    QCoreApplication::processEvents();
    QVERIFY2(detailDialog.pos() != beforeDrag, "Dragging header should move TxDetailDialog");
}

void GovernanceDialogTests::txDetailDialogCloseIconIsResolvedAtRuntime()
{
    QWidget parent;
    TxDetailDialog detailDialog(&parent, false);
    detailDialog.show();
    QTest::qWait(60);
    QCoreApplication::processEvents();

    auto* btnEsc = detailDialog.findChild<QPushButton*>("btnEsc");
    QVERIFY(btnEsc != nullptr);
    if (!btnEsc) {
        QFAIL("Missing TxDetailDialog close button");
        return;
    }

    QVERIFY2(!btnEsc->icon().isNull(), "TxDetailDialog close icon should resolve to a runtime icon");
    const QSize requested = btnEsc->iconSize().isValid() ? btnEsc->iconSize() : QSize(20, 20);
    const QPixmap pixmap = btnEsc->icon().pixmap(requested);
    QVERIFY2(!pixmap.isNull(), "TxDetailDialog close icon pixmap should be available");
}

void GovernanceDialogTests::txDetailDialogBodyActionIconsResolveImmediatelyAfterConstruction()
{
    QWidget parent;
    TxDetailDialog detailDialog(&parent, false);

    const auto assertIconResolved = [](QPushButton* button, const QString& name) {
        QVERIFY2(button != nullptr, qPrintable(QString("Missing TxDetailDialog button: %1").arg(name)));
        if (!button) return;
        QVERIFY2(!button->icon().isNull(), qPrintable(QString("%1 icon should resolve before show").arg(name)));
        const QSize requested = button->iconSize().isValid() ? button->iconSize() : QSize(20, 20);
        const QPixmap pixmap = button->icon().pixmap(requested);
        QVERIFY2(!pixmap.isNull(), qPrintable(QString("%1 icon pixmap should be available before show").arg(name)));
    };

    assertIconResolved(detailDialog.findChild<QPushButton*>("pushCopy"), QStringLiteral("pushCopy"));
    assertIconResolved(detailDialog.findChild<QPushButton*>("pushCopyMemo"), QStringLiteral("pushCopyMemo"));
    assertIconResolved(detailDialog.findChild<QPushButton*>("pushInputs"), QStringLiteral("pushInputs"));
    assertIconResolved(detailDialog.findChild<QPushButton*>("pushOutputs"), QStringLiteral("pushOutputs"));
}

void GovernanceDialogTests::txDetailDialogBodyActionIconsAreResolvedAtRuntime()
{
    QWidget parent;
    TxDetailDialog detailDialog(&parent, false);
    detailDialog.show();
    QTest::qWait(60);
    QCoreApplication::processEvents();

    const auto assertIconResolved = [](QPushButton* button, const QString& name) {
        QVERIFY2(button != nullptr, qPrintable(QString("Missing TxDetailDialog button: %1").arg(name)));
        if (!button) return;
        QVERIFY2(!button->icon().isNull(), qPrintable(QString("%1 icon should resolve").arg(name)));
        const QSize requested = button->iconSize().isValid() ? button->iconSize() : QSize(20, 20);
        const QPixmap pixmap = button->icon().pixmap(requested);
        QVERIFY2(!pixmap.isNull(), qPrintable(QString("%1 icon pixmap should be available").arg(name)));
    };

    assertIconResolved(detailDialog.findChild<QPushButton*>("pushCopy"), QStringLiteral("pushCopy"));
    assertIconResolved(detailDialog.findChild<QPushButton*>("pushCopyMemo"), QStringLiteral("pushCopyMemo"));
    assertIconResolved(detailDialog.findChild<QPushButton*>("pushInputs"), QStringLiteral("pushInputs"));
    assertIconResolved(detailDialog.findChild<QPushButton*>("pushOutputs"), QStringLiteral("pushOutputs"));
}

void GovernanceDialogTests::txDetailDialogExposesAbandonActionPlaceholder()
{
    QWidget parent;
    TxDetailDialog detailDialog(&parent, false);

    auto* abandonButton = detailDialog.findChild<QPushButton*>("btnAbandonTx");
    QVERIFY(abandonButton != nullptr);
    if (!abandonButton) {
        QFAIL("Missing abandon transaction button in transaction details dialog");
        return;
    }

    // Hidden until a conflicted tx is loaded.
    QVERIFY(!abandonButton->isVisible());
}

void GovernanceDialogTests::txDetailDialogConflictActionVisibilityRules()
{
    QVERIFY(TxDetailDialog::shouldShowConflictActions(TransactionStatus::Conflicted, false));
    QVERIFY(TxDetailDialog::shouldShowConflictActions(TransactionStatus::NotAccepted, false));

    QVERIFY(TxDetailDialog::shouldShowConflictActions(TransactionStatus::Unconfirmed, true));
    QVERIFY(!TxDetailDialog::shouldShowConflictActions(TransactionStatus::Unconfirmed, false));

    QVERIFY(!TxDetailDialog::shouldShowConflictActions(TransactionStatus::Confirming, false));
    QVERIFY(!TxDetailDialog::shouldShowConflictActions(TransactionStatus::Confirmed, true));
}

void GovernanceDialogTests::txDetailDialogConflictActionResolutionRules()
{
    QCOMPARE(TxDetailDialog::resolveConflictAction(TransactionStatus::Unconfirmed, true, true),
             TxDetailDialog::ConflictAction::Abandon);
    QCOMPARE(TxDetailDialog::resolveConflictAction(TransactionStatus::Unconfirmed, true, false),
             TxDetailDialog::ConflictAction::HideFromHistory);
    QCOMPARE(TxDetailDialog::resolveConflictAction(TransactionStatus::Conflicted, false, false),
             TxDetailDialog::ConflictAction::HideFromHistory);
    QCOMPARE(TxDetailDialog::resolveConflictAction(TransactionStatus::NotAccepted, false, false),
             TxDetailDialog::ConflictAction::HideFromHistory);
    QCOMPARE(TxDetailDialog::resolveConflictAction(TransactionStatus::Confirmed, false, false),
             TxDetailDialog::ConflictAction::None);
}

void GovernanceDialogTests::sendWidgetRecoveryRulesForFailedBroadcast()
{
    QVERIFY(!SendWidget::shouldRequirePeerConnections(false, 0));
    QVERIFY(SendWidget::shouldRequirePeerConnections(true, 0));
    QVERIFY(!SendWidget::shouldRequirePeerConnections(true, 2));

    WalletModel::SendCoinsReturn okResult(WalletModel::OK);
    QVERIFY(!SendWidget::shouldAutoAbandonFailedCommit(okResult));

    WalletModel::SendCoinsReturn failedResult(WalletModel::TransactionCommitFailed);
    failedResult.commitRes.status = CWallet::CommitStatus::NotAccepted;
    failedResult.commitRes.hashTx = uint256S("01");
    QVERIFY(SendWidget::shouldAutoAbandonFailedCommit(failedResult));

    failedResult.commitRes.hashTx = UINT256_ZERO;
    QVERIFY(!SendWidget::shouldAutoAbandonFailedCommit(failedResult));

    failedResult.commitRes.hashTx = uint256S("02");
    failedResult.commitRes.status = CWallet::CommitStatus::Abandoned;
    QVERIFY(!SendWidget::shouldAutoAbandonFailedCommit(failedResult));
}
