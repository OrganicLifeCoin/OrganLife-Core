// Copyright (c) 2026 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_CHARTUTILS_H
#define PIVX_QT_CHARTUTILS_H

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct ChartAxisSpec {
    double top{1.0};
    double step{1.0};
    int tickCount{2};
};

struct WeekWindow {
    int firstDay{1};
    int lastDay{1};
    bool canGoLeft{false};
    bool canGoRight{false};
};

enum class ChartBucketMode {
    All,
    Year,
    Month
};

struct ChartStakeSample {
    int year{0};
    int month{0};
    int day{0};
    long long amount{0};
    int txType{0};
};

struct ChartRewardAggregation {
    std::map<int, std::pair<long long, long long>> amountsBy;
    bool hasMasternodeRewards{false};
};

namespace chart {
namespace detail {

// Keep these numeric values aligned with TransactionRecord::Type. The
// Qt-free unit tests include this header without pulling in Qt types.
static constexpr int TX_TYPE_GENERATED = 1; // TransactionRecord::Generated
static constexpr int TX_TYPE_MN_REWARD = 6; // TransactionRecord::MNReward
static constexpr int TX_TYPE_BUDGET_PAYMENT = 7; // TransactionRecord::BudgetPayment

inline int ClampInt(const int value, const int low, const int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

inline std::string TrimTrailingZeros(std::string text)
{
    const std::string::size_type dotPos = text.find('.');
    if (dotPos == std::string::npos) return text;

    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

inline double NiceStep(const double rawStep)
{
    const double safeRaw = std::max(rawStep, 1e-12);
    const double magnitude = std::pow(10.0, std::floor(std::log10(safeRaw)));
    const double normalized = safeRaw / magnitude;

    double niceNormalized = 1.0;
    if (normalized <= 1.0) {
        niceNormalized = 1.0;
    } else if (normalized <= 2.0) {
        niceNormalized = 2.0;
    } else if (normalized <= 5.0) {
        niceNormalized = 5.0;
    } else {
        niceNormalized = 10.0;
    }
    return niceNormalized * magnitude;
}

} // namespace detail
} // namespace chart

inline ChartAxisSpec ComputeNiceAxisSpec(const double maxVisible, int targetTicks = 5)
{
    targetTicks = std::max(2, targetTicks);
    if (!std::isfinite(maxVisible) || maxVisible <= 0.0) {
        return {1.0, 1.0, 2};
    }

    const double rawStep = maxVisible / static_cast<double>(targetTicks);
    const double step = chart::detail::NiceStep(rawStep);
    const double topByValue = std::max(step, std::ceil(maxVisible / step) * step);
    const double topByTicks = step * static_cast<double>(targetTicks);
    const double top = std::max(topByValue, topByTicks);
    const int tickCount = std::max(2, static_cast<int>(std::llround(top / step)) + 1);

    return {top, step, tickCount};
}

inline std::string FormatCompactAxisLabel(const double value)
{
    if (!std::isfinite(value)) return "0";

    const double absValue = std::fabs(value);
    double scaled = value;
    const char* suffix = "";

    if (absValue >= 1e9) {
        scaled = value / 1e9;
        suffix = "B";
    } else if (absValue >= 1e6) {
        scaled = value / 1e6;
        suffix = "M";
    } else if (absValue >= 1e3) {
        scaled = value / 1e3;
        suffix = "k";
    } else {
        const long long rounded = static_cast<long long>(std::llround(value));
        return std::to_string(rounded);
    }

    std::ostringstream stream;
    if (std::fabs(scaled - std::round(scaled)) < 1e-9) {
        stream << static_cast<long long>(std::llround(scaled));
    } else {
        stream << std::fixed << std::setprecision(1) << scaled;
    }

    return chart::detail::TrimTrailingZeros(stream.str()) + suffix;
}

inline WeekWindow ResolveMonthWeekWindow(int anchorDay, int daysInMonth, int weekSpan = 7)
{
    const int normalizedDaysInMonth = std::max(1, daysInMonth);
    const int normalizedWeekSpan = std::max(1, weekSpan);
    const int clampedAnchorDay = chart::detail::ClampInt(anchorDay, 1, normalizedDaysInMonth);

    int firstDay = ((clampedAnchorDay - 1) / normalizedWeekSpan) * normalizedWeekSpan + 1;
    int lastDay = firstDay + normalizedWeekSpan - 1;
    if (lastDay > normalizedDaysInMonth) {
        lastDay = normalizedDaysInMonth;
        firstDay = std::max(1, lastDay - normalizedWeekSpan + 1);
    }

    return {
        firstDay,
        lastDay,
        firstDay > 1,
        lastDay < normalizedDaysInMonth
    };
}

inline bool ShouldShowEmptyChart(const bool hasVisibleValues, const bool hasFilteredData, const bool isMonthView)
{
    if (hasVisibleValues) return false;
    if (isMonthView && hasFilteredData) return false;
    return true;
}

inline bool IsMasternodeRewardTypeForChart(const int txType)
{
    return txType == chart::detail::TX_TYPE_MN_REWARD
            || txType == chart::detail::TX_TYPE_BUDGET_PAYMENT
            || txType == chart::detail::TX_TYPE_GENERATED;
}

inline int ChartBucketForSample(const ChartStakeSample& sample, const ChartBucketMode mode)
{
    switch (mode) {
    case ChartBucketMode::All:
        return sample.year;
    case ChartBucketMode::Year:
        return sample.month;
    case ChartBucketMode::Month:
        return sample.day;
    }
    return 0;
}

inline ChartRewardAggregation AggregateChartRewards(const std::vector<ChartStakeSample>& rows,
                                                    const ChartBucketMode mode)
{
    ChartRewardAggregation result;
    for (const ChartStakeSample& sample : rows) {
        const int bucket = ChartBucketForSample(sample, mode);
        if (bucket <= 0) continue;

        const long long amount = std::llabs(sample.amount);
        auto& values = result.amountsBy[bucket];
        if (IsMasternodeRewardTypeForChart(sample.txType)) {
            values.second += amount;
            result.hasMasternodeRewards = true;
        } else {
            values.first += amount;
        }
    }
    return result;
}

#endif // PIVX_QT_CHARTUTILS_H
