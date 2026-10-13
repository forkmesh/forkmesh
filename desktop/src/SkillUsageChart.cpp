#include "SkillUsageChart.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QHash>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kDayCount = 30;
constexpr int kMaximumSeries = 10;

const QColor kSeriesColors[] = {
    QColor(QStringLiteral("#79c0ff")),
    QColor(QStringLiteral("#58a6ff")),
    QColor(QStringLiteral("#2f81f7")),
    QColor(QStringLiteral("#1f5ca8")),
    QColor(QStringLiteral("#b7a0ee")),
    QColor(QStringLiteral("#8957e5")),
    QColor(QStringLiteral("#e275ad")),
    QColor(QStringLiteral("#a84d8c")),
    QColor(QStringLiteral("#e3b341")),
    QColor(QStringLiteral("#db6d28")),
};

QString skillKey(const QString &skill)
{
    return skill.trimmed().toCaseFolded();
}

void appendSmoothLine(QPainterPath &path, const QVector<QPointF> &points)
{
    if (points.isEmpty())
        return;
    path.lineTo(points.first());
    for (int index = 1; index < points.size(); ++index) {
        const QPointF previous = points.at(index - 1);
        const QPointF current = points.at(index);
        const qreal middle = (previous.x() + current.x()) / 2.0;
        path.cubicTo(QPointF(middle, previous.y()),
                     QPointF(middle, current.y()), current);
    }
}

} // namespace

SkillUsageChart::SkillUsageChart(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("teammateSkillUsageChart"));
    setMinimumHeight(205);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Skills used over the last 30 days"));
}

void SkillUsageChart::setEvents(QVector<SkillUsageEvent> events,
                                const QDate &through)
{
    m_lastDate = through.isValid() ? through : QDate::currentDate();
    m_firstDate = m_lastDate.addDays(-(kDayCount - 1));
    m_hoverDay = -1;
    m_totalUses = 0;
    m_series.clear();

    struct Candidate {
        QString name;
        QString key;
        int total = 0;
    };
    QHash<QString, Candidate> candidates;
    QVector<SkillUsageEvent> visible;
    visible.reserve(events.size());
    for (SkillUsageEvent &event : events) {
        event.skill = event.skill.trimmed();
        if (event.timestampMs <= 0 || event.skill.isEmpty())
            continue;
        const QDate date =
            QDateTime::fromMSecsSinceEpoch(event.timestampMs).date();
        if (date < m_firstDate || date > m_lastDate)
            continue;
        const QString key = skillKey(event.skill);
        auto candidate = candidates.find(key);
        if (candidate == candidates.end()) {
            Candidate row;
            row.name = event.skill.left(80);
            row.key = key;
            row.total = 1;
            candidates.insert(key, row);
        } else {
            ++candidate->total;
        }
        visible.append(event);
    }

    QVector<Candidate> ranked;
    ranked.reserve(candidates.size());
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it)
        ranked.append(it.value());
    std::sort(ranked.begin(), ranked.end(), [](const Candidate &left,
                                                const Candidate &right) {
        if (left.total != right.total)
            return left.total > right.total;
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });

    const bool hasOther = ranked.size() > kMaximumSeries;
    const int namedCount = hasOther ? kMaximumSeries - 1 : ranked.size();
    QHash<QString, int> seriesByKey;
    for (int index = 0; index < namedCount; ++index) {
        Series series;
        series.name = ranked.at(index).name;
        series.key = ranked.at(index).key;
        series.color = kSeriesColors[index];
        series.days.fill(0, kDayCount);
        seriesByKey.insert(series.key, m_series.size());
        m_series.append(series);
    }
    if (hasOther) {
        Series other;
        other.name = QStringLiteral("Other");
        other.key = QStringLiteral("__other__");
        other.color = kSeriesColors[kMaximumSeries - 1];
        other.days.fill(0, kDayCount);
        m_series.append(other);
    }

    for (const SkillUsageEvent &event : std::as_const(visible)) {
        const int day = m_firstDate.daysTo(
            QDateTime::fromMSecsSinceEpoch(event.timestampMs).date());
        if (day < 0 || day >= kDayCount)
            continue;
        const int seriesIndex = seriesByKey.value(
            skillKey(event.skill), hasOther ? m_series.size() - 1 : -1);
        if (seriesIndex < 0)
            continue;
        ++m_series[seriesIndex].days[day];
        ++m_totalUses;
    }

    const QStringList names = seriesNames();
    setProperty("skillUsageTotal", m_totalUses);
    setProperty("skillUsageSeries", names);
    setAccessibleDescription(
        m_totalUses == 0
            ? QStringLiteral("No skills were used by agents in this period.")
            : QStringLiteral("%1 skill uses across %2 skill%3.")
                  .arg(m_totalUses)
                  .arg(candidates.size())
                  .arg(candidates.size() == 1 ? QString()
                                              : QStringLiteral("s")));
    update();
}

