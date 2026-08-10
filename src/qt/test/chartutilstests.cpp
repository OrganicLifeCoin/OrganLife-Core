// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chartutilstests.h"

#include "chainparams.h"
#include "chartutils.h"
#include "progressutils.h"
#include "transactionrecord.h"
#include "validation.h"
#include <QString>
#include <vector>

void ChartUtilsTests::axisRoundsToExpectedSteps()
{
    auto specA = ComputeNiceAxisSpec(837.0, 5);
    QCOMPARE(specA.top, 1000.0);
    QCOMPARE(specA.step, 200.0);
    QCOMPARE(specA.tickCount, 6);

    auto specB = ComputeNiceAxisSpec(6420.0, 5);
    QCOMPARE(specB.top, 10000.0);
    QCOMPARE(specB.step, 2000.0);
    QCOMPARE(specB.tickCount, 6);

    auto specC = ComputeNiceAxisSpec(48900.0, 5);
    QCOMPARE(specC.top, 50000.0);
    QCOMPARE(specC.step, 10000.0);
    QCOMPARE(specC.tickCount, 6);
}

void ChartUtilsTests::axisFormattingIsCompactAndStable()
{
    QCOMPARE(QString::fromStdString(FormatCompactAxisLabel(0.0)), QString("0"));
    QCOMPARE(QString::fromStdString(FormatCompactAxisLabel(999.0)), QString("999"));
    QCOMPARE(QString::fromStdString(FormatCompactAxisLabel(1000.0)), QString("1k"));
    QCOMPARE(QString::fromStdString(FormatCompactAxisLabel(2500.0)), QString("2.5k"));
    QCOMPARE(QString::fromStdString(FormatCompactAxisLabel(10000.0)), QString("10k"));
    QCOMPARE(QString::fromStdString(FormatCompactAxisLabel(100000.0)), QString("100k"));
}

void ChartUtilsTests::monthWindowResolvesToCurrentWeek()
{
    const WeekWindow window = ResolveMonthWeekWindow(19, 28, 7);
    QCOMPARE(window.firstDay, 15);
    QCOMPARE(window.lastDay, 21);
    QVERIFY(window.canGoLeft);
    QVERIFY(window.canGoRight);
}

void ChartUtilsTests::monthWindowPagingBoundsAreCorrect()
{
    const WeekWindow startWindow = ResolveMonthWeekWindow(1, 28, 7);
    QCOMPARE(startWindow.firstDay, 1);
    QCOMPARE(startWindow.lastDay, 7);
    QVERIFY(!startWindow.canGoLeft);
    QVERIFY(startWindow.canGoRight);

    const WeekWindow endWindow = ResolveMonthWeekWindow(28, 28, 7);
    QCOMPARE(endWindow.firstDay, 22);
    QCOMPARE(endWindow.lastDay, 28);
    QVERIFY(endWindow.canGoLeft);
    QVERIFY(!endWindow.canGoRight);
}

void ChartUtilsTests::monthEmptyWindowKeepsChartVisible()
{
    QVERIFY(!ShouldShowEmptyChart(false, true, true));
    QVERIFY(ShouldShowEmptyChart(false, false, true));
    QVERIFY(ShouldShowEmptyChart(false, true, false));
    QVERIFY(!ShouldShowEmptyChart(true, true, true));
}

void ChartUtilsTests::rewardTypeClassificationIncludesV6CoinbaseMasternodePayments()
{
    QVERIFY(!IsMasternodeRewardTypeForChart(TransactionRecord::StakeMint));
    QVERIFY(IsMasternodeRewardTypeForChart(TransactionRecord::MNReward));
    QVERIFY(IsMasternodeRewardTypeForChart(TransactionRecord::BudgetPayment));
    QVERIFY(IsMasternodeRewardTypeForChart(TransactionRecord::Generated));
}

void ChartUtilsTests::chartRewardAggregationUsesCopiedRows()
{
    const std::vector<ChartStakeSample> rows = {
        {2026, 6, 1, 5 * COIN, TransactionRecord::StakeMint},
        {2026, 6, 1, 2 * COIN, TransactionRecord::MNReward},
        {2026, 7, 2, 3 * COIN, TransactionRecord::BudgetPayment},
        {2025, 12, 25, -4 * COIN, TransactionRecord::Generated},
    };

    const ChartRewardAggregation byMonth = AggregateChartRewards(rows, ChartBucketMode::Year);
    QCOMPARE(byMonth.amountsBy.at(6).first, static_cast<long long>(5 * COIN));
    QCOMPARE(byMonth.amountsBy.at(6).second, static_cast<long long>(2 * COIN));
    QCOMPARE(byMonth.amountsBy.at(7).first, 0LL);
    QCOMPARE(byMonth.amountsBy.at(7).second, static_cast<long long>(3 * COIN));
    QVERIFY(byMonth.hasMasternodeRewards);

    const ChartRewardAggregation byYear = AggregateChartRewards(rows, ChartBucketMode::All);
    QCOMPARE(byYear.amountsBy.at(2025).first, 0LL);
    QCOMPARE(byYear.amountsBy.at(2025).second, static_cast<long long>(4 * COIN));
    QCOMPARE(byYear.amountsBy.at(2026).first, static_cast<long long>(5 * COIN));
    QCOMPARE(byYear.amountsBy.at(2026).second, static_cast<long long>(5 * COIN));
}

void ChartUtilsTests::coinbaseCreditsAreClassifiedByRewardType()
{
    // v6 activates at genesis on the public networks: pin a concrete height so
    // the coinbase-classification boundary behavior stays testable.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, 1000);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_POS, 50);
    const int v6Height = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight;
    const CAmount mnReward = GetMasternodePayment(v6Height);
    QVERIFY(mnReward > 0);

    QVERIFY(TransactionRecord::classifyCoinbaseCredit(v6Height - 1, mnReward) == TransactionRecord::Generated);
    QVERIFY(TransactionRecord::classifyCoinbaseCredit(v6Height, mnReward) == TransactionRecord::MNReward);
    QVERIFY(TransactionRecord::classifyCoinbaseCredit(v6Height, mnReward + COIN) == TransactionRecord::BudgetPayment);
    QVERIFY(TransactionRecord::classifyCoinbaseCredit(v6Height, mnReward - COIN) == TransactionRecord::Generated);
}

void ChartUtilsTests::progressLabelFormattingIsStable()
{
    QCOMPARE(BuildProgressLabel("Rescanning...", 42), QString("Rescanning... 42%"));
    QCOMPARE(BuildProgressLabel("", 13), QString("Processing... 13%"));
    QCOMPARE(BuildProgressLabel("Replaying blocks...", 100), QString("Replaying blocks..."));
}

void ChartUtilsTests::transactionQueueReloadThresholdWorks()
{
    QVERIFY(!ShouldReloadTransactionModel(10));
    QVERIFY(!ShouldReloadTransactionModel(250));
    QVERIFY(ShouldReloadTransactionModel(2000));
    QVERIFY(ShouldReloadTransactionModel(50000));
}
