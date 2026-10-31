#include "AgentPerformanceCharts.h"

#include "DesignTokens.h"
#include "FontSizing.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMargin = 16;
constexpr int kGap = 10;
constexpr int kLegendHeight = 26;
constexpr int kChartHeight = 172;
constexpr int kChartMinWidth = 250;
constexpr int kMaxColumns = 4;
constexpr int kRadius = 8;

using forkmesh::resizedFont;

double niceCeiling(double value)
{
    if (value <= 0)
        return 1.0;
    const double exponent = std::floor(std::log10(value));
    const double base = std::pow(10.0, exponent);
    for (double step : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (value <= step * base + 1e-9)
            return step * base;
    return 10.0 * base;
}

bool fixedPercentScale(const QString &metricId)
{
    return metricId == QLatin1String("memory") || metricId == QLatin1String("cpu") ||
           metricId == QLatin1String("efficiency");
}

QString changeText(const AgentPerformanceHistory::Metric &metric,
                   const AgentPerformanceSeries &series)
{
    const double change = series.change();
    if (std::isnan(change)) {
        if (series.hasSummary && !series.hasPreviousSummary)
            return QStringLiteral("no earlier data");
        return QString();
    }
    const int percent = qRound(change * 100.0);
    if (percent == 0)
        return QStringLiteral("flat vs before");
    QString text = QStringLiteral("%1 %2% vs before")
                       .arg(percent > 0 ? QStringLiteral("▲") : QStringLiteral("▼"))
                       .arg(qAbs(percent));
    if (!metric.neutral)
        text += (percent > 0) == metric.higherIsBetter ? QStringLiteral(" · better")
                                                       : QStringLiteral(" · worse");
    return text;
}

} // namespace

AgentPerformanceCharts::AgentPerformanceCharts(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("agentPerformanceCharts"));
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    refresh();
}

void AgentPerformanceCharts::setHistory(const AgentPerformanceHistory *history)
{
    m_history = history;
    refresh();
}

void AgentPerformanceCharts::setPeriod(int period)
{
    const int bounded = qBound(0, period, AgentPerformanceHistory::kPeriodCount - 1);
    if (bounded == m_period)
        return;
    m_period = bounded;
    refresh();
}

void AgentPerformanceCharts::refresh(qint64 nowMs)
{
    m_charts.clear();
    const AgentPerformanceHistory empty;
    const AgentPerformanceHistory &history = m_history ? *m_history : empty;
    for (const AgentPerformanceHistory::Metric &metric : AgentPerformanceHistory::metrics()) {
        Chart chart;
        chart.metric = metric;
        chart.series = history.series(metric.id, m_period, nowMs);
        chart.value = chart.series.hasSummary
                          ? AgentPerformanceHistory::formatValue(metric.unit, chart.series.summary)
                          : QStringLiteral("–");
        chart.change = changeText(metric, chart.series);
        chart.scaleMax = fixedPercentScale(metric.id) ? 100.0
                                                      : niceCeiling(chart.series.maximum());
        m_charts.append(chart);
    }
    m_markers = history.markers(m_period, nowMs);
    m_hoverChart = -1;
    m_hoverIndex = -1;
    m_layoutWidth = -1;
    relayout(width());
    update();
}

QSize AgentPerformanceCharts::sizeHint() const
{
    return QSize(2 * kMargin + 3 * kChartMinWidth + 2 * kGap, qMax(m_contentHeight, 200));
}

QSize AgentPerformanceCharts::minimumSizeHint() const
{
    return QSize(kChartMinWidth + 2 * kMargin, m_contentHeight);
}

QString AgentPerformanceCharts::chartTitle(int index) const
{
    return index >= 0 && index < m_charts.size() ? m_charts.at(index).metric.title : QString();
}

QString AgentPerformanceCharts::chartValue(const QString &metricId) const
{
    for (const Chart &chart : m_charts)
        if (chart.metric.id == metricId)
            return chart.value;
    return QString();
}

QString AgentPerformanceCharts::chartChange(const QString &metricId) const
{
    for (const Chart &chart : m_charts)
        if (chart.metric.id == metricId)
            return chart.change;
    return QString();
}

