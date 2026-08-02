// Copyright (c) 2019-2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_EXPANDABLEBUTTON_H
#define PIVX_QT_EXPANDABLEBUTTON_H

#include <QWidget>
#include <QEvent>
#include <QEnterEvent>
#include <QString>

#include <QAbstractAnimation>
#include <QPointer>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QTimer>
#include <QVariantAnimation>

#include <optional>

namespace Ui {
class ExpandableButton;
}

class ExpandableButton : public QWidget
{
    Q_OBJECT

public:
    explicit ExpandableButton(QWidget *parent = nullptr);
    ~ExpandableButton();

    void setButtonClassStyle(const char *name, const QVariant &value, bool forceUpdate = false);
    void setButtonText(const QString& _text);
    void setNoIconText(const QString& _text);
    void setIcon(QString path);

    bool isChecked();
    void setChecked(bool check);
    void setKeepExpanded(bool _keepExpended){
        this->keepExpanded = _keepExpended;
        updateOutsideCursorMonitor();
    }
    void setHoverExpandEnabled(bool enabled) { this->hoverExpandEnabled = enabled; }
    void setSmall(bool animate = true);
    void setExpanded(bool animate = true);
    bool containsGlobalPos(const QPoint& globalPos) const;
    void syncHoverStateToGlobalPos(const QPoint& globalPos);
    
    // Custom expanded width for buttons with longer text (0 = use default)
    void setExpandedWidth(int width);
    int getExpandedWidth() const { return this->expandedWidth > 0 ? this->expandedWidth : 200; }
    
    // Progress fill for sync button (0-100, -1 to disable)
    void setProgress(int progress);
    int getProgress() const { return this->progressValue; }
    
private:
    void animateWidth(int startWidth, int endWidth);
    void handleHoverEnter();
    void handleHoverLeave(bool force = false, std::optional<QPoint> globalPos = std::nullopt);
    void updateOutsideCursorMonitor();
    bool shouldTrackOutsideCursor() const;
    
Q_SIGNALS:
    void Mouse_Pressed();
    void Mouse_Hover();
    void Mouse_HoverLeave();

public Q_SLOTS:
    QString getText(){
        return this->text;
    }

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent* event) override;
#else
    void enterEvent(QEvent* event) override;
#endif
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
    void innerMousePressEvent();
private:
    void updateDisplayedText();
    Ui::ExpandableButton *ui;
    QString notExpandedText;
    QString text;
    QPointer<QAbstractAnimation> animation;
    bool isExpanded = false;
    bool keepExpanded = false;
    bool hoverActive = false;
    bool hoverExpandEnabled = true;
    int expandedWidth = 0;  // Custom expanded width (0 = default 200)
    QFont expandedBaseFont;
    bool hasExpandedBaseFont = false;
    int progressValue = -1; // Target progress value (-1 = disabled)
    qreal paintedProgress = -1.0; // Animated progress used for painting
    QPointer<QVariantAnimation> progressAnimation;
    QPointer<QTimer> outsideCursorMonitor;
};

#endif // PIVX_QT_EXPANDABLEBUTTON_H
