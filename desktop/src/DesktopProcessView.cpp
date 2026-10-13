#include "DesktopProcessView.h"

#include "FleetVisualizer.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace forkmesh {

namespace {

constexpr qreal kMargin = 12.0;
constexpr qreal kGap = 10.0;
constexpr qreal kHeaderHeight = 44.0;
constexpr qreal kGaugesHeight = 84.0;
constexpr qreal kSharesHeight = 76.0;
constexpr qreal kTableRow = 20.0;
constexpr int kHistory = 60;
constexpr int kShareGroups = 7;

QString number(qint64 value)
{
    return QLocale().toString(value);
}

QString percentText(double value)
{
    if (value < 0.0)
        return QStringLiteral("-");
    if (value >= 10.0)
        return QStringLiteral("%1%").arg(qRound(value));
    return QStringLiteral("%1%").arg(value, 0, 'f', 1);
}

QString ageText(qint64 seconds)
{
    if (seconds < 0)
        return QStringLiteral("-");
    if (seconds < 120)
        return QStringLiteral("%1 s").arg(seconds);
    if (seconds < 7200)
        return QStringLiteral("%1 min").arg(seconds / 60);
    if (seconds < 2 * 86400)
        return QStringLiteral("%1 h").arg(seconds / 3600);
    return QStringLiteral("%1 d").arg(seconds / 86400);
}

double share(qint64 used, qint64 total)
{
    if (total <= 0 || used < 0)
        return -1.0;
    return qBound(0.0, double(used) / double(total), 1.0);
}

QColor statusColour(double value)
{
    if (value >= 0.9)
        return QColor("#d03b3b");
    if (value >= 0.75)
        return QColor("#fab219");
    return QColor("#0ca30c");
}

QString statusWord(double value)
{
    if (value >= 0.9)
        return QStringLiteral("critical");
    if (value >= 0.75)
        return QStringLiteral("high");
    return QStringLiteral("ok");
}

QColor shareColour(int index, bool dark)
{
    const QStringList groups = FleetVisualizer::verbOrder();
    if (index < groups.size())
        return FleetVisualizer::verbColour(groups.at(index), dark);
    if (index == groups.size())
        return QColor(dark ? "#c7a227" : "#a8850f");
    return QColor(dark ? "#6f8fa6" : "#55708a");
}

template <typename T> void pushHistory(QVector<T> &history, T value)
{
    history.append(value);
    if (history.size() > kHistory)
        history.remove(0, history.size() - kHistory);
}

} // namespace

DesktopProcessView::DesktopProcessView(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("desktopProcessView"));
    setMouseTracking(true);
    m_timer = new QTimer(this);
    m_timer->setInterval(kSampleMs);
    connect(m_timer, &QTimer::timeout, this, [this] { sampleNow(); });
    setMinimumHeight(sizeHint().height());
}

QSize DesktopProcessView::sizeHint() const
{
    const qreal height = 2 * kMargin + kHeaderHeight + kGaugesHeight +
                         kSharesHeight + 22 + kTableRow * (kRows + 1) + 3 * kGap;
    return QSize(900, int(std::ceil(height)));
}

QSize DesktopProcessView::minimumSizeHint() const
{
    return QSize(560, sizeHint().height());
}

bool DesktopProcessView::sampling() const
{
    return m_timer->isActive();
}

void DesktopProcessView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    sampleNow();
    m_timer->start();
}

void DesktopProcessView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

void DesktopProcessView::sampleNow()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    desktopprocs::Snapshot snapshot =
        sampleSource ? sampleSource(now) : m_sampler.sample(now);
    setSnapshot(snapshot);
    if (sampleSource)
        return;
    const QList<desktopprocs::Process> shown = shownProcesses();
    QHash<qint64, QString> commands;
    for (const desktopprocs::Process &process : shown)
        commands.insert(process.pid, m_sampler.commandLine(process.pid));
    for (desktopprocs::Process &process : m_snapshot.processes) {
        auto it = commands.constFind(process.pid);
        if (it != commands.cend())
            process.command = *it;
    }
}

