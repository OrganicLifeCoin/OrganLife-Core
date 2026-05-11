// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qtutils.h"

#include "guiconstants.h"
#include "logging.h"
#include "qrencode.h"
#include "snackbar.h"

#include <QFile>
#include <QAbstractScrollArea>
#include <QAbstractButton>
#include <QDebug>
#include <QCloseEvent>
#include <QEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QGraphicsDropShadowEffect>
#include <QListView>
#include <QMetaObject>
#include <QParallelAnimationGroup>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <algorithm>
#include <memory>

Qt::Modifier SHORT_KEY
#ifdef Q_OS_MAC
    = Qt::CTRL;
#else
    = Qt::ALT;
#endif

namespace {
constexpr const char* kRoundedDialogManagedProperty = "roundedDialogManaged";
constexpr const char* kRoundedDialogRadiusProperty = "roundedDialogRadiusPx";
constexpr const char* kDialogOwnsOpenPositionProperty = "dialogOwnsOpenPosition";
constexpr const char* kDialogAutoSizeToContentsProperty = "dialogAutoSizeToContents";
constexpr const char* kDialogPopAnimationEnabledProperty = "dialogPopAnimationEnabled";
constexpr const char* kDialogPopAnimationRunningProperty = "dialogPopAnimationRunning";
constexpr const char* kDialogPopAnimationTargetProperty = "dialogPopAnimationTarget";
constexpr const char* kDialogPerformanceModeProperty = "dialogPerformanceMode";
constexpr const char* kDialogPerformanceModeSettingKey = "uiDialogPerformanceMode";
constexpr const char* kDialogAnimationTraceProperty = "dialogAnimationTrace";
constexpr const char* kDialogAnimationTraceSettingKey = "uiDialogAnimationTrace";
constexpr const char* kDialogAnimationTraceEnvVar = "PIVX_QT_DIALOG_ANIM_TRACE";
constexpr const char* kDialogPosTraceEnvVar = "PIVX_QT_DIALOG_POS_TRACE";
constexpr const char* kDialogPerformanceModeEnvVar = "PIVX_QT_DIALOG_PERF_MODE";
constexpr const char* kRoundedMaskLastDialogSizeProperty = "roundedMaskLastDialogSize";
constexpr const char* kRoundedMaskLastFrameSizeProperty = "roundedMaskLastFrameSize";
constexpr const char* kDialogCloseAnimationRunningProperty = "dialogCloseAnimationRunning";
constexpr const char* kDialogCloseAnimationBypassProperty = "dialogCloseAnimationBypass";
constexpr const char* kDialogCloseAnimationPendingResultProperty = "dialogCloseAnimationPendingResult";

bool envFlagEnabled(const char* envVarName, bool defaultValue = false)
{
    const QByteArray rawValue = qgetenv(envVarName);
    if (rawValue.isEmpty()) return defaultValue;

    const QByteArray normalized = rawValue.trimmed().toLower();
    return !(normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off");
}

bool isRoundedDialogManaged(const QDialog* dialog)
{
    return dialog && dialog->property(kRoundedDialogManagedProperty).toBool();
}

bool dialogOwnsOpenPosition(const QDialog* dialog)
{
    return dialog && dialog->property(kDialogOwnsOpenPositionProperty).toBool();
}

bool dialogAutoSizeToContents(const QDialog* dialog)
{
    if (!dialog) return false;
    const QVariant property = dialog->property(kDialogAutoSizeToContentsProperty);
    return !property.isValid() || property.toBool();
}

bool dialogPopAnimationEnabled(const QDialog* dialog)
{
    if (!dialog) return false;
    const QVariant property = dialog->property(kDialogPopAnimationEnabledProperty);
    return !property.isValid() || property.toBool();
}

bool dialogPopAnimationRunning(const QDialog* dialog)
{
    return dialog && dialog->property(kDialogPopAnimationRunningProperty).toBool();
}

bool dialogPerformanceModeEnabled(const QDialog* dialog)
{
    if (!dialog) return true;
    const QVariant property = dialog->property(kDialogPerformanceModeProperty);
    if (property.isValid()) return property.toBool();

    const QByteArray envValue = qgetenv(kDialogPerformanceModeEnvVar);
    if (!envValue.isEmpty()) return envFlagEnabled(kDialogPerformanceModeEnvVar, true);

    QSettings* appSettings = getSettings();
    if (!appSettings) return true;
    if (!appSettings->contains(kDialogPerformanceModeSettingKey)) {
        appSettings->setValue(kDialogPerformanceModeSettingKey, true);
    }
    return appSettings->value(kDialogPerformanceModeSettingKey, true).toBool();
}

bool dialogAnimationTraceEnabled(const QDialog* dialog)
{
    if (!dialog) return false;
    const QVariant property = dialog->property(kDialogAnimationTraceProperty);
    if (property.isValid()) return property.toBool();

    const QByteArray envValue = qgetenv(kDialogAnimationTraceEnvVar);
    if (!envValue.isEmpty()) return envFlagEnabled(kDialogAnimationTraceEnvVar, false);

    QSettings* appSettings = getSettings();
    if (!appSettings) return false;
    if (!appSettings->contains(kDialogAnimationTraceSettingKey)) {
        appSettings->setValue(kDialogAnimationTraceSettingKey, false);
    }
    return appSettings->value(kDialogAnimationTraceSettingKey, false).toBool();
}

class DialogScaleFadeEffect final : public QGraphicsEffect
{
public:
    explicit DialogScaleFadeEffect(QObject* parent = nullptr) : QGraphicsEffect(parent) {}

    qreal scale() const { return m_scale; }
    qreal opacity() const { return m_opacity; }

    void setScale(qreal value)
    {
        value = std::clamp(value, 0.5, 2.0);
        if (qFuzzyCompare(m_scale, value)) return;
        m_scale = value;
        updateBoundingRect();
        update();
    }

    void setOpacity(qreal value)
    {
        value = std::clamp(value, 0.0, 1.0);
        if (qFuzzyCompare(m_opacity, value)) return;
        m_opacity = value;
        update();
    }

protected:
    QRectF boundingRectFor(const QRectF& sourceRect) const override
    {
        const qreal growth = std::max<qreal>(0.0, m_scale - 1.0);
        const qreal dx = sourceRect.width() * growth * 0.5;
        const qreal dy = sourceRect.height() * growth * 0.5;
        return sourceRect.adjusted(-dx, -dy, dx, dy);
    }

    void draw(QPainter* painter) override
    {
        if (!painter) return;

        QPoint offset;
        const QPixmap source = sourcePixmap(Qt::LogicalCoordinates, &offset, QGraphicsEffect::NoPad);
        if (source.isNull()) return;

        painter->save();
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->setOpacity(m_opacity);

        const QRectF sourceRect(QPointF(0, 0), QSizeF(source.width(), source.height()));
        const QPointF center = QPointF(offset.x(), offset.y()) + sourceRect.center();
        painter->translate(center);
        painter->scale(m_scale, m_scale);
        painter->translate(-sourceRect.center());
        painter->drawPixmap(QPointF(0, 0), source);
        painter->restore();
    }

private:
    qreal m_scale{1.0};
    qreal m_opacity{1.0};
};

struct DialogAnimationTiming
{
    int scaleMs;
    int opacityMs;
    qreal overshoot;
};

const DialogAnimationTiming kDefaultDialogAnimationTiming[] = {
    {370, 300, 0.75},
    {440, 340, 0.75},
};

DialogAnimationTiming dialogAnimationTiming(bool performanceMode)
{
    return performanceMode ? kDefaultDialogAnimationTiming[0] : kDefaultDialogAnimationTiming[1];
}

bool dialogPosTraceEnabled()
{
    return envFlagEnabled(kDialogPosTraceEnvVar, false);
}

void traceDialogPositionCheckpointImpl(const QDialog* dialog, const char* checkpoint, const QPoint* targetCenter = nullptr)
{
    if (!dialog || !dialogPosTraceEnabled()) return;

    const QRect geo = dialog->geometry();
    const QRect frame = dialog->frameGeometry();
    const QPoint center = geo.center();
    const QPoint target = targetCenter ? *targetCenter : center;
    const QPoint delta = center - target;
    const char* className = dialog->metaObject() ? dialog->metaObject()->className() : "<unknown>";
    const QByteArray objectName = dialog->objectName().toUtf8();

    LogPrint(BCLog::QT,
             "ui.dialog.pos checkpoint=%s class=%s object=%s geo=(%d,%d,%d,%d) frame=(%d,%d,%d,%d) center=(%d,%d) target=(%d,%d) delta=(%d,%d) dpr=%.3f\n",
             checkpoint,
             className,
             objectName.constData(),
             geo.x(), geo.y(), geo.width(), geo.height(),
             frame.x(), frame.y(), frame.width(), frame.height(),
             center.x(), center.y(),
             target.x(), target.y(),
             delta.x(), delta.y(),
             dialog->devicePixelRatioF());
}

DialogScaleFadeEffect* ensureDialogScaleFadeEffect(QWidget* target)
{
    if (!target) return nullptr;
    if (auto* effect = dynamic_cast<DialogScaleFadeEffect*>(target->graphicsEffect())) {
        return effect;
    }

    if (target->graphicsEffect()) {
        target->setGraphicsEffect(nullptr);
    }

    auto* effect = new DialogScaleFadeEffect(target);
    target->setGraphicsEffect(effect);
    return effect;
}

QWidget* resolveDialogPopTarget(QDialog* dialog, QWidget* fallbackTarget)
{
    if (!dialog) return nullptr;
    const QObject* storedTarget = dialog->property(kDialogPopAnimationTargetProperty).value<QObject*>();
    if (auto* widget = qobject_cast<QWidget*>(const_cast<QObject*>(storedTarget))) {
        return widget;
    }
    return fallbackTarget;
}

QRect resolveWidgetContentRectOnScreen(const QWidget* widget)
{
    if (!widget || !widget->isVisible()) return {};
    const QRect localRect = widget->rect();
    return QRect(widget->mapToGlobal(localRect.topLeft()), localRect.size());
}

QRect resolveDialogAvailableGeometry(const QDialog* dialog)
{
    if (!dialog) return {};
    if (const QWidget* host = dialog->parentWidget() ? dialog->parentWidget()->window() : nullptr;
        host && host->isVisible()) {
        return resolveWidgetContentRectOnScreen(host);
    }
    if (QScreen* screen = dialog->screen()) {
        return screen->availableGeometry();
    }
    if (QScreen* screen = QGuiApplication::screenAt(dialog->mapToGlobal(dialog->rect().center()))) {
        return screen->availableGeometry();
    }
    if (QScreen* primary = QGuiApplication::primaryScreen()) {
        return primary->availableGeometry();
    }
    return {};
}

void fitDialogToContents(QDialog* dialog)
{
    if (!dialog || !dialogAutoSizeToContents(dialog)) return;

    if (dialog->layout()) {
        dialog->layout()->activate();
    }

    dialog->adjustSize();

    QSize targetSize = dialog->size();
    if (!targetSize.isValid() || targetSize.isEmpty()) {
        targetSize = dialog->sizeHint().expandedTo(dialog->minimumSizeHint());
    }
    if (!targetSize.isValid() || targetSize.isEmpty()) return;

    const bool widthFixed = dialog->minimumWidth() > 0 && dialog->minimumWidth() == dialog->maximumWidth();
    const bool heightFixed = dialog->minimumHeight() > 0 && dialog->minimumHeight() == dialog->maximumHeight();

    const QRect available = resolveDialogAvailableGeometry(dialog);
    if (!available.isNull()) {
        constexpr int kMargin = 24;
        int maxWidth = std::max(dialog->minimumWidth(), available.width() - kMargin);
        int maxHeight = std::max(dialog->minimumHeight(), available.height() - kMargin);
        if (dialog->maximumWidth() > 0 && dialog->maximumWidth() < QWIDGETSIZE_MAX) {
            maxWidth = std::min(maxWidth, dialog->maximumWidth());
        }
        if (dialog->maximumHeight() > 0 && dialog->maximumHeight() < QWIDGETSIZE_MAX) {
            maxHeight = std::min(maxHeight, dialog->maximumHeight());
        }

        if (!widthFixed) {
            targetSize.setWidth(std::max(dialog->minimumWidth(), std::min(targetSize.width(), maxWidth)));
        }
        if (!heightFixed) {
            targetSize.setHeight(std::max(dialog->minimumHeight(), std::min(targetSize.height(), maxHeight)));
        }
    }

    if (targetSize != dialog->size()) {
        dialog->resize(targetSize);
    }
}

void runDialogPopAnimation(QDialog* dialog, QWidget* fallbackTarget)
{
    if (!dialog || !dialogPopAnimationEnabled(dialog) || dialogPopAnimationRunning(dialog)) return;

    const QString platformName = QGuiApplication::platformName();
    if (platformName.contains("offscreen", Qt::CaseInsensitive)) return;

    QWidget* target = resolveDialogPopTarget(dialog, fallbackTarget);
    if (!target) return;
    const bool performanceMode = dialogPerformanceModeEnabled(dialog);
    const bool traceAnimation = dialogAnimationTraceEnabled(dialog);
    const auto timing = dialogAnimationTiming(performanceMode);
    DialogScaleFadeEffect* effect = ensureDialogScaleFadeEffect(target);
    if (!effect) return;
    constexpr qreal kStartScale = 0.965;

    struct DialogAnimationStats
    {
        QElapsedTimer timer;
        qint64 lastTickMs{0};
        qint64 maxFrameDeltaMs{0};
        int frameCount{0};
    };
    std::shared_ptr<DialogAnimationStats> animationStats;
    if (traceAnimation) {
        animationStats = std::make_shared<DialogAnimationStats>();
        animationStats->timer.start();
    }

    const auto connectFramePacingTrace = [dialog, &animationStats](QVariantAnimation* animation) {
        if (!animation || !animationStats) return;
        QObject::connect(animation, &QVariantAnimation::valueChanged, dialog,
                         [animationStats, dialog = QPointer<QDialog>(dialog)](const QVariant&) {
            if (!animationStats || !animationStats->timer.isValid()) return;

            const qint64 nowMs = animationStats->timer.elapsed();
            if (animationStats->lastTickMs > 0) {
                const qint64 frameDeltaMs = nowMs - animationStats->lastTickMs;
                animationStats->maxFrameDeltaMs = std::max(animationStats->maxFrameDeltaMs, frameDeltaMs);
                if (frameDeltaMs > 16) {
                    const QString dialogName = dialog && !dialog->objectName().isEmpty()
                            ? dialog->objectName()
                            : QStringLiteral("<unnamed>");
                    qDebug().noquote() << QStringLiteral("ui.anim.dialog.stall name=%1 deltaMs=%2")
                                              .arg(dialogName)
                                              .arg(frameDeltaMs);
                }
            }

            animationStats->lastTickMs = nowMs;
            ++animationStats->frameCount;
        });
    };

    const auto emitAnimationSummary = [dialog, animationStats, performanceMode]() {
        if (!animationStats || !animationStats->timer.isValid()) return;
        const QString dialogName = dialog && !dialog->objectName().isEmpty()
                ? dialog->objectName()
                : QStringLiteral("<unnamed>");
        qDebug().noquote() << QStringLiteral("ui.anim.dialog.summary name=%1 performanceMode=%2 frames=%3 maxFrameDeltaMs=%4 totalMs=%5")
                                  .arg(dialogName)
                                  .arg(performanceMode ? 1 : 0)
                                  .arg(animationStats->frameCount)
                                  .arg(animationStats->maxFrameDeltaMs)
                                  .arg(animationStats->timer.elapsed());
    };
    dialog->setProperty(kDialogPopAnimationRunningProperty, true);
    effect->setScale(kStartScale);
    effect->setOpacity(0.0);
    const QPoint targetCenter = dialog->geometry().center();
    traceDialogPositionCheckpointImpl(dialog, "anim_start", &targetCenter);

    auto* scaleAnimation = new QVariantAnimation(dialog);
    scaleAnimation->setDuration(timing.scaleMs);
    scaleAnimation->setStartValue(kStartScale);
    scaleAnimation->setEndValue(1.0);
    QEasingCurve popCurve(QEasingCurve::OutBack);
    popCurve.setOvershoot(timing.overshoot);
    scaleAnimation->setEasingCurve(popCurve);
    QObject::connect(scaleAnimation, &QVariantAnimation::valueChanged, dialog, [effect = QPointer<DialogScaleFadeEffect>(effect)](const QVariant& value) {
        if (!effect) return;
        effect->setScale(value.toDouble());
    });
    connectFramePacingTrace(scaleAnimation);

    auto* opacityAnimation = new QVariantAnimation(dialog);
    opacityAnimation->setDuration(timing.opacityMs);
    opacityAnimation->setStartValue(0.0);
    opacityAnimation->setEndValue(1.0);
    opacityAnimation->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(opacityAnimation, &QVariantAnimation::valueChanged, dialog, [effect = QPointer<DialogScaleFadeEffect>(effect)](const QVariant& value) {
        if (!effect) return;
        effect->setOpacity(value.toDouble());
    });

    auto* group = new QParallelAnimationGroup(dialog);
    group->addAnimation(scaleAnimation);
    group->addAnimation(opacityAnimation);
    QObject::connect(group, &QParallelAnimationGroup::finished, dialog, [dialog = QPointer<QDialog>(dialog),
                                                                         effect = QPointer<DialogScaleFadeEffect>(effect),
                                                                         emitAnimationSummary,
                                                                         targetCenter]() {
        if (effect) {
            effect->setScale(1.0);
            effect->setOpacity(1.0);
        }
        if (dialog) {
            dialog->setProperty(kDialogPopAnimationRunningProperty, false);
            traceDialogPositionCheckpointImpl(dialog, "anim_end", &targetCenter);
        }
        emitAnimationSummary();
    });
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

bool runDialogCloseAnimation(QDialog* dialog, QWidget* fallbackTarget)
{
    if (!dialog || dialog->property(kDialogCloseAnimationRunningProperty).toBool()) return false;

    const QString platformName = QGuiApplication::platformName();
    if (platformName.contains("offscreen", Qt::CaseInsensitive)) return false;

    QWidget* target = resolveDialogPopTarget(dialog, fallbackTarget);
    if (!target || !target->isVisible()) return false;

    const bool performanceMode = dialogPerformanceModeEnabled(dialog);
    const auto timing = dialogAnimationTiming(performanceMode);
    DialogScaleFadeEffect* effect = ensureDialogScaleFadeEffect(target);
    if (!effect) return false;
    constexpr qreal kEndScale = 0.965;

    dialog->setProperty(kDialogCloseAnimationRunningProperty, true);
    dialog->setProperty(kDialogPopAnimationRunningProperty, true);
    effect->setScale(1.0);
    effect->setOpacity(1.0);
    const QPoint targetCenter = dialog->geometry().center();
    traceDialogPositionCheckpointImpl(dialog, "anim_start", &targetCenter);

    auto* scaleAnimation = new QVariantAnimation(dialog);
    scaleAnimation->setDuration(timing.scaleMs);
    scaleAnimation->setStartValue(1.0);
    scaleAnimation->setEndValue(kEndScale);
    QEasingCurve closeCurve(QEasingCurve::InBack);
    closeCurve.setOvershoot(timing.overshoot);
    scaleAnimation->setEasingCurve(closeCurve);
    QObject::connect(scaleAnimation, &QVariantAnimation::valueChanged, dialog, [effect = QPointer<DialogScaleFadeEffect>(effect)](const QVariant& value) {
        if (!effect) return;
        effect->setScale(value.toDouble());
    });

    auto* opacityAnimation = new QVariantAnimation(dialog);
    opacityAnimation->setDuration(timing.opacityMs);
    opacityAnimation->setStartValue(1.0);
    opacityAnimation->setEndValue(0.0);
    opacityAnimation->setEasingCurve(QEasingCurve::InCubic);
    QObject::connect(opacityAnimation, &QVariantAnimation::valueChanged, dialog, [effect = QPointer<DialogScaleFadeEffect>(effect)](const QVariant& value) {
        if (!effect) return;
        effect->setOpacity(value.toDouble());
    });

    auto* group = new QParallelAnimationGroup(dialog);
    group->addAnimation(scaleAnimation);
    group->addAnimation(opacityAnimation);

    QObject::connect(group, &QParallelAnimationGroup::finished, dialog, [dialog = QPointer<QDialog>(dialog),
                                                                         effect = QPointer<DialogScaleFadeEffect>(effect),
                                                                         targetCenter]() {
        if (!dialog) return;
        if (effect) {
            effect->setScale(1.0);
            effect->setOpacity(1.0);
        }

        dialog->setProperty(kDialogCloseAnimationRunningProperty, false);
        dialog->setProperty(kDialogPopAnimationRunningProperty, false);
        traceDialogPositionCheckpointImpl(dialog, "anim_end", &targetCenter);
        dialog->setProperty(kDialogCloseAnimationBypassProperty, true);
        int pendingResult = dialog->property(kDialogCloseAnimationPendingResultProperty).toInt();
        if (pendingResult == 0) pendingResult = QDialog::Rejected;
        dialog->done(pendingResult);
        dialog->setProperty(kDialogCloseAnimationBypassProperty, false);
    });
    group->start(QAbstractAnimation::DeleteWhenStopped);
    return true;
}

Qt::WindowFlags dialogFlagsFor(const QDialog* dialog)
{
    if (isRoundedDialogManaged(dialog)) {
        return Qt::Dialog | Qt::CustomizeWindowHint | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
    }
    return Qt::Dialog | Qt::CustomizeWindowHint;
}

void prepareDialogWindowChrome(QDialog* dialog)
{
    if (!dialog) return;

    const Qt::WindowFlags desiredFlags = dialogFlagsFor(dialog);
    if (dialog->windowFlags() != desiredFlags) {
        dialog->setWindowFlags(desiredFlags);
    }

    dialog->setAutoFillBackground(false);
    dialog->setAttribute(Qt::WA_TranslucentBackground, true);
    if (isRoundedDialogManaged(dialog)) {
        dialog->setAttribute(Qt::WA_NoSystemBackground, true);
    }
}

void prepareDialogBeforeShow(QDialog* dialog)
{
    if (!dialog) return;
    dialog->ensurePolished();
    prepareDialogWindowChrome(dialog);
    fitDialogToContents(dialog);
}
} // namespace

void traceDialogPositionCheckpoint(const QDialog* dialog, const QString& checkpoint)
{
    const QByteArray checkpointUtf8 = checkpoint.toUtf8();
    traceDialogPositionCheckpointImpl(dialog, checkpointUtf8.constData(), nullptr);
}

// Open dialog at the bottom
bool openDialog(QDialog* widget, QWidget* gui)
{
    if (auto* snack = qobject_cast<SnackBar*>(widget)) {
        QWidget* host = gui ? gui->window() : nullptr;
        if (!host) host = gui;
        if (!host) return false;

        if (snack->parentWidget() != host) {
            snack->setParent(host);
        }

        snack->recalculateGeometry(host->width());
        snack->setWindowFlags(Qt::Widget | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus);
        snack->setAttribute(Qt::WA_TranslucentBackground, true);
        snack->setAttribute(Qt::WA_ShowWithoutActivating, true);

        const int marginBottom = 18;
        const int xLocal = std::max(16, (host->width() - snack->width()) / 2);
        const int yEndLocal = std::max(0, host->height() - snack->height() - marginBottom);
        const int yStartLocal = yEndLocal + 12;

        if (snack->isVisible()) {
            snack->hide();
        }

        snack->setWindowOpacity(0.0);
        snack->move(QPoint(xLocal, yStartLocal));
        snack->show();
        snack->raise();

        QPropertyAnimation* slide = new QPropertyAnimation(snack, "pos");
        slide->setDuration(240);
        slide->setStartValue(QPoint(xLocal, yStartLocal));
        slide->setEndValue(QPoint(xLocal, yEndLocal));
        slide->setEasingCurve(QEasingCurve::OutCubic);

        QPropertyAnimation* fade = new QPropertyAnimation(snack, "windowOpacity");
        fade->setDuration(240);
        fade->setStartValue(0.0);
        fade->setEndValue(1.0);
        fade->setEasingCurve(QEasingCurve::OutCubic);

        auto* group = new QParallelAnimationGroup(snack);
        group->addAnimation(slide);
        group->addAnimation(fade);
        group->start(QAbstractAnimation::DeleteWhenStopped);
        return true;
    }

    prepareDialogBeforeShow(widget);
    QPropertyAnimation* animation = new QPropertyAnimation(widget, "pos");
    animation->setDuration(300);
    animation->setStartValue(QPoint(0, gui->height()));
    animation->setEndValue(QPoint(0, gui->height() - widget->height()));
    animation->setEasingCurve(QEasingCurve::OutQuad);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
    widget->activateWindow();
    widget->raise();
    return widget->exec();
}

void closeDialog(QDialog* widget, PIVXGUI* gui)
{
    prepareDialogBeforeShow(widget);
    QPropertyAnimation* animation = new QPropertyAnimation(widget, "pos");
    animation->setDuration(300);
    animation->setStartValue(widget->pos());
    animation->setEndValue(QPoint(0, gui->height() + 100));
    animation->setEasingCurve(QEasingCurve::OutQuad);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void openDialogFullScreen(QWidget* parent, QWidget* dialog)
{
    dialog->setWindowFlags(Qt::CustomizeWindowHint);
    dialog->move(0, 0);
    dialog->show();
    dialog->activateWindow();
    dialog->resize(parent->width(), parent->height());
}

bool openDialogWithOpaqueBackgroundY(QDialog* widget, PIVXGUI* gui, double posX, int posY, bool hideOpaqueBackground)
{
    if (!widget || !gui) return false;

    prepareDialogBeforeShow(widget);

    const auto resolveCenterTarget = [widget, gui]() {
        QRect anchorRect = resolveDialogAvailableGeometry(widget);
        if (anchorRect.isNull() && gui && gui->isVisible()) {
            const QRect guiRect = gui->rect();
            anchorRect = QRect(gui->mapToGlobal(guiRect.topLeft()), guiRect.size());
        }
        if (anchorRect.isNull()) {
            if (QScreen* primary = QGuiApplication::primaryScreen()) {
                anchorRect = primary->availableGeometry();
            }
        }
        return anchorRect;
    };

    if (dialogOwnsOpenPosition(widget)) {
        const QRect anchorRect = resolveCenterTarget();
        if (!anchorRect.isNull()) {
            widget->move(anchorRect.center() - QPoint(widget->width() / 2, widget->height() / 2));
            const QPoint targetCenter = anchorRect.center();
            traceDialogPositionCheckpointImpl(widget, "open_helper_move", &targetCenter);
        } else {
            traceDialogPositionCheckpointImpl(widget, "open_helper_move", nullptr);
        }
    } else {
        const auto computeTargetPos = [widget, gui, posX, posY]() {
            constexpr int kMargin = 10;
            const int safePosY = std::max(1, posY);

            QSize targetSize = widget->size();
            if (!targetSize.isValid() || targetSize.isEmpty()) {
                targetSize = widget->sizeHint().expandedTo(widget->minimumSizeHint());
            }
            if (targetSize.isValid() && targetSize != widget->size()) {
                widget->resize(targetSize);
            }

            const int maxX = std::max(kMargin, gui->width() - widget->width() - kMargin);
            const int maxY = std::max(kMargin, gui->height() - widget->height() - kMargin);
            const int rawX = static_cast<int>(gui->width() / std::max(1.0, posX));
            const int rawY = gui->height() / safePosY;
            const int clampedX = std::max(kMargin, std::min(maxX, rawX));
            const int clampedY = std::max(kMargin, std::min(maxY, rawY));
            return QPoint(clampedX, clampedY);
        };

        const QPoint targetPos = computeTargetPos();
        widget->move(targetPos);
        const QPoint targetCenter = widget->geometry().center();
        traceDialogPositionCheckpointImpl(widget, "open_helper_move", &targetCenter);
    }

    widget->activateWindow();
    bool res = widget->exec();
    if (hideOpaqueBackground) gui->showHide(false);
    return res;
}

bool openDialogWithOpaqueBackground(QDialog* widget, PIVXGUI* gui, double posX)
{
    return openDialogWithOpaqueBackgroundY(widget, gui, posX, 5);
}

bool openDialogWithOpaqueBackgroundFullScreen(QDialog* widget, PIVXGUI* gui)
{
    prepareDialogBeforeShow(widget);

    widget->resize(gui->width(), gui->height());
    widget->move(gui->mapToGlobal(QPoint(0, 0)));
    widget->raise();
    widget->activateWindow();
    bool res = widget->exec();
    gui->showHide(false);
    return res;
}

bool openDialogCentered(QDialog* widget, PIVXGUI* gui, int width, int height)
{
    widget->setWindowFlags(Qt::CustomizeWindowHint | Qt::FramelessWindowHint);
    widget->setAttribute(Qt::WA_TranslucentBackground, true);
    widget->setFixedSize(width, height);

    // Center the dialog on the parent
    int xPos = (gui->width() - width) / 2;
    int yPos = (gui->height() - height) / 2;
    widget->move(xPos, yPos);

    // Fade in animation
    widget->setWindowOpacity(0.0);
    QPropertyAnimation* fadeAnim = new QPropertyAnimation(widget, "windowOpacity");
    fadeAnim->setDuration(250);
    fadeAnim->setStartValue(0.0);
    fadeAnim->setEndValue(1.0);
    fadeAnim->setEasingCurve(QEasingCurve::InOutCubic);
    fadeAnim->start(QAbstractAnimation::DeleteWhenStopped);

    // Scale animation for premium feel
    QGraphicsDropShadowEffect* shadow = new QGraphicsDropShadowEffect(widget);
    shadow->setBlurRadius(20);
    shadow->setColor(QColor(0, 0, 0, 80));
    shadow->setOffset(0, 4);
    widget->setGraphicsEffect(shadow);

    widget->activateWindow();
    bool res = widget->exec();
    gui->showHide(false);
    return res;
}

void setDialogOwnsOpenPosition(QDialog* dialog, bool enabled)
{
    if (!dialog) return;
    dialog->setProperty(kDialogOwnsOpenPositionProperty, enabled);
}

void setDialogAutoSizeToContents(QDialog* dialog, bool enabled)
{
    if (!dialog) return;
    dialog->setProperty(kDialogAutoSizeToContentsProperty, enabled);
}

void setDialogPopAnimationTarget(QDialog* dialog, QWidget* target)
{
    if (!dialog) return;
    dialog->setProperty(kDialogPopAnimationTargetProperty, QVariant::fromValue(static_cast<QObject*>(target)));
}

void setDialogPerformanceMode(QDialog* dialog, bool enabled)
{
    if (!dialog) return;
    dialog->setProperty(kDialogPerformanceModeProperty, enabled);
}

namespace {
class RoundedMaskBinder final : public QObject
{
public:
    RoundedMaskBinder(QDialog* dialog, QWidget* contentFrame, int radiusPx) :
        QObject(dialog),
        m_dialog(dialog),
        m_contentFrame(contentFrame),
        m_radiusPx(radiusPx)
    {}

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!m_dialog || !m_contentFrame || !event) {
            return QObject::eventFilter(watched, event);
        }

        switch (event->type()) {
        case QEvent::Show:
            traceDialogPositionCheckpointImpl(m_dialog, "binder_show_before_fit", nullptr);
            fitDialogToContents(m_dialog);
            traceDialogPositionCheckpointImpl(m_dialog, "binder_show_after_fit", nullptr);
            applyRoundedDialogMask(m_dialog, m_contentFrame, m_radiusPx);
            runDialogPopAnimation(m_dialog, m_contentFrame);
            break;
        case QEvent::Resize:
        case QEvent::PolishRequest:
            applyRoundedDialogMask(m_dialog, m_contentFrame, m_radiusPx);
            break;
        case QEvent::Hide:
            m_dialog->setProperty(kDialogPopAnimationRunningProperty, false);
            m_dialog->setProperty(kDialogCloseAnimationRunningProperty, false);
            if (QWidget* target = resolveDialogPopTarget(m_dialog, m_contentFrame)) {
                if (auto* scaleEffect = dynamic_cast<DialogScaleFadeEffect*>(target->graphicsEffect())) {
                    scaleEffect->setScale(1.0);
                    scaleEffect->setOpacity(1.0);
                }
            }
            break;
        case QEvent::Close: {
            if (m_dialog->property(kDialogCloseAnimationBypassProperty).toBool()) {
                break;
            }
            if (m_dialog->property(kDialogCloseAnimationRunningProperty).toBool()) {
                event->ignore();
                return true;
            }

            auto* closeEvent = static_cast<QCloseEvent*>(event);
            if (!closeEvent) {
                break;
            }

            closeEvent->ignore();
            int pendingResult = m_dialog->result();
            if (pendingResult == 0) pendingResult = QDialog::Rejected;
            m_dialog->setProperty(kDialogCloseAnimationPendingResultProperty, pendingResult);
            const bool closeAnimStarted = runDialogCloseAnimation(m_dialog, m_contentFrame);
            if (!closeAnimStarted) {
                m_dialog->setProperty(kDialogCloseAnimationBypassProperty, true);
                m_dialog->done(pendingResult);
                m_dialog->setProperty(kDialogCloseAnimationBypassProperty, false);
            }
            return true;
        }
        default:
            break;
        }

        return QObject::eventFilter(watched, event);
    }

private:
    QPointer<QDialog> m_dialog;
    QPointer<QWidget> m_contentFrame;
    int m_radiusPx{16};
};
} // namespace

void applyRoundedDialogMask(QDialog* dialog, QWidget* contentFrame, int radiusPx)
{
    if (!dialog || !contentFrame || radiusPx <= 0) return;
    dialog->setAttribute(Qt::WA_TranslucentBackground, true);
    dialog->setAttribute(Qt::WA_NoSystemBackground, true);
    dialog->setAutoFillBackground(false);
    contentFrame->setAttribute(Qt::WA_StyledBackground, true);
    contentFrame->setAutoFillBackground(false);
    dialog->setProperty(kRoundedDialogRadiusProperty, radiusPx);

    const QSize dialogSize = dialog->size();
    const QSize frameSize = contentFrame->size();
    if (dialog->property(kRoundedMaskLastDialogSizeProperty).toSize() == dialogSize &&
        dialog->property(kRoundedMaskLastFrameSizeProperty).toSize() == frameSize) {
        return;
    }
    dialog->setProperty(kRoundedMaskLastDialogSizeProperty, dialogSize);
    dialog->setProperty(kRoundedMaskLastFrameSizeProperty, frameSize);

    // Qt docs: QWidget::setMask performs coarse clipping and can produce jagged corners.
    // Keep the window per-pixel translucent and clear any stale masks instead.
    if (!dialog->mask().isEmpty()) {
        dialog->clearMask();
    }

    for (QWidget* current = contentFrame->parentWidget(); current && current != dialog; current = current->parentWidget()) {
        current->setAutoFillBackground(false);
        current->setAttribute(Qt::WA_NoSystemBackground, true);
        current->setAttribute(Qt::WA_TranslucentBackground, true);

        if (auto* scrollArea = qobject_cast<QAbstractScrollArea*>(current)) {
            if (QWidget* viewport = scrollArea->viewport()) {
                viewport->setAutoFillBackground(false);
                viewport->setAttribute(Qt::WA_NoSystemBackground, true);
                viewport->setAttribute(Qt::WA_TranslucentBackground, true);
            }
        }
    }
}

void setDialogRoundedFramelessMode(QDialog* dialog, bool enabled)
{
    if (!dialog) return;
    dialog->setProperty(kRoundedDialogManagedProperty, enabled);
    prepareDialogWindowChrome(dialog);
}

void bindRoundedDialogMask(QDialog* dialog, QWidget* contentFrame, int radiusPx)
{
    if (!dialog || !contentFrame) return;
    dialog->setProperty(kRoundedDialogRadiusProperty, radiusPx);
    setDialogRoundedFramelessMode(dialog, true);
    if (!dialog->property(kDialogAutoSizeToContentsProperty).isValid()) {
        dialog->setProperty(kDialogAutoSizeToContentsProperty, true);
    }
    if (!dialog->property(kDialogPopAnimationEnabledProperty).isValid()) {
        dialog->setProperty(kDialogPopAnimationEnabledProperty, true);
    }
    setDialogPopAnimationTarget(dialog, contentFrame);
    dialog->setAttribute(Qt::WA_NoSystemBackground, true);
    dialog->setAutoFillBackground(false);
    auto* binder = new RoundedMaskBinder(dialog, contentFrame, radiusPx);
    dialog->installEventFilter(binder);
    contentFrame->installEventFilter(binder);
    applyRoundedDialogMask(dialog, contentFrame, radiusPx);
}

QPixmap encodeToQr(const QString& str, QString& errorStr, const QColor& qrColor, int scale)
{
    if (!str.isEmpty()) {
        // limit URI length
        if (str.length() > MAX_URI_LENGTH) {
            errorStr = "Resulting URI too long, try to reduce the text for label / message.";
            return QPixmap();
        } else {
            QRcode* code = QRcode_encodeString(str.toUtf8().constData(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);
            if (!code) {
                errorStr = "Error encoding URI into QR Code.";
                return QPixmap();
            }
            // Scale the QR code by an integer factor to maintain sharp edges
            // Use a minimum scale of 1, but allow larger values for higher resolution displays
            int qrWidth = code->width;
            int scaledWidth = qrWidth * scale;
            QImage myImage = QImage(scaledWidth + 2 * scale, scaledWidth + 2 * scale, QImage::Format_RGB32);
            myImage.fill(0xffffff);

            unsigned char* p = code->data;
            for (int y = 0; y < qrWidth; y++) {
                for (int x = 0; x < qrWidth; x++) {
                    bool isBlack = (*p & 1);
                    if (isBlack) {
                        // Fill the scaled pixel block
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                myImage.setPixel(x * scale + sx + scale, y * scale + sy + scale, qrColor.rgb());
                            }
                        }
                    }
                    p++;
                }
            }
            QRcode_free(code);
            return QPixmap::fromImage(myImage);
        }
    }
    return QPixmap();
}

