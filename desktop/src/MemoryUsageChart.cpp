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

constexpr int kNextHeaderHeight = 40;
constexpr int kNextInset = 12;
constexpr int kNextAxisWidth = 48;
constexpr int kNextTimeRow = 22;
constexpr qreal kNextRadius = 12.0;

bool nextLook(const QWidget *widget)
{
    return widget->property("nextAccent").value<QColor>().isValid();
}

QColor lookColor(const QWidget *widget, const char *name, const QColor &fallback)
{
    const QColor color = widget->property(name).value<QColor>();
    return color.isValid() ? color : fallback;
}

QString axisLabel(qint64 bytes)
{
    const double mib = double(bytes) / (1024.0 * 1024.0);
    if (mib >= 1024.0)
        return QStringLiteral("%1 GB").arg(mib / 1024.0, 0, 'f', mib >= 10240.0 ? 0 : 1);
    if (mib >= 10.0 || mib == 0.0)
        return QStringLiteral("%1 MB").arg(qRound(mib));
    return QStringLiteral("%1 MB").arg(mib, 0, 'f', 1);
}

qint64 timeStepMs(qint64 windowMs, int width)
{
    const int room = qMax(1, width / 84);
    for (qint64 step : {5000LL, 10000LL, 15000LL, 20000LL, 30000LL, 60000LL, 120000LL,
                        300000LL, 600000LL, 900000LL, 1800000LL, 3600000LL}) {
        if (windowMs / step <= room)
            return step;
    }
    return 3600000LL;
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
    if (nextLook(this))
        return rect().adjusted(kNextInset + kNextAxisWidth, kNextHeaderHeight + 16,
                               -(kNextInset + 8), -(kNextInset + kNextTimeRow));
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
    if (nextLook(this)) {
        const QColor fg = lookColor(this, "nextFg", text);
        const QColor muted = lookColor(this, "nextMuted", fg);
        const QColor subtle = lookColor(this, "nextSubtle", muted);
        const QColor border = lookColor(this, "nextBorder", subtle);
        const QColor strong = lookColor(this, "nextBorderStrong", border);
        const QColor surface = lookColor(this, "nextSurface", palette().color(QPalette::Base));
        const QColor window = lookColor(this, "nextBg", surface);
        const QColor accent = lookColor(this, "nextAccent", fg);

        p.setPen(QPen(border, 1));
        p.setBrush(surface);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kNextRadius,
                          kNextRadius);
        p.drawLine(QPointF(1, kNextHeaderHeight + 0.5),
                   QPointF(width() - 1, kNextHeaderHeight + 0.5));

        const QRect head(16, 0, qMax(0, width() - 32), kNextHeaderHeight);
        QFont titleFont = font();
        titleFont.setPixelSize(13);
        titleFont.setWeight(QFont::Medium);
        QFont valueFont = font();
        valueFont.setPixelSize(12);
        const QString value =
            m_points.isEmpty() ? QString() : SystemStats::formatBytes(pointTotal(m_points.last()));
        const int valueWidth = QFontMetrics(valueFont).horizontalAdvance(value);
        p.setFont(valueFont);
        p.setPen(muted);
        p.drawText(head, Qt::AlignRight | Qt::AlignVCenter, value);
        p.setFont(titleFont);
        p.setPen(fg);
        p.drawText(head.adjusted(0, 0, -(valueWidth + 12), 0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(titleFont).elidedText(property("nextTitle").toString(),
                                                      Qt::ElideRight,
                                                      qMax(0, head.width() - valueWidth - 12)));

        const qint64 ceiling = niceCeiling(peakTotal());
        QFont axisFont = font();
        axisFont.setPixelSize(10);
        p.setFont(axisFont);
        for (int i = 0; i <= 4; ++i) {
            const qreal y = plot.bottom() + 0.5 - qreal(plot.height()) * i / 4;
            QPen grid(border, 1);
            if (i > 0)
                grid.setDashPattern({2.0, 4.0});
            p.setPen(grid);
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            p.setPen(subtle);
            p.drawText(QRectF(kNextInset, y - 8, plot.left() - kNextInset - 8, 16),
                       Qt::AlignRight | Qt::AlignVCenter, axisLabel(ceiling * i / 4));
        }

        if (m_points.isEmpty() || m_bands.isEmpty()) {
            p.setFont(valueFont);
            p.setPen(muted);
            p.drawText(plot, Qt::AlignCenter, QStringLiteral("Sampling…"));
            return;
        }

        const qint64 newest = m_points.last().timestampMs;
        const qint64 oldest = newest - m_windowMs;
        const auto xFor = [&](qint64 t) {
            return plot.left() + double(t - oldest) / double(m_windowMs) * plot.width();
        };
        const auto yFor = [&](qint64 v) {
            return plot.bottom() - double(v) / double(ceiling) * plot.height();
        };

        const qint64 step = timeStepMs(m_windowMs, plot.width());
        const QFontMetrics axisMetrics(axisFont);
        p.setPen(subtle);
        for (qint64 t = (oldest / step + 1) * step; t <= newest; t += step) {
            const QString label =
                QDateTime::fromMSecsSinceEpoch(t).toString(QStringLiteral("HH:mm:ss"));
            const int labelWidth = axisMetrics.horizontalAdvance(label);
            const int x = qRound(xFor(t));
            QRect box(x - labelWidth / 2, plot.bottom() + 6, labelWidth, 16);
            if (box.left() < plot.left() - kNextAxisWidth / 2 || box.right() > width() - 4)
                continue;
            p.drawText(box, Qt::AlignCenter, label);
        }

        QPainterPath line;
        for (int i = 0; i < m_points.size(); ++i) {
            const QPointF at(xFor(m_points.at(i).timestampMs), yFor(pointTotal(m_points.at(i))));
            if (i == 0)
                line.moveTo(at);
            else
                line.lineTo(at);
        }
        QPen pen(accent, 2);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);

        if (m_hoverIndex >= 0 && m_hoverIndex < m_points.size()) {
            const Point &point = m_points.at(m_hoverIndex);
            const qreal x = xFor(point.timestampMs);
            p.setPen(QPen(strong, 1));
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            p.setPen(QPen(window, 2));
            p.setBrush(accent);
            p.drawEllipse(QPointF(x, yFor(pointTotal(point))), 4.5, 4.5);
        }
        return;
    }
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
