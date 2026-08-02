// Copyright (c) 2019-2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_PWIDGET_H
#define PIVX_QT_PWIDGET_H

#include "prunnable.h"
#include "walletmodel.h"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

class OrganicLifeGUI;
class ClientModel;
class WalletModel;
class WorkerTask;

namespace Ui {
class PWidget;
}

class Translator
{
public:
    virtual QString translate(const char *msg) = 0;
};

class PWidget : public QWidget, public Runnable, public Translator
{
    Q_OBJECT
public:
    explicit PWidget(OrganicLifeGUI* _window = nullptr, QWidget *parent = nullptr);
    explicit PWidget(PWidget *parent = nullptr);

    void setClientModel(ClientModel* model);
    void setWalletModel(WalletModel* model);
    virtual void clearWalletModel();

    OrganicLifeGUI* getWindow() { return this->window; }

    void run(int type) override;
    void onError(QString error, int type) override;

    void inform(const QString& message);
    void emitMessage(const QString& title, const QString& message, unsigned int style, bool* ret = nullptr);

    QString translate(const char *msg) override { return tr(msg); }

Q_SIGNALS:
    void message(const QString& title, const QString& body, unsigned int style, bool* ret = nullptr);
    void showHide(bool show);
    bool execDialog(QDialog *dialog, int xDiv = 3, int yDiv = 5);

protected Q_SLOTS:
    virtual void changeTheme(bool isLightTheme, QString &theme);
    void onChangeTheme(bool isLightTheme, QString &theme);

protected:
    OrganicLifeGUI* window = nullptr;
    QPointer<ClientModel> clientModel;
    QPointer<WalletModel> walletModel;

    virtual void loadClientModel();
    virtual void loadWalletModel();

    void showHideOp(bool show);
    bool execute(int type, std::unique_ptr<WalletModel::UnlockContext> pctx = nullptr);
    void warn(const QString& title, const QString& message);
    bool ask(const QString& title, const QString& message);
    void showDialog(QDialog *dialog, int xDiv = 3, int yDiv = 5);

private:
    QSharedPointer<WorkerTask> task;

    void init();
private Q_SLOTS:
    void errorString(QString, int);

};

#endif // PIVX_QT_PWIDGET_H