QPixmap encodeToQrModern(const QString& str,
                         QString& errorStr,
                         const QColor& qrColor,
                         const QColor& bgColor,
                         int moduleRadius,
                         int quietZone,
                         int scale,
                         const QPixmap& centerLogo)
{
    if (str.isEmpty()) {
        errorStr = "Empty string to encode";
        return QPixmap();
    }

    if (str.length() > MAX_URI_LENGTH) {
        errorStr = "Resulting URI too long, try to reduce the text for label / message.";
        return QPixmap();
    }

    QRcode* code = QRcode_encodeString(str.toUtf8().constData(), 0, QR_ECLEVEL_H, QR_MODE_8, 1);
    if (!code) {
        errorStr = "Error encoding URI into QR Code.";
        return QPixmap();
    }

    int qrWidth = code->width;
    int totalSize = (qrWidth + 2 * quietZone) * scale;

    QImage image(totalSize, totalSize, QImage::Format_ARGB32);

    if (bgColor.alpha() == 0) {
        image.fill(Qt::transparent);
    } else {
        image.fill(bgColor.rgba());
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    painter.setPen(Qt::NoPen);
    painter.setBrush(qrColor);

    // Classic square QR modules with slight gap for clean look
    // Using 92% fill for better visibility
    qreal dotSize = scale * 0.92;
    qreal dotOffset = (scale - dotSize) / 2.0;

    unsigned char* p = code->data;
    for (int y = 0; y < qrWidth; y++) {
        for (int x = 0; x < qrWidth; x++) {
            bool isBlack = (*p & 1);
            if (isBlack) {
                qreal px = (x + quietZone) * scale + dotOffset;
                qreal py = (y + quietZone) * scale + dotOffset;

                // Classic square modules (no rounding)
                QRectF rect(px, py, dotSize, dotSize);
                painter.drawRect(rect);
            }
            p++;
        }
    }

    QRcode_free(code);

    if (!centerLogo.isNull()) {
        int logoSize = totalSize / 5;
        QPixmap scaledLogo = centerLogo.scaled(logoSize, logoSize,
                                                Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation);

        int centerX = (totalSize - logoSize) / 2;
        int centerY = (totalSize - logoSize) / 2;

        painter.setBrush(bgColor);
        painter.drawEllipse(centerX - 4, centerY - 4, logoSize + 8, logoSize + 8);

        painter.drawPixmap(centerX, centerY, scaledLogo);
    }

    painter.end();
    return QPixmap::fromImage(image);
}

void setFilterAddressBook(QComboBox* filter, SortEdit* lineEdit)
{
    initComboBox(filter, lineEdit);
    filter->addItem(QObject::tr("All"), "");
    filter->addItem(QObject::tr("Receiving"), AddressTableModel::Receive);
    filter->addItem(QObject::tr("Contacts"), AddressTableModel::Send);
    filter->addItem(QObject::tr("Cold Staking"), AddressTableModel::ColdStaking);
    filter->addItem(QObject::tr("Delegator"), AddressTableModel::Delegator);
    filter->addItem(QObject::tr("Delegable"), AddressTableModel::Delegable);
    filter->addItem(QObject::tr("Staking Contacts"), AddressTableModel::ColdStakingSend);
    filter->addItem(QObject::tr("Shielded Recv"), AddressTableModel::ShieldedReceive);
    filter->addItem(QObject::tr("Shielded Contact"), AddressTableModel::ShieldedSend);
}

void setSortTx(QComboBox* filter, SortEdit* lineEdit)
{
    // Sort Transactions
    initComboBox(filter, lineEdit);
    filter->addItem(QObject::tr("Date desc"), SortTx::DATE_DESC);
    filter->addItem(QObject::tr("Date asc"), SortTx::DATE_ASC);
    filter->addItem(QObject::tr("Amount desc"), SortTx::AMOUNT_ASC);
    filter->addItem(QObject::tr("Amount asc"), SortTx::AMOUNT_DESC);
}

void setSortTxTypeFilter(QComboBox* filter, SortEdit* lineEditType)
{
    initComboBox(filter, lineEditType);
    filter->addItem(QObject::tr("All"), TransactionFilterProxy::ALL_TYPES);
    filter->addItem(QObject::tr("Received"),
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvWithAddress) |
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvFromOther) |
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvWithShieldedAddress) |
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvWithShieldedAddressMemo));
    filter->addItem(QObject::tr("Sent"),
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToAddress) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToOther) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToShielded) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToNobody));
    filter->addItem(QObject::tr("Shield"),
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvWithShieldedAddress) |
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvWithShieldedAddressMemo) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToShielded) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldToShieldChangeAddress) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldToTransparent) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldedAddress));
    filter->addItem(QObject::tr("Mined"), TransactionFilterProxy::TYPE(TransactionRecord::Generated));
    filter->addItem(QObject::tr("Minted"), TransactionFilterProxy::TYPE(TransactionRecord::StakeMint));
    filter->addItem(QObject::tr("MN reward"), TransactionFilterProxy::TYPE(TransactionRecord::MNReward));
    filter->addItem(QObject::tr("To yourself"), TransactionFilterProxy::TYPE(TransactionRecord::SendToSelf) |
                                            TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldedAddress) |
                                            TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldToShieldChangeAddress) |
                                            TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldToTransparent));
    filter->addItem(QObject::tr("Cold stakes"), TransactionFilterProxy::TYPE(TransactionRecord::StakeDelegated));
    filter->addItem(QObject::tr("Hot stakes"), TransactionFilterProxy::TYPE(TransactionRecord::StakeHot));
    filter->addItem(QObject::tr("Delegated"), TransactionFilterProxy::TYPE(TransactionRecord::P2CSDelegationSent) | TransactionFilterProxy::TYPE(TransactionRecord::P2CSDelegationSentOwner));
    filter->addItem(QObject::tr("Delegations"), TransactionFilterProxy::TYPE(TransactionRecord::P2CSDelegation));
    filter->addItem(QObject::tr("DAO payment"), TransactionFilterProxy::TYPE(TransactionRecord::BudgetPayment));
}

