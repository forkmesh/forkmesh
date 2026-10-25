#include "DesktopProcessView.h"

#include "FleetVisualizer.h"
#include "FontSizing.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QLocale>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
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
constexpr qreal kGovernanceHeight = 58.0;
constexpr qreal kSliderHeight = 20.0;
constexpr int kGovernanceCommitMs = 500;
constexpr qreal kMachineHeight = 132.0;
constexpr qreal kSharesHeight = 76.0;
constexpr qreal kTableRow = 36.0;
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

QString ghzText(double khz)
{
    if (khz <= 0)
        return QStringLiteral("-");
    return QStringLiteral("%1 GHz").arg(khz / 1e6, 0, 'f', 1);
}

QString tempText(double celsius)
{
    return QStringLiteral("%1 \u00b0C").arg(qRound(celsius));
}

QString kindName(const QString &kind)
{
    if (kind == QLatin1String("P"))
        return QStringLiteral("performance");
    if (kind == QLatin1String("E"))
        return QStringLiteral("efficiency");
    return QString();
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
    auto makeSlider = [this](const QString &name, const QString &label,
                             const QString &tip) {
        auto *slider = new QSlider(Qt::Horizontal, this);
        slider->setObjectName(name);
        slider->setAccessibleName(label);
        slider->setToolTip(tip);
        slider->setSingleStep(1);
        slider->setPageStep(5);
        slider->setCursor(Qt::PointingHandCursor);
        slider->hide();
        connect(slider, &QSlider::valueChanged, this, [this] { previewGovernance(); });
        connect(slider, &QSlider::sliderReleased, this, [this] { commitGovernance(); });
        return slider;
    };
    m_cpuSlider = makeSlider(
        QStringLiteral("desktopCpuGovernanceSlider"), QStringLiteral("Agent CPU limit"),
        QStringLiteral("Drag to set how much of this computer's CPU agents, their "
                       "builds and tests may use. The rest is kept for the desktop "
                       "and your other programs, and the governor lowers the agents' "
                       "cap further while other programs are busy."));
    m_memorySlider = makeSlider(
        QStringLiteral("desktopMemoryGovernanceSlider"),
        QStringLiteral("Agent memory limit"),
        QStringLiteral("Drag to set how much memory agents may use before they are "
                       "slowed down. The rest is kept for the desktop and your other "
                       "programs; under a memory stall the newest build is paused "
                       "until it clears."));
    m_governanceCommit = new QTimer(this);
    m_governanceCommit->setSingleShot(true);
    m_governanceCommit->setInterval(kGovernanceCommitMs);
    connect(m_governanceCommit, &QTimer::timeout, this, [this] {
        if (!m_cpuSlider->isSliderDown() && !m_memorySlider->isSliderDown())
            commitGovernance();
    });
    setMinimumHeight(sizeHint().height());
}

QSize DesktopProcessView::sizeHint() const
{
    qreal height = 2 * kMargin + kHeaderHeight + kGaugesHeight + kMachineHeight +
                   kSharesHeight + 22 + kTableRow * (kRows + 1) + 4 * kGap;
    if (governanceShown())
        height += kGovernanceHeight + kGap;
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
    if (m_governanceCommit->isActive())
        commitGovernance();
}

void DesktopProcessView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    placeGovernanceSliders();
}

void DesktopProcessView::sampleNow()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    desktopprocs::Snapshot snapshot =
        sampleSource ? sampleSource(now) : m_sampler.sample(now);
    refreshGovernance();
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
    placeGovernanceSliders();
    update();
}

bool DesktopProcessView::governanceShown() const
{
    return governanceSource && m_governance.available;
}

qreal DesktopProcessView::gaugeWidth(qreal areaWidth) const
{
    return (areaWidth - 4 * kGap) / 5;
}

QRectF DesktopProcessView::governanceArea() const
{
    const QRectF inner = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
    return QRectF(inner.left(), inner.top() + kHeaderHeight + kGap + kGaugesHeight + kGap,
                  inner.width(), kGovernanceHeight);
}

