// Copyright (c) 2019-2020 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "optionbutton.h"
#include "ui_optionbutton.h"

#include "qtutils.h"

#include <QEvent>
#include <QMouseEvent>
#include <QSizePolicy>

OptionButton::OptionButton(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OptionButton)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_Hover, true);
    ui->layoutOptions2->setAttribute(Qt::WA_Hover, true);
    setCssProperty(ui->labelArrow3, "ic-arrow");
    setCssProperty(ui->layoutOptions2, "container-options");
    setMinimumHeight(96);
    ui->layoutOptions2->setContentsMargins(0,12,0,12);
    ui->labelTitleChange->setWordWrap(true);
    ui->labelSubtitleChange->setWordWrap(true);
    ui->labelTitleChange->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    ui->labelSubtitleChange->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    syncHoverState(false);
    setCssProperty(ui->labelCircle, "btn-options-indicator");
    connect(ui->labelArrow3, &QPushButton::clicked, [this](){setChecked(!ui->labelArrow3->isChecked());});
    setActive(false);
}

OptionButton::~OptionButton()
{
    delete ui;
}

void OptionButton::setTitleClassAndText(QString className, QString text)
{
    ui->labelTitleChange->setText(text);
    setCssProperty(ui->labelTitleChange, className);
}

void OptionButton::setTitleText(QString text)
{
    ui->labelTitleChange->setText(text);
}

void OptionButton::setSubTitleClassAndText(QString className, QString text)
{
    ui->labelSubtitleChange->setText(text);
    setCssProperty(ui->labelSubtitleChange, className);
}

void OptionButton::setRightIconClass(QString className, bool forceUpdate)
{
    setCssProperty(ui->labelArrow3, className);
    if (forceUpdate) updateStyle(ui->labelArrow3);
}

void OptionButton::setRightIcon(QPixmap icon)
{
    //ui->labelArrow3->setPixmap(icon);
}

void OptionButton::setActive(bool isActive)
{
    if (isActive) {
        ui->layoutCircle->setVisible(true);
        setCssProperty(ui->labelTitleChange, "btn-title-purple");
        updateStyle(ui->labelTitleChange);
    } else {
        ui->layoutCircle->setVisible(false);
        setCssProperty(ui->labelTitleChange, "btn-title-grey");
        updateStyle(ui->labelTitleChange);
    }
}

void OptionButton::setChecked(bool checked)
{
    ui->labelArrow3->setChecked(checked);
    Q_EMIT clicked();
}

bool OptionButton::event(QEvent* event)
{
    bool updateHover = false;
    bool hovered = false;
    if (event) {
        switch (event->type()) {
            case QEvent::Enter:
            case QEvent::HoverEnter:
            case QEvent::HoverMove:
                updateHover = true;
                hovered = true;
                break;
            case QEvent::Leave:
            case QEvent::HoverLeave:
                updateHover = true;
                hovered = false;
                break;
            default:
                break;
        }
    }

    const bool result = QWidget::event(event);
    if (updateHover) {
        syncHoverState(hovered);
    }
    return result;
}

void OptionButton::mousePressEvent(QMouseEvent *qevent)
{
    if (qevent->button() == Qt::LeftButton){
        setChecked(!ui->labelArrow3->isChecked());
    }
}

void OptionButton::syncHoverState(bool hovered)
{
    if (ui->layoutOptions2->property("hovered").toBool() == hovered) {
        return;
    }
    ui->layoutOptions2->setProperty("hovered", hovered);
    updateStyle(ui->layoutOptions2);
}
