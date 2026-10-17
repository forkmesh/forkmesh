#pragma once

#include "NativePackageExtension.h"
#include "RepoInsight.h"
#include "RepoInsightBar.h"
#include "SynapseLayout.h"

#include <QHash>
#include <QPointer>

#include <atomic>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStringListModel;
class QVBoxLayout;

namespace forkmesh::synapse {

class SynapseView;

class SynapseFeature : public forkmesh::market::NativePackageWidget
{
public:
    explicit SynapseFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~SynapseFeature() override;

    void refresh() override;
    void repositoryChanged() override;
    void setHostSearchQuery(const QString &text) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query,
                                                               int limit) const override;
    void activateSearchHit(const QString &id) override;

    SynapseView *view() const { return m_view; }
    forkmesh::insight::RepoInsightBar *bar() const { return m_bar; }
    forkmesh::insight::SnapshotPtr snapshot() const { return m_snapshot; }
    bool layoutRunning() const { return m_running; }
    QString summary() const { return m_summary; }

    void showSnapshot(const forkmesh::insight::SnapshotPtr &snapshot);
    void selectFile(int file, bool fly);
    void isolateCycle(int cycle);
    void setShowIndex(int index);
    void showcase();

private:
    struct Cached {
        GraphPtr graph;
        QVector<QVector3D> positions;
    };

    void startLayout();
    void graphReady(quint64 generation, const GraphPtr &graph, const QVector<QVector3D> &positions);
    void layoutStep(quint64 generation, const QVector<QVector3D> &positions, double progress);
    void rebuildPanel();
    void clearPanel();
    void handleLink(const QString &link);
    void updateSummary();
    void rebuildSearchModel();
    QString fileLink(int file) const;
    QString linkColour() const;
    QLabel *addSection(const QString &title);
    QLabel *addHtml(const QString &html);

    forkmesh::insight::RepoInsightBar *m_bar = nullptr;
    SynapseView *m_view = nullptr;
    QComboBox *m_colourBy = nullptr;
    QComboBox *m_show = nullptr;
    QComboBox *m_depth = nullptr;
    QCheckBox *m_autoRotate = nullptr;
    QCheckBox *m_pulses = nullptr;
    QPushButton *m_fit = nullptr;
    QLineEdit *m_search = nullptr;
    QStringListModel *m_searchModel = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_panel = nullptr;
    QVBoxLayout *m_panelLayout = nullptr;

    forkmesh::insight::SnapshotPtr m_snapshot;
    GraphPtr m_graph;
    bool m_includeIsolated = false;
    bool m_running = false;
    quint64 m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::shared_ptr<std::atomic_bool> m_inFlight;
    QHash<bool, Cached> m_cache;
    QString m_summary;
    QString m_hostQuery;
    QVector<int> m_orphans;
    QVector<int> m_leaves;
};

}
