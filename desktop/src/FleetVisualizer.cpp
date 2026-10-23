#include "FleetVisualizer.h"
#include "FontSizing.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace forkmesh {

namespace {

constexpr int kFrameMs = 40;
constexpr int kMaxParticles = 360;
constexpr int kShownTables = 15;
constexpr int kHistory = 48;
constexpr int kRateWindow = 60;
constexpr qreal kMargin = 12.0;
constexpr qreal kGap = 10.0;
constexpr qreal kHeaderHeight = 78.0;
constexpr qreal kStreamMinHeight = 280.0;
constexpr qreal kCallersHeight = 124.0;
constexpr qreal kCardHeight = 202.0;
constexpr qreal kCardMinWidth = 250.0;
constexpr qint64 kStaleVitalsMs = 90 * 1000;
const QString kMoreTables = QStringLiteral("\x01more");

int nodeRank(const QString &name)
{
    const QChar kind = name.isEmpty() ? QLatin1Char('n') : name.at(0);
    const int group = kind == QLatin1Char('n')   ? 0
                      : kind == QLatin1Char('a') ? 1
                      : kind == QLatin1Char('r') ? 2
                                                 : 3;
    return group * 10000 + name.mid(1).toInt();
}

QString nodeRole(const QString &name)
{
    const QChar kind = name.isEmpty() ? QLatin1Char('n') : name.at(0);
    if (kind == QLatin1Char('a'))
        return QStringLiteral("app replica");
    if (kind == QLatin1Char('r'))
        return QStringLiteral("runner");
    if (kind == QLatin1Char('b'))
        return QStringLiteral("burst runner");
    return QStringLiteral("core");
}

QString number(qint64 value)
{
    return QLocale().toString(value);
}

QString percent(double share)
{
    return QStringLiteral("%1%").arg(qRound(share * 100.0));
}

QString uptimeText(qint64 seconds)
{
    if (seconds < 0)
        return QString();
    const qint64 days = seconds / 86400;
    const qint64 hours = (seconds % 86400) / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    if (days > 0)
        return QStringLiteral("up %1d %2h").arg(days).arg(hours);
    if (hours > 0)
        return QStringLiteral("up %1h %2m").arg(hours).arg(minutes);
    return QStringLiteral("up %1m").arg(minutes);
}

QString agoText(qint64 ms)
{
    const qint64 seconds = qMax<qint64>(0, ms / 1000);
    if (seconds < 120)
        return QStringLiteral("%1 s ago").arg(seconds);
    if (seconds < 7200)
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    return QStringLiteral("%1 h ago").arg(seconds / 3600);
}

double easeInOut(double t)
{
    return t * t * (3.0 - 2.0 * t);
}

QPointF bezier(const QPointF &a, const QPointF &control, const QPointF &b,
               double t)
{
    const double u = 1.0 - t;
    return a * (u * u) + control * (2.0 * u * t) + b * (t * t);
}

double memoryShare(const corelogs::NodeVitals &v)
{
    if (v.memTotal <= 0 || v.memAvailable < 0)
        return -1.0;
    return qBound(0.0, 1.0 - double(v.memAvailable) / double(v.memTotal), 1.0);
}

double diskShare(const corelogs::NodeVitals &v)
{
    if (v.diskTotal <= 0 || v.diskFree < 0)
        return -1.0;
    return qBound(0.0, 1.0 - double(v.diskFree) / double(v.diskTotal), 1.0);
}

double inodeShare(const corelogs::NodeVitals &v)
{
    if (v.inodesTotal <= 0 || v.inodesFree < 0)
        return -1.0;
    return qBound(0.0, 1.0 - double(v.inodesFree) / double(v.inodesTotal),
                  1.0);
}

double swapShare(const corelogs::NodeVitals &v)
{
    if (v.swapTotal <= 0 || v.swapFree < 0)
        return -1.0;
    return qBound(0.0, 1.0 - double(v.swapFree) / double(v.swapTotal), 1.0);
}

template <typename T> void pushHistory(QVector<T> &history, T value)
{
    history.append(value);
    if (history.size() > kHistory)
        history.remove(0, history.size() - kHistory);
}

} // namespace

FleetVisualizer::FleetVisualizer(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("fleetVisualizer"));
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_perSecond.fill(0, kRateWindow);
    m_timer = new QTimer(this);
    m_timer->setInterval(kFrameMs);
    connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_clock.start();
    updateMinimumHeight();
}

QStringList FleetVisualizer::verbOrder()
{
    return {QStringLiteral("SELECT"), QStringLiteral("UPDATE"),
            QStringLiteral("INSERT"), QStringLiteral("OTHER"),
            QStringLiteral("DELETE")};
}

QString FleetVisualizer::verbGroup(const QString &verb)
{
    if (verb == QLatin1String("SELECT") || verb == QLatin1String("UPDATE") ||
        verb == QLatin1String("INSERT") || verb == QLatin1String("DELETE"))
        return verb;
    return QStringLiteral("OTHER");
}

QColor FleetVisualizer::verbColour(const QString &group, bool dark)
{
    if (group == QLatin1String("SELECT"))
        return QColor(dark ? "#3987e5" : "#2a78d6");
    if (group == QLatin1String("UPDATE"))
        return QColor(dark ? "#d95926" : "#eb6834");
    if (group == QLatin1String("INSERT"))
        return QColor(dark ? "#199e70" : "#1baf7a");
    if (group == QLatin1String("DELETE"))
        return QColor(dark ? "#d55181" : "#e87ba4");
    return QColor(dark ? "#9085e9" : "#4a3aa7");
}

FleetVisualizer::NodeState &FleetVisualizer::nodeState(const QString &node)
{
    auto it = m_nodes.find(node);
    if (it == m_nodes.end()) {
        it = m_nodes.insert(node, NodeState());
        it->name = node;
        updateMinimumHeight();
    }
    return *it;
}

