// Copyright (c) 2019-2020 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "loadingdialog.h"

#include "qtutils.h"

#include "ui_loadingdialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMovie>
#include <QSizePolicy>

#include <algorithm>

namespace {
constexpr int kLoadingCardContentWidth = 420;
constexpr int kLoadingTitleMinHeight = 76;
constexpr int kLoadingSupportMinHeight = 44;

QString defaultLoadingSupportText()
{
    return QObject::tr("Please wait while this action completes.");
}

void clearLayout(QLayout* layout)
{
    if (!layout) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
}

void reserveWrappedLabelBreathingRoom(QLabel* label)
{
    if (!label) return;

    label->ensurePolished();

    const QMargins margins = label->contentsMargins();
    int textWidth = label->maximumWidth();
    if (textWidth <= 0) textWidth = label->width();
    if (textWidth <= 0) textWidth = label->sizeHint().width();
    textWidth = std::max(1, textWidth - margins.left() - margins.right());

    const QRect textBounds = label->fontMetrics().boundingRect(
            QRect(0, 0, textWidth, 4096),
            label->alignment() | Qt::TextWordWrap,
            label->text());
    label->setMinimumHeight(std::max(label->minimumHeight(), textBounds.height() + margins.top() + margins.bottom()));
    label->updateGeometry();
}
} // namespace

void Worker::process(){
    if (runnable) {
        try {
            runnable->run(type);
        } catch (std::exception &e) {
            QString errorStr = QString::fromStdString(e.what());
            runnable->onError(errorStr, type);
            Q_EMIT error(errorStr, type);
        } catch (...) {
            QString errorStr = QString::fromStdString("Unknown error running background task");
            runnable->onError(errorStr, type);
            Q_EMIT error(errorStr, type);
        }
    } else {
        Q_EMIT error("Null runnable", type);
    }
    Q_EMIT finished();
};

LoadingDialog::Content LoadingDialog::legacyContent(const QString& loadingMsg)
{
    Content content;
    content.eyebrow = QObject::tr("Processing");
    content.title = loadingMsg.trimmed().isEmpty() ? QObject::tr("Working") : loadingMsg.trimmed();
    content.supportText = defaultLoadingSupportText();
    return content;
}

LoadingDialog::LoadingDialog(QWidget *parent, QString loadingMsg) :
    ContainerDialog(parent),
    ui(new Ui::LoadingDialog),
    dialogContent(legacyContent(loadingMsg))
{
    initialize();
    applyContent();
}

LoadingDialog::LoadingDialog(QWidget* parent, const Content& content) :
    ContainerDialog(parent),
    ui(new Ui::LoadingDialog),
    dialogContent(content)
{
    initialize();
    applyContent();
}

void LoadingDialog::initialize()
{
    ui->setupUi(this);

    applyParentOrAppStyleSheet(parentWidget());

    ui->frame->setProperty("cssClass", "loading-card-overlay");
    ui->cardFrame->setProperty("cssClass", "loading-card-shell");
    ui->badgeFrame->setProperty("cssClass", "loading-card-badge");
    ui->labelEyebrow->setProperty("cssClass", "loading-card-eyebrow");
    ui->labelTitle->setProperty("cssClass", "loading-card-title");
    ui->labelSupport->setProperty("cssClass", "loading-card-support");
    ui->metaContainer->setProperty("cssClass", "loading-card-meta-row");
    ui->metaLayout->setAlignment(Qt::AlignCenter);
    ui->labelTitle->setMinimumWidth(kLoadingCardContentWidth);
    ui->labelTitle->setMaximumWidth(kLoadingCardContentWidth);
    ui->labelSupport->setMinimumWidth(kLoadingCardContentWidth);
    ui->labelSupport->setMaximumWidth(kLoadingCardContentWidth);
    ui->labelTitle->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    ui->labelSupport->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    ui->labelTitle->setContentsMargins(0, 6, 0, 6);
    ui->labelSupport->setContentsMargins(0, 3, 0, 3);
    ui->labelTitle->setMinimumHeight(kLoadingTitleMinHeight);
    ui->labelSupport->setMinimumHeight(kLoadingSupportMinHeight);

    auto* movie = new QMovie(isLightTheme() ? "://ani-loading-light" : "://ani-loading-dark", QByteArray(), ui->labelMovie);
    movie->setScaledSize(ui->labelMovie->maximumSize());
    ui->labelMovie->setText("");
    ui->labelMovie->setMovie(movie);
    movie->start();
}

void LoadingDialog::setContent(const Content& content)
{
    dialogContent = content;
    applyContent();
}

void LoadingDialog::applyContent()
{
    if (!ui) return;

    const QString eyebrow = dialogContent.eyebrow.trimmed().isEmpty()
            ? tr("Processing")
            : dialogContent.eyebrow.trimmed();
    const QString title = dialogContent.title.trimmed().isEmpty()
            ? tr("Working")
            : dialogContent.title.trimmed();
    const QString supportText = dialogContent.supportText.trimmed().isEmpty()
            ? defaultLoadingSupportText()
            : dialogContent.supportText.trimmed();

    ui->labelEyebrow->setText(eyebrow);
    ui->labelTitle->setText(title);
    ui->labelSupport->setText(supportText);
    reserveWrappedLabelBreathingRoom(ui->labelTitle);
    reserveWrappedLabelBreathingRoom(ui->labelSupport);
    rebuildMetaItems();
}

void LoadingDialog::rebuildMetaItems()
{
    if (!ui) return;

    clearLayout(ui->metaLayout);

    int added = 0;
    for (const ContentItem& item : dialogContent.items) {
        if (item.label.trimmed().isEmpty() || item.value.trimmed().isEmpty()) continue;

        auto* pill = new QFrame(ui->metaContainer);
        pill->setProperty("cssClass", "loading-card-meta-pill");

        auto* pillLayout = new QHBoxLayout(pill);
        pillLayout->setContentsMargins(12, 8, 12, 8);
        pillLayout->setSpacing(8);

        auto* label = new QLabel(item.label.trimmed(), pill);
        label->setProperty("cssClass", "loading-card-meta-label");

        auto* value = new QLabel(item.value.trimmed(), pill);
        value->setProperty("cssClass", "loading-card-meta-value");

        pillLayout->addWidget(label);
        pillLayout->addWidget(value);
        ui->metaLayout->addWidget(pill, 0, Qt::AlignCenter);

        updateStyle(label);
        updateStyle(value);
        updateStyle(pill);

        ++added;
        if (added >= 3) break;
    }

    ui->metaContainer->setVisible(added > 0);
}

void LoadingDialog::execute(Runnable *runnable, int type, std::unique_ptr<WalletModel::UnlockContext> pctx)
{
    QThread* thread = new QThread;
    Worker* worker = (pctx == nullptr ?
                      new Worker(runnable, type) :
                      new WalletWorker(runnable, type, std::move(pctx)));
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &Worker::process);
    connect(worker, &Worker::finished, thread, &QThread::quit);
    connect(worker, &Worker::finished, worker, &Worker::deleteLater);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(worker, &Worker::finished, this, &LoadingDialog::finished);
    thread->start();
}

void LoadingDialog::finished(){
    accept();
    deleteLater();
}

LoadingDialog::~LoadingDialog()
{
    delete ui;
}
