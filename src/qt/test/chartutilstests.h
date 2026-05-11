// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_TEST_CHARTUTILSTESTS_H
#define PIVX_QT_TEST_CHARTUTILSTESTS_H

#include <QObject>
#include <QTest>

class ChartUtilsTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void axisRoundsToExpectedSteps();
    void axisFormattingIsCompactAndStable();
    void monthWindowResolvesToCurrentWeek();
    void monthWindowPagingBoundsAreCorrect();
    void monthEmptyWindowKeepsChartVisible();
    void rewardTypeClassificationIncludesV6CoinbaseMasternodePayments();
    void coinbaseCreditsAreClassifiedByRewardType();
    void progressLabelFormattingIsStable();
    void transactionQueueReloadThresholdWorks();
};

#endif // PIVX_QT_TEST_CHARTUTILSTESTS_H
