#pragma once

#include "NativePackageExtension.h"
#include "RepoInsightBar.h"
#include "StrataModel.h"
#include "StrataViews.h"

#include <QFutureWatcher>
#include <QPointer>

class QComboBox;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTextBrowser;
class QUrl;

namespace forkmesh::strata {

class StrataFeature : public forkmesh::market::NativePackageWidget
{
public:
    enum View { Dashboard, Graph, Matrix, Scatter, Metrics, Rules, ViewCount };

    explicit StrataFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~StrataFeature() override;

    void refresh() override;
    void repositoryChanged() override;
    void setHostSearchQuery(const QString &text) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

    insight::RepoInsightBar *bar() const { return m_bar; }
    ModelPtr model() const { return m_model; }
    bool computing() const;
    void showView(int view);
    int currentView() const { return m_view; }
    void select(const Selection &selection);
    Selection selection() const { return m_selection; }
    void showCell(int from, int to);
    bool writeReport(const QString &path) const;
    QString detailsHtml() const;
    void setModuleDepth(int depth);
    void setDetailsVisible(bool visible);
    GraphView *graphView() const { return m_graph; }
    MetricsView *metricsView() const { return m_metrics; }
    RulesView *rulesView() const { return m_rules; }
    bool settled() const;

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void applySnapshot(const insight::SnapshotPtr &snapshot);
    void computeModel();
    void applyModel(const ModelPtr &model);
    void updateDetails();
    void updateBreadcrumb();
    void exportReport();
    void openFile(int file, int line) const;
    void anchorClicked(const QUrl &url);
    void showModule(int module);
    void restyleTabs();
    QString moduleDetails(int module) const;
    QString fileDetails(int file) const;
    QString cellDetails(int from, int to) const;
    QString overviewDetails() const;

    forkmesh::market::NativePackageHost m_host;
    insight::RepoInsightBar *m_bar = nullptr;
    QComboBox *m_depth = nullptr;
    QPushButton *m_export = nullptr;
    QWidget *m_tabRow = nullptr;
    QList<QPushButton *> m_tabs;
    QStackedWidget *m_tools = nullptr;
    QComboBox *m_colorBy = nullptr;
    QPushButton *m_fit = nullptr;
    QLabel *m_crumb = nullptr;
    QComboBox *m_sizeBy = nullptr;
    QComboBox *m_metricColor = nullptr;
    QPushButton *m_detailsToggle = nullptr;
    QSplitter *m_split = nullptr;
    QStackedWidget *m_views = nullptr;
    QTextBrowser *m_details = nullptr;
    DashboardView *m_dashboard = nullptr;
    GraphView *m_graph = nullptr;
    MatrixView *m_matrix = nullptr;
    ScatterView *m_scatter = nullptr;
    MetricsView *m_metrics = nullptr;
    RulesView *m_rules = nullptr;
    QFutureWatcher<ModelPtr> *m_watcher = nullptr;
    int m_generation = 0;
    insight::SnapshotPtr m_snapshot;
    ModelPtr m_model;
    Selection m_selection;
    int m_cellFrom = -1;
    int m_cellTo = -1;
    int m_view = Graph;
    QString m_query;
};

}
