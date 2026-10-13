#pragma once

#include <QColor>
#include <QDate>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QPainter;
class QRectF;

struct SkillUsageEvent {
    qint64 timestampMs = 0;
    QString skill;
};

// A compact 30-day stacked-area chart for the skills carried into agent runs.
// The parent owns the heading and total; this widget owns aggregation, axes,
// the wrapped legend, and the per-day hover detail.
class SkillUsageChart final : public QWidget
{
public:
    explicit SkillUsageChart(QWidget *parent = nullptr);

    void setEvents(QVector<SkillUsageEvent> events,
                   const QDate &through = QDate());

    int totalUses() const { return m_totalUses; }
    QStringList seriesNames() const;
    QVector<int> dailyTotals() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Series {
        QString name;
        QString key;
        QColor color;
        QVector<int> days;
    };

    int legendHeight() const;
    QRectF plotRect() const;
    int dayAtX(qreal x) const;
    int niceMaximum(int maximum) const;
    void drawLegend(QPainter &painter, const QRectF &plot) const;
    void drawHover(QPainter &painter, const QRectF &plot) const;

    QVector<Series> m_series;
    QDate m_firstDate;
    QDate m_lastDate;
    int m_totalUses = 0;
    int m_hoverDay = -1;
};
