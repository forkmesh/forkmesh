#include "QueryInspector.h"

#include "FleetVisualizer.h"
#include "FontSizing.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace forkmesh {

namespace {

constexpr qreal kMargin = 12.0;
constexpr qreal kGap = 10.0;
constexpr qreal kHeaderHeight = 56.0;
constexpr qreal kRowHeight = 18.0;
constexpr int kBarRows = 6;
constexpr qreal kBarsHeight = 22.0 + kBarRows * kRowHeight;
constexpr qreal kHistogramHeight = 150.0;
constexpr int kShapeRows = 14;
constexpr int kRecentRows = 12;
constexpr qreal kTableRow = 20.0;
constexpr qreal kShapesHeight = 22.0 + kTableRow + kShapeRows * kTableRow;
constexpr qreal kRecentHeight = 22.0 + kTableRow + kRecentRows * kTableRow;
constexpr int kRepaintMs = 250;

const QList<double> &bucketEdges()
{
    static const QList<double> edges{1.0, 5.0, 20.0, 100.0, 500.0};
    return edges;
}

QStringList bucketLabels()
{
    return {QStringLiteral("<1 ms"), QStringLiteral("1-5"), QStringLiteral("5-20"),
            QStringLiteral("20-100"), QStringLiteral("100-500"),
            QStringLiteral("500+ ms")};
}

int bucketOf(double ms)
{
    const QList<double> &edges = bucketEdges();
    for (int i = 0; i < edges.size(); ++i)
        if (ms < edges.at(i))
            return i;
    return edges.size();
}

QString number(qint64 value)
{
    return QLocale().toString(value);
}

QString msText(double ms)
{
    if (ms < 0.0)
        return QStringLiteral("-");
    if (ms >= 1000.0)
        return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 2);
    if (ms >= 100.0)
        return QStringLiteral("%1 ms").arg(qRound(ms));
    return QStringLiteral("%1 ms").arg(ms, 0, 'f', 1);
}

QString codeLabel(const corelogs::StatementLine &statement)
{
    if (statement.originFile.isEmpty())
        return QString();
    if (statement.originFunction.isEmpty())
        return statement.originFile;
    return statement.originFile + QStringLiteral(" · ") + statement.originFunction;
}

QColor kindColour(const QString &kind, bool dark)
{
    if (kind == QLatin1String("request"))
        return FleetVisualizer::verbColour(QStringLiteral("SELECT"), dark);
    if (kind == QLatin1String("cron"))
        return FleetVisualizer::verbColour(QStringLiteral("UPDATE"), dark);
    if (kind == QLatin1String("queue"))
        return FleetVisualizer::verbColour(QStringLiteral("INSERT"), dark);
    if (kind == QLatin1String("alarm"))
        return FleetVisualizer::verbColour(QStringLiteral("OTHER"), dark);
    return FleetVisualizer::verbColour(QStringLiteral("DELETE"), dark);
}

} // namespace

QueryInspector::QueryInspector(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("queryInspector"));
    setMouseTracking(true);
    m_latencies.reserve(kLatencySamples);
    m_repaint = new QTimer(this);
    m_repaint->setSingleShot(true);
    m_repaint->setInterval(kRepaintMs);
    connect(m_repaint, &QTimer::timeout, this, [this] {
        if (m_dirty && isVisible()) {
            m_dirty = false;
            update();
        }
    });
    setMinimumHeight(sizeHint().height());
}

QSize QueryInspector::sizeHint() const
{
    const qreal height = 2 * kMargin + kHeaderHeight + kBarsHeight +
                         kHistogramHeight + kShapesHeight + kRecentHeight +
                         4 * kGap;
    return QSize(900, int(std::ceil(height)));
}

QSize QueryInspector::minimumSizeHint() const
{
    return QSize(560, sizeHint().height());
}

void QueryInspector::bumpCapped(QHash<QString, qint64> &counts,
                                const QString &key, qint64 by, int cap)
{
    auto it = counts.find(key);
    if (it != counts.end()) {
        *it += by;
        return;
    }
    if (counts.size() >= cap) {
        auto smallest = counts.begin();
        for (auto scan = counts.begin(); scan != counts.end(); ++scan)
            if (scan.value() < smallest.value())
                smallest = scan;
        counts.erase(smallest);
    }
    counts.insert(key, by);
}

void QueryInspector::trimShapes()
{
    while (m_shapes.size() > kMaxShapes) {
        auto smallest = m_shapes.begin();
        for (auto scan = m_shapes.begin(); scan != m_shapes.end(); ++scan)
            if (scan->count < smallest->count ||
                (scan->count == smallest->count && scan->lastMs < smallest->lastMs))
                smallest = scan;
        m_shapes.erase(smallest);
    }
}

