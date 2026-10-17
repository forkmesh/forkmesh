#pragma once

#include "EmberMapView.h"
#include "EmberModel.h"
#include "NativePackageExtension.h"
#include "RepoInsight.h"

#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QPointer>

#include <memory>

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTimer;

namespace forkmesh::insight {
class RepoInsightBar;
}

namespace forkmesh::ember {

class HealthRing;
class RangeSlider;
class RowList;
class TrendChart;

struct Prepared {
    insight::SnapshotPtr snapshot;
    MapLayoutPtr layout;
    std::shared_ptr<const CouplingIndex> coupling;
};

class EmberFeature : public forkmesh::market::NativePackageWidget
{
public:
    EmberFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~EmberFeature() override;

    void refresh() override;
    void repositoryChanged() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query,
                                                               int limit) const override;
    void activateSearchHit(const QString &id) override;

    void showSnapshot(insight::SnapshotPtr snapshot);
    bool ready() const;
    void setTab(Tab tab);
    Tab tab() const { return m_tab; }
    void selectFile(int file, bool zoomTo);
    int selectedFile() const { return m_selected; }
    void setFilters(const Filters &filters);
    const TabColoring &coloring() const { return m_coloring; }
    EmberMapView *view() const { return m_view; }
    insight::RepoInsightBar *bar() const { return m_bar; }
    QString crumbText() const;
    int cardTab() const;
    void setCardTab(int index);
    void preparePreview(const QString &viewName);

private:
    void buildPanel(QWidget *panel);
    void prepared(std::shared_ptr<Prepared> result);
    void recolor();
    void updateCrumbs();
    void updateCard();
    void updateOverview();
    void updateTargets();
    void updateCoupling();
    void loadTrend();
    void applyFilters();
    void selectBestMatch();
    void resetData();
    QLabel *addMetric(QGridLayout *grid, const QString &name);

    forkmesh::market::NativePackageHost m_host;
    insight::RepoInsightBar *m_bar = nullptr;
    QTabBar *m_tabs = nullptr;
    QLabel *m_crumbs = nullptr;
    EmberMapView *m_view = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_authors = nullptr;
    RangeSlider *m_health = nullptr;
    RangeSlider *m_commits = nullptr;
    QLabel *m_commitsLabel = nullptr;
    QWidget *m_targetsBox = nullptr;
    QLabel *m_targetsIntro = nullptr;
    RowList *m_targets = nullptr;
    QWidget *m_overview = nullptr;
    RowList *m_bands = nullptr;
    RowList *m_hotspots = nullptr;
    QLabel *m_hotspotsTitle = nullptr;
    QWidget *m_card = nullptr;
    QLabel *m_cardCrumb = nullptr;
    QLabel *m_cardName = nullptr;
    QTabBar *m_cardTabs = nullptr;
    QStackedWidget *m_cardStack = nullptr;
    HealthRing *m_ring = nullptr;
    QLabel *m_band = nullptr;
    QLabel *m_bandNote = nullptr;
    QList<QPair<QLabel *, QLabel *>> m_metricRows;
    QLabel *m_commitsValue = nullptr;
    QLabel *m_sizeValue = nullptr;
    QLabel *m_authorValue = nullptr;
    QLabel *m_knowledgeValue = nullptr;
    QLabel *m_defectsValue = nullptr;
    QLabel *m_modifiedValue = nullptr;
    QLabel *m_complexityValue = nullptr;
    QLabel *m_functionsValue = nullptr;
    QLabel *m_duplicationValue = nullptr;
    QLabel *m_importsValue = nullptr;
    QLabel *m_languageValue = nullptr;
    QLabel *m_issues = nullptr;
    TrendChart *m_trend = nullptr;
    RowList *m_coupled = nullptr;
    QTimer *m_filterTimer = nullptr;

    insight::SnapshotPtr m_snapshot;
    MapLayoutPtr m_layout;
    std::shared_ptr<const CouplingIndex> m_coupling;
    QPointer<QFutureWatcher<std::shared_ptr<Prepared>>> m_prepareWatcher;
    TabColoring m_coloring;
    Filters m_filters;
    Tab m_tab = Tab::Hotspots;
    bool m_tabChosen = false;
    int m_selected = -1;
    QString m_pendingPath;
    QHash<QString, QList<insight::TrendPoint>> m_trends;
    QString m_trendLoading;
    bool m_syncing = false;
};

} // namespace forkmesh::ember
