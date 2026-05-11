// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_CONTAINERDIALOG_H
#define PIVX_QT_CONTAINERDIALOG_H

#include <QDialog>
#include <QFile>
#include <QBoxLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

class QWidget;

namespace GUIUtil {
QString loadStyleSheet();
}

void setCssProperty(QWidget* wid, const QString& value, bool forceUpdate);
void bindRoundedDialogMask(QDialog* dialog, QWidget* contentFrame, int radiusPx);
bool isLightTheme();
void setDialogOwnsOpenPosition(QDialog* dialog, bool enabled);
void setDialogPopAnimationTarget(QDialog* dialog, QWidget* target);
void traceDialogPositionCheckpoint(const QDialog* dialog, const QString& checkpoint);

class ContainerDialog : public QDialog
{
public:
    explicit ContainerDialog(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags()) :
        QDialog(parent, flags)
    {
        setDialogOwnsOpenPosition(this, true);
    }

protected:
    void showEvent(QShowEvent* event) override
    {
        QDialog::showEvent(event);
        ensureKnownCssClassIcons();
        if (!centerOnShow || centeredOnShow) return;
        centeredOnShow = true;
        centerOnParentOrScreen();
        traceDialogPositionCheckpoint(this, QStringLiteral("container_show_center"));
    }

    void hideEvent(QHideEvent* event) override
    {
        QDialog::hideEvent(event);
        centeredOnShow = false;
        headerDragging = false;
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == dragHeader && event) {
            if (event->type() == QEvent::MouseButtonPress) {
                const auto* mouseEvent = static_cast<QMouseEvent*>(event);
                if (mouseEvent && mouseEvent->button() == Qt::LeftButton) {
                    if (tryStartSystemMove()) {
                        headerDragging = false;
                        return true;
                    }
                    headerDragging = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    dragStartGlobal = mouseEvent->globalPosition().toPoint();
#else
                    dragStartGlobal = mouseEvent->globalPos();
#endif
                    dragStartDialogPos = pos();
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove && headerDragging) {
                const auto* mouseEvent = static_cast<QMouseEvent*>(event);
                if (mouseEvent && (mouseEvent->buttons() & Qt::LeftButton)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    const QPoint currentGlobal = mouseEvent->globalPosition().toPoint();
#else
                    const QPoint currentGlobal = mouseEvent->globalPos();
#endif
                    move(dragStartDialogPos + (currentGlobal - dragStartGlobal));
                    return true;
                }
            } else if (event->type() == QEvent::MouseButtonRelease) {
                const auto* mouseEvent = static_cast<QMouseEvent*>(event);
                if (mouseEvent && mouseEvent->button() == Qt::LeftButton) {
                    headerDragging = false;
                    return true;
                }
            } else if (event->type() == QEvent::Leave) {
                headerDragging = false;
            }
        }
        return QDialog::eventFilter(watched, event);
    }

    void applyParentOrAppStyleSheet(QWidget* styleSource = nullptr)
    {
        QWidget* source = styleSource;
        if (!source) source = parentWidget();

        if (source && !source->styleSheet().isEmpty()) {
            setStyleSheet(source->styleSheet());
            return;
        }

        setStyleSheet(GUIUtil::loadStyleSheet());
    }

    void setCenterOnShow(bool enabled)
    {
        centerOnShow = enabled;
        if (!enabled) {
            centeredOnShow = true;
        }
    }

    void centerOnParentOrScreen()
    {
        QRect anchorRect;
        if (QWidget* host = parentWidget() ? parentWidget()->window() : nullptr; host && host->isVisible()) {
            const QRect hostRect = host->rect();
            anchorRect = QRect(host->mapToGlobal(hostRect.topLeft()), hostRect.size());
        } else if (QScreen* dialogScreen = screen()) {
            anchorRect = dialogScreen->availableGeometry();
        } else if (QScreen* primary = QGuiApplication::primaryScreen()) {
            anchorRect = primary->availableGeometry();
        }
        if (anchorRect.isNull()) return;
        move(anchorRect.center() - QPoint(width() / 2, height() / 2));
    }

    void initRoundedContainerFrame(QWidget* frame, int radiusPx = 16)
    {
        if (!frame) return;
        setCssProperty(frame, "container-dialog", true);
        bindRoundedDialogMask(this, frame, radiusPx);
    }

