// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_PROGRESSUTILS_H
#define PIVX_QT_PROGRESSUTILS_H

#include <QString>

#include <algorithm>
#include <cstddef>

static inline QString BuildProgressLabel(const QString& title, int progress, const QString& fallback = QStringLiteral("Processing..."))
{
    QString base = title.trimmed();
    if (base.isEmpty()) base = fallback;
    if (progress >= 0 && progress < 100) {
        base += QStringLiteral(" %1%").arg(progress);
    }
    return base;
}

static inline bool ShouldReloadTransactionModel(std::size_t queuedNotifications)
{
    static constexpr std::size_t RELOAD_THRESHOLD = 2000;
    return queuedNotifications >= RELOAD_THRESHOLD;
}

static inline int ClampProgressPercent(int progress)
{
    return std::clamp(progress, 0, 99);
}

#endif // PIVX_QT_PROGRESSUTILS_H