void FleetVisualizer::noteStatement(const QString &node, const QString &sql,
                                    const QString &originFile, qint64 nowMs)
{
    const corelogs::StatementShape shape = corelogs::statementShape(sql);
    const QString group = verbGroup(shape.verb);
    const QString table = shape.table.isEmpty() ? QStringLiteral("(no table)")
                                                : shape.table;
    NodeState &who = nodeState(node);
    ++who.statements;
    who.pulse = 1.0;
    TableState &target = m_tables[table];
    target.name = table;
    ++target.count;
    ++target.byVerb[group];
    ++m_verbCounts[group];
    if (!originFile.isEmpty())
        ++m_callers[originFile];
    ++m_totalStatements;

    const qint64 second = nowMs / 1000;
    if (m_perSecondHeadMs == 0)
        m_perSecondHeadMs = second;
    const qint64 shift = second - m_perSecondHeadMs;
    if (shift >= kRateWindow) {
        m_perSecond.fill(0, kRateWindow);
    } else if (shift > 0) {
        m_perSecond.remove(0, int(shift));
        m_perSecond.append(QVector<int>(int(shift), 0));
    }
    if (shift > 0)
        m_perSecondHeadMs = second;
    if (shift >= 0)
        ++m_perSecond.last();

    if (!isVisible())
        return;
    int queued = 0;
    for (const Particle &particle : std::as_const(m_particles))
        if (particle.t < 0.0)
            ++queued;
    m_seed = m_seed * 1664525u + 1013904223u;
    const double random = double(m_seed >> 8) / double(1u << 24);
    Particle particle;
    particle.node = node;
    particle.table = table;
    particle.group = group;
    particle.t = -qMin(3.0, queued * 0.05);
    particle.speed = 1.0 / (0.9 + random * 0.6);
    particle.bend = random * 2.0 - 1.0;
    m_particles.append(particle);
    if (m_particles.size() > kMaxParticles)
        m_particles.remove(0, m_particles.size() - kMaxParticles);
    updateTimer();
}

void FleetVisualizer::noteVitals(const QString &node,
                                 const corelogs::NodeVitals &vitals,
                                 qint64 nowMs)
{
    if (!vitals.valid)
        return;
    NodeState &who = nodeState(node);
    who.vitals = vitals;
    who.vitalsReceivedMs = nowMs;
    if (vitals.cpuBusy >= 0.0)
        pushHistory(who.cpuHistory, vitals.cpuBusy);
    const double memory = memoryShare(vitals);
    if (memory >= 0.0)
        pushHistory(who.memHistory, memory);
    if (vitals.filesOpen >= 0)
        pushHistory(who.filesHistory, vitals.filesOpen);
    if (isVisible())
        update();
}

void FleetVisualizer::advance(double seconds)
{
    m_phase += seconds;
    for (NodeState &node : m_nodes)
        node.pulse = qMax(0.0, node.pulse - seconds * 1.8);
    for (TableState &table : m_tables) {
        table.pulse = qMax(0.0, table.pulse - seconds * 1.6);
        if (table.placed) {
            const double pull = qMin(1.0, seconds * 5.0);
            table.position += (table.target - table.position) * pull;
        }
    }
    for (int i = m_particles.size() - 1; i >= 0; --i) {
        Particle &particle = m_particles[i];
        particle.t += seconds * particle.speed;
        if (particle.t >= 1.0) {
            auto table = m_tables.find(particle.table);
            if (table != m_tables.end())
                table->pulse = 1.0;
            m_particles.remove(i);
        }
    }
}

void FleetVisualizer::syncNodes()
{
    if (idleReasonSource)
        m_idleReason = idleReasonSource();
    if (!nodeSource)
        return;
    const QList<corelogs::NodeRow> rows = nodeSource();
    QSet<QString> seen;
    for (const corelogs::NodeRow &row : rows) {
        if (row.node.isEmpty())
            continue;
        seen.insert(row.node);
        NodeState &node = nodeState(row.node);
        node.connected = row.connected;
        node.state = row.state;
    }
    const int before = m_nodes.size();
    for (auto it = m_nodes.begin(); it != m_nodes.end();) {
        if (!seen.contains(it.key()) && it->vitalsReceivedMs == 0 &&
            it->statements == 0)
            it = m_nodes.erase(it);
        else
            ++it;
    }
    if (m_nodes.size() != before)
        updateMinimumHeight();
}

void FleetVisualizer::updateTimer()
{
    if (isVisible() && !m_timer->isActive()) {
        m_lastTickMs = m_clock.elapsed();
        m_timer->start();
    } else if (!isVisible() && m_timer->isActive()) {
        m_timer->stop();
    }
}

void FleetVisualizer::tick()
{
    if (!isVisible()) {
        m_timer->stop();
        return;
    }
    const qint64 now = m_clock.elapsed();
    const double seconds = qBound(0.0, (now - m_lastTickMs) / 1000.0, 0.25);
    m_lastTickMs = now;
    advance(seconds);
    if (now - m_lastSyncMs >= 1000) {
        m_lastSyncMs = now;
        syncNodes();
    }
    update();
}

void FleetVisualizer::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    syncNodes();
    m_lastSyncMs = m_clock.elapsed();
    updateTimer();
}

void FleetVisualizer::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
    m_particles.clear();
}

void FleetVisualizer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateMinimumHeight();
}

int FleetVisualizer::cardColumns(qreal width) const
{
    const qreal inner = qMax<qreal>(kCardMinWidth, width - 2 * kMargin);
    return qMax(1, int((inner + kGap) / (kCardMinWidth + kGap)));
}

int FleetVisualizer::heightFor(int width) const
{
    const int nodes = qMax(1, int(m_nodes.size()));
    const int columns = qMin(nodes, cardColumns(width));
    const int rows = (nodes + columns - 1) / columns;
    const qreal vitals = 22.0 + rows * kCardHeight + (rows - 1) * kGap;
    return int(std::ceil(2 * kMargin + kHeaderHeight + kStreamMinHeight +
                         kCallersHeight + vitals + 3 * kGap));
}

void FleetVisualizer::updateMinimumHeight()
{
    const int height = heightFor(qMax(width(), int(kCardMinWidth)));
    if (minimumHeight() != height)
        setMinimumHeight(height);
}

QSize FleetVisualizer::sizeHint() const
{
    return QSize(900, heightFor(900));
}

QSize FleetVisualizer::minimumSizeHint() const
{
    return QSize(int(kCardMinWidth + 2 * kMargin), heightFor(int(kCardMinWidth)));
}

QList<const FleetVisualizer::NodeState *> FleetVisualizer::orderedNodes() const
{
    QList<const NodeState *> nodes;
    for (const NodeState &node : m_nodes)
        nodes.append(&node);
    std::sort(nodes.begin(), nodes.end(),
              [](const NodeState *a, const NodeState *b) {
                  return nodeRank(a->name) < nodeRank(b->name);
              });
    return nodes;
}

