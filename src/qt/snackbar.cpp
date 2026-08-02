// Copyright (c) 2019-2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "snackbar.h"
#include "ui_snackbar.h"
#include "organiclifegui.h"

#include <QEvent>
#include <QMouseEvent>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QTimer>
#include <algorithm>


SnackBar::SnackBar(OrganicLifeGUI* _window, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SnackBar),
    window(_window),
    hideTimer(new QTimer(this)),
    timeout(MIN_TIMEOUT)
{
    ui->setupUi(this);

    setWindowFlags(Qt::Widget | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus);
    setWindowModality(Qt::NonModal);
    setModal(false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);

    this->setStyleSheet(parent->styleSheet());
    ui->snackContainer->setProperty("cssClass", "container-snackbar");
    ui->label->setProperty("cssClass", "text-snackbar");

    hideTimer->setSingleShot(true);
    connect(hideTimer, &QTimer::timeout, this, &SnackBar::hideAnim);
    if (window)
        connect(window, &OrganicLifeGUI::windowResizeEvent, this, &SnackBar::windowResizeEvent);

    ui->pushButton->hide();
    ui->pushButton->setFixedSize(0, 0);
    ui->pushButton->setEnabled(false);

    // Avoid QGraphicsDropShadowEffect here: on macOS with translucency + rounded corners it
    // tends to produce rectangular artifacts in the bottom corners.
    ui->snackContainer->setGraphicsEffect(nullptr);

    setCursor(Qt::PointingHandCursor);
    ui->snackContainer->setCursor(Qt::PointingHandCursor);
    ui->label->setCursor(Qt::PointingHandCursor);
    installEventFilter(this);
    ui->snackContainer->installEventFilter(this);
    ui->label->installEventFilter(this);

    recalculateGeometry();
}

void SnackBar::windowResizeEvent(QResizeEvent* event) {
    Q_UNUSED(event);
    QWidget* host = parentWidget() ? parentWidget()->window() : nullptr;
    if (!host) host = parentWidget();
    if (!host) return;

    recalculateGeometry(host->width());
    const int marginBottom = 16;
    const int xLocal = std::max(16, (host->width() - width()) / 2);
    const int yLocal = std::max(0, host->height() - height() - marginBottom);
    move(QPoint(xLocal, yLocal));
}

void SnackBar::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    isHiding = false;
    hideTimer->start(timeout);
}

void SnackBar::hideAnim()
{
    if (!isVisible() || isHiding) return;
    isHiding = true;
    hideTimer->stop();

    const QPoint startPos = pos();
    const QPoint endPos = QPoint(startPos.x(), startPos.y() + 10);

    auto* slide = new QPropertyAnimation(this, "pos");
    slide->setDuration(160);
    slide->setStartValue(startPos);
    slide->setEndValue(endPos);
    slide->setEasingCurve(QEasingCurve::InCubic);

    auto* fade = new QPropertyAnimation(this, "windowOpacity");
    fade->setDuration(160);
    fade->setStartValue(windowOpacity());
    fade->setEndValue(0.0);
    fade->setEasingCurve(QEasingCurve::InCubic);

    auto* group = new QParallelAnimationGroup(this);
    group->addAnimation(slide);
    group->addAnimation(fade);

    connect(group, &QParallelAnimationGroup::finished, this, [this, startPos]() {
        setWindowOpacity(1.0);
        move(startPos);
        hide();
        lower();
        isHiding = false;
    });
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void SnackBar::setText(const QString& text)
{
    ui->label->setText(text);
    setTimeoutForText(text);
    recalculateGeometry();
}

void SnackBar::recalculateGeometry(int parentWidth)
{
    QWidget* host = parentWidget();
    const int hostWidth = parentWidth > 0
            ? parentWidth
            : (host ? host->width() : width());

    const int horizontalScreenMargin = 16;
    const int maxToastWidth = std::max(220, std::min(640, hostWidth - (horizontalScreenMargin * 2)));
    const int minToastWidth = std::min(280, maxToastWidth);

    const int closeButtonWidth = ui->pushButton->isVisible() ? (ui->pushButton->width() + 10) : 0;
    const int horizontalPadding = 34;
    const int textWidthLimit = std::max(120, maxToastWidth - closeButtonWidth - horizontalPadding);

    ui->label->setWordWrap(true);
    ui->label->setMaximumWidth(textWidthLimit);
    ui->label->setAlignment(Qt::AlignCenter);

    QFontMetrics fm(ui->label->font());
    const QRect textRect = fm.boundingRect(QRect(0, 0, textWidthLimit, 1000), Qt::TextWordWrap, ui->label->text());
    const int desiredWidth = std::max(minToastWidth, std::min(maxToastWidth, textRect.width() + closeButtonWidth + horizontalPadding));
    const int desiredHeight = std::max(48, textRect.height() + 24);

    setFixedSize(desiredWidth, desiredHeight);
}

bool SnackBar::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == this || watched == ui->snackContainer || watched == ui->label) &&
        event->type() == QEvent::MouseButtonPress) {
        hideAnim();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void SnackBar::mousePressEvent(QMouseEvent* event)
{
    Q_UNUSED(event);
    hideAnim();
}

void SnackBar::setTimeoutForText(const QString& text)
{
    timeout = std::max(MIN_TIMEOUT, std::min(MAX_TIMEOUT, GetTimeout(text)));
}

int SnackBar::GetTimeout(const QString& message)
{
    // 50 milliseconds per char
    return (50 * message.length());
}

SnackBar::~SnackBar()
{
    delete ui;
}

const int SnackBar::MIN_TIMEOUT;
const int SnackBar::MAX_TIMEOUT;
