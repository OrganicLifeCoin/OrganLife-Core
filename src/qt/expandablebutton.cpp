// Copyright (c) 2019-2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "expandablebutton.h"
#include "ui_expandablebutton.h"

#include "qtutils.h"

#include <QApplication>
#include <QCursor>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QStyle>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

ExpandableButton::ExpandableButton(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ExpandableButton)
{
    ui->setupUi(this);

    this->setStyleSheet(parent->styleSheet());
    ui->pushButton->setCheckable(true);
    // Qt6: QLayout::SetFixedSize can prevent the widget from resizing when we
    // change min/max widths on hover. Keep the layout flexible and control the
    // width via setSmall()/setExpanded().
    this->layout()->setSizeConstraint(QLayout::SetDefaultConstraint);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    ui->pushButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMouseTracking(true);
    ui->pushButton->setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    ui->pushButton->setAttribute(Qt::WA_Hover, true);
    ui->pushButton->installEventFilter(this);
    if (QApplication::instance()) {
        QApplication::instance()->installEventFilter(this);
    }

    outsideCursorMonitor = new QTimer(this);
    outsideCursorMonitor->setSingleShot(false);
    outsideCursorMonitor->setInterval(40);
    connect(outsideCursorMonitor, &QTimer::timeout, this, [this]() {
        if (!shouldTrackOutsideCursor()) {
            updateOutsideCursorMonitor();
            return;
        }
        syncHoverStateToGlobalPos(QCursor::pos());
    });

    connect(ui->pushButton, &QPushButton::clicked, this, &ExpandableButton::innerMousePressEvent);

    // Initialize to compact size - Qt6 needs explicit initial sizing
    setSmall();
}

void ExpandableButton::setButtonClassStyle(const char *name, const QVariant &value, bool forceUpdate)
{
    ui->pushButton->setProperty(name, value);
    if (forceUpdate) {
        updateStyle(ui->pushButton);
    }
}

void ExpandableButton::setIcon(QString path)
{
    ui->pushButton->setIcon(QIcon(path));
}

void ExpandableButton::setButtonText(const QString& _text)
{
    this->text = _text;
    if (this->isExpanded) {
        updateDisplayedText();
    }
}

void ExpandableButton::setNoIconText(const QString& _text)
{
    notExpandedText = _text;
    if (!this->isExpanded)
        ui->pushButton->setText(_text);
}

void ExpandableButton::setExpandedWidth(int width)
{
    this->expandedWidth = width;
    if (this->isExpanded) {
        this->setMinimumWidth(getExpandedWidth());
        this->setMaximumWidth(getExpandedWidth());
        updateDisplayedText();
    }
}

ExpandableButton::~ExpandableButton()
{
    if (QApplication::instance()) {
        QApplication::instance()->removeEventFilter(this);
    }
    delete ui;
}

bool ExpandableButton::isChecked()
{
    return ui->pushButton->isChecked();
}

void ExpandableButton::setChecked(bool check)
{
    ui->pushButton->setChecked(check);
}

void ExpandableButton::animateWidth(int startWidth, int endWidth)
{
    // Stop and cleanup any existing animation
    if (animation) {
        animation->stop();
        delete animation;
        animation.clear();
    }
    
    // Use parallel animation group for smooth width animation
    QParallelAnimationGroup* group = new QParallelAnimationGroup(this);
    
    const bool expanding = endWidth > startWidth;
    const int durationMs = expanding ? 170 : 130;
    const QEasingCurve easing = expanding ? QEasingCurve(QEasingCurve::OutCubic) : QEasingCurve::InOutCubic;

    // Animate maximum width
    QPropertyAnimation* maxWidthAnim = new QPropertyAnimation(this, "maximumWidth");
    maxWidthAnim->setDuration(durationMs);
    maxWidthAnim->setStartValue(startWidth);
    maxWidthAnim->setEndValue(endWidth);
    maxWidthAnim->setEasingCurve(easing);
    
    // Animate minimum width
    QPropertyAnimation* minWidthAnim = new QPropertyAnimation(this, "minimumWidth");
    minWidthAnim->setDuration(durationMs);
    minWidthAnim->setStartValue(startWidth);
    minWidthAnim->setEndValue(endWidth);
    minWidthAnim->setEasingCurve(easing);
    
    group->addAnimation(maxWidthAnim);
    group->addAnimation(minWidthAnim);
    
    // Store pointer and start animation
    animation = group;
    group->start();
}

void ExpandableButton::setSmall(bool animate)
{
    ui->pushButton->setText("");
    
    if (animate && this->isExpanded) {
        animateWidth(this->width(), 48);
    } else {
        this->setMinimumWidth(48);
        this->setMaximumWidth(48);
    }
    
    this->isExpanded = false;
    updateOutsideCursorMonitor();
    updateGeometry();
    update();
}