void setupSettings(QSettings* settings)
{
    if (!settings->contains("theme")) {
        settings->setValue("theme", "default");
    }
    if (!settings->contains("lightTheme")) {
        const QString theme = settings->value("theme", "default").toString().toLower();
        const bool inferredLightTheme = !theme.contains("dark");
        settings->setValue("lightTheme", inferredLightTheme);
    }
}

std::unique_ptr<QSettings> settings = nullptr;

QSettings* getSettings()
{
    if (!settings) {
        settings.reset(new QSettings());
        // Setup initial values if them are not there
        setupSettings(settings.get());
    }

    return settings.get();
}

bool isLightTheme()
{
    QSettings* appSettings = getSettings();
    const QString theme = appSettings->value("theme", "").toString().toLower();
    if (!theme.isEmpty()) {
        if (theme.contains("dark")) return false;
        if (theme.contains("light") || theme == "default") return true;
    }
    return appSettings->value("lightTheme", true).toBool();
}

void setTheme(bool isLight)
{
    QSettings* settings = getSettings();
    settings->setValue("theme", isLight ? "default" : "default-dark");
    settings->setValue("lightTheme", isLight);
}


// Style

void updateStyle(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}


QColor getRowColor(bool isLightTheme, bool isHovered, bool isSelected)
{
    if (isSelected) {
        return isLightTheme ? QColor(29, 78, 216, 38) : QColor(96, 165, 250, 64);
    }
    if (isHovered) {
        return isLightTheme ? QColor(15, 23, 42, 18) : QColor(255, 255, 255, 28);
    }
    return isLightTheme ? QColor("#ffffff") : QColor("#0B1220");
}