QRect AgentPerformanceCharts::chartRect(const QString &metricId) const
{
    for (const Chart &chart : m_charts)
        if (chart.metric.id == metricId)
            return chart.rect;
    return QRect();
}

QRect AgentPerformanceCharts::plotRect(const QString &metricId) const
{
    for (const Chart &chart : m_charts)
        if (chart.metric.id == metricId)
            return chart.plot;
    return QRect();
}

void AgentPerformanceCharts::relayout(int width)
{
    if (width <= 0)
        width = sizeHint().width();
    m_layoutWidth = width;
    const int usable = qMax(kChartMinWidth, width - 2 * kMargin);
    m_columns = qBound(1, (usable + kGap) / (kChartMinWidth + kGap), kMaxColumns);
    const int chartWidth = (usable - (m_columns - 1) * kGap) / m_columns;
    const int top = kMargin / 2 + kLegendHeight;
    for (int i = 0; i < m_charts.size(); ++i) {
        const int row = i / m_columns;
        const int column = i % m_columns;
        Chart &chart = m_charts[i];
        chart.rect = QRect(kMargin + column * (chartWidth + kGap),
                           top + row * (kChartHeight + kGap), chartWidth, kChartHeight);
        chart.plot = chart.rect.adjusted(12, 58, -12, -24);
    }
    const int rows = (int(m_charts.size()) + m_columns - 1) / m_columns;
    const int height = top + rows * kChartHeight + qMax(0, rows - 1) * kGap + kMargin;
    if (height != m_contentHeight) {
        m_contentHeight = height;
        setMinimumHeight(height);
        updateGeometry();
    }
}

void AgentPerformanceCharts::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (event->size().width() != m_layoutWidth)
        relayout(event->size().width());
}

bool AgentPerformanceCharts::darkSurface() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QColor AgentPerformanceCharts::accentColor() const
{
    return Theme::nextOr(Theme::Role::Accent,
                         QColor(darkSurface() ? QStringLiteral("#3987e5")
                                              : QStringLiteral("#2a78d6")));
}

QPointF AgentPerformanceCharts::pointFor(const Chart &chart, int index, double value) const
{
    const int count = qMax(2, int(chart.series.values.size()));
    const qreal x = chart.plot.left() + chart.plot.width() * qreal(index) / qreal(count - 1);
    const qreal ratio = qBound(0.0, value / qMax(1e-9, chart.scaleMax), 1.0);
    return QPointF(x, chart.plot.bottom() - chart.plot.height() * ratio);
}

int AgentPerformanceCharts::indexAt(const Chart &chart, const QPoint &point) const
{
    const int count = int(chart.series.values.size());
    if (count < 2 || chart.plot.width() <= 0 ||
        point.x() < chart.plot.left() - 6 || point.x() > chart.plot.right() + 6 ||
        point.y() < chart.plot.top() - 8 || point.y() > chart.plot.bottom() + 8)
        return -1;
    const double ratio = double(point.x() - chart.plot.left()) / chart.plot.width();
    return qBound(0, int(std::lround(ratio * (count - 1))), count - 1);
}

int AgentPerformanceCharts::chartAt(const QPoint &point) const
{
    for (int i = 0; i < m_charts.size(); ++i)
        if (m_charts.at(i).rect.contains(point))
            return i;
    return -1;
}

QString AgentPerformanceCharts::bucketTimeText(const Chart &chart, int index) const
{
    const qint64 start = chart.series.startMs + qint64(index) * chart.series.bucketMs;
    const QDateTime from = QDateTime::fromMSecsSinceEpoch(start);
    const QDateTime to = QDateTime::fromMSecsSinceEpoch(start + chart.series.bucketMs);
    switch (m_period) {
    case AgentPerformanceHistory::Hour:
    case AgentPerformanceHistory::Day:
        return QStringLiteral("%1–%2").arg(from.toString(QStringLiteral("HH:mm")),
                                           to.toString(QStringLiteral("HH:mm")));
    case AgentPerformanceHistory::Week:
        return QStringLiteral("%1–%2").arg(from.toString(QStringLiteral("ddd d MMM HH:mm")),
                                           to.toString(QStringLiteral("HH:mm")));
    default:
        return QStringLiteral("%1–%2").arg(from.toString(QStringLiteral("d MMM HH:mm")),
                                           to.toString(QStringLiteral("HH:mm")));
    }
}

