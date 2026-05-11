// Copyright (c) 2019 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "furabstractlistitemdelegate.h"

#include <algorithm>

FurAbstractListItemDelegate::FurAbstractListItemDelegate(int _rowHeight, FurListRow<> *_row, QObject *parent) :
    QAbstractItemDelegate(parent), rowHeight(_rowHeight), row(_row){}

void FurAbstractListItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                  const QModelIndex &index ) const
{
    painter->save();

    // Status
    bool isStateSelected = option.state & QStyle::State_Selected;
    bool isStateHovered = option.state & QStyle::State_MouseOver;

    QRect selectedRect = option.rect;
    selectedRect.setLeft(0);
    qreal rowOpacity = 1.0;
    const auto opacityIt = transientRowOpacity.constFind(QPersistentModelIndex(index));
    if (opacityIt != transientRowOpacity.constEnd()) {
        rowOpacity = std::clamp(opacityIt.value(), 0.0, 1.0);
        painter->setOpacity(rowOpacity);
    }

    painter->fillRect(selectedRect, this->row->rectColor(isStateHovered, isStateSelected));

    painter->translate(option.rect.topLeft());
    QWidget *row = this->row->createHolder(index.row());
    row->setStyleSheet(qobject_cast<QWidget*>(parent())->styleSheet());
    this->row->init(row, index, isStateHovered, isStateSelected);
    row->setAttribute(Qt::WA_DontShowOnScreen, true);
    row->setGeometry(option.rect);
    row->resize(option.rect.width(),option.rect.height());
    row->render(painter, QPoint(), QRegion(), QWidget::DrawChildren );

    painter->restore();
}

FurListRow<>* FurAbstractListItemDelegate::getRowFactory(){
    return this->row;
}

void FurAbstractListItemDelegate::setTransientRowOpacity(const QModelIndex& index, qreal opacity)
{
    if (!index.isValid()) return;
    transientRowOpacity.insert(QPersistentModelIndex(index), std::clamp(opacity, 0.0, 1.0));
}

void FurAbstractListItemDelegate::clearTransientRowOpacity(const QModelIndex& index)
{
    if (!index.isValid()) return;
    transientRowOpacity.remove(QPersistentModelIndex(index));
}

void FurAbstractListItemDelegate::clearTransientRowOpacities()
{
    transientRowOpacity.clear();
}

QSize FurAbstractListItemDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const
{
    return QSize(rowHeight, rowHeight);
}