void initComboBox(QComboBox* combo, QLineEdit* lineEdit, QString cssClass, bool setView)
{
    setCssProperty(combo, std::move(cssClass));
    combo->setEditable(true);
    if (lineEdit) {
        lineEdit->setReadOnly(true);
        lineEdit->setAlignment(Qt::AlignRight);
        combo->setLineEdit(lineEdit);
    }
    combo->setStyleSheet("selection-background-color:transparent;");
    if (setView) combo->setView(new QListView());
}

void fillAddressSortControls(SortEdit* seType, SortEdit* seOrder, QComboBox* boxType, QComboBox* boxOrder)
{
    // Sort Type
    initComboBox(boxType, seType, "btn-combo-small");
    boxType->addItem(QObject::tr("by Label"), AddressTableModel::Label);
    boxType->addItem(QObject::tr("by Address"), AddressTableModel::Address);
    boxType->addItem(QObject::tr("by Date"), AddressTableModel::Date);
    boxType->setCurrentIndex(0);
    // Sort Order
    initComboBox(boxOrder, seOrder, "btn-combo-small");
    boxOrder->addItem("asc", Qt::AscendingOrder);
    boxOrder->addItem("desc", Qt::DescendingOrder);
    boxOrder->setCurrentIndex(0);
}

void initCssEditLine(QLineEdit* edit, bool isDialog)
{
    if (isDialog)
        setCssEditLineDialog(edit, true, false);
    else
        setCssEditLine(edit, true, false);
    setShadow(edit);
    edit->setAttribute(Qt::WA_MacShowFocusRect, false);
}