void ExpandableButton::setExpanded(bool animate)
{
    int targetWidth = getExpandedWidth();
    if (animate && !this->isExpanded) {
        animateWidth(this->width(), targetWidth);
    } else {
        this->setMinimumWidth(targetWidth);
        this->setMaximumWidth(targetWidth);
    }
    
    if (!hasExpandedBaseFont) {
        expandedBaseFont = ui->pushButton->font();
        hasExpandedBaseFont = true;
    }
    this->isExpanded = true;
    updateDisplayedText();
    updateOutsideCursorMonitor();
    updateGeometry();
    update();
}

bool ExpandableButton::containsGlobalPos(const QPoint& globalPos) const
{
    if (!isVisible()) return false;
    return rect().contains(mapFromGlobal(globalPos));
}

void ExpandableButton::syncHoverStateToGlobalPos(const QPoint& globalPos)
{
    if (!shouldTrackOutsideCursor()) return;
    handleHoverLeave(false, globalPos);
}

void ExpandableButton::updateDisplayedText()
{
    if (!this->isExpanded || this->text.isEmpty()) {
        ui->pushButton->setText(this->text);
        return;
    }

    QFont baseFont = hasExpandedBaseFont ? expandedBaseFont : ui->pushButton->font();
    if (!hasExpandedBaseFont) {
        expandedBaseFont = baseFont;
        hasExpandedBaseFont = true;
    }

    const qreal baseSize = (baseFont.pointSizeF() > 0.0)
            ? baseFont.pointSizeF()
            : static_cast<qreal>(ui->pushButton->fontInfo().pointSize());
    const qreal minSize = 8.0;

    const int iconSpace = ui->pushButton->icon().isNull() ? 0 : (ui->pushButton->iconSize().width() + 8);
    const int frameSpace = ui->pushButton->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, ui->pushButton) * 2;
    const int availableWidth = std::max(0, ui->pushButton->width() - iconSpace - frameSpace - 24);

    if (availableWidth <= 0) {
        ui->pushButton->setFont(baseFont);
        ui->pushButton->setText(this->text);
        return;
    }

    QFont fittedFont = baseFont;
    for (qreal size = baseSize; size >= minSize; size -= 0.5) {
        fittedFont.setPointSizeF(size);
        QFontMetrics metrics(fittedFont);
        if (metrics.horizontalAdvance(this->text) <= availableWidth) {
            ui->pushButton->setFont(fittedFont);
            ui->pushButton->setText(this->text);
            return;
        }
    }

    fittedFont.setPointSizeF(minSize);
    QFontMetrics metrics(fittedFont);
    ui->pushButton->setFont(fittedFont);
    ui->pushButton->setText(metrics.elidedText(this->text, Qt::ElideRight, availableWidth));
}

void ExpandableButton::setProgress(int progress)
{
    const int normalized = std::clamp(progress, -1, 100);
    this->progressValue = normalized;

    if (normalized < 0) {
        if (progressAnimation) {
            progressAnimation->stop();
        }
        paintedProgress = -1.0;
        update();
        return;
    }

    const qreal start = (paintedProgress < 0.0) ? 0.0 : paintedProgress;
    const qreal end = static_cast<qreal>(normalized);
    if (qFuzzyCompare(start, end)) {
        paintedProgress = end;
        update();
        return;
    }

    if (!progressAnimation) {
        progressAnimation = new QVariantAnimation(this);
        connect(progressAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            paintedProgress = value.toReal();
            update();
        });
    } else {
        progressAnimation->stop();
    }

    const int delta = std::abs(static_cast<int>(std::round(end - start)));
    const int durationMs = std::clamp(220 + delta * 10, 220, 1100);
    progressAnimation->setDuration(durationMs);
    progressAnimation->setEasingCurve(QEasingCurve::InOutCubic);
    progressAnimation->setStartValue(start);
    progressAnimation->setEndValue(end);
    progressAnimation->start();
}

void ExpandableButton::handleHoverEnter()
{
    if (hoverActive) {
        return;
    }

    hoverActive = true;
    if (hoverExpandEnabled) {
        setExpanded();
    }
    updateOutsideCursorMonitor();
    Q_EMIT Mouse_Hover();
    update();
}