QStringList FleetVisualizer::shownTables() const
{
    QList<const TableState *> tables;
    for (const TableState &table : m_tables)
        tables.append(&table);
    std::sort(tables.begin(), tables.end(),
              [](const TableState *a, const TableState *b) {
                  if (a->count != b->count)
                      return a->count > b->count;
                  return a->name < b->name;
              });
    QStringList names;
    const int limit =
        tables.size() > kShownTables + 1 ? kShownTables : tables.size();
    for (int i = 0; i < limit; ++i)
        names.append(tables.at(i)->name);
    if (tables.size() > limit)
        names.append(kMoreTables);
    return names;
}

QString FleetVisualizer::tableSlot(const QString &table,
                                   const QStringList &shown) const
{
    if (shown.contains(table))
        return table;
    return shown.contains(kMoreTables) ? kMoreTables : QString();
}

QHash<QString, QPointF> FleetVisualizer::nodePositions(const QRectF &area) const
{
    QHash<QString, QPointF> positions;
    const QList<const NodeState *> nodes = orderedNodes();
    if (nodes.isEmpty())
        return positions;
    const qreal top = area.top() + 34.0;
    const qreal span = area.bottom() - 22.0 - top;
    const qreal step = span / nodes.size();
    for (int i = 0; i < nodes.size(); ++i)
        positions.insert(nodes.at(i)->name,
                         QPointF(area.left() + 40.0, top + step * (i + 0.5)));
    return positions;
}

QHash<QString, QRectF> FleetVisualizer::tableCells(
    const QRectF &area, const QStringList &shown) const
{
    QHash<QString, QRectF> cells;
    if (shown.isEmpty())
        return cells;
    const int count = shown.size();
    const qreal aspect = area.width() / qMax<qreal>(1.0, area.height());
    int columns = qMax(1, int(std::ceil(std::sqrt(count * aspect))));
    columns = qMin(columns, count);
    const int rows = (count + columns - 1) / columns;
    const qreal cellWidth = area.width() / columns;
    const qreal cellHeight = area.height() / rows;
    for (int i = 0; i < count; ++i) {
        const int row = i / columns;
        const int column = i % columns;
        cells.insert(shown.at(i),
                     QRectF(area.left() + column * cellWidth,
                            area.top() + row * cellHeight, cellWidth,
                            cellHeight));
    }
    return cells;
}

bool FleetVisualizer::dark() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QColor FleetVisualizer::ink(int alpha) const
{
    QColor colour = palette().color(QPalette::WindowText);
    colour.setAlpha(alpha);
    return colour;
}

QColor FleetVisualizer::surface() const
{
    return palette().color(QPalette::Window);
}

QColor FleetVisualizer::statusColour(double share) const
{
    if (share >= 0.9)
        return QColor("#d03b3b");
    if (share >= 0.75)
        return QColor("#fab219");
    return QColor("#0ca30c");
}

QString FleetVisualizer::statusWord(double share) const
{
    if (share >= 0.9)
        return QStringLiteral("critical");
    if (share >= 0.75)
        return QStringLiteral("high");
    return QStringLiteral("ok");
}

void FleetVisualizer::paintMarker(QPainter &p, const QPointF &at, double radius,
                                  const QString &group,
                                  const QColor &colour)
{
    p.setBrush(colour);
    p.setPen(Qt::NoPen);
    if (group == QLatin1String("INSERT")) {
        p.drawRoundedRect(QRectF(at.x() - radius, at.y() - radius, 2 * radius,
                                 2 * radius),
                          1.0, 1.0);
    } else if (group == QLatin1String("UPDATE")) {
        const QPointF points[3] = {
            at + QPointF(0, -radius * 1.15), at + QPointF(radius, radius * 0.85),
            at + QPointF(-radius, radius * 0.85)};
        p.drawPolygon(points, 3);
    } else if (group == QLatin1String("DELETE")) {
        const QPointF points[4] = {
            at + QPointF(0, -radius * 1.2), at + QPointF(radius * 1.2, 0),
            at + QPointF(0, radius * 1.2), at + QPointF(-radius * 1.2, 0)};
        p.drawPolygon(points, 4);
    } else if (group == QLatin1String("OTHER")) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(colour, qMax(1.5, radius * 0.5)));
        p.drawEllipse(at, radius * 0.85, radius * 0.85);
    } else {
        p.drawEllipse(at, radius, radius);
    }
}

void FleetVisualizer::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), surface());
    m_hits.clear();
    const QRectF inner = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin,
                                                 -kMargin);
    if (m_nodes.isEmpty() && m_totalStatements == 0) {
        QFont big = font();
        big = forkmesh::scaledFont(big, 1.3);
        big.setBold(true);
        p.setFont(big);
        p.setPen(ink(200));
        const QRectF middle = inner.adjusted(40, 0, -40, 0);
        p.drawText(QRectF(middle.left(), middle.center().y() - 60,
                          middle.width(), 30),
                   Qt::AlignCenter, QStringLiteral("The fleet is quiet"));
        p.setFont(font());
        p.setPen(ink(150));
        p.drawText(
            QRectF(middle.left(), middle.center().y() - 24, middle.width(), 80),
            Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
            m_idleReason.isEmpty()
                ? QStringLiteral("rqlite statements and node memory, disk and "
                                 "files show up here once the desktop follows "
                                 "the fleet nodes' journals.")
                : m_idleReason);
        return;
    }
    const int columns =
        qMin(qMax(1, int(m_nodes.size())), cardColumns(inner.width() + 2 * kMargin));
    const int rows = (qMax(1, int(m_nodes.size())) + columns - 1) / columns;
    const qreal vitalsHeight = 22.0 + rows * kCardHeight + (rows - 1) * kGap;
    const qreal streamHeight =
        qMax(kStreamMinHeight, inner.height() - kHeaderHeight - kCallersHeight -
                                   vitalsHeight - 3 * kGap);
    qreal y = inner.top();
    const QRectF header(inner.left(), y, inner.width(), kHeaderHeight);
    y += kHeaderHeight + kGap;
    const QRectF stream(inner.left(), y, inner.width(), streamHeight);
    y += streamHeight + kGap;
    const QRectF callers(inner.left(), y, inner.width(), kCallersHeight);
    y += kCallersHeight + kGap;
    const QRectF vitals(inner.left(), y, inner.width(), vitalsHeight);
    paintHeader(p, header);
    paintStream(p, stream);
    paintCallers(p, callers);
    paintVitals(p, vitals);
}