void setCssEditLine(QLineEdit* edit, bool isValid, bool forceUpdate)
{
    setCssProperty(edit, isValid ? "edit-primary" : "edit-primary-error", forceUpdate);
}

void setCssEditLineDialog(QLineEdit* edit, bool isValid, bool forceUpdate)
{
    setCssProperty(edit, isValid ? "edit-primary-dialog" : "edit-primary-dialog-error", forceUpdate);
}

void setShadow(QWidget* edit)
{
    QGraphicsDropShadowEffect* shadowEffect = new QGraphicsDropShadowEffect(edit);
    shadowEffect->setColor(QColor(0, 0, 0, 22));
    shadowEffect->setXOffset(0);
    shadowEffect->setYOffset(3);
    shadowEffect->setBlurRadius(6);
    edit->setGraphicsEffect(shadowEffect);
}

void setCssBtnPrimary(QPushButton* btn, bool forceUpdate)
{
    setCssProperty(btn, "btn-primary", forceUpdate);
}

void setCssBtnSecondary(QPushButton* btn, bool forceUpdate)
{
    setCssProperty(btn, "btn-secundary", forceUpdate);
}

void setCssTextBodyDialog(std::initializer_list<QWidget*> args)
{
    for (QWidget* w : args) {
        setCssTextBodyDialog(w);
    }
}

