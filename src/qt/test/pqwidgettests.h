// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_QT_PQWIDGETTESTS_H
#define ORGANICLIFE_QT_PQWIDGETTESTS_H
#include <QObject>
class PQWidgetTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void masternodeControllerNavigation();
    void masternodeRegistryFailsClosed();
    void masternodeControllerCancelsWithoutWrites();
    void seamlessWalletUsesStandardScreens();
    void actualWalletUnloadQuiescesModel();
    void standardReceiveUsesPQAddressAndBackup();
    void standardSendUsesPQBackupAndHistory();
    void chartPeriodsExistBeforeFirstReward();
};
#endif
