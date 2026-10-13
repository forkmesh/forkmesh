#include "LogTimelineChart.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

LogTimelineChart::LogTimelineChart(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("logTimelineChart"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Log activity chart"));
    setAccessibleDescription(
        QStringLiteral("A compact log activity rail. Drag across the rail or "
                       "use the mouse wheel to zoom; double-click to reset."));
}

void LogTimelineChart::setEntries(QVector<LogTimelineEntry> entries)
{
    const auto before = [](const LogTimelineEntry &left,
                           const LogTimelineEntry &right) {
        return left.timestampMs < right.timestampMs;
    };
    if (!std::is_sorted(entries.cbegin(), entries.cend(), before))
        std::sort(entries.begin(), entries.end(), before);
    m_entries = std::move(entries);
#ifdef FORKMESH_WINDOW_TESTS
    ++m_entriesRevision;
#endif
    update();
}

void LogTimelineChart::setHistoryBuckets(QVector<LogTimelineBucket> buckets)
{
    m_historyBuckets = std::move(buckets);
#ifdef FORKMESH_WINDOW_TESTS
    ++m_entriesRevision;
#endif
    update();
}

void LogTimelineChart::appendEntry(const LogTimelineEntry &entry)
{
    if (entry.timestampMs <= 0)
        return;
    const auto position = std::upper_bound(
        m_entries.begin(), m_entries.end(), entry.timestampMs,
        [](qint64 timestamp, const LogTimelineEntry &candidate) {
            return timestamp < candidate.timestampMs;
        });
    m_entries.insert(position, entry);
    scheduleStreamingViewChanged();
}

void LogTimelineChart::removeEntry(qint64 timestampMs,
                                   const QString &category)
{
    if (timestampMs <= 0)
        return;
    auto it = std::lower_bound(
        m_entries.begin(), m_entries.end(), timestampMs,
        [](const LogTimelineEntry &candidate, qint64 timestamp) {
            return candidate.timestampMs < timestamp;
        });
    for (; it != m_entries.end() && it->timestampMs == timestampMs; ++it) {
        if (it->category != category)
            continue;
        m_entries.erase(it);
        update();
        return;
    }
}

void LogTimelineChart::setSeries(QVector<LogTimelineSeries> series,
                                 bool restrictToSeries, const QColor &accent)
{
    const QColor resolvedAccent =
        accent.isValid() ? accent : QColor(QStringLiteral("#58a6ff"));
    const bool sameSeries =
        series.size() == m_series.size() &&
        std::equal(series.cbegin(), series.cend(), m_series.cbegin(),
                   [](const LogTimelineSeries &left,
                      const LogTimelineSeries &right) {
                       return left.category == right.category &&
                              left.color == right.color;
                   });
    if (sameSeries && restrictToSeries == m_restrictToSeries &&
        m_accent == resolvedAccent)
        return;
    m_series = std::move(series);
    m_restrictToSeries = restrictToSeries;
    m_accent = resolvedAccent;
    m_seriesIndex.clear();
    m_seriesIndex.reserve(m_series.size());
    for (int row = 0; row < m_series.size(); ++row)
        m_seriesIndex.insert(m_series.at(row).category, row);
    update();
    notifyViewChanged();
}

void LogTimelineChart::setCategoryFilter(const QString &category,
                                         const QColor &accent)
{
    if (category.isEmpty()) {
        setSeries({}, false, accent);
        return;
    }
    setSeries({{category, accent.isValid() ? accent : m_accent}}, true, accent);
}

void LogTimelineChart::setRange(qint64 fromMs, qint64 toMs)
{
    if (toMs <= fromMs)
        toMs = fromMs + 60 * 1000;
    if (fromMs == m_rangeFromMs && toMs == m_rangeToMs &&
        fromMs == m_viewFromMs && toMs == m_viewToMs && !m_dragging)
        return;
    m_rangeFromMs = fromMs;
    m_rangeToMs = toMs;
    m_viewFromMs = fromMs;
    m_viewToMs = toMs;
    m_dragging = false;
    // Called once per appended line to slide a live preset window along with
    // the clock, so it takes the coalesced path too. The explicit preset /
    // custom-range handlers refresh the summary themselves right after.
    scheduleStreamingViewChanged();
}

void LogTimelineChart::resetZoom()
{
    if (!isZoomed())
        return;
    m_viewFromMs = m_rangeFromMs;
    m_viewToMs = m_rangeToMs;
    update();
    notifyViewChanged();
}

int LogTimelineChart::visibleEntryCount() const
{
    const auto first = std::lower_bound(
        m_entries.cbegin(), m_entries.cend(), m_viewFromMs,
        [](const LogTimelineEntry &entry, qint64 timestamp) {
            return entry.timestampMs < timestamp;
        });
    const auto last = std::upper_bound(
        first, m_entries.cend(), m_viewToMs,
        [](qint64 timestamp, const LogTimelineEntry &entry) {
            return timestamp < entry.timestampMs;
        });
    const int exact =
        !m_restrictToSeries
            ? int(std::distance(first, last))
            : int(std::count_if(first, last,
                                [this](const LogTimelineEntry &entry) {
                                    return seriesRowFor(entry.category) >= 0;
                                }));
    // One column spanning the whole view: the return value is the clipped
    // history total, which is all the summary line needs.
    QVector<int> whole(stackRowCount(), 0);
    return exact + addHistoryBuckets(whole, 1);
}

bool LogTimelineChart::isZoomed() const
{
    return m_viewFromMs != m_rangeFromMs || m_viewToMs != m_rangeToMs;
}

QSize LogTimelineChart::sizeHint() const
{
    return QSize(900, 68);
}

QSize LogTimelineChart::minimumSizeHint() const
{
    return QSize(320, 48);
}

QRectF LogTimelineChart::plotRect() const
{
    return QRectF(rect()).adjusted(8.0, 8.0, -8.0, -8.0);
}

qint64 LogTimelineChart::timeAtX(qreal x) const
{
    const QRectF plot = plotRect();
    const qreal ratio = qBound(0.0, (x - plot.left()) / plot.width(), 1.0);
    return m_viewFromMs + qRound64(ratio * (m_viewToMs - m_viewFromMs));
}

bool LogTimelineChart::entryIsVisible(const LogTimelineEntry &entry) const
{
    return entry.timestampMs >= m_viewFromMs && entry.timestampMs <= m_viewToMs &&
           seriesRowFor(entry.category) >= 0;
}

int LogTimelineChart::seriesRowFor(const QString &category) const
{
    if (m_series.isEmpty())
        return m_restrictToSeries ? -1 : 0;
    const auto it = m_seriesIndex.constFind(category);
    if (it != m_seriesIndex.cend())
        return it.value();
    // Everything the reader did not pick: hidden while a selection filters the
    // chart, otherwise the neutral remainder band on top of the stack.
    return m_restrictToSeries ? -1 : m_series.size();
}

int LogTimelineChart::stackRowCount() const
{
    if (m_series.isEmpty())
        return 1;
    return m_series.size() + (m_restrictToSeries ? 0 : 1);
}

QColor LogTimelineChart::rowColor(int row) const
{
    return row >= 0 && row < m_series.size() ? m_series.at(row).color : m_accent;
}

int LogTimelineChart::addHistoryBuckets(QVector<int> &cells,
                                        int columnCount) const
{
    if (m_historyBuckets.isEmpty() || cells.isEmpty() || columnCount <= 0)
        return 0;
    const qint64 span = qMax<qint64>(1, m_viewToMs - m_viewFromMs);
    const int rows = cells.size() / columnCount;
    if (rows <= 0)
        return 0;
    int total = 0;
    for (const LogTimelineBucket &bucket : m_historyBuckets) {
        if (bucket.count <= 0 || bucket.spanMs <= 0)
            continue;
        const int row = qMin(seriesRowFor(bucket.category), rows - 1);
        if (row < 0)
            continue;
        const qint64 from = qMax(bucket.startMs, m_viewFromMs);
        const qint64 to = qMin(bucket.startMs + bucket.spanMs, m_viewToMs);
        if (to <= from)
            continue;
        // A bucket half off the edge of the view contributes half its lines.
        const bool whole = bucket.startMs >= m_viewFromMs &&
                           bucket.startMs + bucket.spanMs <= m_viewToMs;
        const int visible =
            whole ? bucket.count
                  : int((qint64(bucket.count) * (to - from) + bucket.spanMs / 2) /
                        bucket.spanMs);
        if (visible <= 0)
            continue;
        const qint64 lastIndex = columnCount - 1;
        const int firstColumn = int(qBound<qint64>(
            0, (from - m_viewFromMs) * columnCount / span, lastIndex));
        const int lastColumn = int(qBound<qint64>(
            0, (to - 1 - m_viewFromMs) * columnCount / span, lastIndex));
        if (firstColumn == lastColumn) {
            cells[firstColumn * rows + row] += visible;
        } else {
            // Zoomed in far enough that one bucket covers several columns:
            // spread it rather than spiking a single one.
            const int used = lastColumn - firstColumn + 1;
            int remaining = visible;
            for (int column = firstColumn; column <= lastColumn; ++column) {
                const int share =
                    column == lastColumn ? remaining : visible / used;
                cells[column * rows + row] += share;
                remaining -= share;
            }
        }
        total += visible;
    }
    return total;
}

void LogTimelineChart::notifyViewChanged()
{
    if (viewChanged)
        viewChanged();
}

void LogTimelineChart::scheduleStreamingViewChanged()
{
    if (m_streamNotifyPending)
        return;
    m_streamNotifyPending = true;
    constexpr int kStreamCoalesceMs = 200;
    QTimer::singleShot(kStreamCoalesceMs, this, [this] {
        m_streamNotifyPending = false;
        update();
        notifyViewChanged();
    });
}

void LogTimelineChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPalette colors = palette();
    const QColor canvas = colors.color(QPalette::Base);
    const QColor text = colors.color(QPalette::Text);
    const QColor muted = colors.color(QPalette::PlaceholderText);
    QColor border = colors.color(QPalette::Mid);
    border.setAlpha(150);
    QColor grid = border;
    grid.setAlpha(80);

    painter.setPen(QPen(border, 1));
    painter.setBrush(canvas);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            8.0, 8.0);

    const QRectF plot = plotRect();
    if (plot.width() <= 1 || plot.height() <= 1)
        return;

    const int bucketCount = qBound(16, int(plot.width() / 7.0), 180);
    const int rows = stackRowCount();
    // (column, stack row) counts, row-major inside each column, beside the
    // per-column totals the bar height and the tooltip are scaled against.
    QVector<int> cells(bucketCount * rows, 0);
    QVector<int> buckets(bucketCount, 0);
    const qint64 span = qMax<qint64>(1, m_viewToMs - m_viewFromMs);
    const auto first = std::lower_bound(
        m_entries.cbegin(), m_entries.cend(), m_viewFromMs,
        [](const LogTimelineEntry &entry, qint64 timestamp) {
            return entry.timestampMs < timestamp;
        });
    const auto last = std::upper_bound(
        first, m_entries.cend(), m_viewToMs,
        [](qint64 timestamp, const LogTimelineEntry &entry) {
            return timestamp < entry.timestampMs;
        });
    int total = 0;
    for (auto it = first; it != last; ++it) {
        const LogTimelineEntry &entry = *it;
        const int row = seriesRowFor(entry.category);
        if (row < 0)
            continue;
        const int bucket = qBound(
            0, int((entry.timestampMs - m_viewFromMs) * bucketCount / span),
            bucketCount - 1);
        ++cells[bucket * rows + qMin(row, rows - 1)];
        ++total;
    }
    // Everything older than the in-memory buffer arrives pre-counted.
    total += addHistoryBuckets(cells, bucketCount);
    for (int index = 0; index < bucketCount; ++index) {
        int column = 0;
        for (int row = 0; row < rows; ++row)
            column += cells.at(index * rows + row);
        buckets[index] = column;
    }
    const int maximum = qMax(1, *std::max_element(buckets.cbegin(), buckets.cend()));
    painter.setPen(QPen(grid, 1));
    painter.drawLine(QPointF(plot.left(), plot.bottom()),
                     QPointF(plot.right(), plot.bottom()));

    const qreal slot = plot.width() / bucketCount;
    const qreal lineWidth = qBound(1.0, slot - 2.0, 3.0);
    QColor emptyTick = m_accent;
    emptyTick.setAlpha(38);
    const QPen emptyPen(emptyTick, lineWidth, Qt::SolidLine, Qt::FlatCap);
    painter.save();
    painter.setClipRect(plot);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    for (int index = 0; index < bucketCount; ++index) {
        const qreal x = plot.left() + (index + 0.5) * slot;
        const int count = buckets.at(index);
        if (count <= 0) {
            painter.setPen(emptyPen);
            painter.drawLine(QPointF(x, plot.bottom()),
                             QPointF(x, plot.bottom() - 1.5));
            painter.setPen(Qt::NoPen);
            continue;
        }
        // One bar, stacked bottom-up in each series' own colour. Band edges
        // come off the running total so rounding never opens a seam between
        // two bands, and a band that would round away still keeps a pixel.
        const qreal height = qMax(2.0, qreal(count) / maximum * plot.height());
        const qreal scale = height / count;
        qreal bottom = plot.bottom();
        int stacked = 0;
        for (int row = 0; row < rows; ++row) {
            const int share = cells.at(index * rows + row);
            if (share <= 0)
                continue;
            stacked += share;
            const qreal top =
                qMin(plot.bottom() - stacked * scale, bottom - 1.0);
            painter.setBrush(rowColor(row));
            painter.drawRect(QRectF(x - lineWidth / 2.0, top, lineWidth,
                                    bottom - top));
            bottom = top;
        }
    }
    painter.restore();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (total == 0) {
        QFont railFont = font();
        railFont.setPointSizeF(qMax(8.0, railFont.pointSizeF() - 1.0));
        painter.setFont(railFont);
        painter.setPen(muted);
        const QString what =
            !m_restrictToSeries || m_series.isEmpty()
                ? QStringLiteral("No logs in this timeframe")
            : m_series.size() == 1
                ? QStringLiteral("No %1 logs in this timeframe")
                      .arg(m_series.first().category)
                : QStringLiteral("No logs in this timeframe for the %1 "
                                 "selected categories")
                      .arg(m_series.size());
        painter.drawText(plot, Qt::AlignCenter, what);
    }

    if (m_pointerInside && !m_dragging && plot.contains(m_pointerPosition)) {
        const int bucket = qBound(
            0, int((m_pointerPosition.x() - plot.left()) / plot.width() *
                   bucketCount),
            bucketCount - 1);
        const qreal x = plot.left() +
                        (bucket + 0.5) * plot.width() / bucketCount;
        painter.setPen(QPen(m_accent, 1, Qt::DashLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));

        const qint64 bucketFrom =
            m_viewFromMs + bucket * span / bucketCount;
        const qint64 bucketTo =
            m_viewFromMs + (bucket + 1) * span / bucketCount;
        const QString tooltip =
            QStringLiteral("%1 log%2  ·  %3 – %4")
                .arg(buckets.at(bucket))
                .arg(buckets.at(bucket) == 1 ? QString() : QStringLiteral("s"))
                .arg(QDateTime::fromMSecsSinceEpoch(bucketFrom)
                         .toString(QStringLiteral("MMM d, h:mm AP")))
                .arg(QDateTime::fromMSecsSinceEpoch(bucketTo)
                         .toString(QStringLiteral("h:mm AP")));
        const QFontMetrics metrics(font());
        QStringList tooltipLines{tooltip};
        // On the All chart these values let the reader see what made a tall
        // stack rather than mistaking the neutral total for one category.
        if (m_series.size() > 1) {
            QStringList composition;
            for (int row = 0; row < rows; ++row) {
                const int share = cells.at(bucket * rows + row);
                if (share <= 0)
                    continue;
                const QString category =
                    row < m_series.size() ? m_series.at(row).category
                                          : QStringLiteral("Other");
                composition.append(QStringLiteral("%1 %2").arg(category).arg(share));
            }
            if (!composition.isEmpty())
                tooltipLines.append(composition.join(QStringLiteral(" · ")));
        }
        const int maximumTipWidth = qMax(80, int(plot.width() - 8));
        int wantedTipWidth = 0;
        for (const QString &line : std::as_const(tooltipLines))
            wantedTipWidth = qMax(wantedTipWidth, metrics.horizontalAdvance(line));
        const int tipWidth = qMin(wantedTipWidth + 18, maximumTipWidth);
        const QSize tipSize(tipWidth, metrics.height() * tooltipLines.size() + 12);
        const qreal tipX = qBound(plot.left(), x - tipSize.width() / 2.0,
                                  plot.right() - tipSize.width());
        QRectF tipRect(tipX, plot.top() + 8, tipSize.width(), tipSize.height());
        QColor tipBackground = colors.color(QPalette::Window);
        painter.setPen(border);
        painter.setBrush(tipBackground);
        painter.drawRoundedRect(tipRect, 5, 5);
        painter.setPen(text);
        for (int line = 0; line < tooltipLines.size(); ++line) {
            const QString visibleLine = metrics.elidedText(
                tooltipLines.at(line), Qt::ElideRight, qMax(40, tipWidth - 18));
            const QRectF lineRect(tipRect.left() + 9,
                                  tipRect.top() + 6 + line * metrics.height(),
                                  tipRect.width() - 18, metrics.height());
            painter.drawText(lineRect, Qt::AlignCenter, visibleLine);
        }
    }

    if (m_dragging) {
        const qreal left = qBound(plot.left(), qMin(m_dragStartX, m_dragCurrentX),
                                  plot.right());
        const qreal right = qBound(plot.left(), qMax(m_dragStartX, m_dragCurrentX),
                                   plot.right());
        QRectF selection(QPointF(left, plot.top()),
                         QPointF(right, plot.bottom()));
        QColor selectionFill = m_accent;
        selectionFill.setAlpha(45);
        painter.setBrush(selectionFill);
        painter.setPen(QPen(m_accent, 1));
        painter.drawRect(selection);
    }
}