    QWidget* initDraggableHeaderChrome(QWidget* bodyFrame,
                                       QLabel* titleLabel,
                                       QPushButton* closeButton,
                                       int radiusPx = 16,
                                       const QString& shellCssClass = QStringLiteral("container-dialog-shell"),
                                       const QString& headerCssClass = QStringLiteral("container-dialog-header"),
                                       const QString& bodyCssClass = QStringLiteral("container-dialog-body"),
                                       const QString& headerObjectName = QStringLiteral("containerDialogHeader"))
    {
        if (!bodyFrame || !titleLabel || !closeButton) return nullptr;

        QWidget* host = bodyFrame->parentWidget();
        if (!host) return nullptr;
        QLayout* hostLayout = host->layout();
        if (!hostLayout) return nullptr;

        int bodyIndex = -1;
        for (int i = 0; i < hostLayout->count(); ++i) {
            QLayoutItem* item = hostLayout->itemAt(i);
            if (item && item->widget() == bodyFrame) {
                bodyIndex = i;
                break;
            }
        }
        if (bodyIndex < 0) return nullptr;

        QLayoutItem* bodyItem = hostLayout->takeAt(bodyIndex);
        delete bodyItem;

        auto* shell = new QFrame(host);
        shell->setObjectName(QStringLiteral("containerDialogShell"));
        shell->setSizePolicy(bodyFrame->sizePolicy());
        auto* shellLayout = new QVBoxLayout(shell);
        shellLayout->setContentsMargins(0, 0, 0, 0);
        shellLayout->setSpacing(0);

        auto* header = new QWidget(shell);
        header->setObjectName(headerObjectName);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        header->setMinimumHeight(52);
        header->setMaximumHeight(58);

        auto* headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(18, 10, 14, 10);
        headerLayout->setSpacing(8);

        if (QWidget* titleParent = titleLabel->parentWidget()) {
            if (QLayout* titleParentLayout = titleParent->layout()) {
                titleParentLayout->removeWidget(titleLabel);
            }
        }
        if (QWidget* closeParent = closeButton->parentWidget()) {
            if (QLayout* closeParentLayout = closeParent->layout()) {
                closeParentLayout->removeWidget(closeButton);
            }
        }

        titleLabel->setParent(header);
        titleLabel->setAlignment(Qt::AlignCenter);
        closeButton->setParent(header);

        const QSize closeRefSize = closeButton->minimumSize().isValid()
                ? closeButton->minimumSize()
                : closeButton->sizeHint();
        auto* leftAnchor = new QWidget(header);
        leftAnchor->setFixedSize(std::max(20, closeRefSize.width()), std::max(20, closeRefSize.height()));
        leftAnchor->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

        headerLayout->addWidget(leftAnchor, 0, Qt::AlignLeft | Qt::AlignVCenter);
        headerLayout->addWidget(titleLabel, 1, Qt::AlignCenter);
        headerLayout->addWidget(closeButton, 0, Qt::AlignRight | Qt::AlignVCenter);

        bodyFrame->setParent(shell);
        shellLayout->addWidget(header, 0);
        shellLayout->addWidget(bodyFrame, 1);

        if (auto* boxLayout = qobject_cast<QBoxLayout*>(hostLayout)) {
            boxLayout->insertWidget(bodyIndex, shell);
        } else if (auto* gridLayout = qobject_cast<QGridLayout*>(hostLayout)) {
            int row = 0;
            int column = 0;
            int rowSpan = 1;
            int columnSpan = 1;
            gridLayout->getItemPosition(bodyIndex, &row, &column, &rowSpan, &columnSpan);
            gridLayout->addWidget(shell, row, column, rowSpan, columnSpan);
        } else {
            hostLayout->addWidget(shell);
        }

        setCssProperty(shell, shellCssClass, true);
        setCssProperty(header, headerCssClass, true);
        setCssProperty(bodyFrame, bodyCssClass, true);

        bindRoundedDialogMask(this, shell, radiusPx);
        setDialogPopAnimationTarget(this, shell);

        dragHeader = header;
        header->installEventFilter(this);
        return header;
    }

    void ensureButtonIcon(QPushButton* button,
                          const QStringList& resourceCandidates,
                          const QSize& iconSize = QSize())
    {
        if (!button) return;
        if (iconSize.isValid()) {
            button->setIconSize(iconSize);
        }
        if (!button->icon().isNull()) return;

        for (const QString& candidate : resourceCandidates) {
            const QString path = normalizeIconResourcePath(candidate);
            if (path.isEmpty() || !QFile::exists(path)) continue;

            QIcon icon(path);
            if (icon.isNull()) continue;

            button->setIcon(icon);
            if (!button->icon().isNull()) return;
        }
    }

