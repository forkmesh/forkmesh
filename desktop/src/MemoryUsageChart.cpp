#include "MemoryUsageChart.h"

#include "SystemStats.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <limits>

namespace {

qint64 pointTotal(const MemoryUsageChart::Point &point)
{
    qint64 total = 0;
    for (qint64 v : point.values)
        total += v;
    return total;
}

} // namespace

MemoryUsageChart::MemoryUsageChart(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(180);
}

void MemoryUsageChart::setBands(const QVector<Band> &bands)
{
    m_bands = bands;
    update();
}

void MemoryUsageChart::appendPoint(const Point &point)
{
    m_points.append(point);
    const qint64 cutoff = point.timestampMs - m_windowMs;
    int drop = 0;
    while (drop < m_points.size() - 1 && m_points.at(drop).timestampMs < cutoff)
        ++drop;
    if (drop > 0)
        m_points.remove(0, drop);
    if (m_hoverIndex >= m_points.size())
        m_hoverIndex = -1;
    update();
}

void MemoryUsageChart::clear()
{
    m_points.clear();
    m_hoverIndex = -1;
    update();
}

void MemoryUsageChart::setWindowMs(qint64 windowMs)
{
    m_windowMs = qMax<qint64>(10 * 1000, windowMs);
    update();
}

qint64 MemoryUsageChart::peakTotal() const
{
    qint64 peak = 0;
    for (const Point &point : m_points)
        peak = qMax(peak, pointTotal(point));
    return peak;
}

qint64 MemoryUsageChart::niceCeiling(qint64 bytes)
{
    if (bytes <= 0)
        return 1024 * 1024;
    // 1, 2, 5 steps of a decimal multiple of 1 MB read cleanly on the axis.
    qint64 unit = 1024 * 1024;
    while (unit * 10 < bytes)
        unit *= 10;
    for (qint64 step : {1, 2, 5, 10}) {
        if (unit * step >= bytes)
            return unit * step;
    }
    return unit * 10;
}

QRect MemoryUsageChart::plotRect() const
{
    return rect().adjusted(64, 10, -12, -22);
}

int MemoryUsageChart::pointIndexAt(int x) const
{
    if (m_points.isEmpty())
        return -1;
    const QRect plot = plotRect();
    const qint64 newest = m_points.last().timestampMs;
    const qint64 oldest = newest - m_windowMs;
    const qint64 t =
        oldest + qint64(double(x - plot.left()) / qMax(1, plot.width()) * m_windowMs);
    int best = -1;
    qint64 bestGap = std::numeric_limits<qint64>::max();
    for (int i = 0; i < m_points.size(); ++i) {
        const qint64 gap = qAbs(m_points.at(i).timestampMs - t);
        if (gap < bestGap) {
            bestGap = gap;
            best = i;
        }
    }
    return best;
}