void QueryInspector::noteStatement(const QString &node,
                                   const corelogs::StatementLine &statement,
                                   qint64 nowMs)
{
    if (!statement.valid)
        return;
    const QString key = corelogs::statementKey(statement.sql);
    const corelogs::StatementShape shape = corelogs::statementShape(statement.sql);
    const QString group = FleetVisualizer::verbGroup(shape.verb);
    const QString table =
        shape.table.isEmpty() ? QStringLiteral("(no table)") : shape.table;
    const QString code = codeLabel(statement);
    const corelogs::CallerShape caller = corelogs::callerShape(statement.caller);

    auto it = m_shapes.find(key);
    if (it == m_shapes.end()) {
        Shape fresh;
        fresh.key = key;
        fresh.sql = corelogs::statementText(statement.sql);
        fresh.verb = shape.verb.isEmpty() ? QStringLiteral("?") : shape.verb;
        fresh.group = group;
        fresh.table = table;
        fresh.args = corelogs::statementArgCount(statement.sql);
        it = m_shapes.insert(key, fresh);
    }
    it->kind = statement.kind;
    if (!code.isEmpty())
        it->code = code;
    ++it->count;
    it->lastMs = nowMs;
    bumpCapped(it->callers, caller.label, 1, 64);
    ++it->perNode[node];
    trimShapes();

    ++m_total;
    bumpCapped(m_routes, caller.label, 1, kMaxNames);
    m_routeKinds.insert(caller.label, caller.kind);
    if (m_routeKinds.size() > 2 * kMaxNames)
        m_routeKinds.clear();
    if (!code.isEmpty())
        bumpCapped(m_code, code, 1, kMaxNames);
    bumpCapped(m_kinds, statement.kind.isEmpty() ? QStringLiteral("query")
                                                 : statement.kind,
               1, 64);
    if (caller.kind == QLatin1String("cron") ||
        caller.kind == QLatin1String("queue") ||
        caller.kind == QLatin1String("alarm"))
        ++m_background;
    else if (caller.user.isEmpty())
        ++m_anonymous;
    else
        bumpCapped(m_users, caller.user, 1, kMaxNames);
    bumpCapped(m_tableCalls, table, 1, kMaxNames);

    Recent recent;
    recent.atMs = nowMs;
    recent.node = node;
    recent.key = key;
    recent.group = group;
    recent.verb = it->verb;
    recent.table = table;
    recent.kind = statement.kind;
    recent.code = code;
    recent.caller = statement.caller;
    recent.sql = statement.sql;
    m_recent.append(recent);
    if (m_recent.size() > kRecent)
        m_recent.remove(0, m_recent.size() - kRecent);
    scheduleUpdate();
}

void QueryInspector::noteDone(const QString &node,
                              const corelogs::StatementDone &done, qint64 nowMs)
{
    Q_UNUSED(nowMs);
    if (!done.valid)
        return;
    auto it = m_shapes.find(done.key);
    if (it == m_shapes.end()) {
        ++m_unmatchedDone;
        return;
    }
    ++it->timed;
    it->totalMs += done.ms;
    it->maxMs = qMax(it->maxMs, done.ms);
    it->rows += done.rows;
    it->changes += done.changes;
    if (done.failed)
        ++it->failed;
    if (done.served == QLatin1String("local"))
        ++it->local;

    ++m_timed;
    if (done.failed)
        ++m_failed;
    if (done.served == QLatin1String("local"))
        ++m_local;
    m_maxMs = qMax(m_maxMs, done.ms);
    if (m_latencies.size() < kLatencySamples) {
        m_latencies.append(done.ms);
    } else {
        m_latencies[m_latencyHead] = done.ms;
        m_latencyHead = (m_latencyHead + 1) % kLatencySamples;
    }
    QVector<qint64> &buckets = m_histogram[it->group];
    if (buckets.isEmpty())
        buckets.fill(0, bucketEdges().size() + 1);
    ++buckets[bucketOf(done.ms)];
    if (m_tableMs.contains(it->table) || m_tableMs.size() < kMaxNames)
        m_tableMs[it->table] += done.ms;

    for (int i = m_recent.size() - 1; i >= 0; --i) {
        Recent &recent = m_recent[i];
        if (recent.ms < 0.0 && recent.key == done.key && recent.node == node) {
            recent.ms = done.ms;
            recent.rows = done.rows;
            recent.changes = done.changes;
            recent.served = done.served;
            recent.failed = done.failed;
            break;
        }
    }
    if (!m_sortChosen && m_sort == SortColumn::Calls)
        m_sort = SortColumn::TotalTime;
    scheduleUpdate();
}

void QueryInspector::setSortColumn(SortColumn column)
{
    m_sort = column;
    m_sortChosen = true;
    update();
}

double QueryInspector::percentileMs(double share) const
{
    if (m_latencies.isEmpty())
        return -1.0;
    QVector<double> sorted = m_latencies;
    std::sort(sorted.begin(), sorted.end());
    const int index =
        qBound(0, int(std::ceil(share * sorted.size())) - 1, int(sorted.size()) - 1);
    return sorted.at(index);
}

