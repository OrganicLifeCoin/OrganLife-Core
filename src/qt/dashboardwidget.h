// Copyright (c) 2019-2021 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_DASHBOARDWIDGET_H
#define PIVX_QT_DASHBOARDWIDGET_H

#include "chartutils.h"
#include "furabstractlistitemdelegate.h"
#include "furlistrow.h"
#include "pwidget.h"
#include "transactionfilterproxy.h"
#include "transactiontablemodel.h"
#include "txviewholder.h"

#include <atomic>
#include <cstdlib>
#include <vector>
#include <QWidget>
#include <QLineEdit>
#include <QMap>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPointer>
#include <QVariantAnimation>
#include <QtGlobal>

#if defined(HAVE_CONFIG_H)
#include "config/pivx-config.h" /* for USE_QTCHARTS */
#endif

#ifdef USE_QTCHARTS

#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QAreaSeries>
#include <QtCharts/QBarCategoryAxis>
#include <QtCharts/QChart>
#include <QtCharts/QValueAxis>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
using namespace QtCharts;
#endif

#endif

class OrganicLifeGUI;
class WalletModel;
class QLabel;
class QPushButton;
class QVBoxLayout;
class GovernanceDialogTests;

namespace Ui {
class DashboardWidget;
}

class SortEdit : public QLineEdit{
    Q_OBJECT
public:
    explicit SortEdit(QWidget* parent = nullptr) : QLineEdit(parent){}

    inline void mousePressEvent(QMouseEvent *) override{
        Q_EMIT Mouse_Pressed();
    }

    ~SortEdit() override{}

Q_SIGNALS:
    void Mouse_Pressed();

};

enum SortTx {
    DATE_DESC = 0,
    DATE_ASC = 1,
    AMOUNT_DESC = 2,
    AMOUNT_ASC = 3
};

enum ChartShowType {
    ALL,
    YEAR,
    MONTH,
    DAY
};

class ChartData {
public:
    ChartData() {}

    QMap<int, std::pair<qint64, qint64>> amountsByCache;
    qreal maxValue = 0;
    qint64 totalPiv = 0;
    qint64 totalMN = 0;
    QList<qreal> valuesPiv;
    QList<qreal> valuesMN;
    QStringList xLabels;
};

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

class DashboardWidget : public PWidget
{
    Q_OBJECT

public:
    explicit DashboardWidget(OrganicLifeGUI* _window);
    ~DashboardWidget();

    void loadWalletModel() override;
    void clearWalletModel() override;
    void loadChart();

    void run(int type) override;
    void onError(QString error, int type) override;

public Q_SLOTS:
    void walletSynced(bool isSync);
    /**
     * Show incoming transaction notification for new transactions.
     * The new items are those between start and end inclusive, under the given parent item.
    */
    void processNewTransaction(const QModelIndex& parent, int start, int /*end*/);
    void prepareTransactionInsertionAnimation(const QModelIndex& parent, int start, int end);
Q_SIGNALS:
    /** Notify that a new transaction appeared */
    void incomingTransaction(const QString& date, int unit, const CAmount& amount, const QString& type, const QString& address);
private Q_SLOTS:
    void handleTransactionClicked(const QModelIndex &index);
    void changeTheme(bool isLightTheme, QString &theme) override;
    void onSortChanged(const QString&);
    void onSortTypeChanged(const QString& value);
    void updateDisplayUnit();
    void showList();
    void onTxArrived(const QString& hash, const bool isCoinStake, const bool isMNReward, const bool isCSAnyType);

#ifdef USE_QTCHARTS
    void windowResizeEvent(QResizeEvent* event);
    void changeChartColors();
    void onChartYearChanged(const QString&);
    void onChartMonthChanged(const QString&);
    void onChartArrowClicked(bool goLeft);
#endif

private:
    friend class GovernanceDialogTests;

    Ui::DashboardWidget *ui{nullptr};
    FurAbstractListItemDelegate* txViewDelegate{nullptr};
    TransactionFilterProxy* filter{nullptr};
    TxViewHolder* txHolder{nullptr};
    TransactionTableModel* txModel{nullptr};
    int nDisplayUnit{-1};
    bool isSync{false};
    QPixmap preInsertViewportSnapshot;
    bool txInsertAnimationPending{false};
    QPersistentModelIndex animatedTxProxyIndex;
    QPointer<QVariantAnimation> txRowFadeAnimation;

    // V2 stat row
    QLabel* statValueAvailable{nullptr};
    QLabel* statValueStaking{nullptr};
    QLabel* statValueRewards{nullptr};
    QVBoxLayout* feedRowsLayout{nullptr};
    QWidget* feedBody{nullptr};
    QPushButton* feedToggle{nullptr};
    bool feedExpanded{false};
    void setFeedExpanded(bool expanded);
    QWidget* chartBody{nullptr};
    QWidget* analyticsCard{nullptr};
    QPushButton* chartToggle{nullptr};
    int chartBottomStretchIdx{-1};
    bool chartExpanded{true};
    void setChartExpanded(bool expanded);
    void animateSection(QWidget* body, bool expand, const std::function<void()>& onFinish = nullptr);
    void updateStatBalances(const interfaces::WalletBalances& balances);
    void updateStatRewards();
    void updateFeedNotes();

    void changeSort(int nSortIndex);
    void startInsertedRowAnimations(const QModelIndex& proxyIndex);
    void updateTransactionViewState(bool hasTransactions, bool hasVisibleTransactions);

#ifdef USE_QTCHARTS

    int64_t lastRefreshTime{0};
    std::atomic<bool> isLoading;

    // Chart
    TransactionFilterProxy* stakesFilter{nullptr};
    bool isChartInitialized{false};
	    QChartView *chartView{nullptr};
	    QLineSeries *stakesLine{nullptr};
	    QAreaSeries *areaStakes{nullptr};
	    QLineSeries *mnLine{nullptr};

	    QBarCategoryAxis *axisX{nullptr};
	    QValueAxis *axisY{nullptr};

	    QChart *chart{nullptr};
    bool isChartMin{false};
    ChartShowType chartShow{YEAR};
    int yearFilter{0};
    int monthFilter{0};
    int visibleDayFirst{1};
    int visibleDayLast{7};
    int weekSpanDays{7};
    bool hasMNRewards{false};
    bool chartHasRenderedData{false};
    std::vector<ChartStakeSample> chartStakeRowsSnapshot;

    ChartData* chartData{nullptr};
    bool hasStakes{false};
    bool fShowCharts{true};
    std::atomic<bool> filterUpdateNeeded{false};

    void initChart();
    void showHideEmptyChart(bool show, bool loading, bool forceView = false);
    bool refreshChart();
    void tryChartRefresh();
    void updateStakeFilter();
    void snapshotChartRows();
    QMap<int, std::pair<qint64, qint64>> getAmountBy();
    bool loadChartData(bool withMonthNames);
    void updateAxisX(const QStringList *arg = nullptr);
    void setChartShow(ChartShowType type);
    std::pair<int, int> getChartRange(const QMap<int, std::pair<qint64, qint64>>& amountsBy);
    void resolveMonthWindowForFilters();
    void pageMonthWindow(bool goLeft);
    void updateMonthArrowState();
    int monthDaysInFilter() const;

private Q_SLOTS:
    void onChartRefreshed();
    void onHideChartsChanged(bool fHide);

#endif

};

#endif // PIVX_QT_DASHBOARDWIDGET_H
