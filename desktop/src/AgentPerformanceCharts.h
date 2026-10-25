#pragma once

#include "AgentPerformanceHistory.h"

#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

class AgentPerformanceCharts : public QWidget
{
public:
    explicit AgentPerformanceCharts(QWidget *parent = nullptr);

    void setHistory(const AgentPerformanceHistory *history);
    void setPeriod(int period);
    int period() const { return m_period; }
    void refresh(qint64 nowMs = -1);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    int chartCount() const { return m_charts.size(); }
    int columns() const { return m_columns; }
    QString chartTitle(int index) const;
    QString chartValue(const QString &metricId) const;
    QString chartChange(const QString &metricId) const;
    QRect chartRect(const QString &metricId) const;
    QRect plotRect(const QString &metricId) const;
    int markerCount() const { return m_markers.size(); }
    QString tooltipAt(const QPoint &point) const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Chart {
        AgentPerformanceHistory::Metric metric;
        AgentPerformanceSeries series;
        QString value;
        QString change;
        QRect rect;
        QRect plot;
        double scaleMax = 1.0;
    };

    void relayout(int width);
    void paintLegend(QPainter &painter) const;
    void paintChart(QPainter &painter, const Chart &chart, int hoverIndex) const;
    void paintLine(QPainter &painter, const Chart &chart, const QVector<double> &values,
                   bool current) const;
    QPointF pointFor(const Chart &chart, int index, double value) const;
    int indexAt(const Chart &chart, const QPoint &point) const;
    int chartAt(const QPoint &point) const;
    QString bucketTimeText(const Chart &chart, int index) const;
    QColor accentColor() const;
    bool darkSurface() const;

    const AgentPerformanceHistory *m_history = nullptr;
    QVector<Chart> m_charts;
    QVector<AgentPerformanceMarker> m_markers;
    int m_period = AgentPerformanceHistory::Day;
    int m_columns = 1;
    int m_contentHeight = 0;
    int m_layoutWidth = -1;
    int m_hoverChart = -1;
    int m_hoverIndex = -1;
};