void FleetVisualizer::paintHeader(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    QFont title = font();
    title = forkmesh::scaledFont(title, 1.25);
    title.setBold(true);
    p.setFont(title);
    p.setPen(ink());
    const QFontMetricsF titleMetrics(title);
    p.drawText(QPointF(area.left(), area.top() + titleMetrics.ascent()),
               QStringLiteral("rqlite, live"));
    int nowRate = m_perSecond.isEmpty() ? 0 : m_perSecond.last();
    if (m_perSecondHeadMs > 0 &&
        QDateTime::currentMSecsSinceEpoch() / 1000 - m_perSecondHeadMs > 1)
        nowRate = 0;
    const qreal titleWidth =
        titleMetrics.horizontalAdvance(QStringLiteral("rqlite, live"));
    p.setFont(font());
    p.setPen(ink(160));
    p.drawText(QPointF(area.left() + titleWidth + 12,
                       area.top() + titleMetrics.ascent()),
               QStringLiteral("%1 statements · %2 in the last second · %3 "
                              "tables")
                   .arg(number(m_totalStatements))
                   .arg(nowRate)
                   .arg(m_tables.size()));

    const QRectF spark(area.right() - 220, area.top() + 2, 220,
                       area.height() - 20);
    qreal x = area.left();
    const qreal legendY = area.top() + titleMetrics.height() + 18;
    const QFontMetricsF metrics(font());
    for (const QString &group : verbOrder()) {
        const QColor colour = verbColour(group, isDark);
        paintMarker(p, QPointF(x + 6, legendY), 5.0, group, colour);
        const QString text =
            QStringLiteral("%1 %2").arg(group, number(m_verbCounts.value(group)));
        p.setPen(ink(220));
        p.drawText(QPointF(x + 16, legendY + metrics.ascent() / 2 - 1), text);
        const qreal chipWidth = 16 + metrics.horizontalAdvance(text) + 18;
        m_hits.append(
            {QRectF(x, legendY - 10, chipWidth, 20),
             group == QLatin1String("OTHER")
                 ? QStringLiteral("Schema, pragma and transaction statements "
                                  "(CREATE, DROP, ALTER, PRAGMA, BEGIN, "
                                  "COMMIT): %1")
                       .arg(number(m_verbCounts.value(group)))
                 : QStringLiteral("%1 statements: %2")
                       .arg(group, number(m_verbCounts.value(group)))});
        x += chipWidth;
        if (x > spark.left() - 80)
            break;
    }

    int peak = 1;
    for (int value : std::as_const(m_perSecond))
        peak = qMax(peak, value);
    p.setPen(Qt::NoPen);
    p.setBrush(ink(10));
    p.drawRoundedRect(spark, 6, 6);
    const QRectF bars = spark.adjusted(6, 6, -6, -6);
    const qreal barWidth = bars.width() / kRateWindow;
    const QColor accent = verbColour(QStringLiteral("SELECT"), isDark);
    for (int i = 0; i < m_perSecond.size(); ++i) {
        const int value = m_perSecond.at(i);
        if (value <= 0)
            continue;
        const qreal height = qMax<qreal>(2.0, bars.height() * value / peak);
        QColor colour = accent;
        colour.setAlphaF(0.35 + 0.65 * double(i) / kRateWindow);
        p.setBrush(colour);
        p.drawRoundedRect(QRectF(bars.left() + i * barWidth + 0.5,
                                 bars.bottom() - height,
                                 qMax<qreal>(1.0, barWidth - 1.0), height),
                          1.0, 1.0);
    }
    QFont small = font();
    small = forkmesh::scaledFont(small, 0.85);
    p.setFont(small);
    p.setPen(ink(140));
    p.drawText(QRectF(spark.left(), spark.bottom() + 2, spark.width(), 16),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("per second, last 60 s"));
    p.drawText(QRectF(spark.left(), spark.bottom() + 2, spark.width(), 16),
               Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("peak %1/s").arg(peak));
    m_hits.append({spark, QStringLiteral("Statements per second over the last "
                                         "minute; the busiest second had %1.")
                              .arg(peak)});
    p.setFont(font());
}

