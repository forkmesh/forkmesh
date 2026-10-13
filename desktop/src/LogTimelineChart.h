#ifndef FORKMESH_LOG_TIMELINE_CHART_H
#define FORKMESH_LOG_TIMELINE_CHART_H

#include <QColor>
#include <QHash>
#include <QVector>
#include <QWidget>

#include <functional>

struct LogTimelineEntry {
    qint64 timestampMs = 0;
    QString category;
};

// A pre-aggregated slice of history. Thirty days of individual lines will not
// fit in memory, so anything older than the in-memory log buffer arrives as
// counts per (bucket, category) and is drawn alongside the exact entries.
struct LogTimelineBucket {
    qint64 startMs = 0;
    qint64 spanMs = 0;
    QString category;
    int count = 0;
};

// One coloured band of the stacked bars. The chip row above the chart is its
// legend: a series carries the same accent its chip does.
struct LogTimelineSeries {
    QString category;
    QColor color;
};

class LogTimelineChart : public QWidget {
public:
    explicit LogTimelineChart(QWidget *parent = nullptr);

    void setEntries(QVector<LogTimelineEntry> entries);
    void setHistoryBuckets(QVector<LogTimelineBucket> buckets);
    void appendEntry(const LogTimelineEntry &entry);
    void removeEntry(qint64 timestampMs, const QString &category);
    // Every bar is stacked out of `series`, one band per category, in the
    // order given. `restrictToSeries` drops every other category from the
    // chart - that is what a picked set of chips means; with it off the rest
    // of the log stacks on top in `accent`, so the All view still shows the
    // whole bar with its selected categories called out inside it.
    void setSeries(QVector<LogTimelineSeries> series, bool restrictToSeries,
                   const QColor &accent);
    void setCategoryFilter(const QString &category, const QColor &accent);
    void setRange(qint64 fromMs, qint64 toMs);
    void resetZoom();

    qint64 viewFromMs() const { return m_viewFromMs; }
    qint64 viewToMs() const { return m_viewToMs; }
    int loadedEntryCount() const { return m_entries.size(); }
    int historyBucketCount() const { return m_historyBuckets.size(); }
    int visibleEntryCount() const;
    bool isZoomed() const;
#ifdef FORKMESH_WINDOW_TESTS
    quint64 entriesRevisionForTest() const { return m_entriesRevision; }
    int seriesCountForTest() const { return m_series.size(); }
    bool seriesContainsForTest(const QString &category) const
    {
        return m_seriesIndex.contains(category);
    }
    bool restrictsToSeriesForTest() const { return m_restrictToSeries; }
#endif

    std::function<void()> viewChanged;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QRectF plotRect() const;
    qint64 timeAtX(qreal x) const;
    bool entryIsVisible(const LogTimelineEntry &entry) const;
    // Which stacked band a category belongs in: -1 once a picked set of chips
    // has filtered it out, otherwise a row index into `cells` below.
    int seriesRowFor(const QString &category) const;
    int stackRowCount() const;
    QColor rowColor(int row) const;
    // Spreads the visible slice of every history bucket across `cells`, a
    // (column, stack row) histogram of the current view laid out row-major per
    // column, and returns how many lines that added.
    int addHistoryBuckets(QVector<int> &cells, int columnCount) const;
    void notifyViewChanged();
    // The live log feeds appendEntry()/setRange() once per logged line (one
    // to several a second on a busy node), and every viewChanged callback
    // re-counts the visible entries and re-ranks the frequency chart. Those
    // streaming edits coalesce into one repaint + one notification per
    // kStreamCoalesceMs instead of one per line; user-driven changes (zoom,
    // filter, preset) still notify synchronously.
    void scheduleStreamingViewChanged();

    QVector<LogTimelineEntry> m_entries;
    QVector<LogTimelineBucket> m_historyBuckets;
    QVector<LogTimelineSeries> m_series;
    // category -> row in m_series. A paint walks every visible entry, so the
    // lookup has to stay off a linear scan of the picked categories.
    QHash<QString, int> m_seriesIndex;
    bool m_restrictToSeries = false;
    QColor m_accent = QColor(QStringLiteral("#58a6ff"));
    qint64 m_rangeFromMs = 0;
    qint64 m_rangeToMs = 1;
    qint64 m_viewFromMs = 0;
    qint64 m_viewToMs = 1;
    QPointF m_pointerPosition;
    qreal m_dragStartX = -1.0;
    qreal m_dragCurrentX = -1.0;
    bool m_dragging = false;
    bool m_pointerInside = false;
    bool m_streamNotifyPending = false;
#ifdef FORKMESH_WINDOW_TESTS
    quint64 m_entriesRevision = 0;
#endif
};

#endif
