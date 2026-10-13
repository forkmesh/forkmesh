#include "AgentResourceGovernor.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SystemPressure.h"
#include "SystemStats.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace ar = forkmesh::agentresources;
using namespace forkmesh::ui;

namespace {

constexpr int kGovernorPageIntervalMs = 2000;
constexpr int kSortRole = Qt::UserRole + 1;

enum UnitColumn {
    UnitName = 0,
    UnitMemory,
    UnitCpu,
    UnitState,
    UnitSeen,
    UnitFrozen,
    UnitNote
};
enum EventColumn { EventTime = 0, EventAction, EventUnit, EventReason };

class SortableItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant a = data(column, kSortRole);
        const QVariant b = other.data(column, kSortRole);
        if (a.isValid() && b.isValid())
            return a.toLongLong() < b.toLongLong();
        return text(column).compare(other.text(column), Qt::CaseInsensitive) < 0;
    }
};

struct GovernorPageSample {
    ar::GovernorSnapshot snapshot;
    ar::SliceCounters slice;
    forkmesh::SystemPressureSnapshot machine;
};

QString byteSize(qint64 bytes)
{
    if (bytes < 0)
        return QString::fromUtf8("\xE2\x80\x94");
    if (bytes == std::numeric_limits<qint64>::max())
        return QStringLiteral("no limit");
    return SystemStats::formatBytes(bytes);
}

QString percent(double value)
{
    return QStringLiteral("%1 %").arg(value, 0, 'f', 1);
}

QString duration(qint64 ms)
{
    if (ms < 0)
        return QString::fromUtf8("\xE2\x80\x94");
    const qint64 seconds = ms / 1000;
    if (seconds < 60)
        return QStringLiteral("%1 s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1 min %2 s").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1 h %2 min").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString cores(double value)
{
    return QStringLiteral("%1 cores").arg(value, 0, 'f', 1);
}

QString clockTime(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("h:mm:ss AP"));
}

QString yesNo(bool value)
{
    return value ? QStringLiteral("Yes") : QStringLiteral("No");
}

QTreeWidget *makeGovernorTree(const QStringList &headers)
{
    auto *tree = new QTreeWidget;
    tree->setColumnCount(headers.size());
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setStretchLastSection(true);
    for (int c = 0; c + 1 < headers.size(); ++c)
        tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    return tree;
}

QString unitNote(const ar::UnitSample &unit, const ar::GovernorPolicy &policy,
                 const QString &candidate)
{
    if (unit.name.startsWith(QLatin1String("forkmesh-agent-")))
        return QStringLiteral("Agent CLI scope, never frozen");
    if (unit.name.startsWith(QLatin1String("forkmesh-action-")))
        return QStringLiteral("Actions step, never frozen");
    if (!ar::freezable(unit.name))
        return QStringLiteral("Not a service or scope");
    if (unit.frozen)
        return QStringLiteral("Paused; resumes when the stall clears");
    if (unit.name == candidate)
        return QStringLiteral("Next to freeze if the stall reaches %1")
            .arg(percent(policy.freezeAbovePercent));
    if (unit.memoryBytes < policy.minFreezeBytes)
        return QStringLiteral("Under %1, skipped").arg(byteSize(policy.minFreezeBytes));
    return QStringLiteral("Can be frozen");
}

QTreeWidgetItem *settingsRow(QTreeWidget *tree, const QString &group,
                             const QString &name)
{
    QTreeWidgetItem *parent = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->text(0) == group)
            parent = tree->topLevelItem(i);
    if (!parent) {
        parent = new QTreeWidgetItem(tree, {group});
        QFont bold = parent->font(0);
        bold.setBold(true);
        parent->setFont(0, bold);
        parent->setFirstColumnSpanned(true);
        parent->setExpanded(true);
    }
    for (int i = 0; i < parent->childCount(); ++i)
        if (parent->child(i)->text(0) == name)
            return parent->child(i);
    return new QTreeWidgetItem(parent, {name});
}

}