double QueryInspector::recentMs(int newestIndex) const
{
    const int index = m_recent.size() - 1 - newestIndex;
    return index >= 0 && index < m_recent.size() ? m_recent.at(index).ms : -1.0;
}

double QueryInspector::sortValue(const Shape &shape) const
{
    switch (m_sort) {
    case SortColumn::TotalTime:
        return shape.totalMs;
    case SortColumn::AverageTime:
        return shape.timed > 0 ? shape.totalMs / shape.timed : -1.0;
    case SortColumn::MaxTime:
        return shape.maxMs;
    case SortColumn::Rows:
        return double(shape.rows + shape.changes);
    case SortColumn::Calls:
        break;
    }
    return double(shape.count);
}

QStringList QueryInspector::shapeOrder() const
{
    QList<const Shape *> shapes;
    for (const Shape &shape : m_shapes)
        shapes.append(&shape);
    std::sort(shapes.begin(), shapes.end(), [this](const Shape *a, const Shape *b) {
        const double av = sortValue(*a);
        const double bv = sortValue(*b);
        if (av != bv)
            return av > bv;
        if (a->count != b->count)
            return a->count > b->count;
        return a->key < b->key;
    });
    QStringList keys;
    for (const Shape *shape : std::as_const(shapes))
        keys.append(shape->key);
    return keys;
}

void QueryInspector::scheduleUpdate()
{
    m_dirty = true;
    if (isVisible() && !m_repaint->isActive())
        m_repaint->start();
}

void QueryInspector::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_dirty = false;
    update();
}

bool QueryInspector::dark() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QColor QueryInspector::ink(int alpha) const
{
    QColor colour = palette().color(QPalette::WindowText);
    colour.setAlpha(alpha);
    return colour;
}

QList<QueryInspector::Bar> QueryInspector::topBars(
    const QHash<QString, qint64> &counts, int limit, const QColor &colour) const
{
    QList<QPair<QString, qint64>> rows;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        rows.append({it.key(), it.value()});
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    QList<Bar> bars;
    for (int i = 0; i < rows.size() && i < limit; ++i) {
        Bar bar;
        bar.label = rows.at(i).first;
        bar.value = double(rows.at(i).second);
        bar.colour = colour;
        bar.detail = QStringLiteral("%1: %2 statements (%3%)")
                         .arg(bar.label, number(rows.at(i).second))
                         .arg(qRound(100.0 * rows.at(i).second / qMax<qint64>(1, m_total)));
        bars.append(bar);
    }
    return bars;
}

void QueryInspector::paintBars(QPainter &p, const QRectF &area,
                               const QString &title, const QList<Bar> &bars,
                               const QString &empty)
{
    QFont bold = font();
    bold.setBold(true);
    const QFontMetricsF metrics(font());
    p.setFont(bold);
    p.setPen(ink());
    p.drawText(QPointF(area.left(), area.top() + metrics.ascent()), title);
    p.setFont(font());
    qreal y = area.top() + 22;
    if (bars.isEmpty()) {
        p.setPen(ink(140));
        p.drawText(QRectF(area.left(), y, area.width(), 3 * kRowHeight),
                   Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, empty);
        return;
    }
    double peak = 0.0;
    for (const Bar &bar : bars)
        peak = qMax(peak, bar.value);
    const qreal labelWidth = qMin<qreal>(area.width() * 0.55, 210.0);
    const qreal valueWidth = 54.0;
    const qreal barSpace = qMax<qreal>(10.0, area.width() - labelWidth - valueWidth);
    for (const Bar &bar : bars) {
        p.setPen(ink(220));
        p.drawText(QRectF(area.left(), y, labelWidth - 6, kRowHeight),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   metrics.elidedText(bar.label, Qt::ElideMiddle, labelWidth - 6));
        const qreal width =
            peak > 0.0 ? qMax<qreal>(3.0, barSpace * bar.value / peak) : 3.0;
        p.setPen(Qt::NoPen);
        p.setBrush(bar.colour);
        p.drawRoundedRect(QRectF(area.left() + labelWidth, y + 4, width,
                                 kRowHeight - 8),
                          3, 3);
        p.setPen(ink(200));
        p.drawText(QRectF(area.left() + labelWidth + width + 4, y, valueWidth,
                          kRowHeight),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   number(qint64(std::llround(bar.value))));
        m_hits.append({QRectF(area.left(), y, area.width(), kRowHeight), bar.detail});
        y += kRowHeight;
    }
}

