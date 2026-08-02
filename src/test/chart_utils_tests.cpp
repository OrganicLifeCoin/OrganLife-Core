// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/chartutils.h"
#include "test/test_organiclife.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(chart_utils_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(axis_rounding_expected_steps)
{
    const auto specA = ComputeNiceAxisSpec(837.0, 5);
    BOOST_CHECK_EQUAL(specA.top, 1000.0);
    BOOST_CHECK_EQUAL(specA.step, 200.0);
    BOOST_CHECK_EQUAL(specA.tickCount, 6);

    const auto specB = ComputeNiceAxisSpec(6420.0, 5);
    BOOST_CHECK_EQUAL(specB.top, 10000.0);
    BOOST_CHECK_EQUAL(specB.step, 2000.0);
    BOOST_CHECK_EQUAL(specB.tickCount, 6);

    const auto specC = ComputeNiceAxisSpec(48900.0, 5);
    BOOST_CHECK_EQUAL(specC.top, 50000.0);
    BOOST_CHECK_EQUAL(specC.step, 10000.0);
    BOOST_CHECK_EQUAL(specC.tickCount, 6);
}

BOOST_AUTO_TEST_CASE(axis_format_compact_labels)
{
    BOOST_CHECK_EQUAL(FormatCompactAxisLabel(0.0), "0");
    BOOST_CHECK_EQUAL(FormatCompactAxisLabel(999.0), "999");
    BOOST_CHECK_EQUAL(FormatCompactAxisLabel(1000.0), "1k");
    BOOST_CHECK_EQUAL(FormatCompactAxisLabel(2500.0), "2.5k");
    BOOST_CHECK_EQUAL(FormatCompactAxisLabel(10000.0), "10k");
    BOOST_CHECK_EQUAL(FormatCompactAxisLabel(100000.0), "100k");
}

BOOST_AUTO_TEST_CASE(month_week_window_resolution)
{
    const auto midMonth = ResolveMonthWeekWindow(19, 28, 7);
    BOOST_CHECK_EQUAL(midMonth.firstDay, 15);
    BOOST_CHECK_EQUAL(midMonth.lastDay, 21);
    BOOST_CHECK(midMonth.canGoLeft);
    BOOST_CHECK(midMonth.canGoRight);

    const auto startMonth = ResolveMonthWeekWindow(1, 28, 7);
    BOOST_CHECK_EQUAL(startMonth.firstDay, 1);
    BOOST_CHECK_EQUAL(startMonth.lastDay, 7);
    BOOST_CHECK(!startMonth.canGoLeft);
    BOOST_CHECK(startMonth.canGoRight);

    const auto endMonth = ResolveMonthWeekWindow(28, 28, 7);
    BOOST_CHECK_EQUAL(endMonth.firstDay, 22);
    BOOST_CHECK_EQUAL(endMonth.lastDay, 28);
    BOOST_CHECK(endMonth.canGoLeft);
    BOOST_CHECK(!endMonth.canGoRight);
}

BOOST_AUTO_TEST_CASE(month_empty_window_keeps_chart_controls_visible)
{
    BOOST_CHECK(!ShouldShowEmptyChart(false, true, true));
    BOOST_CHECK(ShouldShowEmptyChart(false, false, true));
    BOOST_CHECK(ShouldShowEmptyChart(false, true, false));
    BOOST_CHECK(!ShouldShowEmptyChart(true, true, true));
}

BOOST_AUTO_TEST_SUITE_END()