void DesktopProcessView::setSnapshot(const desktopprocs::Snapshot &snapshot)
{
    m_snapshot = snapshot;
    if (snapshot.vitals.valid) {
        if (snapshot.vitals.cpuBusy >= 0.0)
            pushHistory(m_cpuHistory, snapshot.vitals.cpuBusy);
        const double memory = share(
            snapshot.vitals.memTotal - snapshot.vitals.memAvailable,
            snapshot.vitals.memTotal);
        if (memory >= 0.0)
            pushHistory(m_memHistory, memory);
    }
    update();
}

void DesktopProcessView::setSortColumn(SortColumn column)
{
    m_sort = column;
    update();
}

void DesktopProcessView::setForkMeshOnly(bool only)
{
    m_forkmeshOnly = only;
    update();
}

QList<desktopprocs::Process> DesktopProcessView::shownProcesses() const
{
    QList<desktopprocs::Process> rows;
    for (const desktopprocs::Process &process : m_snapshot.processes)
        if (!m_forkmeshOnly || process.forkmesh)
            rows.append(process);
    const SortColumn sort = m_sort;
    std::sort(rows.begin(), rows.end(),
              [sort](const desktopprocs::Process &a, const desktopprocs::Process &b) {
                  switch (sort) {
                  case SortColumn::Memory:
                      if (a.rssBytes != b.rssBytes)
                          return a.rssBytes > b.rssBytes;
                      break;
                  case SortColumn::Threads:
                      if (a.threads != b.threads)
                          return a.threads > b.threads;
                      break;
                  case SortColumn::Name: {
                      const int order = a.name.compare(b.name, Qt::CaseInsensitive);
                      if (order != 0)
                          return order < 0;
                      break;
                  }
                  case SortColumn::Cpu:
                      if (a.cpuPercent != b.cpuPercent)
                          return a.cpuPercent > b.cpuPercent;
                      if (a.rssBytes != b.rssBytes)
                          return a.rssBytes > b.rssBytes;
                      break;
                  }
                  return a.pid < b.pid;
              });
    if (rows.size() > kRows)
        rows = rows.mid(0, kRows);
    return rows;
}

bool DesktopProcessView::dark() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QColor DesktopProcessView::ink(int alpha) const
{
    QColor colour = palette().color(QPalette::WindowText);
    colour.setAlpha(alpha);
    return colour;
}

void DesktopProcessView::paintChip(QPainter &p, QRectF &cursor,
                                   const QString &text, bool active,
                                   std::function<void()> action)
{
    const QFontMetricsF metrics(font());
    const qreal width = metrics.horizontalAdvance(text) + 18;
    const QRectF chip(cursor.right() - width, cursor.top(), width, cursor.height());
    p.setPen(QPen(ink(active ? 160 : 60), 1.0));
    p.setBrush(active ? QBrush(ink(dark() ? 40 : 24)) : QBrush(Qt::NoBrush));
    p.drawRoundedRect(chip, chip.height() / 2, chip.height() / 2);
    p.setPen(ink(active ? 250 : 180));
    p.drawText(chip, Qt::AlignCenter, text);
    m_clicks.append({chip, std::move(action)});
    cursor.setRight(chip.left() - 6);
}

void DesktopProcessView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().color(QPalette::Window));
    m_hits.clear();
    m_clicks.clear();
    const QRectF inner = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
    qreal y = inner.top();
    paintHeader(p, QRectF(inner.left(), y, inner.width(), kHeaderHeight));
    y += kHeaderHeight + kGap;
    if (!m_snapshot.valid) {
        p.setPen(ink(150));
        p.drawText(QRectF(inner.left(), y, inner.width(), 60),
                   Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   m_snapshot.reason.isEmpty()
                       ? QStringLiteral("Sampling this desktop's processes…")
                       : m_snapshot.reason);
        return;
    }
    paintGauges(p, QRectF(inner.left(), y, inner.width(), kGaugesHeight));
    y += kGaugesHeight + kGap;
    paintShares(p, QRectF(inner.left(), y, inner.width(), kSharesHeight));
    y += kSharesHeight + kGap;
    paintTable(p, QRectF(inner.left(), y, inner.width(), inner.bottom() - y));
}

