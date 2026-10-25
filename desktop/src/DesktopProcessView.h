#pragma once

#include "DesktopProcesses.h"

#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QPainter;
class QTimer;

namespace forkmesh {

class DesktopProcessView : public QWidget
{
public:
    enum class SortColumn { Cpu, Memory, Threads, Name };

    explicit DesktopProcessView(QWidget *parent = nullptr);

    std::function<desktopprocs::Snapshot(qint64)> sampleSource;
    std::function<void(const desktopprocs::Process &)> onProcessClicked;

    void setSnapshot(const desktopprocs::Snapshot &snapshot);
    void sampleNow();
    void setSortColumn(SortColumn column);
    SortColumn sortColumn() const { return m_sort; }
    void setForkMeshOnly(bool only);
    bool forkMeshOnly() const { return m_forkmeshOnly; }
    QList<desktopprocs::Process> shownProcesses() const;
    QString toolTipAt(const QPointF &point) const;
    bool clickAt(const QPointF &point);
    bool sampling() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    static constexpr int kSampleMs = 2000;
    static constexpr int kRows = 16;

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Hit {
        QRectF area;
        QString text;
    };
    struct ClickHit {
        QRectF area;
        std::function<void()> action;
    };

    bool dark() const;
    QColor ink(int alpha = 255) const;
    void paintHeader(QPainter &p, const QRectF &area);
    void paintGauges(QPainter &p, const QRectF &area);
    void paintCores(QPainter &p, const QRectF &area);
    void paintTuning(QPainter &p, const QRectF &area);
    QRectF paintCard(QPainter &p, const QRectF &box);
    void paintShares(QPainter &p, const QRectF &area);
    void paintTable(QPainter &p, const QRectF &area);
    void paintChip(QPainter &p, QRectF &cursor, const QString &text, bool active,
                   std::function<void()> action);

    desktopprocs::Sampler m_sampler;
    desktopprocs::Snapshot m_snapshot;
    QVector<double> m_cpuHistory;
    QVector<double> m_memHistory;
    SortColumn m_sort = SortColumn::Cpu;
    bool m_forkmeshOnly = false;
    QTimer *m_timer = nullptr;
    QVector<Hit> m_hits;
    QVector<ClickHit> m_clicks;
};

} // namespace forkmesh