void DesktopProcessView::refreshGovernance()
{
    if (m_governanceCommit->isActive() || m_cpuSlider->isSliderDown() ||
        m_memorySlider->isSliderDown())
        return;
    m_governance = governanceSource ? governanceSource(-1, -1) : Governance();
    {
        const QSignalBlocker cpuBlocker(m_cpuSlider);
        const QSignalBlocker memoryBlocker(m_memorySlider);
        for (QSlider *slider : {m_cpuSlider, m_memorySlider})
            slider->setRange(100 - m_governance.maxReservePercent,
                             100 - m_governance.minReservePercent);
        m_cpuSlider->setValue(100 - m_governance.cpuReservePercent);
        m_memorySlider->setValue(100 - m_governance.memoryReservePercent);
    }
    setMinimumHeight(sizeHint().height());
    placeGovernanceSliders();
    update();
}

void DesktopProcessView::placeGovernanceSliders()
{
    const bool shown = governanceShown() && m_snapshot.valid;
    m_cpuSlider->setVisible(shown);
    m_memorySlider->setVisible(shown);
    if (!shown)
        return;
    const QRectF area = governanceArea();
    const qreal width = gaugeWidth(area.width());
    const qreal top = area.top() + QFontMetricsF(font()).height() + 2;
    m_cpuSlider->setGeometry(
        QRectF(area.left() + 10, top, width - 20, kSliderHeight).toRect());
    m_memorySlider->setGeometry(
        QRectF(area.left() + width + kGap + 10, top, width - 20, kSliderHeight).toRect());
}

void DesktopProcessView::previewGovernance()
{
    if (!governanceSource)
        return;
    m_governance = governanceSource(100 - m_memorySlider->value(),
                                    100 - m_cpuSlider->value());
    m_governanceCommit->start();
    update();
}

void DesktopProcessView::commitGovernance()
{
    m_governanceCommit->stop();
    if (!governanceSource)
        return;
    const int memory = 100 - m_memorySlider->value();
    const int cpu = 100 - m_cpuSlider->value();
    const Governance saved = governanceSource(-1, -1);
    if (onGovernanceChanged &&
        (memory != saved.memoryReservePercent || cpu != saved.cpuReservePercent))
        onGovernanceChanged(memory, cpu);
    refreshGovernance();
}

