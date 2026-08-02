// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_SENDMULTIROW_H
#define PIVX_QT_SENDMULTIROW_H

#include "amount.h"
#include "pwidget.h"
#include "walletmodel.h"
#include <QAction>
#include <QPushButton>
#include <QWidget>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEnterEvent>
#endif

class WalletModel;
class SendCoinsRecipient;

namespace Ui {
class SendMultiRow;
class QPushButton;
}

class SendMultiRow : public PWidget
{
    Q_OBJECT

public:
    explicit SendMultiRow(OrganicLifeGUI* _window, PWidget *parent = nullptr);
    ~SendMultiRow();

    void hideLabels();
    void showLabels();
    void setNumber(int number);
    int getNumber();

    void loadWalletModel() override;
    bool validate();
    SendCoinsRecipient getValue();
    QString getAddress();
    CAmount getAmountValue();
    QString getMemo();

    /** Return whether the entry is still empty and unedited */
    bool isClear();
    void setOnlyStakingAddressAccepted(bool onlyStakingAddress);

    void setAddress(const QString& address);
    void setLabel(const QString& label);
    void setAmount(const QString& amount);
    void setAddressAndLabelOrDescription(const QString& address, const QString& message);
    void setFocus();
    void toggleSubtractFeeFromAmount();

    QRect getEditLineRect();
    int getEditHeight();
    int getEditWidth();
    int getMenuBtnWidth();
    bool getSubtractFeeFromAmount() const;

    // Return true if memo was set and false if it was cleared.
    bool launchMemoDialog();

public Q_SLOTS:
    void clear();
    void updateDisplayUnit();
    void onMemoClicked();

Q_SIGNALS:
    void removeEntry(SendMultiRow* entry);
    void onContactsClicked(SendMultiRow* entry);
    void onMenuClicked(SendMultiRow* entry);
    void onValueChanged();
    void onUriParsed(const SendCoinsRecipient& rcp);

protected:
    void resizeEvent(QResizeEvent *event) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent* event) override;
#else
    void enterEvent(QEvent* event) override;
#endif
    void leaveEvent(QEvent* event) override;

private Q_SLOTS:
    void amountChanged(const QString&);
    bool addressChanged(const QString&, bool fOnlyValidate = false);
    void deleteClicked();
    //void on_payTo_textChanged(const QString& address);
    //void on_addressBookButton_clicked();

private:
    Ui::SendMultiRow *ui{nullptr};
    QPushButton *iconNumber{nullptr};
    QAction *btnContact{nullptr};

    int displayUnit{0};
    int number{0};
    bool isExpanded{false};
    bool onlyStakingAddressAccepted{false};

    SendCoinsRecipient recipient;

};

#endif // PIVX_QT_SENDMULTIROW_H
