#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

// Stacked area of this process's resident memory over time, one band per
// mapping group, newest sample at the right edge. Hovering shows the breakdown
// at that moment. Plain QWidget (no moc), like ComponentHealthBoard: the
// Memory page feeds it and nothing calls back.
class MemoryUsageChart : public QWidget
{
public:
    struct Band {
        QString title;
        QColor color;
    };
    struct Point {
        qint64 timestampMs = 0;
        QVector<qint64> values; // one per band, same order
    };

    explicit MemoryUsageChart(QWidget *parent = nullptr);

    void setBands(const QVector<Band> &bands);
    // Appends and drops anything older than the window.
    void appendPoint(const Point &point);
    void clear();
    void setWindowMs(qint64 windowMs);

    int pointCount() const { return m_points.size(); }
    qint64 peakTotal() const;
    // "1.2 GB" style axis maximum the chart rounds its scale up to; public for
    // the tests.
    static qint64 niceCeiling(qint64 bytes);

    QSize sizeHint() const override { return QSize(600, 180); }
    QSize minimumSizeHint() const override { return QSize(200, 120); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRect plotRect() const;
    int pointIndexAt(int x) const;

    QVector<Band> m_bands;
    QVector<Point> m_points;
    qint64 m_windowMs = 5 * 60 * 1000;
    int m_hoverIndex = -1;
};