void QueryInspector::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().color(QPalette::Window));
    m_hits.clear();
    m_sortHits.clear();
    const QRectF inner = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
    qreal y = inner.top();
    paintHeader(p, QRectF(inner.left(), y, inner.width(), kHeaderHeight));
    y += kHeaderHeight + kGap;

    const bool isDark = dark();
    const qreal quarter = (inner.width() - 3 * kGap) / 4;
    QList<Bar> routes;
    {
        QList<QPair<QString, qint64>> rows;
        for (auto it = m_routes.cbegin(); it != m_routes.cend(); ++it)
            rows.append({it.key(), it.value()});
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        for (int i = 0; i < rows.size() && i < kBarRows; ++i) {
            const QString kind = m_routeKinds.value(rows.at(i).first);
            Bar bar;
            bar.label = rows.at(i).first;
            bar.value = double(rows.at(i).second);
            bar.colour = kindColour(kind, isDark);
            bar.detail = QStringLiteral("%1 (%2): %3 statements")
                             .arg(bar.label, kind.isEmpty() ? QStringLiteral("other")
                                                            : kind,
                                  number(rows.at(i).second));
            routes.append(bar);
        }
    }
    paintBars(p, QRectF(inner.left(), y, quarter, kBarsHeight),
              QStringLiteral("Who asked"), routes,
              QStringLiteral("No statement has named its caller yet."));
    paintBars(p, QRectF(inner.left() + quarter + kGap, y, quarter, kBarsHeight),
              QStringLiteral("Code"), topBars(m_code, kBarRows, ink(110)),
              QStringLiteral("No statement has named its module yet."));
    paintBars(p, QRectF(inner.left() + 2 * (quarter + kGap), y, quarter, kBarsHeight),
              QStringLiteral("Statement kind"), topBars(m_kinds, kBarRows, ink(110)),
              QStringLiteral("Nothing yet."));
    QHash<QString, qint64> people = m_users;
    if (m_anonymous > 0)
        people.insert(QStringLiteral("(signed out)"), m_anonymous);
    if (m_background > 0)
        people.insert(QStringLiteral("(cron, queue, alarm)"), m_background);
    paintBars(p, QRectF(inner.left() + 3 * (quarter + kGap), y, quarter, kBarsHeight),
              QStringLiteral("Users"),
              topBars(people, kBarRows,
                      FleetVisualizer::verbColour(QStringLiteral("SELECT"), isDark)),
              QStringLiteral("Nothing yet."));
    y += kBarsHeight + kGap;

    const qreal half = (inner.width() - kGap) / 2;
    paintHistogram(p, QRectF(inner.left(), y, half, kHistogramHeight));
    paintTableTime(p, QRectF(inner.left() + half + kGap, y, half, kHistogramHeight));
    y += kHistogramHeight + kGap;
    paintShapes(p, QRectF(inner.left(), y, inner.width(), kShapesHeight));
    y += kShapesHeight + kGap;
    paintRecent(p, QRectF(inner.left(), y, inner.width(), kRecentHeight));
}

