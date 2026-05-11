// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_LOADINGDIALOG_H
#define PIVX_QT_LOADINGDIALOG_H

#include "containerdialog.h"
#include "prunnable.h"
#include "qt/walletmodel.h"
#include <QPointer>
#include <QThread>
#include <QVector>
#include <iostream>

namespace Ui {
class LoadingDialog;
}

class Worker : public QObject {
    Q_OBJECT
public:
    Worker(Runnable* runnable, int type):runnable(runnable), type(type){}
    ~Worker(){
        runnable = nullptr;
    }
    virtual void clean() {};
    void setType(int _type) { type = _type; }
public Q_SLOTS:
    void process();
Q_SIGNALS:
    void finished();
    void error(QString err, int type);

private:
    Runnable* runnable;
    int type;
};

/*
 * Worker that keeps track of the wallet unlock context
 */
class WalletWorker : public Worker {
    Q_OBJECT
public:
    WalletWorker(Runnable* runnable, int type, std::unique_ptr<WalletModel::UnlockContext> _pctx):
        Worker::Worker(runnable, type),
        pctx(std::move(_pctx))
    {}
    void clean() override
    {
        if (pctx) pctx.reset();
    }
    void setContext(std::unique_ptr<WalletModel::UnlockContext> _pctx)
    {
        clean();
        pctx = std::move(_pctx);
    }
private:
    std::unique_ptr<WalletModel::UnlockContext> pctx{nullptr};
};

class LoadingDialog : public ContainerDialog
{
    Q_OBJECT

public:
    struct ContentItem {
        QString label;
        QString value;
    };

    struct Content {
        QString eyebrow;
        QString title;
        QString supportText;
        QVector<ContentItem> items;
    };

    explicit LoadingDialog(QWidget *parent = nullptr, QString loadingMsg = "");
    explicit LoadingDialog(QWidget* parent, const Content& content);
    ~LoadingDialog();

    void execute(Runnable *runnable, int type, std::unique_ptr<WalletModel::UnlockContext> pctx = nullptr);
    void setContent(const Content& content);

public Q_SLOTS:
    void finished();

private:
    static Content legacyContent(const QString& loadingMsg);
    void initialize();
    void applyContent();
    void rebuildMetaItems();

    Ui::LoadingDialog *ui;
    Content dialogContent;
};

#endif // PIVX_QT_LOADINGDIALOG_H