QWidget *MainWindow::buildGovernorSection()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("governorSection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Agent governor"));
    title->setObjectName(QStringLiteral("pageTitle"));
    title->setToolTip(QStringLiteral(
        "The agent governor: agents and the builds they start share one "
        "memory budget, and under a sustained memory stall the newest build "
        "is paused until the others finish. Their CPU is capped below the "
        "machine's core count, and the cap drops while other programs need "
        "the CPU. Read live while this page is open."));
    heading->addWidget(title);

    m_governorStatus = new QLabel;
    m_governorStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_governorStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    heading->addWidget(m_governorStatus);
    heading->addStretch();

    auto *memory = new QPushButton(QStringLiteral("Memory"));
    memory->setObjectName(QStringLiteral("ghostButton"));
    memory->setCursor(Qt::PointingHandCursor);
    memory->setToolTip(QStringLiteral("Back to this app's own memory page"));
    setOcticon(memory, "database", 14);
    connect(memory, &QPushButton::clicked, this,
            [this] { showSection(kMemorySectionIndex); });
    heading->addWidget(memory);

    auto *settings = new QPushButton(QStringLiteral("Settings"));
    settings->setObjectName(QStringLiteral("ghostButton"));
    settings->setCursor(Qt::PointingHandCursor);
    settings->setToolTip(QStringLiteral(
        "Settings > Agents holds the switch for the governor; it applies after "
        "a restart"));
    setOcticon(settings, "gear", 14);
    connect(settings, &QPushButton::clicked, this, [this] { showSection(1); });
    heading->addWidget(settings);

    auto *copy = new QPushButton(QStringLiteral("Copy report"));
    copy->setObjectName(QStringLiteral("ghostButton"));
    copy->setCursor(Qt::PointingHandCursor);
    copy->setToolTip(QStringLiteral(
        "Copy the governor's status, settings, units and activity as plain "
        "text"));
    setOcticon(copy, "copy", 14);
    connect(copy, &QPushButton::clicked, this, [this] {
        if (m_governorLastReport.isEmpty())
            return;
        QApplication::clipboard()->setText(m_governorLastReport);
        flashMessage(QStringLiteral("Governor report copied"));
    });
    heading->addWidget(copy);
    outer->addLayout(heading);

    m_governorSummary = new QLabel;
    m_governorSummary->setObjectName(QStringLiteral("mutedLabel"));
    m_governorSummary->setWordWrap(true);
    m_governorSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(m_governorSummary);

    auto *tiles = new QGridLayout;
    tiles->setHorizontalSpacing(10);
    tiles->setVerticalSpacing(10);
    struct TileSpec {
        const char *id;
        const char *title;
        const char *hint;
    };
    static const TileSpec kTiles[] = {
        {"pressure", "Memory stall",
         "The larger of the machine's and the slice's memory PSI \"full\" "
         "avg10: the share of the last ten seconds in which nothing made "
         "progress waiting on memory. This is what the governor acts on"},
        {"machine", "Machine stall", "Memory PSI for the whole machine"},
        {"slice", "Slice stall", "Memory PSI for the agent slice alone"},
        {"available", "Available memory",
         "MemAvailable: what the kernel could hand out without swapping"},
        {"sliceMemory", "Slice memory",
         "Memory charged to the agent slice right now, against its limits"},
        {"units", "Units", "Services and scopes inside the agent slice"},
        {"actions", "Freezes / thaws", "What the governor did since it started"},
        {"tick", "Last check", "When the governor last sampled the slice"},
        {"machineCpu", "Machine CPU",
         "Share of all cores busy between the governor's last two checks"},
        {"sliceCpu", "Agent CPU",
         "Cores the agent slice used between the governor's last two checks"},
        {"cpuCap", "Agent CPU cap",
         "The slice's CPUQuota right now: the ceiling minus what other "
         "programs use, never below the floor"},
        {"cpuThrottle", "CPU throttling",
         "Scheduler periods in which the agent slice hit its CPU cap"},
    };
    int tileIndex = 0;
    for (const TileSpec &spec : kTiles) {
        auto *frame = new QFrame;
        frame->setObjectName(QStringLiteral("statsTile"));
        frame->setFrameShape(QFrame::StyledPanel);
        frame->setToolTip(QString::fromUtf8(spec.hint));
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(2);
        auto *name = new QLabel(QString::fromUtf8(spec.title));
        name->setObjectName(QStringLiteral("mutedLabel"));
        layout->addWidget(name);
        auto *value = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
        QFont big = value->font();
        big.setPointSizeF(big.pointSizeF() * 1.6);
        big.setBold(true);
        value->setFont(big);
        layout->addWidget(value);
        auto *note = new QLabel;
        note->setObjectName(QStringLiteral("mutedLabel"));
        layout->addWidget(note);
        m_governorTiles.insert(QString::fromLatin1(spec.id), value);
        m_governorTileNotes.insert(QString::fromLatin1(spec.id), note);
        tiles->addWidget(frame, tileIndex / 4, tileIndex % 4);
        ++tileIndex;
    }
    outer->addLayout(tiles);

    auto *tabs = new QTabWidget;
    m_governorUnits = makeGovernorTree(
        {QStringLiteral("Unit"), QStringLiteral("Memory"), QStringLiteral("CPU"),
         QStringLiteral("State"),
         QStringLiteral("Seen for"), QStringLiteral("Frozen for"),
         QStringLiteral("Governor")});
    m_governorUnits->setObjectName(QStringLiteral("governorUnitsTree"));
    m_governorUnits->setSortingEnabled(true);
    m_governorUnits->sortByColumn(UnitMemory, Qt::DescendingOrder);
    tabs->addTab(m_governorUnits, QStringLiteral("Units"));

    m_governorEvents = makeGovernorTree(
        {QStringLiteral("Time"), QStringLiteral("Action"), QStringLiteral("Unit"),
         QStringLiteral("Reason")});
    m_governorEvents->setObjectName(QStringLiteral("governorEventsTree"));
    tabs->addTab(m_governorEvents, QStringLiteral("Activity"));

    m_governorSettings =
        makeGovernorTree({QStringLiteral("Setting"), QStringLiteral("Value")});
    m_governorSettings->setObjectName(QStringLiteral("governorSettingsTree"));
    m_governorSettings->setRootIsDecorated(true);
    tabs->addTab(m_governorSettings, QStringLiteral("Settings"));
    outer->addWidget(tabs, 1);

    m_governorTimer = new QTimer(page);
    m_governorTimer->setInterval(kGovernorPageIntervalMs);
    connect(m_governorTimer, &QTimer::timeout, this, [this] { sampleGovernorPage(); });
    return page;
}