void DesktopProcessView::paintGovernance(QPainter &p, const QRectF &area)
{
    const QFontMetricsF metrics(font());
    QFont small = forkmesh::scaledFont(font(), 0.85);
    const QFontMetricsF smallMetrics(small);
    const qreal width = gaugeWidth(area.width());
    auto cell = [&](int index, const QString &label, const QSlider *slider,
                    const QString &text) {
        const QRectF box(area.left() + index * (width + kGap) + 10, area.top(),
                         width - 20, area.height());
        const QRectF line(box.left(), box.top(), box.width(), metrics.height());
        p.setFont(font());
        p.setPen(ink(170));
        p.drawText(line, Qt::AlignLeft | Qt::AlignVCenter, label);
        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(ink());
        p.drawText(line, Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1%").arg(slider->value()));
        p.setFont(small);
        p.setPen(ink(160));
        const qreal top = line.bottom() + 2 + kSliderHeight + 2;
        p.drawText(QRectF(box.left(), top, box.width(), smallMetrics.height()),
                   Qt::AlignLeft | Qt::AlignTop,
                   smallMetrics.elidedText(text, Qt::ElideRight, box.width()));
        m_hits.append({box, slider->toolTip() + QStringLiteral("\n\n") + text});
    };
    cell(0, QStringLiteral("Agent CPU limit"), m_cpuSlider, m_governance.cpuText);
    cell(1, QStringLiteral("Agent memory limit"), m_memorySlider, m_governance.memoryText);
    const QRectF note(area.left() + 2 * (width + kGap) + 10, area.top(),
                      area.width() - 2 * (width + kGap) - 10, area.height());
    p.setFont(small);
    p.setPen(ink(150));
    p.drawText(note, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_governance.note);
    m_hits.append({note, m_governance.note});
    p.setFont(font());
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
    if (governanceShown()) {
        paintGovernance(p, QRectF(inner.left(), y, inner.width(), kGovernanceHeight));
        y += kGovernanceHeight + kGap;
    }
    if (m_snapshot.machine.valid) {
        const qreal unit = (inner.width() - 4 * kGap) / 5;
        const qreal coresWidth = 3 * unit + 2 * kGap;
        paintCores(p, QRectF(inner.left(), y, coresWidth, kMachineHeight));
        paintTuning(p, QRectF(inner.left() + coresWidth + kGap, y,
                              inner.width() - coresWidth - kGap, kMachineHeight));
    }
    y += kMachineHeight + kGap;
    paintShares(p, QRectF(inner.left(), y, inner.width(), kSharesHeight));
    y += kSharesHeight + kGap;
    paintTable(p, QRectF(inner.left(), y, inner.width(), inner.bottom() - y));
}

void DesktopProcessView::paintHeader(QPainter &p, const QRectF &area)
{
    QFont title = font();
    title = forkmesh::scaledFont(title, 1.25);
    title.setBold(true);
    p.setFont(title);
    p.setPen(ink());
    const QFontMetricsF titleMetrics(title);
    p.drawText(QPointF(area.left(), area.top() + titleMetrics.ascent()),
               QStringLiteral("Desktop resource diagnostics"));
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
    small = forkmesh::scaledFont(small, 0.85);
    const QFontMetricsF smallMetrics(small);
    const int gauges = 5;
    const qreal width = (area.width() - (gauges - 1) * kGap) / gauges;
    auto card = [&](int index) {
        return paintCard(p, QRectF(area.left() + index * (width + kGap), area.top(), width,
                                   area.height()));
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

QRectF DesktopProcessView::paintCard(QPainter &p, const QRectF &box)
{
    p.setPen(QPen(ink(40), 1.0));
    p.setBrush(ink(dark() ? 10 : 6));
    p.drawRoundedRect(box, 8, 8);
    return box.adjusted(10, 8, -10, -8);
}

void DesktopProcessView::paintCores(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    const desktopprocs::Machine &m = m_snapshot.machine;
    const QFontMetricsF metrics(font());
    QFont small = forkmesh::scaledFont(font(), 0.85);
    const QFontMetricsF smallMetrics(small);
    QFont bold = font();
    bold.setBold(true);
    const QRectF box = paintCard(p, area);

    double khzSum = 0.0;
    int khzCount = 0;
    double hottest = -1.0;
    for (const desktopprocs::Core &core : m.cores) {
        if (core.curKhz > 0) {
            khzSum += core.curKhz;
            ++khzCount;
        }
        hottest = qMax(hottest, core.tempC);
    }
    QStringList headline;
    if (khzCount > 0)
        headline.append(ghzText(khzSum / khzCount) + QStringLiteral(" avg"));
    const double temp = m.packageTempC >= 0.0 ? m.packageTempC : hottest;
    if (temp >= 0.0)
        headline.append(tempText(temp));
    const QString value = headline.join(QStringLiteral(" · "));
    p.setFont(bold);
    const qreal valueWidth = QFontMetricsF(bold).horizontalAdvance(value);
    p.setPen(temp >= 90.0 ? statusColour(1.0) : ink());
    p.drawText(QRectF(box.left(), box.top(), box.width(), metrics.height()),
               Qt::AlignRight | Qt::AlignVCenter, value);
    p.setFont(font());
    p.setPen(ink(170));
    p.drawText(QRectF(box.left(), box.top(), box.width() - valueWidth - 12, metrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               metrics.elidedText(m.cpuModel.isEmpty() ? QStringLiteral("Processor")
                                                       : m.cpuModel,
                                  Qt::ElideRight, box.width() - valueWidth - 12));
    m_hits.append({QRectF(box.left(), box.top(), box.width(), metrics.height()),
                   temp >= 0.0
                       ? QStringLiteral("Average clock across all threads now; %1 is the "
                                        "processor package temperature")
                             .arg(tempText(temp))
                       : QStringLiteral("Average clock across all threads now")});

    QMap<QString, QSet<QPair<int, int>>> physicalByKind;
    for (const desktopprocs::Core &core : m.cores)
        physicalByKind[core.kind].insert({core.package, core.coreId});
    QStringList facts;
    facts.append(QStringLiteral("%1 cores · %2 threads").arg(m.physicalCores).arg(m.cores.size()));
    if (physicalByKind.size() > 1 && physicalByKind.contains(QStringLiteral("P")))
        facts.append(QStringLiteral("%1 P + %2 E")
                         .arg(physicalByKind.value(QStringLiteral("P")).size())
                         .arg(physicalByKind.value(QStringLiteral("E")).size()));
    if (m.cacheL3Bytes > 0)
        facts.append(QStringLiteral("L3 %1").arg(corelogs::compactByteSize(m.cacheL3Bytes)));
    if (m.baseKhz > 0)
        facts.append(QStringLiteral("base %1").arg(ghzText(m.baseKhz)));
    qint64 peak = -1;
    for (const desktopprocs::Core &core : m.cores)
        peak = qMax(peak, core.maxKhz);
    if (peak > 0)
        facts.append(QStringLiteral("max %1").arg(ghzText(peak)));
    p.setFont(small);
    p.setPen(ink(150));
    const qreal factsTop = box.top() + metrics.height() + 2;
    p.drawText(QRectF(box.left(), factsTop, box.width(), smallMetrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               smallMetrics.elidedText(facts.join(QStringLiteral(" · ")), Qt::ElideRight,
                                       box.width()));

    if (m.cores.isEmpty())
        return;
    QList<QPair<QString, QList<int>>> groups;
    for (int i = 0; i < m.cores.size(); ++i) {
        const QString &kind = m.cores.at(i).kind;
        if (groups.isEmpty() || groups.last().first != kind)
            groups.append({kind, {}});
        groups.last().second.append(i);
    }
    bool anyTemp = false;
    for (const desktopprocs::Core &core : m.cores)
        anyTemp |= core.tempC >= 0.0;
    const qreal labelHeight = smallMetrics.height();
    const qreal barsTop = factsTop + smallMetrics.height() + 6;
    const qreal barsBottom = box.bottom() - labelHeight - (anyTemp ? 7 : 2);
    const int count = m.cores.size();
    const qreal groupGap = 10.0;
    qreal gap = 2.0;
    qreal barWidth = (box.width() - groupGap * (groups.size() - 1) - gap * (count - 1)) / count;
    if (barWidth < 4.0) {
        gap = 1.0;
        barWidth = (box.width() - groupGap * (groups.size() - 1) - gap * (count - 1)) / count;
    }
    const QColor accent = FleetVisualizer::verbColour(QStringLiteral("SELECT"), isDark);
    qreal x = box.left();
    for (const auto &group : groups) {
        const qreal groupLeft = x;
        double groupKhz = 0.0;
        int groupKhzCount = 0;
        double groupBusy = 0.0;
        int groupBusyCount = 0;
        double groupHot = -1.0;
        for (const int index : group.second) {
            const desktopprocs::Core &core = m.cores.at(index);
            const QRectF track(x, barsTop, barWidth, barsBottom - barsTop);
            p.setPen(Qt::NoPen);
            p.setBrush(ink(18));
            p.drawRoundedRect(track, 2, 2);
            if (core.busy > 0.0) {
                const qreal height = qMax<qreal>(1.5, track.height() * core.busy / 100.0);
                p.setBrush(accent);
                p.drawRoundedRect(QRectF(track.left(), track.bottom() - height, track.width(),
                                         height),
                                  2, 2);
            }
            if (core.tempC >= 0.0) {
                p.setBrush(statusColour(core.tempC / 100.0));
                p.drawRoundedRect(QRectF(track.left(), track.bottom() + 3, track.width(), 3),
                                  1.5, 1.5);
            }
            QStringList tip{QStringLiteral("CPU %1").arg(core.cpu)};
            const QString kind = kindName(core.kind);
            tip.last() += kind.isEmpty()
                              ? QStringLiteral(" · core %1").arg(core.coreId)
                              : QStringLiteral(" · %1 core %2").arg(kind).arg(core.coreId);
            QStringList now;
            if (core.busy >= 0.0)
                now.append(QStringLiteral("%1 busy").arg(percentText(core.busy)));
            if (core.curKhz > 0)
                now.append(core.maxKhz > 0 ? QStringLiteral("%1 of %2").arg(ghzText(core.curKhz),
                                                                             ghzText(core.maxKhz))
                                           : ghzText(core.curKhz));
            if (core.tempC >= 0.0)
                now.append(tempText(core.tempC));
            if (!now.isEmpty())
                tip.append(now.join(QStringLiteral(" · ")));
            m_hits.append({QRectF(x, barsTop, barWidth + gap, box.bottom() - barsTop - labelHeight),
                           tip.join(QLatin1Char('\n'))});
            if (core.curKhz > 0) {
                groupKhz += core.curKhz;
                ++groupKhzCount;
            }
            if (core.busy >= 0.0) {
                groupBusy += core.busy;
                ++groupBusyCount;
            }
            groupHot = qMax(groupHot, core.tempC);
            x += barWidth + gap;
        }
        const qreal groupRight = x - gap;
        QStringList label;
        if (!group.first.isEmpty())
            label.append(QStringLiteral("%1-cores").arg(group.first));
        else
            label.append(QStringLiteral("%1 threads").arg(group.second.size()));
        if (groupBusyCount > 0)
            label.append(percentText(groupBusy / groupBusyCount));
        if (groupKhzCount > 0)
            label.append(ghzText(groupKhz / groupKhzCount));
        if (groupHot >= 0.0)
            label.append(QStringLiteral("up to ") + tempText(groupHot));
        p.setFont(small);
        p.setPen(ink(150));
        p.drawText(QRectF(groupLeft, box.bottom() - labelHeight, groupRight - groupLeft,
                          labelHeight),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(label.join(QStringLiteral(" · ")), Qt::ElideRight,
                                           groupRight - groupLeft));
        x += groupGap - gap;
    }
    p.setFont(font());
}

void DesktopProcessView::paintTuning(QPainter &p, const QRectF &area)
{
    const desktopprocs::Machine &m = m_snapshot.machine;
    const QFontMetricsF metrics(font());
    QFont small = forkmesh::scaledFont(font(), 0.85);
    const QFontMetricsF smallMetrics(small);
    const QRectF box = paintCard(p, area);
    p.setFont(font());
    p.setPen(ink(170));
    p.drawText(QRectF(box.left(), box.top(), box.width(), metrics.height()),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("CPU and memory settings"));

    struct Entry {
        QString key;
        QString value;
        QString tip;
        bool warn = false;
    };
    QList<Entry> entries;
    auto join = [](const QStringList &parts) {
        QStringList kept;
        for (const QString &part : parts)
            if (!part.isEmpty())
                kept.append(part);
        return kept.join(QStringLiteral(" · "));
    };
    const QString driver = m.driver.isEmpty()
                               ? QString()
                               : m.driverStatus.isEmpty()
                                   ? m.driver
                                   : QStringLiteral("%1 %2").arg(m.driver, m.driverStatus);
    entries.append({QStringLiteral("Governor"), join({m.governor, driver}),
                    QStringLiteral("CPU frequency governor and the driver applying it.%1")
                        .arg(m.governors.isEmpty()
                                 ? QString()
                                 : QStringLiteral("\nAvailable: %1")
                                       .arg(m.governors.join(QStringLiteral(", "))))});
    entries.append({QStringLiteral("Energy pref"), m.energyPreference,
                    QStringLiteral("Energy-performance preference (EPP): the hint the "
                                   "processor uses to trade speed for power")});
    QString clocks;
    if (m.minKhz > 0 && m.maxKhz > 0)
        clocks = QStringLiteral("%1–%2").arg(QString::number(m.minKhz / 1e6, 'f', 1),
                                                  ghzText(m.maxKhz));
    entries.append({QStringLiteral("Clocks"),
                    join({clocks, m.turbo < 0 ? QString()
                                  : m.turbo ? QStringLiteral("turbo on")
                                            : QStringLiteral("turbo off")}),
                    QStringLiteral("The range the governor may pick from, and whether "
                                   "turbo/boost clocks are allowed"),
                    m.turbo == 0});
    QStringList pressure;
    if (m.pressureCpu >= 0.0)
        pressure.append(QStringLiteral("CPU %1").arg(percentText(m.pressureCpu)));
    if (m.pressureMemory >= 0.0)
        pressure.append(QStringLiteral("memory %1").arg(percentText(m.pressureMemory)));
    if (m.pressureIo >= 0.0)
        pressure.append(QStringLiteral("I/O %1").arg(percentText(m.pressureIo)));
    entries.append({QStringLiteral("Stalled"), join(pressure),
                    QStringLiteral("Pressure stall information: the share of the last 10 s "
                                   "in which some task waited for CPU, memory or I/O"),
                    qMax(m.pressureMemory, m.pressureIo) >= 10.0 || m.pressureCpu >= 50.0});
    entries.append({QStringLiteral("Huge pages"),
                    join({m.hugePages, m.hugePagesDefrag.isEmpty()
                                           ? QString()
                                           : QStringLiteral("defrag ") + m.hugePagesDefrag}),
                    QStringLiteral("Transparent huge pages: when the kernel backs memory with "
                                   "2 MB pages, and when it compacts memory to make them")});
    QStringList swap;
    if (m.swappiness >= 0)
        swap.append(QStringLiteral("swappiness %1").arg(m.swappiness));
    if (!m.zswap.isEmpty())
        swap.append(QStringLiteral("zswap ") + m.zswap);
    for (const desktopprocs::SwapDevice &device : m.swaps)
        swap.append(QStringLiteral("%1 %2").arg(device.type,
                                                corelogs::compactByteSize(device.sizeBytes)));
    if (m.swaps.isEmpty() && m.swappiness >= 0)
        swap.append(QStringLiteral("no swap"));
    entries.append({QStringLiteral("Swap"), join(swap),
                    QStringLiteral("How eagerly memory is swapped out (0–200), whether "
                                   "compressed swap is on, and each swap device")});
    QStringList memory;
    if (m.overcommit >= 0)
        memory.append(QStringLiteral("overcommit %1")
                          .arg(m.overcommit == 0   ? QStringLiteral("heuristic")
                               : m.overcommit == 1 ? QStringLiteral("always")
                                                   : QStringLiteral("strict")));
    if (m.dirtyRatio >= 0 && m.dirtyBackgroundRatio >= 0)
        memory.append(QStringLiteral("dirty %1/%2%").arg(m.dirtyRatio).arg(m.dirtyBackgroundRatio));
    if (m.cachePressure >= 0)
        memory.append(QStringLiteral("cache pressure %1").arg(m.cachePressure));
    entries.append({QStringLiteral("Memory"), join(memory),
                    QStringLiteral("vm.overcommit_memory, the dirty page limits (writer "
                                   "blocks / background flush) and vfs_cache_pressure")});
    QStringList system;
    if (!m.kernel.isEmpty())
        system.append(QStringLiteral("Linux ") + m.kernel);
    if (m_snapshot.vitals.uptimeSeconds >= 0)
        system.append(QStringLiteral("up ") + ageText(m_snapshot.vitals.uptimeSeconds));
    if (!m.smt.isEmpty())
        system.append(QStringLiteral("SMT ") + m.smt);
    if (m.numaNodes > 1)
        system.append(QStringLiteral("%1 NUMA nodes").arg(m.numaNodes));
    entries.append({QStringLiteral("System"), join(system),
                    QStringLiteral("Kernel release, time since boot and whether "
                                   "hyper-threading (SMT) is enabled")});
    QList<Entry> shown;
    for (const Entry &entry : std::as_const(entries))
        if (!entry.value.isEmpty())
            shown.append(entry);

    const int columns = box.width() >= 440 ? 2 : 1;
    const int rowsPerColumn = int(std::ceil(double(shown.size()) / columns));
    const qreal top = box.top() + metrics.height() + 4;
    const qreal rowHeight = rowsPerColumn > 0 ? (box.bottom() - top) / rowsPerColumn : 0.0;
    const qreal columnWidth = (box.width() - (columns - 1) * 14) / columns;
    const qreal keyWidth = qMin<qreal>(84.0, columnWidth * 0.4);
    p.setFont(small);
    for (int i = 0; i < shown.size(); ++i) {
        const int column = i / qMax(1, rowsPerColumn);
        const int row = i % qMax(1, rowsPerColumn);
        if (column >= columns || top + (row + 1) * rowHeight > box.bottom() + 1)
            continue;
        const QRectF cell(box.left() + column * (columnWidth + 14), top + row * rowHeight,
                          columnWidth, rowHeight);
        p.setPen(ink(140));
        p.drawText(QRectF(cell.left(), cell.top(), keyWidth, cell.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(shown.at(i).key, Qt::ElideRight, keyWidth - 4));
        p.setPen(shown.at(i).warn ? statusColour(0.8) : ink(225));
        p.drawText(QRectF(cell.left() + keyWidth, cell.top(), cell.width() - keyWidth,
                          cell.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(shown.at(i).value, Qt::ElideRight,
                                           cell.width() - keyWidth));
        m_hits.append({cell, QStringLiteral("%1: %2\n%3").arg(shown.at(i).key, shown.at(i).value,
                                                             shown.at(i).tip)});
    }
    p.setFont(font());
}

void DesktopProcessView::paintShares(QPainter &p, const QRectF &area)
{
    const bool isDark = dark();
    QFont small = font();
    small = forkmesh::scaledFont(small, 0.85);
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
    small = forkmesh::scaledFont(small, 0.9);
    const QFontMetricsF metrics(font());
    const QFontMetricsF smallMetrics(small);
    p.setFont(bold);
    p.setPen(ink());
    p.drawText(QPointF(area.left(), area.top() + metrics.ascent()),
               m_forkmeshOnly ? QStringLiteral("ForkMesh processes")
                              : QStringLiteral("Running processes"));
    p.setPen(ink(150));
    p.drawText(QRectF(area.left() + 220, area.top(), area.width() - 220,
                      metrics.height()),
               Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("Select a process to inspect or stop it"));
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
        p.drawText(QRectF(x + 14, y + 2, nameWidth - 20, kTableRow / 2),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(process.self
                                               ? process.name + QStringLiteral(" (this app)")
                                               : process.name,
                                           Qt::ElideRight, nameWidth - 20));
        p.setPen(ink(140));
        p.drawText(QRectF(x + 14, y + kTableRow / 2 - 2, nameWidth - 20,
                          kTableRow / 2),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   smallMetrics.elidedText(desktopprocs::descriptionFor(process),
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
        m_clicks.append({row, [this, process] {
                             if (onProcessClicked)
                                 onProcessClicked(process);
                         }});
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