QString AgentPerformanceCharts::tooltipAt(const QPoint &point) const
{
    const int chartIndex = chartAt(point);
    if (chartIndex < 0)
        return QString();
    const Chart &chart = m_charts.at(chartIndex);
    const int index = indexAt(chart, point);
    const AgentPerformanceHistory::Unit unit = chart.metric.unit;
    if (index < 0) {
        QString text = QStringLiteral("<b>%1</b><br>%2").arg(chart.metric.title.toHtmlEscaped(),
                                                            chart.metric.description.toHtmlEscaped());
        if (chart.series.hasSummary)
            text += QStringLiteral("<br>Last %1: %2")
                        .arg(AgentPerformanceHistory::periodLabel(m_period),
                             AgentPerformanceHistory::formatValue(unit, chart.series.summary));
        if (chart.series.hasPreviousSummary)
            text += QStringLiteral("<br>The %1 before: %2")
                        .arg(AgentPerformanceHistory::periodLabel(m_period),
                             AgentPerformanceHistory::formatValue(unit,
                                                                  chart.series.previousSummary));
        if (!chart.series.summaryNote.isEmpty())
            text += QStringLiteral("<br>%1").arg(chart.series.summaryNote.toHtmlEscaped());
        return text;
    }
    const double value = chart.series.values.value(index);
    const double previous = chart.series.previous.value(index);
    QString text = QStringLiteral("<b>%1</b> · %2<br>%3")
                       .arg(chart.metric.title.toHtmlEscaped(), bucketTimeText(chart, index),
                            std::isnan(value)
                                ? QStringLiteral("no data")
                                : AgentPerformanceHistory::formatValue(unit, value));
    const QString note = chart.series.notes.value(index);
    if (!note.isEmpty())
        text += QStringLiteral(" · %1").arg(note.toHtmlEscaped());
    if (!std::isnan(previous))
        text += QStringLiteral("<br>Same time, %1 before: %2")
                    .arg(AgentPerformanceHistory::periodLabel(m_period),
                         AgentPerformanceHistory::formatValue(unit, previous));
    const qint64 start = chart.series.startMs + qint64(index) * chart.series.bucketMs;
    for (const AgentPerformanceMarker &marker : m_markers)
        if (marker.ms >= start && marker.ms < start + chart.series.bucketMs)
            text += QStringLiteral("<br>Changed: %1").arg(marker.label.toHtmlEscaped());
    return text;
}

void AgentPerformanceCharts::mouseMoveEvent(QMouseEvent *event)
{
    const int chartIndex = chartAt(event->pos());
    const int index = chartIndex >= 0 ? indexAt(m_charts.at(chartIndex), event->pos()) : -1;
    if (chartIndex != m_hoverChart || index != m_hoverIndex) {
        m_hoverChart = chartIndex;
        m_hoverIndex = index;
        update();
    }
    if (chartIndex >= 0)
        QToolTip::showText(event->globalPosition().toPoint(), tooltipAt(event->pos()), this,
                           m_charts.at(chartIndex).rect);
    else
        QToolTip::hideText();
    QWidget::mouseMoveEvent(event);
}