void ExpandableButton::handleHoverLeave(bool force, std::optional<QPoint> globalPos)
{
    if (!force) {
        const QPoint probeGlobalPos = globalPos.value_or(QCursor::pos());
        if (containsGlobalPos(probeGlobalPos)) {
            return;
        }
    }

    if (!hoverActive && !isExpanded) {
        return;
    }

    hoverActive = false;
    if (hoverExpandEnabled && !keepExpanded) {
        setSmall();
    }
    updateOutsideCursorMonitor();
    Q_EMIT Mouse_HoverLeave();
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void ExpandableButton::enterEvent(QEnterEvent*)
#else
void ExpandableButton::enterEvent(QEvent *)
#endif
{
    handleHoverEnter();
}

void ExpandableButton::leaveEvent(QEvent*)
{
    handleHoverLeave();
}

bool ExpandableButton::event(QEvent* e)
{
    // Some platforms/styles (notably Qt6 + macOS) can miss QWidget::leaveEvent()
    // when the internal QPushButton eats hover/leave transitions. Handle hover-leave
    // explicitly as a backstop.
    if (e->type() == QEvent::HoverLeave || e->type() == QEvent::WindowDeactivate) {
        handleHoverLeave(e->type() == QEvent::WindowDeactivate);
    }
    return QWidget::event(e);
}

bool ExpandableButton::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == ui->pushButton) {
        if (e->type() == QEvent::Enter || e->type() == QEvent::HoverEnter) {
            handleHoverEnter();
        } else if (e->type() == QEvent::Leave || e->type() == QEvent::HoverLeave) {
            // Collapse only if the cursor is truly outside the widget (moving between
            // child and parent should not collapse).
            handleHoverLeave();
        }
    }

    if (shouldTrackOutsideCursor()) {
        switch (e->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease: {
            auto* mouseEvent = static_cast<QMouseEvent*>(e);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            syncHoverStateToGlobalPos(mouseEvent->globalPosition().toPoint());
#else
            syncHoverStateToGlobalPos(mouseEvent->globalPos());
#endif
            break;
        }
        case QEvent::Wheel: {
            auto* wheelEvent = static_cast<QWheelEvent*>(e);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            syncHoverStateToGlobalPos(wheelEvent->globalPosition().toPoint());
#else
            syncHoverStateToGlobalPos(wheelEvent->globalPos());
#endif
            break;
        }
        case QEvent::WindowDeactivate:
            handleHoverLeave(true);
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, e);
}

void ExpandableButton::updateOutsideCursorMonitor()
{
    if (!outsideCursorMonitor) return;

    if (shouldTrackOutsideCursor()) {
        if (!outsideCursorMonitor->isActive()) {
            outsideCursorMonitor->start();
        }
    } else {
        outsideCursorMonitor->stop();
    }
}

bool ExpandableButton::shouldTrackOutsideCursor() const
{
    return !keepExpanded && (hoverActive || isExpanded);
}

void ExpandableButton::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (this->isExpanded) {
        updateDisplayedText();
    }
}

void ExpandableButton::innerMousePressEvent()
{
    Q_EMIT Mouse_Pressed();
}


void ExpandableButton::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    if (paintedProgress < 0.0) return;

    const qreal ratio = std::clamp(paintedProgress / 100.0, 0.0, 1.0);
    if (ratio <= 0.0) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Draw under the QPushButton: sync button styles use alpha backgrounds, so the fill
    // remains visible in both compact and expanded states without custom button subclasses.
    const QRectF buttonRect = QRectF(ui->pushButton->geometry()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (buttonRect.width() <= 2.0 || buttonRect.height() <= 2.0) return;

    const qreal radius = std::min(10.0, buttonRect.height() / 2.0);
    QPainterPath clipPath;
    clipPath.addRoundedRect(buttonRect, radius, radius);

    qreal fillWidth = buttonRect.width() * ratio;
    if (ratio >= 0.998) {
        fillWidth = buttonRect.width();
    }
    const QRectF fillRect(buttonRect.left(), buttonRect.top(), fillWidth, buttonRect.height());
    if (fillRect.width() <= 0.0) return;

    const QColor textColor = ui->pushButton->palette().color(QPalette::ButtonText);
    const bool darkTheme = textColor.lightness() > 150;

    const QColor fillStart = darkTheme ? QColor(192, 132, 69, 72) : QColor(156, 78, 26, 64);
    const QColor fillEnd = darkTheme ? QColor(209, 154, 93, 150) : QColor(156, 78, 26, 120);
    QLinearGradient fillGradient(fillRect.topLeft(), fillRect.bottomLeft());
    fillGradient.setColorAt(0.0, fillStart);
    fillGradient.setColorAt(0.55, fillEnd);
    fillGradient.setColorAt(1.0, fillStart);

    painter.save();
    painter.setClipPath(clipPath);
    painter.setPen(Qt::NoPen);
    QPainterPath fillPath;
    fillPath.addRoundedRect(fillRect, radius, radius);
    painter.fillPath(fillPath, fillGradient);
    painter.restore();
}