bool MainWindow::governorPageOnScreen() const
{
    return m_sectionStack && m_governorUnits &&
           m_sectionStack->currentIndex() == kGovernorSectionIndex &&
           isVisible() && !isMinimized();
}

void MainWindow::refreshGovernorPage()
{
    if (!m_governorTimer)
        return;
    m_governorTimer->start();
    sampleGovernorPage();
}

void MainWindow::sampleGovernorPage()
{
    if (!governorPageOnScreen()) {
        if (m_governorTimer)
            m_governorTimer->stop();
        return;
    }
    if (m_governorSampling)
        return;
    m_governorSampling = true;
    const QString controlGroup = ar::governorSnapshot().status.controlGroup;
    runOffThread<GovernorPageSample>(
        [controlGroup] {
            GovernorPageSample sample;
            sample.slice = ar::readSliceCounters(controlGroup);
            sample.machine = forkmesh::cachedSystemPressure(1000);
            return sample;
        },
        [this](GovernorPageSample sample) {
            m_governorSampling = false;
            if (!m_governorUnits)
                return;
            applyGovernorSample(ar::governorSnapshot(), sample.slice,
                                sample.machine);
        });
}

void MainWindow::applyGovernorSample(const ar::GovernorSnapshot &snapshot,
                                     const ar::SliceCounters &slice,
                                     const forkmesh::SystemPressureSnapshot &machine)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const ar::Status &status = snapshot.status;
    const ar::GovernorPolicy &policy = snapshot.policy;
    const ar::Budget &budget = status.budget;
    const bool savedOn = ar::enabled();

    auto setTile = [this](const char *id, const QString &value,
                          const QString &note) {
        if (QLabel *label = m_governorTiles.value(QString::fromLatin1(id)))
            label->setText(value);
        if (QLabel *label = m_governorTileNotes.value(QString::fromLatin1(id)))
            label->setText(note);
    };

    QString state;
    if (snapshot.running)
        state = QStringLiteral("Running since %1").arg(clockTime(snapshot.startedMs));
    else if (!status.available)
        state = QStringLiteral("Off");
    else
        state = QStringLiteral("Starting");
    if (savedOn != status.enabled)
        state += savedOn ? QStringLiteral(" · turned on, applies after a restart")
                         : QStringLiteral(" · turned off, applies after a restart");
    m_governorStatus->setText(state);
    m_governorSummary->setText(ar::describe(status));

    const double used = snapshot.running
                            ? snapshot.pressure.memoryFullAvg10
                            : qMax(machine.valid ? machine.memoryFullAvg10 : 0.0,
                                   slice.fullAvg10);
    QString pressureNote;
    if (used >= policy.freezeAbovePercent)
        pressureNote = QStringLiteral("at or above the %1 freeze line")
                           .arg(percent(policy.freezeAbovePercent));
    else if (used < policy.thawBelowPercent)
        pressureNote = QStringLiteral("below the %1 thaw line")
                           .arg(percent(policy.thawBelowPercent));
    else
        pressureNote = QStringLiteral("between thaw %1 and freeze %2")
                           .arg(percent(policy.thawBelowPercent),
                                percent(policy.freezeAbovePercent));
    setTile("pressure", percent(used), pressureNote);
    setTile("machine",
            machine.valid ? percent(machine.memoryFullAvg10)
                          : QStringLiteral("n/a"),
            machine.valid ? QStringLiteral("some %1 · full avg60 %2")
                                .arg(percent(machine.memorySomeAvg10),
                                     percent(machine.memoryFullAvg60))
                          : QStringLiteral("PSI not exposed"));
    setTile("slice", slice.valid ? percent(slice.fullAvg10) : QStringLiteral("n/a"),
            slice.valid ? QStringLiteral("some %1 · full avg60 %2")
                              .arg(percent(slice.someAvg10), percent(slice.fullAvg60))
                        : QStringLiteral("no slice cgroup"));
    setTile("available", byteSize(machine.memoryAvailableBytes),
            QStringLiteral("a thaw needs %1").arg(byteSize(policy.thawMinAvailableBytes)));
    setTile("sliceMemory", byteSize(slice.currentBytes),
            slice.valid ? QStringLiteral("throttled above %1 · limit %2")
                              .arg(byteSize(slice.highBytes), byteSize(slice.maxBytes))
                        : QStringLiteral("no slice cgroup"));

    const QString candidate =
        ar::nextFreezeCandidate(policy, snapshot.units, snapshot.state);
    int frozen = 0;
    int freezable = 0;
    for (const ar::UnitSample &unit : snapshot.units) {
        if (unit.frozen)
            ++frozen;
        else if (ar::freezable(unit.name))
            ++freezable;
    }
    setTile("units", QString::number(snapshot.units.size()),
            QStringLiteral("%1 frozen · %2 can be frozen").arg(frozen).arg(freezable));

    int freezes = 0;
    int thaws = 0;
    int failures = 0;
    for (const ar::GovernorEvent &event : snapshot.events) {
        if (event.failed)
            ++failures;
        else if (event.action == QLatin1String("freeze"))
            ++freezes;
        else if (event.action == QLatin1String("thaw"))
            ++thaws;
    }
    setTile("actions", QStringLiteral("%1 / %2").arg(freezes).arg(thaws),
            failures ? QStringLiteral("%1 systemctl failures").arg(failures)
                     : QStringLiteral("no failures"));
    setTile("tick",
            snapshot.lastTickMs ? QStringLiteral("%1 ago").arg(
                                      duration(nowMs - snapshot.lastTickMs))
                                : QStringLiteral("Not running"),
            QStringLiteral("every %1").arg(duration(snapshot.tickMs)));

    const ar::CpuInput &cpu = snapshot.cpu;
    const int cpuCap = snapshot.state.cpuQuota > 0 ? snapshot.state.cpuQuota
                                                   : budget.sliceCpuQuota;
    setTile("machineCpu",
            cpu.valid ? percent(100.0 * cpu.machineBusyCores / cpu.machineCpus)
                      : QString::fromUtf8("\xE2\x80\x94"),
            cpu.valid ? QStringLiteral("%1 of %2 · PSI some %3")
                            .arg(cores(cpu.machineBusyCores))
                            .arg(cpu.machineCpus)
                            .arg(percent(cpu.someAvg10))
                      : QStringLiteral("needs two checks"));
    setTile("sliceCpu", cpu.valid ? cores(cpu.sliceCores) : QString::fromUtf8("\xE2\x80\x94"),
            cpu.valid ? QStringLiteral("other programs %1").arg(cores(cpu.otherCores))
                      : QStringLiteral("needs two checks"));
    setTile("cpuCap", cpuCap > 0 ? ar::cpuCores(cpuCap) : QStringLiteral("none"),
            QStringLiteral("floor %1 · ceiling %2")
                .arg(ar::cpuCores(budget.sliceCpuFloor),
                     ar::cpuCores(budget.sliceCpuQuota)));
    setTile("cpuThrottle", QString::number(slice.cpuThrottledPeriods),
            slice.valid ? QStringLiteral("%1 held back · %2 cap changes")
                              .arg(duration(slice.cpuThrottledUsec / 1000))
                              .arg(snapshot.state.cpuChanges)
                        : QStringLiteral("no slice cgroup"));

    QSet<QString> seen;
    QHash<QString, QTreeWidgetItem *> rows;
    for (int i = 0; i < m_governorUnits->topLevelItemCount(); ++i)
        rows.insert(m_governorUnits->topLevelItem(i)->text(UnitName),
                    m_governorUnits->topLevelItem(i));
    m_governorUnits->setSortingEnabled(false);
    for (const ar::UnitSample &unit : snapshot.units) {
        seen.insert(unit.name);
        QTreeWidgetItem *item = rows.value(unit.name);
        if (!item) {
            item = new SortableItem(m_governorUnits);
            item->setText(UnitName, unit.name);
        }
        item->setText(UnitMemory, byteSize(unit.memoryBytes));
        item->setData(UnitMemory, kSortRole, unit.memoryBytes);
        item->setTextAlignment(UnitMemory, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(UnitCpu, unit.cpuCores >= 0.0 ? cores(unit.cpuCores) : QString());
        item->setData(UnitCpu, kSortRole, qint64(std::lround(unit.cpuCores * 100.0)));
        item->setTextAlignment(UnitCpu, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(UnitState, unit.frozen ? QStringLiteral("Frozen")
                                             : QStringLiteral("Running"));
        const qint64 firstSeen = snapshot.state.firstSeenMs.value(unit.name, -1);
        const qint64 seenFor = firstSeen >= 0 ? nowMs - firstSeen : -1;
        item->setText(UnitSeen, duration(seenFor));
        item->setData(UnitSeen, kSortRole, seenFor);
        const qint64 frozenAt = snapshot.state.frozenAtMs.value(unit.name, -1);
        const qint64 frozenFor = frozenAt >= 0 ? nowMs - frozenAt : -1;
        item->setText(UnitFrozen, frozenAt >= 0 ? duration(frozenFor) : QString());
        item->setData(UnitFrozen, kSortRole, frozenFor);
        item->setText(UnitNote, unitNote(unit, policy, candidate));
    }
    for (auto it = rows.cbegin(); it != rows.cend(); ++it)
        if (!seen.contains(it.key()))
            delete it.value();
    m_governorUnits->setSortingEnabled(true);

    const QString eventsKey =
        snapshot.events.isEmpty()
            ? QString()
            : QStringLiteral("%1:%2").arg(snapshot.events.size()).arg(
                  snapshot.events.constLast().atMs);
    if (m_governorEvents->property("eventsKey").toString() != eventsKey ||
        m_governorEvents->topLevelItemCount() == 0) {
        m_governorEvents->setProperty("eventsKey", eventsKey);
        m_governorEvents->clear();
        for (auto it = snapshot.events.crbegin(); it != snapshot.events.crend(); ++it) {
            auto *item = new QTreeWidgetItem(m_governorEvents);
            item->setText(EventTime, clockTime(it->atMs));
            item->setText(EventAction, it->failed ? it->action + QStringLiteral(" (failed)")
                                                  : it->action);
            item->setText(EventUnit, it->unit);
            item->setText(EventReason, it->reason);
            item->setToolTip(EventReason, it->reason);
        }
        if (snapshot.events.isEmpty())
            new QTreeWidgetItem(m_governorEvents,
                                {QString(), QString(), QString(),
                                 snapshot.running
                                     ? QStringLiteral("Nothing frozen or thawed yet")
                                     : QStringLiteral("The governor is not running")});
    }

    const QSettings saved;
    const int reserveSetting =
        saved.value(ar::kReservePercentSetting, ar::kDefaultReservePercent).toInt();
    const int cpuReserveSetting =
        saved.value(ar::kCpuReservePercentSetting, ar::kDefaultCpuReservePercent).toInt();
    struct Row {
        QString group;
        QString name;
        QString value;
    };
    QList<Row> settingsRows = {
        {QStringLiteral("Switch"),
         QStringLiteral("Keep agents from running the machine out of memory and "
                        "CPU (%1)")
             .arg(ar::kEnabledSetting),
         savedOn ? QStringLiteral("On") : QStringLiteral("Off")},
        {QStringLiteral("Switch"), QStringLiteral("On when the desktop started"),
         yesNo(status.enabled)},
        {QStringLiteral("Switch"), QStringLiteral("Containment available"),
         status.available ? QStringLiteral("Yes")
                          : QStringLiteral("No: %1").arg(
                                status.reason.isEmpty() ? QStringLiteral("not prepared")
                                                        : status.reason)},
        {QStringLiteral("Switch"), QStringLiteral("Governor loop running"),
         yesNo(snapshot.running)},
        {QStringLiteral("Budget"),
         QStringLiteral("Reserve percent (%1)").arg(ar::kReservePercentSetting),
         QStringLiteral("%1 % (default %2, clamped %3 to %4)")
             .arg(reserveSetting)
             .arg(ar::kDefaultReservePercent)
             .arg(ar::kMinReservePercent)
             .arg(ar::kMaxReservePercent)},
        {QStringLiteral("Budget"), QStringLiteral("Total memory"), byteSize(budget.totalBytes)},
        {QStringLiteral("Budget"), QStringLiteral("Kept for the desktop and system"),
         byteSize(budget.reserveBytes)},
        {QStringLiteral("Budget"), QStringLiteral("Slice throttled above (MemoryHigh)"),
         byteSize(budget.sliceHighBytes)},
        {QStringLiteral("Budget"), QStringLiteral("Slice hard limit (MemoryMax)"),
         byteSize(budget.sliceMaxBytes)},
        {QStringLiteral("Budget"), QStringLiteral("Each agent throttled above"),
         byteSize(budget.agentHighBytes)},
        {QStringLiteral("Budget"), QStringLiteral("Each agent capped at"),
         byteSize(budget.agentMaxBytes)},
        {QStringLiteral("Budget"), QStringLiteral("Build jobs per agent"),
         QString::number(budget.jobsPerAgent)},
        {QStringLiteral("Budget"), QStringLiteral("OOMPolicy=continue supported"),
         yesNo(status.oomPolicy)},
        {QStringLiteral("CPU budget"),
         QStringLiteral("CPU reserve percent (%1)").arg(ar::kCpuReservePercentSetting),
         QStringLiteral("%1 % (default %2, clamped %3 to %4)")
             .arg(cpuReserveSetting)
             .arg(ar::kDefaultCpuReservePercent)
             .arg(ar::kMinReservePercent)
             .arg(ar::kMaxReservePercent)},
        {QStringLiteral("CPU budget"), QStringLiteral("Cores"),
         QString::number(budget.cpuCount)},
        {QStringLiteral("CPU budget"), QStringLiteral("Kept for the desktop and system"),
         ar::cpuCores(budget.cpuReserve)},
        {QStringLiteral("CPU budget"), QStringLiteral("Slice ceiling (CPUQuota)"),
         QStringLiteral("%1 (%2 %)").arg(ar::cpuCores(budget.sliceCpuQuota))
             .arg(budget.sliceCpuQuota)},
        {QStringLiteral("CPU budget"), QStringLiteral("Slice floor"),
         ar::cpuCores(budget.sliceCpuFloor)},
        {QStringLiteral("CPU budget"), QStringLiteral("Cap now"),
         cpuCap > 0 ? ar::cpuCores(cpuCap) : QStringLiteral("none")},
        {QStringLiteral("CPU budget"), QStringLiteral("Slice CPUWeight"),
         QStringLiteral("50 (other programs 100)")},
        {QStringLiteral("CPU budget"), QStringLiteral("Lower the cap at most every"),
         duration(policy.cpuLowerCooldownMs)},
        {QStringLiteral("CPU budget"), QStringLiteral("Raise the cap at most every"),
         duration(policy.cpuRaiseCooldownMs)},
        {QStringLiteral("CPU budget"), QStringLiteral("Other programs (smoothed)"),
         snapshot.state.otherCores >= 0.0 ? cores(snapshot.state.otherCores)
                                          : QString::fromUtf8("\xE2\x80\x94")},
        {QStringLiteral("CPU budget"), QStringLiteral("Last cap change"),
         snapshot.state.lastCpuChangeMs ? clockTime(snapshot.state.lastCpuChangeMs)
                                        : QStringLiteral("never")},
        {QStringLiteral("Freeze policy"), QStringLiteral("Check interval"),
         duration(snapshot.tickMs)},
        {QStringLiteral("Freeze policy"), QStringLiteral("Freeze at a memory stall of"),
         percent(policy.freezeAbovePercent)},
        {QStringLiteral("Freeze policy"), QStringLiteral("Thaw below a memory stall of"),
         percent(policy.thawBelowPercent)},
        {QStringLiteral("Freeze policy"), QStringLiteral("Thaw needs available memory of"),
         byteSize(policy.thawMinAvailableBytes)},
        {QStringLiteral("Freeze policy"), QStringLiteral("Smallest unit frozen"),
         byteSize(policy.minFreezeBytes)},
        {QStringLiteral("Freeze policy"), QStringLiteral("At most one freeze every"),
         duration(policy.freezeCooldownMs)},
        {QStringLiteral("Freeze policy"), QStringLiteral("At most one thaw every"),
         duration(policy.thawCooldownMs)},
        {QStringLiteral("Freeze policy"), QStringLiteral("Longest a unit stays frozen"),
         duration(policy.maxFrozenMs)},
        {QStringLiteral("Freeze policy"), QStringLiteral("Last freeze"),
         snapshot.state.lastFreezeMs ? clockTime(snapshot.state.lastFreezeMs)
                                     : QStringLiteral("never")},
        {QStringLiteral("Freeze policy"), QStringLiteral("Last thaw"),
         snapshot.state.lastThawMs ? clockTime(snapshot.state.lastThawMs)
                                   : QStringLiteral("never")},
        {QStringLiteral("Freeze policy"), QStringLiteral("Next to freeze"),
         candidate.isEmpty() ? QStringLiteral("nothing eligible") : candidate},
        {QStringLiteral("Slice cgroup"), QStringLiteral("Slice"), ar::sliceName()},
        {QStringLiteral("Slice cgroup"), QStringLiteral("Control group"),
         status.controlGroup.isEmpty() ? QStringLiteral("none") : status.controlGroup},
        {QStringLiteral("Slice cgroup"), QStringLiteral("memory.current"),
         byteSize(slice.currentBytes)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("memory.peak"),
         byteSize(slice.peakBytes)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("memory.swap.current"),
         byteSize(slice.swapBytes)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("memory.high"),
         byteSize(slice.highBytes)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("memory.max"),
         byteSize(slice.maxBytes)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("Throttle events (high)"),
         QString::number(slice.highEvents)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("Limit events (max)"),
         QString::number(slice.maxEvents)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("Out of memory events"),
         QString::number(slice.oomEvents)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("Processes killed (oom_kill)"),
         QString::number(slice.oomKills)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("cpu.max"),
         slice.cpuMaxPercent > 0 ? ar::cpuCores(slice.cpuMaxPercent)
                                 : QStringLiteral("no limit")},
        {QStringLiteral("Slice cgroup"), QStringLiteral("cpu.stat usage"),
         slice.cpuUsageUsec >= 0 ? duration(slice.cpuUsageUsec / 1000)
                                 : QString::fromUtf8("\xE2\x80\x94")},
        {QStringLiteral("Slice cgroup"), QStringLiteral("Throttled periods"),
         QString::number(slice.cpuThrottledPeriods)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("Throttled time"),
         duration(slice.cpuThrottledUsec / 1000)},
        {QStringLiteral("Slice cgroup"), QStringLiteral("CPU PSI some / full (avg10)"),
         QStringLiteral("%1 / %2").arg(percent(slice.cpuSomeAvg10),
                                       percent(slice.cpuFullAvg10))},
        {QStringLiteral("Tools"), QStringLiteral("systemd-run"),
         status.systemdRun.isEmpty() ? QStringLiteral("not found") : status.systemdRun},
        {QStringLiteral("Tools"), QStringLiteral("systemctl"),
         status.systemctl.isEmpty() ? QStringLiteral("not found") : status.systemctl},
        {QStringLiteral("Tools"), QStringLiteral("systemd-run shim folder"),
         status.shimDirectory.isEmpty() ? QStringLiteral("none") : status.shimDirectory},
    };
    for (const QString &line : ar::environmentDefaults(budget)) {
        const int equals = line.indexOf(QLatin1Char('='));
        settingsRows.append({QStringLiteral("Agent environment defaults"),
                             equals > 0 ? line.left(equals) : line,
                             equals > 0 ? line.mid(equals + 1) : QString()});
    }
    for (const Row &row : std::as_const(settingsRows)) {
        QTreeWidgetItem *item = settingsRow(m_governorSettings, row.group, row.name);
        item->setText(1, row.value);
        item->setToolTip(1, row.value);
    }

    QStringList report;
    report << QStringLiteral("Agent governor, %1")
                  .arg(QDateTime::fromMSecsSinceEpoch(nowMs).toString(
                      QStringLiteral("yyyy-MM-dd h:mm:ss AP")))
           << state << ar::describe(status)
           << QStringLiteral("Memory stall used %1 (machine %2, slice %3); "
                             "available %4; slice %5 of %6")
                  .arg(percent(used),
                       machine.valid ? percent(machine.memoryFullAvg10)
                                     : QStringLiteral("n/a"),
                       slice.valid ? percent(slice.fullAvg10) : QStringLiteral("n/a"),
                       byteSize(machine.memoryAvailableBytes), byteSize(slice.currentBytes),
                       byteSize(slice.highBytes))
           << (cpu.valid ? QStringLiteral("CPU: machine %1 busy of %2 cores, agents "
                                          "%3, other programs %4, agent cap %5")
                               .arg(cores(cpu.machineBusyCores))
                               .arg(cpu.machineCpus)
                               .arg(cores(cpu.sliceCores), cores(cpu.otherCores),
                                    ar::cpuCores(cpuCap))
                         : QStringLiteral("CPU: agent cap %1, not sampled yet")
                               .arg(ar::cpuCores(cpuCap)))
           << QString();
    QString group;
    for (const Row &row : std::as_const(settingsRows)) {
        if (row.group != group) {
            group = row.group;
            report << group;
        }
        report << QStringLiteral("  %1: %2").arg(row.name, row.value);
    }
    report << QString() << QStringLiteral("Units");
    for (const ar::UnitSample &unit : snapshot.units)
        report << QStringLiteral("  %1  %2  %3  %4  %5")
                      .arg(unit.name, byteSize(unit.memoryBytes),
                           unit.cpuCores >= 0.0 ? cores(unit.cpuCores)
                                                : QStringLiteral("cpu n/a"),
                           unit.frozen ? QStringLiteral("frozen")
                                       : QStringLiteral("running"),
                           unitNote(unit, policy, candidate));
    report << QString() << QStringLiteral("Activity");
    for (const ar::GovernorEvent &event : snapshot.events)
        report << QStringLiteral("  %1  %2%3  %4  %5")
                      .arg(clockTime(event.atMs), event.action,
                           event.failed ? QStringLiteral(" (failed)") : QString(),
                           event.unit, event.reason);
    m_governorLastReport = report.join(QLatin1Char('\n'));
}
