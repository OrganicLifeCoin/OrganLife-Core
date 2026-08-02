// Copyright (c) 2019 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_CSROW_H
#define PIVX_QT_CSROW_H

#include <QEnterEvent>
#include <QWidget>

namespace Ui {
class CSRow;
}

class CSRow : public QWidget
{
    Q_OBJECT

public:
    explicit CSRow(QWidget *parent = nullptr);
    ~CSRow();

    void updateView(const QString& address, const QString& label, bool isStaking, bool isReceivedDelegation, const QString& amount);
    void updateState(bool isLightTheme, bool isHovered, bool isSelected);
    void showMenuButton(bool show);
protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent* event) override;
#else
    void enterEvent(QEvent* event) override;
#endif
    void leaveEvent(QEvent* event) override;

private:
    Ui::CSRow *ui;

    bool fShowMenuButton = true;
};

#endif // PIVX_QT_CSROW_H