void FleetVisualizer::paintStream(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    p.setPen(Qt::NoPen);
    p.setBrush(ink(isDark ? 12 : 8));
    p.drawRoundedRect(area, 10, 10);
    QFont small = font();
    small = forkmesh::scaledFont(small, 0.85);
    p.setFont(small);
    p.setPen(ink(140));
    p.drawText(area.adjusted(12, 8, -12, 0), Qt::AlignLeft | Qt::AlignTop,
               QStringLiteral("Every statement flies from the node that ran "
                              "it to the table it touched. Bubble size is "
                              "traffic, the ring is its mix."));

    const QHash<QString, QPointF> nodes = nodePositions(area);
    const QStringList shown = shownTables();
    const QRectF tableArea = area.adjusted(140, 34, -14, -10);
    const QHash<QString, QRectF> cells = tableCells(tableArea, shown);
    qint64 peak = 1;
    for (const QString &name : shown) {
        if (name != kMoreTables)
            peak = qMax(peak, m_tables.value(name).count);
    }
    QMap<QString, qint64> moreByVerb;
    qint64 moreCount = 0;
    for (const TableState &table : std::as_const(m_tables)) {
        if (shown.contains(table.name))
            continue;
        moreCount += table.count;
        for (auto it = table.byVerb.cbegin(); it != table.byVerb.cend(); ++it)
            moreByVerb[it.key()] += it.value();
    }
    QHash<QString, QPointF> centres;
    QHash<QString, qreal> radii;
    for (const QString &name : shown) {
        const QRectF cell = cells.value(name);
        const QPointF target = cell.center() - QPointF(0, 8);
        QPointF centre = target;
        if (name != kMoreTables) {
            TableState &table = m_tables[name];
            table.target = target;
            if (!table.placed) {
                table.position = target;
                table.placed = true;
            }
            centre = table.position;
        }
        const qint64 count =
            name == kMoreTables ? moreCount : m_tables.value(name).count;
        const qreal maxRadius =
            qMax<qreal>(8.0, qMin(cell.width(), cell.height() - 20) / 2 - 4);
        const qreal radius = qMax<qreal>(
            7.0, maxRadius * std::sqrt(qMin(1.0, double(count) / peak)));
        centres.insert(name, centre);
        radii.insert(name, radius);
    }

    for (const Particle &particle : std::as_const(m_particles)) {
        if (particle.t < 0.0 || !nodes.contains(particle.node))
            continue;
        const QString slot = tableSlot(particle.table, shown);
        if (slot.isEmpty())
            continue;
        const QPointF from = nodes.value(particle.node) + QPointF(18, 0);
        const QPointF to = centres.value(slot);
        const QPointF control((from.x() + to.x()) / 2,
                              (from.y() + to.y()) / 2 +
                                  particle.bend * area.height() * 0.3);
        const QColor colour = verbColour(particle.group, isDark);
        for (int ghost = 3; ghost >= 0; --ghost) {
            const double t = particle.t - ghost * 0.035;
            if (t < 0.0)
                continue;
            QColor faded = colour;
            faded.setAlphaF(ghost == 0 ? 1.0 : 0.45 - ghost * 0.12);
            paintMarker(p, bezier(from, control, to, easeInOut(t)),
                        ghost == 0 ? 4.5 : 3.5 - ghost * 0.6, particle.group,
                        faded);
        }
    }

    const QFontMetricsF smallMetrics(small);
    for (const QString &name : shown) {
        const QPointF centre = centres.value(name);
        const qreal radius = radii.value(name);
        const QMap<QString, qint64> byVerb =
            name == kMoreTables ? moreByVerb : m_tables.value(name).byVerb;
        const qint64 count =
            name == kMoreTables ? moreCount : m_tables.value(name).count;
        const double pulse = name == kMoreTables ? 0.0 : m_tables.value(name).pulse;
        const qreal grow = radius * (1.0 + 0.12 * pulse);
        if (pulse > 0.0) {
            QColor glow = ink(int(90 * pulse));
            p.setPen(QPen(glow, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(centre, grow + 4 + (1.0 - pulse) * 10,
                          grow + 4 + (1.0 - pulse) * 10);
        }
        const QRectF disc(centre.x() - grow, centre.y() - grow, 2 * grow,
                          2 * grow);
        int start = 90 * 16;
        qint64 total = 0;
        for (qint64 value : byVerb)
            total += value;
        p.setPen(QPen(surface(), 2.0));
        for (const QString &group : verbOrder()) {
            const qint64 value = byVerb.value(group);
            if (value <= 0 || total <= 0)
                continue;
            const int span = -int(std::round(360.0 * 16 * value / total));
            p.setBrush(verbColour(group, isDark));
            if (value == total)
                p.drawEllipse(disc);
            else
                p.drawPie(disc, start, span);
            start += span;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(surface());
        p.drawEllipse(centre, grow * 0.56, grow * 0.56);
        p.setFont(small);
        p.setPen(ink(230));
        if (grow * 0.56 >= 9.0) {
            const QString text = count >= 10000
                                     ? QStringLiteral("%1k").arg(count / 1000)
                                     : QString::number(count);
            p.drawText(QRectF(centre.x() - grow, centre.y() - 8, 2 * grow, 16),
                       Qt::AlignCenter, text);
        }
        const QRectF cell = cells.value(name);
        const QString label =
            name == kMoreTables
                ? QStringLiteral("+%1 more").arg(m_tables.size() - shown.size() + 1)
                : name;
        p.drawText(QRectF(cell.left() + 2, centre.y() + grow + 3,
                          cell.width() - 4, smallMetrics.height()),
                   Qt::AlignHCenter | Qt::AlignTop,
                   smallMetrics.elidedText(label, Qt::ElideMiddle,
                                           cell.width() - 4));
        QStringList parts;
        for (const QString &group : verbOrder()) {
            const qint64 value = byVerb.value(group);
            if (value > 0)
                parts.append(QStringLiteral("%1 %2").arg(group, number(value)));
        }
        m_hits.append(
            {disc.adjusted(-4, -4, 4, 4),
             QStringLiteral("%1\n%2 statements (%3 of all)\n%4")
                 .arg(name == kMoreTables ? label + QStringLiteral(" tables")
                                          : name,
                      number(count),
                      percent(double(count) / qMax<qint64>(1, m_totalStatements)),
                      parts.join(QStringLiteral(" · ")))});
    }

    p.setFont(font());
    for (const NodeState *node : orderedNodes()) {
        const QPointF at = nodes.value(node->name);
        const QColor colour(corelogs::nodeInkColour(node->name));
        if (node->pulse > 0.0) {
            QColor ring = colour;
            ring.setAlphaF(0.6 * node->pulse);
            p.setPen(QPen(ring, 2.0));
            p.setBrush(Qt::NoBrush);
            const qreal r = 17 + (1.0 - node->pulse) * 12;
            p.drawEllipse(at, r, r);
        }
        QColor fill = colour;
        fill.setAlphaF(node->connected ? 0.28 : 0.1);
        p.setBrush(fill);
        QPen outline(colour, 2.0);
        if (!node->connected)
            outline.setStyle(Qt::DashLine);
        p.setPen(outline);
        p.drawEllipse(at, 16, 16);
        p.setPen(ink());
        p.drawText(QRectF(at.x() - 16, at.y() - 16, 32, 32), Qt::AlignCenter,
                   node->name);
        m_hits.append(
            {QRectF(at.x() - 20, at.y() - 20, 40, 40),
             QStringLiteral("%1 (%2)\n%3 statements seen\nfeed: %4")
                 .arg(node->name, nodeRole(node->name),
                      number(node->statements),
                      node->state.isEmpty() ? QStringLiteral("not followed")
                                            : node->state)});
    }
}

void FleetVisualizer::paintCallers(QPainter &p, const QRectF &area)
{
    QFont bold = font();
    bold.setBold(true);
    const QFontMetricsF metrics(font());
    const qreal half = (area.width() - kGap) / 2;
    const QRectF left(area.left(), area.top(), half, area.height());
    const QRectF right(area.left() + half + kGap, area.top(), half,
                       area.height());

    p.setFont(bold);
    p.setPen(ink());
    p.drawText(QPointF(left.left(), left.top() + metrics.ascent()),
               QStringLiteral("Busiest code"));
    p.drawText(QPointF(right.left(), right.top() + metrics.ascent()),
               QStringLiteral("Statements by node"));
    p.setFont(font());

    QList<QPair<QString, qint64>> callers;
    for (auto it = m_callers.cbegin(); it != m_callers.cend(); ++it)
        callers.append({it.key(), it.value()});
    std::sort(callers.begin(), callers.end(),
              [](const auto &a, const auto &b) {
                  return a.second != b.second ? a.second > b.second
                                              : a.first < b.first;
              });
    if (callers.size() > 5)
        callers = callers.mid(0, 5);
    const qreal rowHeight = 18.0;
    const qreal labelWidth = qMin<qreal>(190.0, half * 0.45);
    qint64 callerPeak = callers.isEmpty() ? 1 : callers.first().second;
    qreal y = left.top() + metrics.height() + 8;
    if (callers.isEmpty()) {
        p.setPen(ink(140));
        p.drawText(QPointF(left.left(), y + metrics.ascent()),
                   QStringLiteral("No statement has named its module yet."));
    }
    for (const auto &caller : std::as_const(callers)) {
        p.setPen(ink(220));
        p.drawText(QRectF(left.left(), y, labelWidth, rowHeight),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   metrics.elidedText(caller.first, Qt::ElideMiddle,
                                      labelWidth - 6));
        const qreal barSpace = left.width() - labelWidth - 60;
        const qreal width =
            qMax<qreal>(3.0, barSpace * double(caller.second) / callerPeak);
        p.setPen(Qt::NoPen);
        p.setBrush(ink(90));
        p.drawRoundedRect(QRectF(left.left() + labelWidth, y + 4, width,
                                 rowHeight - 8),
                          3, 3);
        p.setPen(ink(200));
        p.drawText(QRectF(left.left() + labelWidth + width + 6, y, 60,
                          rowHeight),
                   Qt::AlignLeft | Qt::AlignVCenter, number(caller.second));
        m_hits.append({QRectF(left.left(), y, left.width(), rowHeight),
                       QStringLiteral("%1: %2 statements")
                           .arg(caller.first, number(caller.second))});
        y += rowHeight;
    }

    const QList<const NodeState *> nodes = orderedNodes();
    qint64 nodePeak = 1;
    for (const NodeState *node : nodes)
        nodePeak = qMax(nodePeak, node->statements);
    y = right.top() + metrics.height() + 8;
    int shown = 0;
    for (const NodeState *node : nodes) {
        if (++shown > 5)
            break;
        p.setPen(ink(220));
        p.drawText(QRectF(right.left(), y, 44, rowHeight),
                   Qt::AlignLeft | Qt::AlignVCenter, node->name);
        const qreal barSpace = right.width() - 44 - 60;
        const qreal width = node->statements > 0
                                ? qMax<qreal>(3.0, barSpace * double(node->statements) /
                                                       nodePeak)
                                : 0.0;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(corelogs::nodeInkColour(node->name)));
        if (width > 0)
            p.drawRoundedRect(QRectF(right.left() + 44, y + 4, width,
                                     rowHeight - 8),
                              3, 3);
        p.setPen(ink(200));
        p.drawText(QRectF(right.left() + 44 + width + 6, y, 60, rowHeight),
                   Qt::AlignLeft | Qt::AlignVCenter, number(node->statements));
        y += rowHeight;
    }
}

void FleetVisualizer::paintVitals(QPainter &p, const QRectF &area)
{
    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    p.setPen(ink());
    const QFontMetricsF metrics(bold);
    p.drawText(QPointF(area.left(), area.top() + metrics.ascent()),
               QStringLiteral("Node vitals"));
    p.setFont(font());
    const QList<const NodeState *> nodes = orderedNodes();
    if (nodes.isEmpty())
        return;
    const int columns =
        qMin(int(nodes.size()), cardColumns(area.width() + 2 * kMargin));
    const qreal cardWidth = (area.width() - (columns - 1) * kGap) / columns;
    for (int i = 0; i < nodes.size(); ++i) {
        const int row = i / columns;
        const int column = i % columns;
        const QRectF card(area.left() + column * (cardWidth + kGap),
                          area.top() + 22 + row * (kCardHeight + kGap),
                          cardWidth, kCardHeight);
        paintNodeCard(p, card, *nodes.at(i));
    }
}

void FleetVisualizer::paintNodeCard(QPainter &p, const QRectF &card,
                                    const NodeState &node)
{
    const bool isDark = dark();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const corelogs::NodeVitals &v = node.vitals;
    const QColor nodeColour(corelogs::nodeInkColour(node.name));
    p.setPen(QPen(ink(40), 1.0));
    p.setBrush(ink(isDark ? 10 : 6));
    p.drawRoundedRect(card, 10, 10);

    QFont bold = font();
    bold.setBold(true);
    QFont small = font();
    small = forkmesh::scaledFont(small, 0.85);
    const QFontMetricsF metrics(font());
    const QFontMetricsF smallMetrics(small);
    const QRectF inner = card.adjusted(12, 10, -12, -10);

    p.setPen(Qt::NoPen);
    p.setBrush(nodeColour);
    p.drawEllipse(QPointF(inner.left() + 5, inner.top() + metrics.height() / 2),
                  5, 5);
    p.setFont(bold);
    p.setPen(ink());
    p.drawText(QPointF(inner.left() + 16, inner.top() + metrics.ascent()),
               node.name);
    const qreal nameWidth = QFontMetricsF(bold).horizontalAdvance(node.name);
    p.setFont(small);
    p.setPen(ink(150));
    p.drawText(QPointF(inner.left() + 22 + nameWidth,
                       inner.top() + metrics.ascent()),
               nodeRole(node.name));
    const bool stale =
        node.vitalsReceivedMs > 0 && nowMs - node.vitalsReceivedMs > kStaleVitalsMs;
    const QString corner =
        stale ? QStringLiteral("last sample %1")
                    .arg(agoText(nowMs - node.vitalsReceivedMs))
              : node.state;
    p.drawText(QRectF(inner.left(), inner.top(), inner.width(), metrics.height()),
               Qt::AlignRight | Qt::AlignVCenter,
               smallMetrics.elidedText(corner, Qt::ElideRight,
                                       inner.width() * 0.45));

    QStringList facts;
    if (v.uptimeSeconds >= 0)
        facts.append(uptimeText(v.uptimeSeconds));
    if (v.cpus > 0)
        facts.append(QStringLiteral("%1 CPU%2").arg(v.cpus).arg(
            v.cpus == 1 ? QString() : QStringLiteral("s")));
    if (v.procsTotal >= 0)
        facts.append(QStringLiteral("%1 processes").arg(number(v.procsTotal)));
    const qreal factsY = inner.top() + metrics.height() + 2;
    p.drawText(QRectF(inner.left(), factsY, inner.width(), smallMetrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               facts.join(QStringLiteral(" · ")));

    const QRectF body(inner.left(), factsY + smallMetrics.height() + 8,
                      inner.width(), inner.bottom() - factsY -
                                         smallMetrics.height() - 8);
    if (!v.valid) {
        p.setPen(QPen(ink(70), 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        const QRectF tank(body.left(), body.top(), 46,
                          body.height() - 22 - smallMetrics.height());
        p.drawRoundedRect(tank, 8, 8);
        p.setPen(ink(150));
        p.drawText(QRectF(tank.right() + 14, body.top(),
                          body.width() - tank.width() - 14, body.height()),
                   Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                   QStringLiteral("No vitals yet. A node sends memory, disk "
                                  "and file counts once its log reader is "
                                  "rolled (install %1 --steps logread).")
                       .arg(node.name));
        m_hits.append({card, QStringLiteral("%1: waiting for the node's log "
                                            "reader to send vitals")
                                 .arg(node.name)});
        return;
    }

    const double memory = memoryShare(v);
    const QRectF tank(body.left(), body.top(), 46,
                      body.height() - 22 - smallMetrics.height());
    QPainterPath tankPath;
    tankPath.addRoundedRect(tank, 8, 8);
    p.setPen(Qt::NoPen);
    p.setBrush(ink(isDark ? 18 : 10));
    p.drawPath(tankPath);
    if (memory >= 0.0) {
        p.save();
        p.setClipPath(tankPath);
        const QColor liquid = statusColour(memory);
        const qreal level = tank.bottom() - tank.height() * memory;
        for (int layer = 1; layer >= 0; --layer) {
            QPainterPath wave;
            wave.moveTo(tank.left(), tank.bottom());
            for (qreal x = tank.left(); x <= tank.right() + 1; x += 2) {
                const qreal phase = m_phase * (layer ? 2.1 : 3.0) + layer * 1.7;
                wave.lineTo(x, level + std::sin((x - tank.left()) * 0.22 + phase) *
                                           (layer ? 3.0 : 2.2));
            }
            wave.lineTo(tank.right() + 1, tank.bottom());
            wave.closeSubpath();
            QColor colour = liquid;
            colour.setAlphaF(layer ? 0.35 : 0.75);
            p.setBrush(colour);
            p.drawPath(wave);
        }
        p.setBrush(QColor(255, 255, 255, 110));
        for (int bubble = 0; bubble < 4; ++bubble) {
            const double rise =
                std::fmod(m_phase * (0.25 + bubble * 0.07) + bubble * 0.31, 1.0);
            const qreal by = tank.bottom() - rise * (tank.bottom() - level);
            const qreal bx = tank.left() + 9 + bubble * 9 +
                             std::sin(m_phase * 2 + bubble) * 2;
            if (by > level + 4)
                p.drawEllipse(QPointF(bx, by), 1.6, 1.6);
        }
        p.restore();
    }
    p.setPen(QPen(ink(70), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(tankPath);
    const double swap = swapShare(v);
    if (swap >= 0.0) {
        const QRectF swapBar(tank.left(), tank.bottom() + 5, tank.width(), 4);
        p.setPen(Qt::NoPen);
        p.setBrush(ink(25));
        p.drawRoundedRect(swapBar, 2, 2);
        p.setBrush(statusColour(swap));
        p.drawRoundedRect(QRectF(swapBar.left(), swapBar.top(),
                                 qMax<qreal>(2.0, swapBar.width() * swap), 4),
                          2, 2);
    }
    p.setFont(small);
    p.setPen(ink(220));
    p.drawText(QRectF(tank.left() - 10, tank.bottom() + 10, tank.width() + 20,
                      2 * smallMetrics.height()),
               Qt::AlignHCenter | Qt::AlignTop,
               memory >= 0.0 ? QStringLiteral("mem %1\n%2")
                                   .arg(percent(memory), statusWord(memory))
                             : QStringLiteral("mem ?"));

    const qreal columnLeft = tank.right() + 14;
    const qreal columnWidth = body.right() - columnLeft;
    const qreal rowHeight = (body.height()) / 3.0;
    const qreal textLeft = columnLeft + 40;
    const qreal textWidth = columnWidth - 40;

    const double disk = diskShare(v);
    const QRectF cabinet(columnLeft + 4, body.top() + 2, 26, rowHeight - 8);
    p.setPen(QPen(ink(90), 1.2));
    p.setBrush(ink(isDark ? 16 : 8));
    p.drawRoundedRect(cabinet, 3, 3);
    const int drawers = 4;
    const qreal drawerHeight = (cabinet.height() - 4) / drawers;
    for (int d = 0; d < drawers; ++d) {
        const QRectF drawer(cabinet.left() + 3,
                            cabinet.bottom() - 2 - (d + 1) * drawerHeight + 1,
                            cabinet.width() - 6, drawerHeight - 2);
        const double fill =
            disk < 0.0 ? 0.0 : qBound(0.0, disk * drawers - d, 1.0);
        p.setPen(Qt::NoPen);
        p.setBrush(ink(25));
        p.drawRect(drawer);
        if (fill > 0.0) {
            p.setBrush(statusColour(disk));
            p.drawRect(QRectF(drawer.left(), drawer.top(),
                              drawer.width() * fill, drawer.height()));
        }
        p.setBrush(ink(120));
        p.drawRect(QRectF(drawer.center().x() - 3, drawer.center().y() - 0.75,
                          6, 1.5));
    }
    p.setFont(font());
    p.setPen(ink());
    p.drawText(QRectF(textLeft, body.top(), textWidth, metrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               disk >= 0.0 ? QStringLiteral("Disk %1 · %2")
                                 .arg(percent(disk), statusWord(disk))
                           : QStringLiteral("Disk ?"));
    p.setFont(small);
    p.setPen(ink(160));
    p.drawText(QRectF(textLeft, body.top() + metrics.height(), textWidth,
                      smallMetrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               v.diskFree >= 0
                   ? QStringLiteral("%1 free of %2")
                         .arg(corelogs::compactByteSize(v.diskFree),
                              corelogs::compactByteSize(v.diskTotal))
                   : QString());

    const qreal filesTop = body.top() + rowHeight;
    const double inodes = inodeShare(v);
    const int sheets =
        v.filesOpen > 0
            ? qBound(1, int(std::log2(double(v.filesOpen)) / 2.0), 6)
            : 0;
    for (int s = sheets - 1; s >= 0; --s) {
        const QRectF sheet(columnLeft + 4 + s * 2.5, filesTop + 2 + s * 2.0, 18,
                           rowHeight - 14);
        QPainterPath paper;
        paper.moveTo(sheet.topLeft());
        paper.lineTo(sheet.right() - 5, sheet.top());
        paper.lineTo(sheet.right(), sheet.top() + 5);
        paper.lineTo(sheet.bottomRight());
        paper.lineTo(sheet.bottomLeft());
        paper.closeSubpath();
        p.setPen(QPen(ink(110), 1.0));
        p.setBrush(isDark ? QColor(60, 60, 58) : QColor(255, 255, 255));
        p.drawPath(paper);
    }
    p.setFont(font());
    p.setPen(ink());
    p.drawText(QRectF(textLeft, filesTop, textWidth, metrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               v.filesOpen >= 0
                   ? QStringLiteral("%1 open files").arg(number(v.filesOpen))
                   : QStringLiteral("Open files ?"));
    p.setFont(small);
    p.setPen(ink(160));
    p.drawText(QRectF(textLeft, filesTop + metrics.height(), textWidth,
                      smallMetrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               inodes >= 0.0 ? QStringLiteral("inodes %1 used · %2")
                                   .arg(percent(inodes), statusWord(inodes))
                             : QString());

    const qreal cpuTop = body.top() + 2 * rowHeight;
    const QRectF ekg(columnLeft, cpuTop + 2, 34, rowHeight - 10);
    if (node.cpuHistory.size() >= 2) {
        QPainterPath line;
        for (int i = 0; i < node.cpuHistory.size(); ++i) {
            const qreal x =
                ekg.left() + ekg.width() * i / (node.cpuHistory.size() - 1);
            const qreal yv =
                ekg.bottom() - ekg.height() * node.cpuHistory.at(i) / 100.0;
            if (i == 0)
                line.moveTo(x, yv);
            else
                line.lineTo(x, yv);
        }
        p.setPen(QPen(nodeColour, 2.0, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
        const double beat = 0.5 + 0.5 * std::sin(m_phase * 6.0);
        QColor dot = nodeColour;
        dot.setAlphaF(0.5 + 0.5 * beat);
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(line.currentPosition(), 2.5 + beat, 2.5 + beat);
    } else {
        p.setPen(QPen(ink(70), 1.5, Qt::DotLine));
        p.drawLine(QPointF(ekg.left(), ekg.center().y()),
                   QPointF(ekg.right(), ekg.center().y()));
    }
    p.setFont(font());
    p.setPen(ink());
    p.drawText(QRectF(textLeft, cpuTop, textWidth, metrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               v.cpuBusy >= 0.0
                   ? QStringLiteral("CPU %1%").arg(qRound(v.cpuBusy))
                   : QStringLiteral("CPU warming up"));
    p.setFont(small);
    p.setPen(ink(160));
    p.drawText(QRectF(textLeft, cpuTop + metrics.height(), textWidth,
                      smallMetrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               v.load1 >= 0.0
                   ? QStringLiteral("load %1 · %2 · %3")
                         .arg(v.load1, 0, 'f', 2)
                         .arg(v.load5, 0, 'f', 2)
                         .arg(v.load15, 0, 'f', 2)
                   : QString());

    QStringList lines;
    lines.append(QStringLiteral("%1 (%2)").arg(node.name, nodeRole(node.name)));
    if (memory >= 0.0)
        lines.append(QStringLiteral("Memory: %1 used (%2), %3 available of %4")
                         .arg(percent(memory), statusWord(memory),
                              corelogs::compactByteSize(v.memAvailable),
                              corelogs::compactByteSize(v.memTotal)));
    if (swap >= 0.0)
        lines.append(QStringLiteral("Swap: %1 used of %2")
                         .arg(percent(swap),
                              corelogs::compactByteSize(v.swapTotal)));
    if (disk >= 0.0)
        lines.append(QStringLiteral("Disk /: %1 used (%2), %3 free")
                         .arg(percent(disk), statusWord(disk),
                              corelogs::compactByteSize(v.diskFree)));
    if (inodes >= 0.0)
        lines.append(QStringLiteral("Inodes: %1 of %2 used")
                         .arg(number(v.inodesTotal - v.inodesFree),
                              number(v.inodesTotal)));
    if (v.filesOpen >= 0)
        lines.append(QStringLiteral("Open file handles: %1 (limit %2)")
                         .arg(number(v.filesOpen), number(v.filesMax)));
    if (v.cpuBusy >= 0.0)
        lines.append(QStringLiteral("CPU busy: %1%").arg(v.cpuBusy, 0, 'f', 1));
    if (v.procsRunning >= 0)
        lines.append(QStringLiteral("Processes: %1 running of %2")
                         .arg(v.procsRunning)
                         .arg(v.procsTotal));
    lines.append(QStringLiteral("Sampled %1")
                     .arg(agoText(nowMs - node.vitalsReceivedMs)));
    m_hits.append({card, lines.join(QLatin1Char('\n'))});
}

QString FleetVisualizer::toolTipAt(const QPointF &point) const
{
    for (int i = m_hits.size() - 1; i >= 0; --i)
        if (m_hits.at(i).area.contains(point))
            return m_hits.at(i).text;
    return QString();
}

void FleetVisualizer::mouseMoveEvent(QMouseEvent *event)
{
    const QString text = toolTipAt(event->position());
    if (text.isEmpty())
        QToolTip::hideText();
    else
        QToolTip::showText(event->globalPosition().toPoint(), text, this);
    QWidget::mouseMoveEvent(event);
}

void FleetVisualizer::leaveEvent(QEvent *event)
{
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}

} // namespace forkmesh