void LogTimelineChart::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && plotRect().contains(event->position())) {
        m_dragStartX = event->position().x();
        m_dragCurrentX = m_dragStartX;
        m_dragging = true;
        setCursor(Qt::CrossCursor);
        update();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void LogTimelineChart::mouseMoveEvent(QMouseEvent *event)
{
    m_pointerInside = true;
    m_pointerPosition = event->position();
    if (m_dragging)
        m_dragCurrentX = event->position().x();
    update();
    QWidget::mouseMoveEvent(event);
}

void LogTimelineChart::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragCurrentX = event->position().x();
    m_dragging = false;
    unsetCursor();
    if (qAbs(m_dragCurrentX - m_dragStartX) >= 10.0) {
        const qint64 from = timeAtX(qMin(m_dragStartX, m_dragCurrentX));
        const qint64 to = timeAtX(qMax(m_dragStartX, m_dragCurrentX));
        if (to - from >= 60 * 1000) {
            m_viewFromMs = from;
            m_viewToMs = to;
            notifyViewChanged();
        }
    }
    update();
    event->accept();
}

void LogTimelineChart::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && plotRect().contains(event->position())) {
        resetZoom();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void LogTimelineChart::leaveEvent(QEvent *event)
{
    m_pointerInside = false;
    update();
    QWidget::leaveEvent(event);
}

void LogTimelineChart::wheelEvent(QWheelEvent *event)
{
    const QRectF plot = plotRect();
    if (!plot.contains(event->position()) || event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const qint64 oldSpan = qMax<qint64>(1, m_viewToMs - m_viewFromMs);
    const qreal factor = event->angleDelta().y() > 0 ? 0.72 : 1.38;
    const qint64 newSpan = qBound<qint64>(60 * 1000,
                                         qRound64(oldSpan * factor),
                                         m_rangeToMs - m_rangeFromMs);
    const qreal anchorRatio =
        qBound(0.0, (event->position().x() - plot.left()) / plot.width(), 1.0);
    const qint64 anchor = timeAtX(event->position().x());
    qint64 from = anchor - qRound64(anchorRatio * newSpan);
    qint64 to = from + newSpan;
    if (from < m_rangeFromMs) {
        from = m_rangeFromMs;
        to = from + newSpan;
    }
    if (to > m_rangeToMs) {
        to = m_rangeToMs;
        from = to - newSpan;
    }
    m_viewFromMs = from;
    m_viewToMs = to;
    update();
    notifyViewChanged();
    event->accept();
}
