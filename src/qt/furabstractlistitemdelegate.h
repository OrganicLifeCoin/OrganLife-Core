// Copyright (c) 2019-2020 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_FURABSTRACTLISTITEMDELEGATE_H
#define PIVX_QT_FURABSTRACTLISTITEMDELEGATE_H

#include "furlistrow.h"

#include <QAbstractItemDelegate>
#include <QColor>
#include <QHash>
#include <QModelIndex>
#include <QObject>
#include <QPaintEngine>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

class FurAbstractListItemDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    FurAbstractListItemDelegate(int _rowHeight, FurListRow<>* _row, QObject *parent=nullptr);

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const;
    void setTransientRowOpacity(const QModelIndex& index, qreal opacity);
    void clearTransientRowOpacity(const QModelIndex& index);
    void clearTransientRowOpacities();

    FurListRow<> *getRowFactory();
private:
    int rowHeight = 0;
    FurListRow<>* row = nullptr;
    mutable QHash<QPersistentModelIndex, qreal> transientRowOpacity;

};

#endif // PIVX_QT_FURABSTRACTLISTITEMDELEGATE_H