void setCssTextBodyDialog(QWidget* widget)
{
    setCssProperty(widget, "text-body1-dialog", false);
}

void setCssTitleScreen(QLabel* label)
{
    setCssProperty(label, "text-title-screen", false);
}

void setCssSubtitleScreen(QWidget* wid)
{
    setCssProperty(wid, "text-subtitle", false);
}

void setCssProperty(std::initializer_list<QWidget*> args, const QString& value)
{
    for (QWidget* w : args) {
        setCssProperty(w, value);
    }
}

void setCssProperty(QWidget* wid, const QString& value, bool forceUpdate)
{
    if (!wid) return;

    const bool iconClass = value.startsWith(QLatin1String("ic-"));
    const bool iconCarrier = qobject_cast<QAbstractButton*>(wid) != nullptr;
    const bool shouldForceIconPolish = iconClass && iconCarrier;

    if (wid->property("cssClass") == value) {
        if (forceUpdate || shouldForceIconPolish) {
            updateStyle(wid);
        }
        return;
    }
    wid->setProperty("cssClass", value);
    forceUpdateStyle(wid, forceUpdate || shouldForceIconPolish);
}

void forceUpdateStyle(QWidget* widget, bool forceUpdate)
{
    if (forceUpdate)
        updateStyle(widget);
}

void forceUpdateStyle(std::initializer_list<QWidget*> args)
{
    for (QWidget* w : args) {
        forceUpdateStyle(w, true);
    }
}