void AgentPerformanceCharts::leaveEvent(QEvent *event)
{
    if (m_hoverChart >= 0 || m_hoverIndex >= 0) {
        m_hoverChart = -1;
        m_hoverIndex = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void AgentPerformanceCharts::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paintLegend(painter);
    for (int i = 0; i < m_charts.size(); ++i)
        paintChart(painter, m_charts.at(i), i == m_hoverChart ? m_hoverIndex : -1);
}

void AgentPerformanceCharts::paintLegend(QPainter &painter) const
{
    const QPalette colors = palette();
    const QColor muted = colors.color(QPalette::PlaceholderText);
    const QFont small = resizedFont(font(), -1.0);
    const QFontMetrics metrics(small);
    painter.setFont(small);
    const int y = kMargin / 2 + kLegendHeight / 2;
    int x = kMargin;
    const auto label = [&](const QString &text) {
        painter.setPen(muted);
        painter.drawText(QRect(x, y - 9, metrics.horizontalAdvance(text) + 2, 18),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
        x += metrics.horizontalAdvance(text) + 16;
    };
    label(QStringLiteral("Last %1, by %2")
              .arg(AgentPerformanceHistory::periodLabel(m_period),
                   AgentPerformanceHistory::bucketLabel(m_period)));
    painter.setPen(QPen(accentColor(), 2, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(x, y, x + 18, y);
    x += 24;
    label(QStringLiteral("this period"));
    QColor previous = muted;
    previous.setAlpha(190);
    painter.setPen(QPen(previous, 1.5, Qt::DashLine, Qt::RoundCap));
    painter.drawLine(x, y, x + 18, y);
    x += 24;
    label(QStringLiteral("the %1 before").arg(AgentPerformanceHistory::periodLabel(m_period)));
    QColor marker = colors.color(QPalette::Text);
    marker.setAlpha(110);
    painter.setPen(QPen(marker, 1, Qt::DotLine));
    painter.drawLine(x + 4, y - 7, x + 4, y + 7);
    x += 14;
    label(QStringLiteral("build, memory, CPU or agent-limit change"));
}

void AgentPerformanceCharts::paintLine(QPainter &painter, const Chart &chart,
                                       const QVector<double> &values, bool current) const
{
    QVector<QVector<QPointF>> segments;
    QVector<QPointF> segment;
    for (int i = 0; i < values.size(); ++i) {
        if (std::isnan(values.at(i))) {
            if (!segment.isEmpty())
                segments.append(segment);
            segment.clear();
            continue;
        }
        segment.append(pointFor(chart, i, values.at(i)));
    }
    if (!segment.isEmpty())
        segments.append(segment);
    const QColor accent = accentColor();
    QColor muted = palette().color(QPalette::PlaceholderText);
    muted.setAlpha(190);
    for (const QVector<QPointF> &points : std::as_const(segments)) {
        if (points.size() == 1) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(current ? accent : muted);
            painter.drawEllipse(points.first(), current ? 2.5 : 1.5, current ? 2.5 : 1.5);
            continue;
        }
        QPainterPath path(points.first());
        for (int i = 1; i < points.size(); ++i)
            path.lineTo(points.at(i));
        if (current) {
            QPainterPath area = path;
            area.lineTo(points.last().x(), chart.plot.bottom());
            area.lineTo(points.first().x(), chart.plot.bottom());
            area.closeSubpath();
            QColor fill = accent;
            fill.setAlpha(darkSurface() ? 46 : 34);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawPath(area);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(current ? QPen(accent, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                               : QPen(muted, 1.5, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
    }
}

void AgentPerformanceCharts::paintChart(QPainter &painter, const Chart &chart,
                                        int hoverIndex) const
{
    const QPalette colors = palette();
    const QColor text = colors.color(QPalette::Text);
    const QColor muted = colors.color(QPalette::PlaceholderText);
    QColor border = colors.color(QPalette::Mid);
    border.setAlpha(140);
    painter.setPen(QPen(border, 1));
    painter.setBrush(colors.color(QPalette::Base));
    painter.drawRoundedRect(QRectF(chart.rect).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);

    const QRect inner = chart.rect.adjusted(12, 9, -12, -8);
    const QFont small = resizedFont(font(), -1.5);
    const QFontMetrics smallMetrics(small);
    painter.setFont(small);
    painter.setPen(muted);
    const int changeWidth = qMin(inner.width() / 2, smallMetrics.horizontalAdvance(chart.change) + 2);
    painter.drawText(QRect(inner.right() - changeWidth + 1, inner.top(), changeWidth, 16),
                     Qt::AlignRight | Qt::AlignVCenter,
                     smallMetrics.elidedText(chart.change, Qt::ElideRight, changeWidth));
    QFont titleFont = resizedFont(font(), -0.5);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(text);
    const QRect titleRect(inner.left(), inner.top(), inner.width() - changeWidth - 8, 16);
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(titleFont).elidedText(chart.metric.title, Qt::ElideRight,
                                                        titleRect.width()));

    QFont valueFont = resizedFont(font(), 4.0);
    valueFont.setWeight(QFont::DemiBold);
    const QFontMetrics valueMetrics(valueFont);
    painter.setFont(valueFont);
    const QRect valueRect(inner.left(), inner.top() + 18, inner.width(), 26);
    painter.drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter, chart.value);
    const int valueAdvance = valueMetrics.horizontalAdvance(chart.value) + 10;
    painter.setFont(small);
    painter.setPen(muted);
    const QRect noteRect(inner.left() + valueAdvance, valueRect.top() + 4,
                         inner.width() - valueAdvance, valueRect.height() - 4);
    painter.drawText(noteRect, Qt::AlignLeft | Qt::AlignVCenter,
                     smallMetrics.elidedText(chart.series.summaryNote, Qt::ElideRight,
                                             noteRect.width()));

    const QRect plot = chart.plot;
    QColor grid = colors.color(QPalette::Mid);
    grid.setAlpha(110);
    painter.setPen(QPen(grid, 1));
    painter.drawLine(plot.left(), plot.bottom(), plot.right(), plot.bottom());
    grid.setAlpha(55);
    painter.setPen(QPen(grid, 1, Qt::DashLine));
    painter.drawLine(plot.left(), plot.top() + plot.height() / 2, plot.right(),
                     plot.top() + plot.height() / 2);
    painter.drawLine(plot.left(), plot.top(), plot.right(), plot.top());

    painter.setPen(muted);
    painter.drawText(QRect(plot.left(), plot.top() - 15, plot.width(), 14),
                     Qt::AlignLeft | Qt::AlignBottom,
                     AgentPerformanceHistory::formatValue(chart.metric.unit, chart.scaleMax));
    painter.drawText(QRect(plot.left(), plot.bottom() + 3, plot.width(), 16),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("−%1").arg(AgentPerformanceHistory::periodShortLabel(m_period)));
    painter.drawText(QRect(plot.left(), plot.bottom() + 3, plot.width(), 16),
                     Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("now"));

    const int count = int(chart.series.values.size());
    if (count >= 2) {
        QColor markerColor = text;
        markerColor.setAlpha(110);
        painter.setPen(QPen(markerColor, 1, Qt::DotLine));
        const double spanBuckets = double(count - 1);
        for (const AgentPerformanceMarker &marker : m_markers) {
            const double position =
                double(marker.ms - chart.series.startMs) / double(chart.series.bucketMs);
            if (position < 0 || position > spanBuckets + 1)
                continue;
            const qreal x = plot.left() + plot.width() * qMin(1.0, position / spanBuckets);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
    }

    if (!chart.series.hasData()) {
        painter.setPen(muted);
        painter.setFont(small);
        painter.drawText(plot, Qt::AlignCenter,
                         QStringLiteral("No data in the last %1")
                             .arg(AgentPerformanceHistory::periodLabel(m_period)));
        return;
    }
    painter.save();
    painter.setClipRect(plot.adjusted(-3, -3, 3, 3));
    paintLine(painter, chart, chart.series.previous, false);
    paintLine(painter, chart, chart.series.values, true);
    painter.restore();

    if (hoverIndex >= 0 && hoverIndex < count) {
        const QPointF top = pointFor(chart, hoverIndex, chart.scaleMax);
        QColor cross = text;
        cross.setAlpha(90);
        painter.setPen(QPen(cross, 1));
        painter.drawLine(QPointF(top.x(), plot.top()), QPointF(top.x(), plot.bottom()));
        const double value = chart.series.values.at(hoverIndex);
        if (!std::isnan(value)) {
            const QPointF point = pointFor(chart, hoverIndex, value);
            painter.setPen(QPen(colors.color(QPalette::Base), 2));
            painter.setBrush(accentColor());
            painter.drawEllipse(point, 4.5, 4.5);
        }
    }
}