void DesktopProcessView::paintHeader(QPainter &p, const QRectF &area)
{
    QFont title = font();
    title.setPointSizeF(title.pointSizeF() * 1.25);
    title.setBold(true);
    p.setFont(title);
    p.setPen(ink());
    const QFontMetricsF titleMetrics(title);
    p.drawText(QPointF(area.left(), area.top() + titleMetrics.ascent()),
               QStringLiteral("This desktop"));
    p.setFont(font());
    const QFontMetricsF metrics(font());
    p.setPen(ink(160));
    QStringList facts;
    if (m_snapshot.valid) {
        facts.append(QStringLiteral("%1 processes").arg(number(m_snapshot.processes.size())));
        facts.append(QStringLiteral("%1 running").arg(m_snapshot.running));
        facts.append(QStringLiteral("%1 threads").arg(number(m_snapshot.threads)));
        facts.append(QStringLiteral("ForkMesh: %1 processes, %2 CPU, %3")
                         .arg(m_snapshot.forkmeshCount)
                         .arg(percentText(m_snapshot.forkmeshCpu),
                              corelogs::compactByteSize(m_snapshot.forkmeshRss)));
    }
    p.drawText(QRectF(area.left(), area.top() + titleMetrics.height() + 2,
                      area.width(), metrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               metrics.elidedText(facts.join(QStringLiteral(" · ")), Qt::ElideRight,
                                  area.width() * 0.6));
    if (m_snapshot.valid)
        m_hits.append({QRectF(area.left(), area.top(), area.width() * 0.6, area.height()),
                       QStringLiteral("ForkMesh is this desktop app and every process "
                                      "it started (agents, git, builds), %1 in all")
                           .arg(m_snapshot.forkmeshCount)});

    QRectF cursor(area.left(), area.top(), area.width(), metrics.height() + 6);
    paintChip(p, cursor, QStringLiteral("ForkMesh only"), m_forkmeshOnly,
              [this] { setForkMeshOnly(true); });
    paintChip(p, cursor, QStringLiteral("All"), !m_forkmeshOnly,
              [this] { setForkMeshOnly(false); });
    cursor.setRight(cursor.right() - 10);
    paintChip(p, cursor, QStringLiteral("Name"), m_sort == SortColumn::Name,
              [this] { setSortColumn(SortColumn::Name); });
    paintChip(p, cursor, QStringLiteral("Threads"), m_sort == SortColumn::Threads,
              [this] { setSortColumn(SortColumn::Threads); });
    paintChip(p, cursor, QStringLiteral("Memory"), m_sort == SortColumn::Memory,
              [this] { setSortColumn(SortColumn::Memory); });
    paintChip(p, cursor, QStringLiteral("CPU"), m_sort == SortColumn::Cpu,
              [this] { setSortColumn(SortColumn::Cpu); });
    p.setPen(ink(140));
    p.drawText(QRectF(cursor.right() - 60, cursor.top(), 60, cursor.height()),
               Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Sort by"));
}

void DesktopProcessView::paintGauges(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    const corelogs::NodeVitals &v = m_snapshot.vitals;
    const QFontMetricsF metrics(font());
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    const QFontMetricsF smallMetrics(small);
    const int gauges = 5;
    const qreal width = (area.width() - (gauges - 1) * kGap) / gauges;
    auto card = [&](int index) {
        const QRectF box(area.left() + index * (width + kGap), area.top(), width,
                         area.height());
        p.setPen(QPen(ink(40), 1.0));
        p.setBrush(ink(isDark ? 10 : 6));
        p.drawRoundedRect(box, 8, 8);
        return box.adjusted(10, 8, -10, -8);
    };
    auto title = [&](const QRectF &box, const QString &text, const QString &value) {
        p.setFont(font());
        p.setPen(ink(170));
        p.drawText(QRectF(box.left(), box.top(), box.width(), metrics.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, text);
        p.setPen(ink());
        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.drawText(QRectF(box.left(), box.top(), box.width(), metrics.height()),
                   Qt::AlignRight | Qt::AlignVCenter, value);
        p.setFont(small);
    };
    auto bar = [&](const QRectF &box, double value) {
        const QRectF track(box.left(), box.top() + metrics.height() + 10, box.width(), 10);
        p.setPen(Qt::NoPen);
        p.setBrush(ink(25));
        p.drawRoundedRect(track, 5, 5);
        if (value >= 0.0) {
            p.setBrush(statusColour(value));
            p.drawRoundedRect(QRectF(track.left(), track.top(),
                                     qMax<qreal>(4.0, track.width() * value), 10),
                              5, 5);
        }
        return track.bottom() + 6;
    };
    auto caption = [&](const QRectF &box, qreal top, const QString &text) {
        p.setPen(ink(160));
        p.drawText(QRectF(box.left(), top, box.width(), smallMetrics.height() * 2),
                   Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
    };

    QRectF box = card(0);
    title(box, QStringLiteral("CPU"), percentText(v.cpuBusy));
    const QRectF spark(box.left(), box.top() + metrics.height() + 6, box.width(),
                       box.height() - metrics.height() - smallMetrics.height() - 10);
    if (m_cpuHistory.size() >= 2) {
        QPainterPath line;
        QPainterPath fill;
        for (int i = 0; i < m_cpuHistory.size(); ++i) {
            const qreal x = spark.left() + spark.width() * i / (kHistory - 1) +
                            spark.width() * (kHistory - m_cpuHistory.size()) / (kHistory - 1);
            const qreal yv = spark.bottom() - spark.height() * m_cpuHistory.at(i) / 100.0;
            if (i == 0) {
                line.moveTo(x, yv);
                fill.moveTo(x, spark.bottom());
            } else {
                line.lineTo(x, yv);
            }
            fill.lineTo(x, yv);
        }
        fill.lineTo(spark.right(), spark.bottom());
        fill.closeSubpath();
        QColor accent = FleetVisualizer::verbColour(QStringLiteral("SELECT"), isDark);
        QColor wash = accent;
        wash.setAlphaF(0.2);
        p.setPen(Qt::NoPen);
        p.setBrush(wash);
        p.drawPath(fill);
        p.setPen(QPen(accent, 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
    }
    caption(box, spark.bottom() + 2,
            v.load1 >= 0.0 ? QStringLiteral("load %1 · %2 · %3 on %4 CPUs")
                                 .arg(v.load1, 0, 'f', 2)
                                 .arg(v.load5, 0, 'f', 2)
                                 .arg(v.load15, 0, 'f', 2)
                                 .arg(v.cpus)
                           : QString());
    m_hits.append({box, QStringLiteral("Whole-machine CPU busy over the last %1 "
                                       "samples, one every %2 s")
                            .arg(m_cpuHistory.size())
                            .arg(kSampleMs / 1000)});

    const double memory = share(v.memTotal - v.memAvailable, v.memTotal);
    box = card(1);
    title(box, QStringLiteral("Memory"),
          memory >= 0.0 ? QStringLiteral("%1 %2").arg(percentText(memory * 100),
                                                      statusWord(memory))
                        : QStringLiteral("?"));
    caption(box, bar(box, memory),
            QStringLiteral("%1 available of %2")
                .arg(corelogs::compactByteSize(v.memAvailable),
                     corelogs::compactByteSize(v.memTotal)));

    const double swap = share(v.swapTotal - v.swapFree, v.swapTotal);
    box = card(2);
    title(box, QStringLiteral("Swap"),
          swap >= 0.0 ? percentText(swap * 100) : QStringLiteral("none"));
    caption(box, bar(box, swap),
            v.swapTotal > 0 ? QStringLiteral("%1 free of %2")
                                  .arg(corelogs::compactByteSize(v.swapFree),
                                       corelogs::compactByteSize(v.swapTotal))
                            : QStringLiteral("No swap configured"));

    const double disk = share(v.diskTotal - v.diskFree, v.diskTotal);
    box = card(3);
    title(box, QStringLiteral("Home disk"),
          disk >= 0.0 ? QStringLiteral("%1 %2").arg(percentText(disk * 100),
                                                    statusWord(disk))
                      : QStringLiteral("?"));
    caption(box, bar(box, disk),
            QStringLiteral("%1 free of %2")
                .arg(corelogs::compactByteSize(v.diskFree),
                     corelogs::compactByteSize(v.diskTotal)));

    const double files = share(v.filesOpen, v.filesMax);
    box = card(4);
    title(box, QStringLiteral("Open files"),
          v.filesOpen >= 0 ? number(v.filesOpen) : QStringLiteral("?"));
    caption(box, bar(box, files),
            v.filesMax > 0 ? QStringLiteral("system limit %1").arg(number(v.filesMax))
                           : QString());
    p.setFont(font());
}

void DesktopProcessView::paintShares(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    const QFontMetricsF smallMetrics(small);
    QHash<QString, double> cpu;
    QHash<QString, double> memory;
    double cpuTotal = 0.0;
    double memoryTotal = 0.0;
    for (const desktopprocs::Process &process : m_snapshot.processes) {
        const QString &name = process.name;
        const double load = qMax(0.0, process.cpuPercent);
        cpu[name] += load;
        cpuTotal += load;
        memory[name] += double(process.rssBytes);
        memoryTotal += double(process.rssBytes);
    }
    const qreal half = (area.height() - 4) / 2;
    auto strip = [&](qreal top, const QString &title, const QHash<QString, double> &values,
                     double total, double capacity, bool bytes) {
        p.setFont(font());
        p.setPen(ink());
        const qreal labelWidth = 150.0;
        p.drawText(QRectF(area.left(), top, labelWidth, half), Qt::AlignLeft | Qt::AlignVCenter,
                   title);
        const QRectF bar(area.left() + labelWidth, top + 6, area.width() - labelWidth,
                         half - 12);
        p.setPen(Qt::NoPen);
        p.setBrush(ink(18));
        p.drawRoundedRect(bar, 4, 4);
        if (total <= 0.0)
            return;
        QList<QPair<QString, double>> rows;
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            rows.append({it.key(), it.value()});
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        double rest = 0.0;
        for (int i = kShareGroups; i < rows.size(); ++i)
            rest += rows.at(i).second;
        if (rows.size() > kShareGroups)
            rows = rows.mid(0, kShareGroups);
        if (rest > 0.0)
            rows.append({QStringLiteral("everything else"), rest});
        const double scale = qMax(total, capacity);
        qreal x = bar.left();
        for (int i = 0; i < rows.size(); ++i) {
            const qreal width = bar.width() * rows.at(i).second / scale;
            if (width < 0.5)
                continue;
            const QRectF segment(x, bar.top(), width, bar.height());
            p.setBrush(i == rows.size() - 1 && rest > 0.0 ? ink(60)
                                                           : shareColour(i, isDark));
            p.setPen(QPen(palette().color(QPalette::Window), 1.0));
            p.drawRect(segment);
            const QString value =
                bytes ? corelogs::compactByteSize(qint64(rows.at(i).second))
                      : percentText(rows.at(i).second);
            const QString label = rows.at(i).first + QLatin1Char(' ') + value;
            if (width > smallMetrics.horizontalAdvance(label) + 8) {
                p.setFont(small);
                p.setPen(Qt::white);
                p.drawText(segment, Qt::AlignCenter, label);
            }
            m_hits.append({segment, QStringLiteral("%1: %2 (%3% of %4)")
                                        .arg(rows.at(i).first, value)
                                        .arg(qRound(100.0 * rows.at(i).second / total))
                                        .arg(bytes ? QStringLiteral("resident memory")
                                                   : QStringLiteral("CPU in use"))});
            x += width;
        }
    };
    strip(area.top(), QStringLiteral("Where the CPU goes"), cpu, cpuTotal,
          100.0 * qMax(1, m_snapshot.vitals.cpus), false);
    strip(area.top() + half + 4, QStringLiteral("Where memory goes"), memory,
          memoryTotal, double(m_snapshot.vitals.memTotal), true);
}

void DesktopProcessView::paintTable(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    QFont bold = font();
    bold.setBold(true);
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    const QFontMetricsF metrics(font());
    const QFontMetricsF smallMetrics(small);
    p.setFont(bold);
    p.setPen(ink());
    p.drawText(QPointF(area.left(), area.top() + metrics.ascent()),
               m_forkmeshOnly ? QStringLiteral("ForkMesh processes")
                              : QStringLiteral("Running processes"));
    const QList<desktopprocs::Process> rows = shownProcesses();
    double cpuPeak = 1.0;
    qint64 memoryPeak = 1;
    for (const desktopprocs::Process &process : rows) {
        cpuPeak = qMax(cpuPeak, process.cpuPercent);
        memoryPeak = qMax(memoryPeak, process.rssBytes);
    }
    const QList<QPair<QString, qreal>> columns{
        {QStringLiteral("Process"), 0}, {QStringLiteral("PID"), 70},
        {QStringLiteral("User"), 90}, {QStringLiteral("CPU"), 150},
        {QStringLiteral("Memory"), 150}, {QStringLiteral("Threads"), 64},
        {QStringLiteral("State"), 110}, {QStringLiteral("Age"), 64}};
    qreal fixed = 0;
    for (const auto &column : columns)
        fixed += column.second;
    const qreal nameWidth = qMax<qreal>(120.0, area.width() - fixed);
    p.setFont(small);
    const qreal headerY = area.top() + 22;
    qreal x = area.left();
    for (const auto &column : columns) {
        const qreal width = column.second > 0 ? column.second : nameWidth;
        p.setPen(ink(150));
        p.drawText(QRectF(x, headerY, width - 6, kTableRow), Qt::AlignLeft | Qt::AlignVCenter,
                   column.first);
        x += width;
    }
    p.setPen(QPen(ink(40), 1.0));
    p.drawLine(QPointF(area.left(), headerY + kTableRow),
               QPointF(area.right(), headerY + kTableRow));
    qreal y = headerY + kTableRow;
    const QColor accent(corelogs::nodeInkColour(QStringLiteral("n1")));
    for (int i = 0; i < rows.size(); ++i) {
        const desktopprocs::Process &process = rows.at(i);
        const QRectF row(area.left(), y, area.width(), kTableRow);
        if (i % 2 == 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(ink(isDark ? 8 : 5));
            p.drawRect(row);
        }
        x = area.left();
        if (process.forkmesh) {
            p.setPen(Qt::NoPen);
            p.setBrush(accent);
            p.drawEllipse(QPointF(x + 5, y + kTableRow / 2), 3.5, 3.5);
        }
        p.setPen(ink(process.state == QLatin1Char('Z') ? 120 : 230));
        p.drawText(QRectF(x + 14, y, nameWidth - 20, kTableRow),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(process.self
                                               ? process.name + QStringLiteral(" (this app)")
                                               : process.name,
                                           Qt::ElideRight, nameWidth - 20));
        x += nameWidth;
        p.setPen(ink(170));
        p.drawText(QRectF(x, y, 64, kTableRow), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::number(process.pid));
        x += 70;
        p.drawText(QRectF(x, y, 84, kTableRow), Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(process.user, Qt::ElideRight, 84));
        x += 90;
        auto meter = [&](double value, double peak, const QString &text,
                         const QColor &colour) {
            const QRectF track(x, y + 5, 80, kTableRow - 10);
            p.setPen(Qt::NoPen);
            p.setBrush(ink(18));
            p.drawRoundedRect(track, 3, 3);
            if (value > 0.0) {
                p.setBrush(colour);
                p.drawRoundedRect(QRectF(track.left(), track.top(),
                                         qMax<qreal>(2.0, track.width() * qMin(1.0, value / peak)),
                                         track.height()),
                                  3, 3);
            }
            p.setPen(ink(220));
            p.drawText(QRectF(track.right() + 6, y, 60, kTableRow),
                       Qt::AlignLeft | Qt::AlignVCenter, text);
            x += 150;
        };
        meter(process.cpuPercent, cpuPeak, percentText(process.cpuPercent),
              FleetVisualizer::verbColour(QStringLiteral("SELECT"), isDark));
        meter(double(process.rssBytes), double(memoryPeak),
              corelogs::compactByteSize(process.rssBytes),
              FleetVisualizer::verbColour(QStringLiteral("UPDATE"), isDark));
        p.setPen(ink(200));
        p.drawText(QRectF(x, y, 58, kTableRow), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::number(process.threads));
        x += 64;
        p.drawText(QRectF(x, y, 104, kTableRow), Qt::AlignLeft | Qt::AlignVCenter,
                   desktopprocs::stateWord(process.state));
        x += 110;
        p.drawText(QRectF(x, y, 58, kTableRow), Qt::AlignLeft | Qt::AlignVCenter,
                   ageText(process.ageSeconds));
        QStringList detail{QStringLiteral("%1 (pid %2, parent %3)")
                               .arg(process.name)
                               .arg(process.pid)
                               .arg(process.parentPid)};
        if (!process.command.isEmpty())
            detail.append(process.command);
        detail.append(QStringLiteral("CPU %1 · memory %2 · %3 threads · %4 · user %5")
                          .arg(percentText(process.cpuPercent),
                               corelogs::compactByteSize(process.rssBytes))
                          .arg(process.threads)
                          .arg(desktopprocs::stateWord(process.state), process.user));
        if (process.forkmesh)
            detail.append(process.self ? QStringLiteral("This ForkMesh desktop")
                                       : QStringLiteral("Started by this ForkMesh "
                                                        "desktop"));
        m_hits.append({row, detail.join(QLatin1Char('\n'))});
        y += kTableRow;
    }
    if (rows.isEmpty()) {
        p.setPen(ink(140));
        p.drawText(QRectF(area.left(), y + 6, area.width(), kTableRow),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("No processes."));
    }
    p.setFont(font());
}

QString DesktopProcessView::toolTipAt(const QPointF &point) const
{
    for (int i = m_hits.size() - 1; i >= 0; --i)
        if (m_hits.at(i).area.contains(point))
            return m_hits.at(i).text;
    return QString();
}

bool DesktopProcessView::clickAt(const QPointF &point)
{
    for (const ClickHit &hit : std::as_const(m_clicks)) {
        if (hit.area.contains(point)) {
            const auto action = hit.action;
            action();
            return true;
        }
    }
    return false;
}

void DesktopProcessView::mouseMoveEvent(QMouseEvent *event)
{
    const QString text = toolTipAt(event->position());
    if (text.isEmpty())
        QToolTip::hideText();
    else
        QToolTip::showText(event->globalPosition().toPoint(), text, this);
    bool overClick = false;
    for (const ClickHit &hit : std::as_const(m_clicks))
        overClick |= hit.area.contains(event->position());
    setCursor(overClick ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}

void DesktopProcessView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && clickAt(event->position())) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DesktopProcessView::leaveEvent(QEvent *event)
{
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}

} // namespace forkmesh