    void initDialogCloseButton(QPushButton* button,
                               const QString& cssClass = QStringLiteral("ic-close"),
                               const QSize& iconSize = QSize(20, 20))
    {
        if (!button) return;

        button->setText("");
        setCssProperty(button, cssClass, true);
        button->setCursor(Qt::PointingHandCursor);

        QStringList iconCandidates;
        if (cssClass == QLatin1String("ic-close")) {
            iconCandidates << themeAwareIconPath("ic-close", "ic-close-white")
                           << normalizeIconResourcePath("ic-close")
                           << normalizeIconResourcePath("ic-close-white");
        } else {
            iconCandidates << themeAwareIconPath(cssClass, cssClass)
                           << normalizeIconResourcePath(cssClass);
        }

        ensureButtonIcon(button, iconCandidates, iconSize);
    }

    static QString normalizeIconResourcePath(const QString& path)
    {
        QString normalized = path.trimmed();
        while (normalized.startsWith(':')) normalized.remove(0, 1);
        while (normalized.startsWith('/')) normalized.remove(0, 1);
        return normalized.isEmpty() ? QString() : QStringLiteral(":/") + normalized;
    }

    static QString themeAwareIconPath(const QString& lightAlias, const QString& darkAlias)
    {
        return normalizeIconResourcePath(isLightTheme() ? lightAlias : darkAlias);
    }

    static QStringList iconCandidatesForCssClass(const QString& cssClass)
    {
        if (cssClass == QLatin1String("ic-close")) {
            return {
                themeAwareIconPath("ic-close", "ic-close-white"),
                normalizeIconResourcePath("ic-close"),
                normalizeIconResourcePath("ic-close-white")
            };
        }
        if (cssClass == QLatin1String("ic-close-white")) {
            return {
                normalizeIconResourcePath("ic-close-white"),
                normalizeIconResourcePath("ic-close")
            };
        }
        if (cssClass == QLatin1String("ic-copy")) {
            return {
                themeAwareIconPath("ic-copy", "ic-copy-liliac"),
                normalizeIconResourcePath("ic-copy"),
                normalizeIconResourcePath("ic-copy-liliac")
            };
        }
        if (cssClass == QLatin1String("ic-copy-big")) {
            return {
                themeAwareIconPath("ic-copy-big", "ic-copy-big-white"),
                normalizeIconResourcePath("ic-copy-big"),
                normalizeIconResourcePath("ic-copy-big-white")
            };
        }
        if (cssClass == QLatin1String("ic-arrow-down")) {
            return {
                themeAwareIconPath("ic-arrow-drop-down", "ic-arrow-drop-down-white"),
                normalizeIconResourcePath("ic-arrow-drop-down"),
                normalizeIconResourcePath("ic-arrow-drop-down-white")
            };
        }
        if (cssClass == QLatin1String("ic-arrow")) {
            return {
                themeAwareIconPath("ic-arrow-right", "ic-arrow-right-white"),
                normalizeIconResourcePath("ic-arrow-right"),
                normalizeIconResourcePath("ic-arrow-right-white")
            };
        }
        if (cssClass == QLatin1String("ic-chevron-left")) {
            return {
                normalizeIconResourcePath("ic-chevron-left")
            };
        }
        return {};
    }

    void ensureKnownCssClassIcons()
    {
        const auto buttons = findChildren<QPushButton*>();
        for (QPushButton* button : buttons) {
            if (!button || !button->icon().isNull()) continue;
            const QString cssClass = button->property("cssClass").toString().trimmed();
            if (cssClass.isEmpty()) continue;
            const QStringList candidates = iconCandidatesForCssClass(cssClass);
            if (candidates.isEmpty()) continue;
            const QSize desiredSize = button->iconSize().isValid() ? button->iconSize() : QSize(20, 20);
            ensureButtonIcon(button, candidates, desiredSize);
        }
    }

private:
    bool tryStartSystemMove()
    {
        if (QWindow* win = windowHandle()) {
            return win->startSystemMove();
        }
        return false;
    }

    QPointer<QWidget> dragHeader;
    bool headerDragging{false};
    QPoint dragStartGlobal;
    QPoint dragStartDialogPos;
    bool centerOnShow{true};
    bool centeredOnShow{false};
};

#endif // PIVX_QT_CONTAINERDIALOG_H
