// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_QT_PQWIDGET_H
#define ORGANICLIFE_QT_PQWIDGET_H

#include "pwidget.h"
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class PQWidget : public PWidget
{
    Q_OBJECT
public:
    explicit PQWidget(OrganicLifeGUI* window, QWidget* parent = nullptr);
    void clearWalletModel() override;
Q_SIGNALS:
    void encryptWalletRequested();
protected:
    void loadWalletModel() override;
private:
    friend class PQWidgetTests;
    QLabel* balance;
    QLabel* status;
    QLabel* backupStatus;
    QLineEdit* receiveAddress;
    QLineEdit* recipient;
    QLineEdit* amount;
    QPushButton* createAddress;
    QPushButton* send;
    QTableWidget* history;
    QString backupDirectory;
    QString backupSettingsKey() const;
    bool chooseBackupDirectory();
    bool backupSnapshot();
    void refresh();
    void receive();
    void pay();
};
#endif