QStringList SkillUsageChart::seriesNames() const
{
    QStringList result;
    result.reserve(m_series.size());
    for (const Series &series : m_series)
        result.append(series.name);
    return result;
}

QVector<int> SkillUsageChart::dailyTotals() const
{
    QVector<int> totals(kDayCount, 0);
    for (const Series &series : m_series)
        for (int day = 0; day < qMin(kDayCount, series.days.size()); ++day)
            totals[day] += series.days.at(day);
    return totals;
}

QSize SkillUsageChart::sizeHint() const
{
    return QSize(960, 235);
}

QSize SkillUsageChart::minimumSizeHint() const
{
    return QSize(360, 205);
}

int SkillUsageChart::legendHeight() const
{
    if (m_series.isEmpty())
        return 10;
    const QFontMetrics metrics(font());
    const int available = qMax(100, width() - 48);
    int x = 0;
    int rows = 1;
    for (const Series &series : m_series) {
        const int itemWidth = qMin(
            available,
            10 + 5 + metrics.horizontalAdvance(series.name) + 18);
        if (x > 0 && x + itemWidth > available) {
            ++rows;
            x = 0;
        }
        x += itemWidth;
    }
    return rows * (metrics.height() + 5) + 7;
}

QRectF SkillUsageChart::plotRect() const
{
    return QRectF(rect()).adjusted(38, 8, -12, -(29 + legendHeight()));
}

int SkillUsageChart::dayAtX(qreal x) const
{
    const QRectF plot = plotRect();
    if (!plot.contains(QPointF(x, plot.center().y())))
        return -1;
    return qBound(0, qRound((x - plot.left()) / plot.width() *
                            (kDayCount - 1)),
                  kDayCount - 1);
}

int SkillUsageChart::niceMaximum(int maximum) const
{
    if (maximum <= 4)
        return qMax(2, ((maximum + 1) / 2) * 2);
    if (maximum <= 10)
        return ((maximum + 1) / 2) * 2;
    if (maximum <= 25)
        return ((maximum + 4) / 5) * 5;
    if (maximum <= 50)
        return ((maximum + 9) / 10) * 10;
    const int magnitude = std::pow(10.0, std::floor(std::log10(maximum)));
    const int step = qMax(10, magnitude / 2);
    return ((maximum + step - 1) / step) * step;
}

void SkillUsageChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPalette colors = palette();
    const QColor text = colors.color(QPalette::Text);
    const QColor muted = colors.color(QPalette::PlaceholderText);
    QColor grid = colors.color(QPalette::Mid);
    grid.setAlpha(95);
    const QRectF plot = plotRect();
    if (plot.width() <= 20 || plot.height() <= 20)
        return;

    const QVector<int> totals = dailyTotals();
    const int rawMaximum = totals.isEmpty()
                               ? 0
                               : *std::max_element(totals.cbegin(), totals.cend());
    const int maximum = niceMaximum(rawMaximum);

    QFont axisFont = font();
    axisFont.setPointSizeF(qMax(8.0, axisFont.pointSizeF() - 1.0));
    painter.setFont(axisFont);
    for (int tick = 0; tick <= 2; ++tick) {
        const qreal y = plot.bottom() - plot.height() * tick / 2.0;
        painter.setPen(QPen(grid, 1, Qt::DotLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(muted);
        painter.drawText(QRectF(0, y - 9, plot.left() - 7, 18),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(maximum * tick / 2));
    }

    auto xForDay = [&plot](int day) {
        return plot.left() + plot.width() * day / (kDayCount - 1.0);
    };
    auto yForCount = [&plot, maximum](int count) {
        return plot.bottom() - plot.height() * count / qreal(maximum);
    };

    QVector<int> cumulative(kDayCount, 0);
    painter.save();
    painter.setClipRect(plot.adjusted(-1, -1, 1, 1));
    for (const Series &series : m_series) {
        QVector<QPointF> top;
        QVector<QPointF> bottom;
        top.reserve(kDayCount);
        bottom.reserve(kDayCount);
        bool hasValue = false;
        for (int day = 0; day < kDayCount; ++day) {
            const int base = cumulative.at(day);
            const int value = series.days.at(day);
            bottom.append(QPointF(xForDay(day), yForCount(base)));
            top.append(QPointF(xForDay(day), yForCount(base + value)));
            cumulative[day] += value;
            hasValue |= value > 0;
        }
        if (!hasValue)
            continue;
        QPainterPath area;
        area.moveTo(top.first());
        appendSmoothLine(area, top);
        QVector<QPointF> reversedBottom = bottom;
        std::reverse(reversedBottom.begin(), reversedBottom.end());
        appendSmoothLine(area, reversedBottom);
        area.closeSubpath();
        QColor fill = series.color;
        fill.setAlpha(215);
        painter.setPen(QPen(series.color, 1.1));
        painter.setBrush(fill);
        painter.drawPath(area);
    }
    painter.restore();

    const int labelDays[] = {0, 7, 14, 21, 29};
    painter.setPen(muted);
    for (int day : labelDays) {
        const QString label = m_firstDate.addDays(day).toString(
            QStringLiteral("MMM d"));
        const qreal x = xForDay(day);
        QRectF labelRect(x - 42, plot.bottom() + 7, 84, 18);
        Qt::Alignment alignment = Qt::AlignHCenter;
        if (day == 0) {
            labelRect.moveLeft(x);
            alignment = Qt::AlignLeft;
        } else if (day == kDayCount - 1) {
            labelRect.moveRight(x);
            alignment = Qt::AlignRight;
        }
        painter.drawText(labelRect, alignment | Qt::AlignTop, label);
    }

    if (m_totalUses == 0) {
        painter.setPen(muted);
        painter.drawText(plot, Qt::AlignCenter,
                         QStringLiteral("No agent skill use in the last 30 days"));
    }

    drawLegend(painter, plot);
    if (m_hoverDay >= 0 && m_totalUses > 0)
        drawHover(painter, plot);
}

void SkillUsageChart::drawLegend(QPainter &painter, const QRectF &plot) const
{
    if (m_series.isEmpty())
        return;
    const QFontMetrics metrics(painter.font());
    const qreal left = plot.left();
    const qreal right = plot.right();
    qreal x = left;
    qreal y = plot.bottom() + 31;
    for (const Series &series : m_series) {
        const int itemWidth = qMin(
            int(right - left),
            10 + 5 + metrics.horizontalAdvance(series.name) + 18);
        if (x > left && x + itemWidth > right) {
            x = left;
            y += metrics.height() + 5;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(series.color);
        painter.drawEllipse(QPointF(x + 4, y + metrics.height() / 2.0), 4, 4);
        painter.setPen(palette().color(QPalette::PlaceholderText));
        const QString label = metrics.elidedText(
            series.name, Qt::ElideRight, qMax(20, itemWidth - 13));
        painter.drawText(QRectF(x + 13, y, itemWidth - 13, metrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, label);
        x += itemWidth;
    }
}

void SkillUsageChart::drawHover(QPainter &painter, const QRectF &plot) const
{
    if (m_hoverDay < 0 || m_hoverDay >= kDayCount)
        return;
    const qreal x = plot.left() +
                    plot.width() * m_hoverDay / (kDayCount - 1.0);
    QColor accent = palette().color(QPalette::Highlight);
    painter.setPen(QPen(accent, 1, Qt::DashLine));
    painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));

    QStringList parts;
    int total = 0;
    for (const Series &series : m_series) {
        const int count = series.days.value(m_hoverDay);
        if (count <= 0)
            continue;
        total += count;
        parts.append(QStringLiteral("%1 %2").arg(series.name).arg(count));
    }
    QString detail = parts.join(QStringLiteral(" · "));
    if (detail.isEmpty())
        detail = QStringLiteral("No skills used");
    const QString heading = QStringLiteral("%1 · %2 use%3")
                                .arg(m_firstDate.addDays(m_hoverDay)
                                         .toString(QStringLiteral("MMM d")))
                                .arg(total)
                                .arg(total == 1 ? QString()
                                                : QStringLiteral("s"));
    const QFontMetrics metrics(painter.font());
    const int maximumWidth = qMax(120, int(plot.width() - 10));
    const int wanted = qMax(metrics.horizontalAdvance(heading),
                            metrics.horizontalAdvance(detail)) + 20;
    const int width = qMin(maximumWidth, wanted);
    const int height = metrics.height() * 2 + 14;
    const qreal boxX = qBound(plot.left(), x - width / 2.0,
                              plot.right() - width);
    QRectF box(boxX, plot.top() + 8, width, height);
    QColor background = palette().color(QPalette::Window);
    background.setAlpha(245);
    QColor border = palette().color(QPalette::Mid);
    painter.setPen(border);
    painter.setBrush(background);
    painter.drawRoundedRect(box, 5, 5);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(box.adjusted(9, 5, -9, -metrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     metrics.elidedText(heading, Qt::ElideRight, width - 18));
    painter.setPen(palette().color(QPalette::PlaceholderText));
    painter.drawText(box.adjusted(9, metrics.height() + 4, -9, -5),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     metrics.elidedText(detail, Qt::ElideRight, width - 18));
}

void SkillUsageChart::mouseMoveEvent(QMouseEvent *event)
{
    const int day = plotRect().contains(event->position())
                        ? dayAtX(event->position().x())
                        : -1;
    if (day != m_hoverDay) {
        m_hoverDay = day;
        update();
    }
}

void SkillUsageChart::leaveEvent(QEvent *)
{
    if (m_hoverDay >= 0) {
        m_hoverDay = -1;
        update();
    }
}