void QueryInspector::paintHeader(QPainter &p, const QRectF &area)
{
    QFont title = font();
    title = forkmesh::scaledFont(title, 1.25);
    title.setBold(true);
    p.setFont(title);
    p.setPen(ink());
    const QFontMetricsF titleMetrics(title);
    p.drawText(QPointF(area.left(), area.top() + titleMetrics.ascent()),
               QStringLiteral("Every query"));
    p.setFont(font());
    p.setPen(ink(170));
    QStringList facts{QStringLiteral("%1 statements").arg(number(m_total)),
                      QStringLiteral("%1 distinct").arg(number(m_shapes.size()))};
    if (m_timed > 0) {
        facts.append(QStringLiteral("%1 timed").arg(number(m_timed)));
        facts.append(QStringLiteral("median %1").arg(msText(percentileMs(0.5))));
        facts.append(QStringLiteral("p95 %1").arg(msText(percentileMs(0.95))));
        facts.append(QStringLiteral("slowest %1").arg(msText(m_maxMs)));
        facts.append(QStringLiteral("%1% local reads")
                         .arg(qRound(100.0 * m_local / qMax<qint64>(1, m_timed))));
        if (m_failed > 0)
            facts.append(QStringLiteral("%1 failed").arg(number(m_failed)));
    }
    const QFontMetricsF metrics(font());
    const qreal titleWidth =
        titleMetrics.horizontalAdvance(QStringLiteral("Every query"));
    p.drawText(QRectF(area.left() + titleWidth + 12, area.top(),
                      area.width() - titleWidth - 12, titleMetrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               metrics.elidedText(facts.join(QStringLiteral(" · ")), Qt::ElideRight,
                                  area.width() - titleWidth - 12));
    p.setPen(ink(140));
    QString note;
    if (m_total == 0)
        note = QStringLiteral("Each D1 statement the cores run lands here with its "
                              "caller, code, kind and time.");
    else if (m_timed == 0)
        note = QStringLiteral("Times, rows and local-read share appear once the "
                              "cores run a host that writes D1-done lines.");
    else
        note = QStringLiteral("Click a column of the statement table to sort it. "
                              "Hover anything for the full statement.");
    p.drawText(QRectF(area.left(), area.top() + titleMetrics.height() + 4,
                      area.width(), metrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter, note);
    if (m_unmatchedDone > 0)
        m_hits.append({area, QStringLiteral("%1 timings could not be matched to a "
                                            "statement (the node redacted words "
                                            "in its SQL).")
                                 .arg(number(m_unmatchedDone))});
}

void QueryInspector::paintHistogram(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    QFont bold = font();
    bold.setBold(true);
    const QFontMetricsF metrics(font());
    p.setFont(bold);
    p.setPen(ink());
    p.drawText(QPointF(area.left(), area.top() + metrics.ascent()),
               QStringLiteral("How long statements take"));
    p.setFont(font());
    const QStringList labels = bucketLabels();
    const int buckets = labels.size();
    QVector<qint64> totals(buckets, 0);
    for (auto it = m_histogram.cbegin(); it != m_histogram.cend(); ++it)
        for (int i = 0; i < buckets && i < it->size(); ++i)
            totals[i] += it->at(i);
    qint64 peak = 1;
    for (qint64 value : std::as_const(totals))
        peak = qMax(peak, value);
    const QRectF chart = area.adjusted(0, 24, 0, -metrics.height() - 4);
    p.setPen(Qt::NoPen);
    p.setBrush(ink(isDark ? 12 : 8));
    p.drawRoundedRect(chart, 6, 6);
    if (m_timed == 0) {
        p.setPen(ink(140));
        p.drawText(chart, Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("No timings yet"));
        return;
    }
    const qreal slot = chart.width() / buckets;
    for (int i = 0; i < buckets; ++i) {
        const QRectF column(chart.left() + i * slot + 6, chart.top() + 6,
                            slot - 12, chart.height() - 12);
        qreal bottom = column.bottom();
        QStringList parts;
        for (const QString &group : FleetVisualizer::verbOrder()) {
            const qint64 value = m_histogram.value(group).value(i);
            if (value <= 0)
                continue;
            const qreal height = column.height() * value / peak;
            p.setPen(Qt::NoPen);
            p.setBrush(FleetVisualizer::verbColour(group, isDark));
            p.drawRect(QRectF(column.left(), bottom - height, column.width(), height));
            bottom -= height;
            parts.append(QStringLiteral("%1 %2").arg(group, number(value)));
        }
        p.setPen(ink(170));
        p.drawText(QRectF(chart.left() + i * slot, chart.bottom() + 2, slot,
                          metrics.height()),
                   Qt::AlignHCenter | Qt::AlignTop, labels.at(i));
        m_hits.append({QRectF(chart.left() + i * slot, chart.top(), slot,
                              chart.height()),
                       QStringLiteral("%1: %2 statements\n%3")
                           .arg(labels.at(i), number(totals.at(i)),
                                parts.join(QStringLiteral(" · ")))});
    }
}

void QueryInspector::paintTableTime(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    QList<Bar> bars;
    const bool timed = !m_tableMs.isEmpty();
    QList<QPair<QString, double>> rows;
    if (timed) {
        for (auto it = m_tableMs.cbegin(); it != m_tableMs.cend(); ++it)
            rows.append({it.key(), it.value()});
    } else {
        for (auto it = m_tableCalls.cbegin(); it != m_tableCalls.cend(); ++it)
            rows.append({it.key(), double(it.value())});
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (int i = 0; i < rows.size() && i < 6; ++i) {
        Bar bar;
        bar.label = rows.at(i).first;
        bar.value = rows.at(i).second;
        bar.colour = FleetVisualizer::verbColour(QStringLiteral("SELECT"), isDark);
        const qint64 calls = m_tableCalls.value(bar.label);
        bar.detail = timed ? QStringLiteral("%1: %2 spent over %3 statements")
                                 .arg(bar.label, msText(bar.value), number(calls))
                           : QStringLiteral("%1: %2 statements")
                                 .arg(bar.label, number(calls));
        bars.append(bar);
    }
    paintBars(p, area,
              timed ? QStringLiteral("Time by table (ms)")
                    : QStringLiteral("Statements by table"),
              bars, QStringLiteral("Nothing yet."));
}

QString QueryInspector::shapeDetail(const Shape &shape) const
{
    QStringList lines{shape.sql};
    lines.append(QStringLiteral("%1 on %2 · %3 · %4 bound value%5")
                     .arg(shape.verb, shape.table,
                          shape.kind.isEmpty() ? QStringLiteral("query") : shape.kind)
                     .arg(shape.args)
                     .arg(shape.args == 1 ? QString() : QStringLiteral("s")));
    if (!shape.code.isEmpty())
        lines.append(QStringLiteral("From %1").arg(shape.code));
    lines.append(QStringLiteral("%1 calls, last %2")
                     .arg(number(shape.count),
                          QDateTime::fromMSecsSinceEpoch(shape.lastMs)
                              .toString(QStringLiteral("HH:mm:ss"))));
    if (shape.timed > 0)
        lines.append(QStringLiteral("Time: %1 total, %2 average, %3 slowest "
                                    "(%4 timed)")
                         .arg(msText(shape.totalMs),
                              msText(shape.totalMs / shape.timed),
                              msText(shape.maxMs), number(shape.timed)));
    if (shape.timed > 0)
        lines.append(QStringLiteral("Rows: %1 returned, %2 changed · %3% served "
                                    "from the local replica%4")
                         .arg(number(shape.rows), number(shape.changes))
                         .arg(qRound(100.0 * shape.local / shape.timed))
                         .arg(shape.failed > 0
                                  ? QStringLiteral(" · %1 failed")
                                        .arg(number(shape.failed))
                                  : QString()));
    QStringList nodes;
    for (auto it = shape.perNode.cbegin(); it != shape.perNode.cend(); ++it)
        nodes.append(QStringLiteral("%1 %2").arg(it.key(), number(it.value())));
    lines.append(QStringLiteral("Nodes: %1").arg(nodes.join(QStringLiteral(", "))));
    QList<QPair<QString, qint64>> callers;
    for (auto it = shape.callers.cbegin(); it != shape.callers.cend(); ++it)
        callers.append({it.key(), it.value()});
    std::sort(callers.begin(), callers.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (int i = 0; i < callers.size() && i < 5; ++i)
        lines.append(QStringLiteral("  %1 × %2")
                         .arg(number(callers.at(i).second), callers.at(i).first));
    return lines.join(QLatin1Char('\n'));
}

void QueryInspector::paintShapes(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    QFont bold = font();
    bold.setBold(true);
    QFont small = font();
    small = forkmesh::scaledFont(small, 0.9);
    const QFontMetricsF metrics(font());
    const QFontMetricsF smallMetrics(small);
    p.setFont(bold);
    p.setPen(ink());
    p.drawText(QPointF(area.left(), area.top() + metrics.ascent()),
               QStringLiteral("Statements"));

    struct Column {
        QString title;
        qreal width;
        bool sortable;
        SortColumn sort;
        Qt::Alignment align;
    };
    const QList<Column> numeric{
        {QStringLiteral("Calls"), 64, true, SortColumn::Calls, Qt::AlignRight},
        {QStringLiteral("Total"), 72, true, SortColumn::TotalTime, Qt::AlignRight},
        {QStringLiteral("Avg"), 64, true, SortColumn::AverageTime, Qt::AlignRight},
        {QStringLiteral("Max"), 64, true, SortColumn::MaxTime, Qt::AlignRight},
        {QStringLiteral("Rows"), 64, true, SortColumn::Rows, Qt::AlignRight},
        {QStringLiteral("Local"), 50, false, SortColumn::Calls, Qt::AlignRight},
    };
    qreal numericWidth = 0;
    for (const Column &column : numeric)
        numericWidth += column.width;
    const qreal tableWidth = qBound<qreal>(90.0, area.width() * 0.14, 170.0);
    const qreal codeWidth = qBound<qreal>(90.0, area.width() * 0.18, 220.0);
    const qreal sqlWidth = qMax<qreal>(
        80.0, area.width() - 22 - tableWidth - numericWidth - codeWidth - 8);

    const qreal headerY = area.top() + 22;
    p.setFont(small);
    qreal x = area.left() + 22;
    auto header = [&](const QString &title, qreal width, Qt::Alignment align) {
        p.setPen(ink(150));
        p.drawText(QRectF(x, headerY, width - 6, kTableRow), align | Qt::AlignVCenter,
                   title);
        x += width;
    };
    header(QStringLiteral("Table"), tableWidth, Qt::AlignLeft);
    header(QStringLiteral("Statement"), sqlWidth, Qt::AlignLeft);
    for (const Column &column : numeric) {
        const QRectF cell(x, headerY, column.width, kTableRow);
        const bool active = column.sortable && column.sort == m_sort;
        if (active) {
            p.setPen(Qt::NoPen);
            p.setBrush(ink(isDark ? 26 : 16));
            p.drawRoundedRect(cell.adjusted(2, 1, -2, -1), 4, 4);
        }
        QFont headerFont = small;
        headerFont.setBold(active);
        headerFont.setUnderline(column.sortable && !active);
        p.setFont(headerFont);
        p.setPen(ink(active ? 240 : 150));
        p.drawText(cell.adjusted(0, 0, -6, 0), column.align | Qt::AlignVCenter,
                   active ? column.title + QStringLiteral(" ▾") : column.title);
        if (column.sortable) {
            m_sortHits.append({cell, column.sort});
            m_hits.append({cell, QStringLiteral("Sort by %1").arg(column.title.toLower())});
        }
        x += column.width;
    }
    p.setFont(small);
    header(QStringLiteral("Code"), codeWidth, Qt::AlignLeft);
    p.setPen(QPen(ink(40), 1.0));
    p.drawLine(QPointF(area.left(), headerY + kTableRow),
               QPointF(area.right(), headerY + kTableRow));

    const QStringList order = shapeOrder();
    qreal y = headerY + kTableRow;
    if (order.isEmpty()) {
        p.setPen(ink(140));
        p.drawText(QRectF(area.left(), y + 6, area.width(), kTableRow),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("No statements yet."));
    }
    double peak = 0.0;
    for (int i = 0; i < order.size() && i < kShapeRows; ++i)
        peak = qMax(peak, sortValue(m_shapes.value(order.at(i))));
    for (int i = 0; i < order.size() && i < kShapeRows; ++i) {
        const Shape &shape = *m_shapes.constFind(order.at(i));
        const QRectF row(area.left(), y, area.width(), kTableRow);
        if (i % 2 == 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(ink(isDark ? 8 : 5));
            p.drawRect(row);
        }
        const double value = sortValue(shape);
        if (peak > 0.0 && value > 0.0) {
            QColor wash = FleetVisualizer::verbColour(shape.group, isDark);
            wash.setAlphaF(0.16);
            p.setPen(Qt::NoPen);
            p.setBrush(wash);
            p.drawRect(QRectF(row.left() + 18, row.top() + 2,
                              (row.width() - 18) * value / peak, row.height() - 4));
        }
        FleetVisualizer::paintMarker(p, QPointF(area.left() + 9, y + kTableRow / 2),
                                     4.5, shape.group,
                                     FleetVisualizer::verbColour(shape.group, isDark));
        x = area.left() + 22;
        p.setPen(ink(230));
        p.drawText(QRectF(x, y, tableWidth - 6, kTableRow), Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(shape.table, Qt::ElideMiddle, tableWidth - 6));
        x += tableWidth;
        p.setPen(ink(180));
        p.drawText(QRectF(x, y, sqlWidth - 6, kTableRow), Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(shape.sql, Qt::ElideRight, sqlWidth - 6));
        x += sqlWidth;
        const QStringList cells{
            number(shape.count),
            shape.timed > 0 ? msText(shape.totalMs) : QStringLiteral("-"),
            shape.timed > 0 ? msText(shape.totalMs / shape.timed) : QStringLiteral("-"),
            shape.timed > 0 ? msText(shape.maxMs) : QStringLiteral("-"),
            shape.timed > 0
                ? QString::number(double(shape.rows + shape.changes) / shape.timed, 'f',
                                  shape.rows + shape.changes >= 10 * shape.timed ? 0 : 1)
                : QStringLiteral("-"),
            shape.timed > 0
                ? QStringLiteral("%1%").arg(qRound(100.0 * shape.local / shape.timed))
                : QStringLiteral("-")};
        for (int c = 0; c < numeric.size(); ++c) {
            p.setPen(c == 3 && shape.maxMs >= 500.0 ? QColor("#d03b3b") : ink(220));
            p.drawText(QRectF(x, y, numeric.at(c).width - 6, kTableRow),
                       Qt::AlignRight | Qt::AlignVCenter, cells.at(c));
            x += numeric.at(c).width;
        }
        p.setPen(ink(170));
        p.drawText(QRectF(x, y, codeWidth - 4, kTableRow), Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(shape.code, Qt::ElideMiddle, codeWidth - 4));
        m_hits.append({row, shapeDetail(shape)});
        y += kTableRow;
    }
    if (order.size() > kShapeRows) {
        p.setPen(ink(130));
        p.drawText(QRectF(area.left(), area.top(), area.width(), metrics.height()),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("top %1 of %2").arg(kShapeRows).arg(number(order.size())));
    }
    p.setFont(font());
}

void QueryInspector::paintRecent(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    QFont bold = font();
    bold.setBold(true);
    QFont small = font();
    small = forkmesh::scaledFont(small, 0.9);
    const QFontMetricsF metrics(font());
    const QFontMetricsF smallMetrics(small);
    p.setFont(bold);
    p.setPen(ink());
    p.drawText(QPointF(area.left(), area.top() + metrics.ascent()),
               QStringLiteral("Latest statements"));
    p.setFont(small);
    const QList<QPair<QString, qreal>> columns{
        {QStringLiteral("Time"), 70}, {QStringLiteral("Node"), 44},
        {QString(), 18}, {QStringLiteral("Table"), 130},
        {QStringLiteral("Kind"), 90}, {QStringLiteral("Took"), 70},
        {QStringLiteral("Rows"), 56}, {QStringLiteral("Code"), 200},
        {QStringLiteral("Caller"), 0}};
    const qreal headerY = area.top() + 22;
    qreal x = area.left();
    for (const auto &column : columns) {
        const qreal width = column.second > 0 ? column.second : area.right() - x;
        p.setPen(ink(150));
        p.drawText(QRectF(x, headerY, width - 6, kTableRow),
                   Qt::AlignLeft | Qt::AlignVCenter, column.first);
        x += width;
    }
    p.setPen(QPen(ink(40), 1.0));
    p.drawLine(QPointF(area.left(), headerY + kTableRow),
               QPointF(area.right(), headerY + kTableRow));
    qreal y = headerY + kTableRow;
    const int shown = qMin<int>(kRecentRows, m_recent.size());
    if (shown == 0) {
        p.setPen(ink(140));
        p.drawText(QRectF(area.left(), y + 6, area.width(), kTableRow),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("No statements yet."));
    }
    for (int i = 0; i < shown; ++i) {
        const Recent &recent = m_recent.at(m_recent.size() - 1 - i);
        const QRectF row(area.left(), y, area.width(), kTableRow);
        if (i % 2 == 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(ink(isDark ? 8 : 5));
            p.drawRect(row);
        }
        QStringList cells{
            QDateTime::fromMSecsSinceEpoch(recent.atMs).toString(QStringLiteral("HH:mm:ss")),
            recent.node, QString(), recent.table,
            recent.kind.isEmpty() ? QStringLiteral("query") : recent.kind,
            recent.failed ? QStringLiteral("failed") : msText(recent.ms),
            recent.rows < 0 ? QStringLiteral("-")
                            : recent.changes > 0
                                  ? QStringLiteral("%1 chg").arg(number(recent.changes))
                                  : number(recent.rows),
            recent.code, corelogs::callerShape(recent.caller).label};
        x = area.left();
        for (int c = 0; c < columns.size(); ++c) {
            const qreal width =
                columns.at(c).second > 0 ? columns.at(c).second : area.right() - x;
            if (c == 2) {
                FleetVisualizer::paintMarker(
                    p, QPointF(x + 7, y + kTableRow / 2), 4.0, recent.group,
                    FleetVisualizer::verbColour(recent.group, isDark));
            } else {
                QColor colour = ink(c == 0 ? 150 : 220);
                if (c == 1)
                    colour = QColor(corelogs::nodeInkColour(recent.node));
                if (c == 5 && (recent.failed || recent.ms >= 500.0))
                    colour = QColor("#d03b3b");
                p.setPen(colour);
                p.drawText(QRectF(x, y, width - 6, kTableRow),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           smallMetrics.elidedText(cells.at(c), Qt::ElideMiddle,
                                                   width - 6));
            }
            x += width;
        }
        QStringList detail{recent.sql,
                           QStringLiteral("%1 on %2 · %3 · node %4")
                               .arg(recent.verb, recent.table,
                                    recent.kind.isEmpty() ? QStringLiteral("query")
                                                          : recent.kind,
                                    recent.node)};
        if (!recent.code.isEmpty())
            detail.append(QStringLiteral("From %1").arg(recent.code));
        if (!recent.caller.isEmpty())
            detail.append(QStringLiteral("For %1").arg(recent.caller));
        if (recent.ms >= 0.0)
            detail.append(QStringLiteral("Took %1 · %2 rows · %3 changed%4%5")
                              .arg(msText(recent.ms), number(recent.rows),
                                   number(recent.changes),
                                   recent.served.isEmpty()
                                       ? QString()
                                       : QStringLiteral(" · served ") + recent.served,
                                   recent.failed ? QStringLiteral(" · failed")
                                                 : QString()));
        m_hits.append({row, detail.join(QLatin1Char('\n'))});
        y += kTableRow;
    }
    p.setFont(font());
}

QString QueryInspector::toolTipAt(const QPointF &point) const
{
    for (int i = m_hits.size() - 1; i >= 0; --i)
        if (m_hits.at(i).area.contains(point))
            return m_hits.at(i).text;
    return QString();
}

bool QueryInspector::clickAt(const QPointF &point)
{
    for (const SortHit &hit : std::as_const(m_sortHits)) {
        if (hit.area.contains(point)) {
            setSortColumn(hit.column);
            return true;
        }
    }
    return false;
}

void QueryInspector::mouseMoveEvent(QMouseEvent *event)
{
    const QString text = toolTipAt(event->position());
    if (text.isEmpty())
        QToolTip::hideText();
    else
        QToolTip::showText(event->globalPosition().toPoint(), text, this);
    bool overSort = false;
    for (const SortHit &hit : std::as_const(m_sortHits))
        overSort |= hit.area.contains(event->position());
    setCursor(overSort ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}

void QueryInspector::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && clickAt(event->position())) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void QueryInspector::leaveEvent(QEvent *event)
{
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}

} // namespace forkmesh