void MemoryUsageChart::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRect plot = plotRect();
    const QColor text = palette().color(QPalette::WindowText);
    QColor grid = text;
    grid.setAlphaF(0.12);
    QColor muted = text;
    muted.setAlphaF(0.6);

    const qint64 ceiling = niceCeiling(peakTotal());
    QFont small = font();
    small.setPointSizeF(qMax(7.0, small.pointSizeF() - 1.5));
    p.setFont(small);

    // Horizontal grid with byte labels on the left.
    for (int i = 0; i <= 4; ++i) {
        const int y = plot.bottom() - int(double(plot.height()) * i / 4);
        p.setPen(QPen(grid, 1));
        p.drawLine(plot.left(), y, plot.right(), y);
        p.setPen(muted);
        p.drawText(QRect(0, y - 8, plot.left() - 6, 16),
                   Qt::AlignRight | Qt::AlignVCenter,
                   SystemStats::formatBytes(ceiling * i / 4));
    }

    if (m_points.isEmpty() || m_bands.isEmpty()) {
        p.setPen(muted);
        p.drawText(plot, Qt::AlignCenter, QStringLiteral("Sampling…"));
        return;
    }

    const qint64 newest = m_points.last().timestampMs;
    const qint64 oldest = newest - m_windowMs;
    const auto xFor = [&](qint64 t) {
        return plot.left() +
               double(t - oldest) / double(m_windowMs) * plot.width();
    };
    const auto yFor = [&](qint64 v) {
        return plot.bottom() - double(v) / double(ceiling) * plot.height();
    };

    // Time labels: the window's start and "now".
    p.setPen(muted);
    p.drawText(QRect(plot.left(), plot.bottom() + 4, 120, 16),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("-%1 min").arg(m_windowMs / 60000.0, 0, 'f',
                                             m_windowMs % 60000 ? 1 : 0));
    p.drawText(QRect(plot.right() - 120, plot.bottom() + 4, 120, 16),
               Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("now"));

    // Stack bottom-up: band 0 sits on the axis.
    QVector<double> base(m_points.size(), 0.0);
    for (int b = 0; b < m_bands.size(); ++b) {
        QPainterPath path;
        QVector<QPointF> top;
        top.reserve(m_points.size());
        for (int i = 0; i < m_points.size(); ++i) {
            const Point &point = m_points.at(i);
            const qint64 v = b < point.values.size() ? point.values.at(b) : 0;
            top.append(QPointF(xFor(point.timestampMs),
                               yFor(qint64(base[i]) + v)));
        }
        path.moveTo(top.first());
        for (int i = 1; i < top.size(); ++i)
            path.lineTo(top.at(i));
        for (int i = m_points.size() - 1; i >= 0; --i)
            path.lineTo(QPointF(xFor(m_points.at(i).timestampMs), yFor(qint64(base[i]))));
        path.closeSubpath();
        QColor fill = m_bands.at(b).color;
        fill.setAlphaF(0.55);
        p.fillPath(path, fill);
        p.setPen(QPen(m_bands.at(b).color, 1.2));
        for (int i = 1; i < top.size(); ++i)
            p.drawLine(top.at(i - 1), top.at(i));
        for (int i = 0; i < m_points.size(); ++i) {
            const Point &point = m_points.at(i);
            base[i] += b < point.values.size() ? point.values.at(b) : 0;
        }
    }

    if (m_hoverIndex >= 0 && m_hoverIndex < m_points.size()) {
        const int x = int(xFor(m_points.at(m_hoverIndex).timestampMs));
        QColor rule = text;
        rule.setAlphaF(0.5);
        p.setPen(QPen(rule, 1, Qt::DashLine));
        p.drawLine(x, plot.top(), x, plot.bottom());
    }
}

void MemoryUsageChart::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    const int index = plotRect().contains(pos) ? pointIndexAt(pos.x()) : -1;
    if (index != m_hoverIndex) {
        m_hoverIndex = index;
        update();
    }
    if (index < 0) {
        QToolTip::hideText();
        return;
    }
    const Point &point = m_points.at(index);
    QString tip = QStringLiteral("<b>%1</b> — %2 resident<br>")
                      .arg(QDateTime::fromMSecsSinceEpoch(point.timestampMs)
                               .toString(QStringLiteral("HH:mm:ss")),
                           SystemStats::formatBytes(pointTotal(point)));
    // Largest first, so the answer to "what is it" is on the first line.
    QVector<int> order;
    for (int b = 0; b < m_bands.size() && b < point.values.size(); ++b)
        order.append(b);
    std::sort(order.begin(), order.end(), [&point](int a, int b) {
        return point.values.at(a) > point.values.at(b);
    });
    for (int b : order) {
        if (point.values.at(b) <= 0)
            continue;
        tip += QStringLiteral("<span style='color:%1'>■</span> %2: %3<br>")
                   .arg(m_bands.at(b).color.name(), m_bands.at(b).title.toHtmlEscaped(),
                        SystemStats::formatBytes(point.values.at(b)));
    }
    QToolTip::showText(event->globalPosition().toPoint(), tip, this);
}

void MemoryUsageChart::leaveEvent(QEvent *)
{
    m_hoverIndex = -1;
    QToolTip::hideText();
    update();
}
