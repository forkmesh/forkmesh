// Memory: where this process's memory is, live. Every mapping the kernel has
// for us, grouped (heap, anonymous blocks, thread stacks, libraries, fonts,
// shared buffers...) with resident, private, shared, proportional and swapped
// bytes; the allocator's in-use versus held-but-free split; and the app's own
// counts for the caches and buffers it knows it keeps. A stacked chart keeps
// the last few minutes so growth can be watched as it happens.
//
// The sampler reads /proc/self/smaps, which walks every page table entry of
// the process: milliseconds at a gigabyte, so it runs on a worker through
// runOffThread and never overlaps itself. It only runs while this page is on
// screen - no timer survives leaving it.
#include "MainWindow.h"
#include "CodeMapModel.h"
#include "CodeMapView.h"
#include "MainWindowInternal.h"
#include "MemoryUsageChart.h"
#include "ProcessMemory.h"
#include "SystemStats.h"
#include "FontSizing.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPixmapCache>
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#ifdef Q_OS_LINUX
#include <malloc.h>
#endif

using namespace forkmesh::ui;
using namespace forkmesh::memory;

namespace {

enum MemoryColumn {
    ColName = 0,
    ColResident,
    ColDelta,
    ColPrivate,
    ColShared,
    ColProportional,
    ColSwap,
    ColVirtual,
    ColMappings,
    ColumnCount
};

enum AppColumn { AppName = 0, AppSize, AppDelta, AppDetail, AppColumnCount };

constexpr int kKeyRole = Qt::UserRole;
constexpr int kSortRole = Qt::UserRole + 1;

// Numeric columns sort by the byte count, not by "1.2 MB" as text.
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

// One colour per MappingGroup, in enum order; the chart bands and the tree's
// group swatches use the same list so the legend is the tree.
const QVector<QColor> &groupColors()
{
    static const QVector<QColor> colors = {
        QColor("#e5534b"), // Heap
        QColor("#f0883e"), // Anonymous
        QColor("#d29922"), // Thread stacks
        QColor("#b08800"), // Main stack
        QColor("#bf4b8a"), // Shared memory
        QColor("#8957e5"), // Graphics
        QColor("#6e7fd9"), // Fonts
        QColor("#4493f8"), // Libraries
        QColor("#2f9e8f"), // Executable
        QColor("#3fb950"), // Mapped files
        Theme::remapColor(QColor("#6e7681")), // Reserved
        Theme::remapColor(QColor("#484f58")), // Kernel
    };
    return colors;
}

QColor colorFor(MappingGroup group)
{
    const int index = int(group);
    return index >= 0 && index < groupColors().size() ? groupColors().at(index)
                                                      : Theme::remapColor(QColor("#8b949e"));
}

QString bytes(qint64 value)
{
    return SystemStats::formatBytes(value);
}

QString signedBytes(qint64 value)
{
    if (value == 0)
        return QString();
    return (value > 0 ? QStringLiteral("+") : QString::fromUtf8("\xE2\x88\x92")) +
           SystemStats::formatBytes(qAbs(value));
}

void setBytes(QTreeWidgetItem *item, int column, qint64 value)
{
    item->setText(column, bytes(value));
    item->setData(column, kSortRole, value);
    item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
}

void setDelta(QTreeWidgetItem *item, int column, qint64 delta, double seconds)
{
    item->setText(column, signedBytes(delta));
    item->setData(column, kSortRole, delta);
    item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    // Growth is the thing being watched for; shrinking is good news.
    item->setForeground(column, delta > 0   ? QBrush(QColor("#e5534b"))
                                : delta < 0 ? QBrush(QColor("#3fb950"))
                                            : QBrush());
    if (delta != 0 && seconds > 0) {
        item->setToolTip(column,
                         QStringLiteral("%1 per second since the last sample")
                             .arg(signedBytes(qint64(delta / seconds))));
    } else {
        item->setToolTip(column, QString());
    }
}

QPixmap swatch(const QColor &color)
{
    QPixmap pixmap(10, 10);
    pixmap.fill(color);
    return pixmap;
}

qint64 pixmapBytes(const QPixmap &pixmap)
{
    return qint64(pixmap.width()) * pixmap.height() * qMax(1, pixmap.depth()) / 8;
}

qint64 stringListBytes(const QStringList &list)
{
    qint64 total = qint64(list.size()) * qint64(sizeof(QString));
    for (const QString &s : list)
        total += qint64(s.capacity()) * 2;
    return total;
}

// Update-in-place: find the child of `parent` (or the top level) keyed `key`,
// creating it when missing. Rebuilding the tree each second would lose
// expansion, selection and scroll position, and flicker.
QTreeWidgetItem *ensureItem(QTreeWidget *tree, QTreeWidgetItem *parent,
                            QHash<QString, QTreeWidgetItem *> &index,
                            const QString &key, int columns)
{
    if (QTreeWidgetItem *found = index.value(key))
        return found;
    auto *item = new SortableItem;
    item->setData(0, kKeyRole, key);
    for (int c = 1; c < columns; ++c)
        item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
    if (parent)
        parent->addChild(item);
    else
        tree->addTopLevelItem(item);
    index.insert(key, item);
    return item;
}

QHash<QString, QTreeWidgetItem *> indexTree(QTreeWidget *tree)
{
    QHash<QString, QTreeWidgetItem *> index;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = tree->topLevelItem(i);
        index.insert(top->data(0, kKeyRole).toString(), top);
        for (int c = 0; c < top->childCount(); ++c) {
            QTreeWidgetItem *child = top->child(c);
            index.insert(child->data(0, kKeyRole).toString(), child);
        }
    }
    return index;
}

void removeUnseen(QTreeWidget *tree, const QSet<QString> &seen)
{
    for (int i = tree->topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem *top = tree->topLevelItem(i);
        for (int c = top->childCount() - 1; c >= 0; --c) {
            if (!seen.contains(top->child(c)->data(0, kKeyRole).toString()))
                delete top->takeChild(c);
        }
        if (!seen.contains(top->data(0, kKeyRole).toString()))
            delete tree->takeTopLevelItem(i);
    }
}

void applyFilter(QTreeWidget *tree, const QString &needle)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = tree->topLevelItem(i);
        const bool groupMatches =
            needle.isEmpty() || top->text(0).contains(needle, Qt::CaseInsensitive);
        int visible = 0;
        for (int c = 0; c < top->childCount(); ++c) {
            QTreeWidgetItem *child = top->child(c);
            const bool show = groupMatches ||
                              child->text(0).contains(needle, Qt::CaseInsensitive) ||
                              child->toolTip(0).contains(needle, Qt::CaseInsensitive);
            child->setHidden(!show);
            visible += show ? 1 : 0;
        }
        top->setHidden(!groupMatches && visible == 0);
        if (!needle.isEmpty() && visible > 0 && !groupMatches)
            top->setExpanded(true);
    }
}

enum class MapMetric { Resident, Proportional, Private, Virtual, Swap, App };

struct MapMetricSpec {
    MapMetric metric;
    const char *label;
    const char *whole;
    const char *tip;
};

const MapMetricSpec kMapMetrics[] = {
    {MapMetric::Resident, "Resident", "resident memory",
     "Pages of each mapping currently in RAM, shared ones included"},
    {MapMetric::Proportional, "Proportional", "proportional memory",
     "Private pages plus this process's share of each shared page"},
    {MapMetric::Private, "Private", "private memory",
     "Resident pages no other process shares"},
    {MapMetric::Virtual, "Virtual", "address space",
     "Address space reserved, touched or not"},
    {MapMetric::Swap, "Swapped out", "swap", "Pages the kernel moved to swap"},
    {MapMetric::App, "Inside the app", "the app's own counts",
     "The caches and buffers the app counts itself"},
};

qint64 mappingMetric(const MappingSample &item, MapMetric metric)
{
    switch (metric) {
    case MapMetric::Resident: return item.residentBytes;
    case MapMetric::Proportional: return item.proportionalBytes;
    case MapMetric::Private: return item.privateBytes;
    case MapMetric::Virtual: return item.virtualBytes;
    case MapMetric::Swap: return item.swapBytes;
    case MapMetric::App: break;
    }
    return 0;
}

QString mapLeafName(const QString &label)
{
    QString name = label;
    name.replace(QLatin1Char('/'), QChar(0x2215));
    return name.isEmpty() ? QStringLiteral("unnamed") : name;
}

QString mapRow(const QString &name, const QString &value)
{
    return QStringLiteral("%1 %2").arg(name, -13).arg(value, 10);
}

struct MemoryMapContent {
    QVector<forkmesh::codemap::Entry> entries;
    QHash<QString, QString> texts;
};

MemoryMapContent memoryMapContent(const ProcessMemorySnapshot &snapshot, MapMetric metric)
{
    MemoryMapContent out;
    if (metric == MapMetric::App) {
        for (const ProbeSample &probe : snapshot.probes) {
            if (probe.bytes <= 0)
                continue;
            const QString group = probe.group.isEmpty() ? QStringLiteral("App") : probe.group;
            const QString path = mapLeafName(group) + QLatin1Char('/') + mapLeafName(probe.name);
            out.entries.append({path, probe.bytes, group, forkmesh::codemap::Preview::Code});
            QStringList lines = {probe.name, group, QString(),
                                 mapRow(QStringLiteral("size"), bytes(probe.bytes))};
            if (!probe.detail.isEmpty())
                lines << probe.detail;
            if (probe.estimated)
                lines << QStringLiteral("estimated");
            out.texts.insert(path, lines.join(QLatin1Char('\n')));
        }
        return out;
    }
    for (const MappingGroupSample &group : snapshot.groups) {
        const QString title = groupTitle(group.group);
        for (const MappingSample &item : group.items) {
            const qint64 size = mappingMetric(item, metric);
            if (size <= 0)
                continue;
            const QString path = mapLeafName(title) + QLatin1Char('/') + mapLeafName(item.label);
            out.entries.append({path, size, title, forkmesh::codemap::Preview::Code});
            QStringList lines = {item.label};
            if (!item.detail.isEmpty() && item.detail != item.label)
                lines << item.detail;
            lines << title << QString()
                  << mapRow(QStringLiteral("resident"), bytes(item.residentBytes))
                  << mapRow(QStringLiteral("private"), bytes(item.privateBytes))
                  << mapRow(QStringLiteral("  dirty"), bytes(item.privateDirtyBytes))
                  << mapRow(QStringLiteral("shared"), bytes(item.sharedBytes))
                  << mapRow(QStringLiteral("proportional"), bytes(item.proportionalBytes))
                  << mapRow(QStringLiteral("swap"), bytes(item.swapBytes))
                  << mapRow(QStringLiteral("virtual"), bytes(item.virtualBytes))
                  << mapRow(QStringLiteral("mappings"), QLocale().toString(item.mappings));
            out.texts.insert(path, lines.join(QLatin1Char('\n')));
        }
    }
    return out;
}

QTreeWidget *makeTree(const QStringList &headers)
{
    auto *tree = new QTreeWidget;
    tree->setColumnCount(headers.size());
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(true);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->setSortingEnabled(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < headers.size(); ++c)
        tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    return tree;
}

} // namespace

QWidget *MainWindow::buildMemorySection()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("memorySection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Memory"));
    title->setObjectName(QStringLiteral("pageTitle"));
    title->setToolTip(QStringLiteral(
        "Every byte of memory this app holds, read from the kernel while you "
        "watch: each mapping in /proc/self/smaps grouped by what it is, the "
        "allocator's in-use and held-but-free split, and the app's own counts "
        "for the caches and buffers it keeps. Sampling runs only while this "
        "page is open."));
    heading->addWidget(title);

    m_memoryStatus = new QLabel;
    m_memoryStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_memoryStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    heading->addWidget(m_memoryStatus);
    heading->addStretch();

    m_memoryFilter = new QLineEdit;
    m_memoryFilter->setPlaceholderText(QStringLiteral("Filter…"));
    m_memoryFilter->setClearButtonEnabled(true);
    m_memoryFilter->setFixedWidth(180);
    m_memoryFilter->setToolTip(QStringLiteral(
        "Show only groups, mappings and app items whose name or path contains "
        "this text"));
    connect(m_memoryFilter, &QLineEdit::textChanged, this,
            [this](const QString &text) {
                if (m_memoryTree)
                    applyFilter(m_memoryTree, text.trimmed());
                if (m_memoryAppTree)
                    applyFilter(m_memoryAppTree, text.trimmed());
            });
    heading->addWidget(m_memoryFilter);

    m_memoryInterval = new QComboBox;
    m_memoryInterval->addItem(QStringLiteral("Every 0.5 s"), 500);
    m_memoryInterval->addItem(QStringLiteral("Every 1 s"), 1000);
    m_memoryInterval->addItem(QStringLiteral("Every 2 s"), 2000);
    m_memoryInterval->addItem(QStringLiteral("Every 5 s"), 5000);
    m_memoryInterval->setCurrentIndex(1);
    m_memoryInterval->setToolTip(QStringLiteral(
        "How often to read the kernel's memory maps while this page is open"));
    connect(m_memoryInterval, &QComboBox::currentIndexChanged, this, [this] {
        if (m_memoryTimer)
            m_memoryTimer->setInterval(m_memoryInterval->currentData().toInt());
    });
    heading->addWidget(m_memoryInterval);

    m_memoryPause = new QPushButton(QStringLiteral("Pause"));
    m_memoryPause->setObjectName(QStringLiteral("ghostButton"));
    m_memoryPause->setCursor(Qt::PointingHandCursor);
    m_memoryPause->setCheckable(true);
    m_memoryPause->setToolTip(QStringLiteral(
        "Freeze the numbers where they are, to read or copy them"));
    connect(m_memoryPause, &QPushButton::toggled, this, [this](bool paused) {
        m_memoryPause->setText(paused ? QStringLiteral("Resume")
                                      : QStringLiteral("Pause"));
        if (paused) {
            if (m_memoryTimer)
                m_memoryTimer->stop();
        } else {
            refreshMemoryPage();
        }
    });
    heading->addWidget(m_memoryPause);

#if defined(Q_OS_LINUX) && defined(__GLIBC__)
    auto *trim = new QPushButton(QStringLiteral("Trim heap"));
    trim->setObjectName(QStringLiteral("ghostButton"));
    trim->setCursor(Qt::PointingHandCursor);
    trim->setToolTip(QStringLiteral(
        "Ask malloc to return freed memory to the kernel (malloc_trim). "
        "Whatever resident drops by was never a leak - it was freed memory "
        "the allocator was keeping for reuse."));
    connect(trim, &QPushButton::clicked, this, [this] {
        const qint64 before = SystemStats::residentBytes();
        runOffThread<int>([] { return malloc_trim(0); },
                          [this, before](int) {
                              const qint64 after = SystemStats::residentBytes();
                              flashMessage(QStringLiteral(
                                               "Heap trimmed: resident %1 → %2")
                                               .arg(bytes(before), bytes(after)));
                              sampleMemoryPage();
                          });
    });
    heading->addWidget(trim);
#endif

    auto *copy = new QPushButton(QStringLiteral("Copy report"));
    copy->setObjectName(QStringLiteral("ghostButton"));
    copy->setCursor(Qt::PointingHandCursor);
    copy->setToolTip(QStringLiteral(
        "Copy the latest sample as plain text: totals, every group and its "
        "largest mappings, the allocator and the app's own counts"));
    setOcticon(copy, "copy", 14);
    connect(copy, &QPushButton::clicked, this, [this] {
        if (m_memoryLastReport.isEmpty())
            return;
        QApplication::clipboard()->setText(m_memoryLastReport);
        flashMessage(QStringLiteral("Memory report copied"));
    });
    heading->addWidget(copy);

    auto *governor = new QPushButton(QStringLiteral("Agent governor"));
    governor->setObjectName(QStringLiteral("ghostButton"));
    governor->setCursor(Qt::PointingHandCursor);
    governor->setToolTip(QStringLiteral(
        "The agent memory governor: the shared budget agents run in, its "
        "freeze and thaw thresholds, the units it watches and what it has "
        "done"));
    setOcticon(governor, "shield-check", 14);
    connect(governor, &QPushButton::clicked, this,
            [this] { showSection(kGovernorSectionIndex); });

    auto *memoryMap = new QPushButton(QStringLiteral("Memory map"));
    memoryMap->setObjectName(QStringLiteral("ghostButton"));
    memoryMap->setCursor(Qt::PointingHandCursor);
    memoryMap->setToolTip(QStringLiteral(
        "Every mapping as a treemap sized by its bytes, like the code map: "
        "groups as folders, mappings inside them, in 2D or 3D"));
    setOcticon(memoryMap, "graph", 14);
    connect(memoryMap, &QPushButton::clicked, this, [this] { openMemoryMap(); });
    heading->addWidget(memoryMap);
    heading->addWidget(governor);
    outer->addLayout(heading);

    // Headline tiles. Each is a value and a line under it saying what moved.
    auto *tiles = new QGridLayout;
    tiles->setHorizontalSpacing(10);
    tiles->setVerticalSpacing(10);
    struct TileSpec {
        const char *id;
        const char *title;
        const char *hint;
    };
    static const TileSpec kTiles[] = {
        {"resident", "Resident",
         "RSS: pages of this process currently in RAM, shared ones included"},
        {"private", "Private",
         "Resident pages no other process shares - what would come back if the "
         "app exited"},
        {"proportional", "Proportional",
         "PSS: private pages plus this process's share of each shared page. "
         "Summed across processes, this adds up to real RAM use"},
        {"heap", "Allocated (malloc)",
         "Bytes the app has asked malloc for and not freed yet"},
        {"held", "Freed, still held",
         "Bytes freed back to malloc that it keeps rather than returning to the "
         "kernel; Trim heap releases what it can"},
        {"virtual", "Virtual",
         "Address space reserved. Mostly never touched; not memory"},
        {"swap", "Swapped out", "Pages of this process the kernel moved to swap"},
        {"threads", "Threads", "Live threads, each with its own stack"},
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
        auto *value = new QLabel(QStringLiteral("—"));
        QFont big = value->font();
        big = forkmesh::scaledFont(big, 1.6);
        big.setBold(true);
        value->setFont(big);
        layout->addWidget(value);
        auto *note = new QLabel;
        note->setObjectName(QStringLiteral("mutedLabel"));
        layout->addWidget(note);
        m_memoryTiles.insert(QString::fromLatin1(spec.id), value);
        m_memoryTileNotes.insert(QString::fromLatin1(spec.id), note);
        tiles->addWidget(frame, tileIndex / 4, tileIndex % 4);
        ++tileIndex;
    }
    outer->addLayout(tiles);

    m_memoryChart = new MemoryUsageChart;
    QVector<MemoryUsageChart::Band> bands;
    for (int g = 0; g < int(MappingGroup::GroupCount); ++g)
        bands.append({groupTitle(MappingGroup(g)), colorFor(MappingGroup(g))});
    m_memoryChart->setBands(bands);
    m_memoryChart->setToolTip(QString());
    outer->addWidget(m_memoryChart);

    auto *tabs = new QTabWidget;
    m_memoryTree = makeTree({QStringLiteral("Mapping"), QStringLiteral("Resident"),
                             QStringLiteral("Change"), QStringLiteral("Private"),
                             QStringLiteral("Shared"),
                             QStringLiteral("Proportional"),
                             QStringLiteral("Swap"), QStringLiteral("Virtual"),
                             QStringLiteral("Mappings")});
    m_memoryTree->sortByColumn(ColResident, Qt::DescendingOrder);
    m_memoryTree->headerItem()->setToolTip(
        ColDelta, QStringLiteral("Resident change since the previous sample"));
    m_memoryTree->headerItem()->setToolTip(
        ColProportional,
        QStringLiteral("Private bytes plus this process's share of shared ones"));
    tabs->addTab(m_memoryTree, QStringLiteral("By mapping"));

    m_memoryAppTree =
        makeTree({QStringLiteral("Held by"), QStringLiteral("Size"),
                  QStringLiteral("Change"), QStringLiteral("Detail")});
    m_memoryAppTree->header()->setSectionResizeMode(AppDetail,
                                                    QHeaderView::Stretch);
    m_memoryAppTree->header()->setSectionResizeMode(AppName,
                                                    QHeaderView::ResizeToContents);
    m_memoryAppTree->sortByColumn(AppSize, Qt::DescendingOrder);
    tabs->addTab(m_memoryAppTree, QStringLiteral("Inside the app"));
    outer->addWidget(tabs, 1);

    m_memoryTimer = new QTimer(page);
    m_memoryTimer->setInterval(1000);
    connect(m_memoryTimer, &QTimer::timeout, this, [this] { sampleMemoryPage(); });

    installMemoryProbes();
    return page;
}

bool MainWindow::memoryPageOnScreen() const
{
    return m_sectionStack && m_memoryTree &&
           m_sectionStack->currentIndex() == kMemorySectionIndex &&
           isVisible() && !isMinimized();
}

void MainWindow::refreshMemoryPage()
{
    if (!m_memoryTimer || (m_memoryPause && m_memoryPause->isChecked()))
        return;
    if (m_memoryInterval)
        m_memoryTimer->setInterval(m_memoryInterval->currentData().toInt());
    m_memoryTimer->start();
    sampleMemoryPage();
}

void MainWindow::sampleMemoryPage()
{
    if (!memoryPageOnScreen()) {
        // Left the page: nothing keeps sampling behind it.
        if (m_memoryTimer)
            m_memoryTimer->stop();
        return;
    }
    if (m_memorySampling)
        return; // the previous read is still walking page tables
    m_memorySampling = true;
    runOffThread<ProcessMemorySnapshot>(
        [] { return sampleProcessMemory(); },
        [this](ProcessMemorySnapshot snapshot) {
            m_memorySampling = false;
            // Probes read GUI-thread state, so they are gathered here rather
            // than on the worker.
            snapshot.probes = sampleProbes();
            applyMemorySample(snapshot);
        });
}

void MainWindow::applyMemorySample(const ProcessMemorySnapshot &snapshot)
{
    if (!m_memoryTree || !m_memoryAppTree)
        return;
    if (!snapshot.valid) {
        m_memoryStatus->setText(QStringLiteral(
            "This platform does not expose per-mapping memory; only Linux "
            "reports it."));
        return;
    }
    const double seconds =
        m_memoryPreviousAtMs > 0
            ? qMax(0.001, (snapshot.sampledAtMs - m_memoryPreviousAtMs) / 1000.0)
            : 0.0;
    QHash<QString, qint64> current;
    const auto delta = [&](const QString &key, qint64 value) -> qint64 {
        current.insert(key, value);
        const auto previous = m_memoryPrevious.constFind(key);
        return previous == m_memoryPrevious.constEnd() ? 0 : value - previous.value();
    };

    // Tiles.
    const auto setTile = [this](const char *id, const QString &value,
                                const QString &note) {
        if (QLabel *label = m_memoryTiles.value(QString::fromLatin1(id)))
            label->setText(value);
        if (QLabel *label = m_memoryTileNotes.value(QString::fromLatin1(id)))
            label->setText(note);
    };
    const auto changeNote = [&](const QString &key, qint64 value) {
        const qint64 d = delta(key, value);
        return d == 0 ? QStringLiteral("steady")
                      : QStringLiteral("%1 since last sample").arg(signedBytes(d));
    };
    QString residentNote = changeNote(QStringLiteral("t:resident"),
                                      snapshot.residentBytes);
    if (snapshot.machineTotalBytes > 0) {
        residentNote += QStringLiteral(" · %1% of RAM")
                            .arg(100.0 * snapshot.residentBytes /
                                     snapshot.machineTotalBytes,
                                 0, 'f', 1);
    }
    setTile("resident", bytes(snapshot.residentBytes), residentNote);
    setTile("private", bytes(snapshot.privateBytes),
            changeNote(QStringLiteral("t:private"), snapshot.privateBytes));
    setTile("proportional", bytes(snapshot.proportionalBytes),
            changeNote(QStringLiteral("t:pss"), snapshot.proportionalBytes));
    setTile("virtual", bytes(snapshot.virtualBytes),
            QStringLiteral("peak %1").arg(bytes(snapshot.peakVirtualBytes)));
    setTile("swap", bytes(snapshot.swapBytes),
            changeNote(QStringLiteral("t:swap"), snapshot.swapBytes));
    setTile("threads", QString::number(snapshot.threads),
            QStringLiteral("peak resident %1").arg(bytes(snapshot.peakResidentBytes)));
    const AllocatorSample &alloc = snapshot.allocator;
    if (alloc.valid) {
        setTile("heap", bytes(alloc.inUseBytes + alloc.mmapBytes),
                changeNote(QStringLiteral("t:malloc"),
                           alloc.inUseBytes + alloc.mmapBytes));
        setTile("held", bytes(alloc.freeBytes),
                QStringLiteral("%1 trimmable at the top")
                    .arg(bytes(alloc.releasableBytes)));
    } else {
        setTile("heap", QStringLiteral("—"), QStringLiteral("allocator not glibc"));
        setTile("held", QStringLiteral("—"), QString());
    }

    // Chart: one band per group, in enum order.
    MemoryUsageChart::Point point;
    point.timestampMs = snapshot.sampledAtMs;
    for (int g = 0; g < int(MappingGroup::GroupCount); ++g)
        point.values.append(snapshot.groupResident(MappingGroup(g)));
    m_memoryChart->appendPoint(point);

    // Mapping tree. Sorting is switched off while items change so each
    // setText does not re-sort the whole tree.
    m_memoryTree->setSortingEnabled(false);
    QHash<QString, QTreeWidgetItem *> index = indexTree(m_memoryTree);
    QSet<QString> seen;
    int mappings = 0;
    for (const MappingGroupSample &group : snapshot.groups) {
        const QString groupKey = QStringLiteral("g:") + groupTitle(group.group);
        seen.insert(groupKey);
        mappings += group.mappings;
        QTreeWidgetItem *top =
            ensureItem(m_memoryTree, nullptr, index, groupKey, ColumnCount);
        if (top->text(ColName).isEmpty()) {
            top->setText(ColName, groupTitle(group.group));
            top->setIcon(ColName, QIcon(swatch(colorFor(group.group))));
            top->setToolTip(ColName, groupDescription(group.group));
            QFont bold = top->font(ColName);
            bold.setBold(true);
            top->setFont(ColName, bold);
        }
        setBytes(top, ColResident, group.residentBytes);
        setDelta(top, ColDelta, delta(groupKey, group.residentBytes), seconds);
        setBytes(top, ColPrivate, group.privateBytes);
        setBytes(top, ColShared, group.sharedBytes);
        setBytes(top, ColProportional, group.proportionalBytes);
        setBytes(top, ColSwap, group.swapBytes);
        setBytes(top, ColVirtual, group.virtualBytes);
        top->setText(ColMappings, QString::number(group.mappings));
        top->setData(ColMappings, kSortRole, group.mappings);

        for (const MappingSample &item : group.items) {
            const QString key = groupKey + QLatin1Char('|') + item.label;
            seen.insert(key);
            QTreeWidgetItem *row =
                ensureItem(m_memoryTree, top, index, key, ColumnCount);
            if (row->text(ColName).isEmpty()) {
                row->setText(ColName, item.label);
            }
            row->setToolTip(ColName, item.detail.isEmpty() ? item.label
                                                           : item.detail);
            setBytes(row, ColResident, item.residentBytes);
            setDelta(row, ColDelta, delta(key, item.residentBytes), seconds);
            setBytes(row, ColPrivate, item.privateBytes);
            setBytes(row, ColShared, item.sharedBytes);
            setBytes(row, ColProportional, item.proportionalBytes);
            setBytes(row, ColSwap, item.swapBytes);
            setBytes(row, ColVirtual, item.virtualBytes);
            row->setText(ColMappings, QString::number(item.mappings));
            row->setData(ColMappings, kSortRole, item.mappings);
        }
    }
    removeUnseen(m_memoryTree, seen);
    m_memoryTree->setSortingEnabled(true);

    // Inside-the-app tree: allocator, toolkit, then every registered probe.
    m_memoryAppTree->setSortingEnabled(false);
    QHash<QString, QTreeWidgetItem *> appIndex = indexTree(m_memoryAppTree);
    QSet<QString> appSeen;
    const auto appGroup = [&](const QString &title, const QString &tip) {
        const QString key = QStringLiteral("a:") + title;
        appSeen.insert(key);
        QTreeWidgetItem *top =
            ensureItem(m_memoryAppTree, nullptr, appIndex, key, AppColumnCount);
        if (top->text(AppName).isEmpty()) {
            top->setText(AppName, title);
            top->setToolTip(AppName, tip);
            QFont bold = top->font(AppName);
            bold.setBold(true);
            top->setFont(AppName, bold);
            top->setExpanded(true);
        }
        return top;
    };
    const auto appRow = [&](QTreeWidgetItem *top, const QString &name,
                            qint64 size, const QString &detail,
                            const QString &tip = QString()) {
        const QString key = top->data(0, kKeyRole).toString() +
                            QLatin1Char('|') + name;
        appSeen.insert(key);
        QTreeWidgetItem *row =
            ensureItem(m_memoryAppTree, top, appIndex, key, AppColumnCount);
        row->setText(AppName, name);
        if (!tip.isEmpty())
            row->setToolTip(AppName, tip);
        if (size >= 0) {
            setBytes(row, AppSize, size);
            setDelta(row, AppDelta, delta(key, size), seconds);
        }
        row->setText(AppDetail, detail);
        row->setTextAlignment(AppDetail, Qt::AlignLeft | Qt::AlignVCenter);
        return row;
    };
    const auto groupTotal = [&](QTreeWidgetItem *top, qint64 total) {
        setBytes(top, AppSize, total);
        setDelta(top, AppDelta,
                 delta(top->data(0, kKeyRole).toString(), total), seconds);
    };

    if (alloc.valid) {
        QTreeWidgetItem *top = appGroup(
            QStringLiteral("Allocator (glibc malloc)"),
            QStringLiteral("What malloc itself reports. Heap plus mmap chunks is "
                           "what the allocator holds from the kernel; in use is "
                           "what the app still has; the rest is free space "
                           "malloc keeps for reuse."));
        appRow(top, QStringLiteral("In use"), alloc.inUseBytes,
               QStringLiteral("small allocations handed out, not freed"));
        appRow(top, QStringLiteral("Large blocks (mmap)"), alloc.mmapBytes,
               QStringLiteral("%1 chunks, each its own mapping").arg(alloc.mmapChunks));
        appRow(top, QStringLiteral("Freed, still held"), alloc.freeBytes,
               QStringLiteral("%1 free chunks").arg(alloc.freeChunks));
        appRow(top, QStringLiteral("Trimmable at the top"), alloc.releasableBytes,
               QStringLiteral("what malloc_trim can release immediately"));
        groupTotal(top, alloc.arenaBytes + alloc.mmapBytes);
    }

    {
        QTreeWidgetItem *top = appGroup(
            QStringLiteral("Toolkit (Qt)"),
            QStringLiteral("Counts from Qt itself. Widgets are not sized by Qt; a "
                           "count that climbs while the screen looks the same is "
                           "a leak."));
        const int widgetCount = QApplication::allWidgets().size();
        QTreeWidgetItem *widgetsRow = appRow(
            top, QStringLiteral("Live widgets"), -1,
            QStringLiteral("%1 widgets, %2 top-level")
                .arg(widgetCount)
                .arg(QApplication::topLevelWidgets().size()));
        // A count, not bytes: the change column says widgets, not MB.
        const qint64 widgetDelta =
            delta(QStringLiteral("q:widgets"), widgetCount);
        widgetsRow->setText(AppDelta,
                            widgetDelta == 0
                                ? QString()
                                : QStringLiteral("%1%2 widgets")
                                      .arg(widgetDelta > 0 ? "+" : "")
                                      .arg(widgetDelta));
        widgetsRow->setData(AppDelta, kSortRole, widgetDelta);
        widgetsRow->setData(AppSize, kSortRole, 0);
        appRow(top, QStringLiteral("Pixmap cache limit"),
               qint64(QPixmapCache::cacheLimit()) * 1024,
               QStringLiteral("the most QPixmapCache will keep; not its fill"));
    }

    QHash<QString, QTreeWidgetItem *> probeGroups;
    QHash<QString, qint64> probeTotals;
    qint64 probeTotal = 0;
    for (const ProbeSample &probe : snapshot.probes) {
        QTreeWidgetItem *top = probeGroups.value(probe.group);
        if (!top) {
            top = appGroup(probe.group,
                           QStringLiteral("The app's own count of what it keeps "
                                          "here. Estimates: container overhead "
                                          "is not included."));
            probeGroups.insert(probe.group, top);
        }
        appRow(top, probe.name, probe.bytes, probe.detail,
               probe.estimated ? QStringLiteral("Estimated from the contents")
                               : QString());
        probeTotals[probe.group] += probe.bytes;
        probeTotal += probe.bytes;
    }
    for (auto it = probeGroups.constBegin(); it != probeGroups.constEnd(); ++it)
        groupTotal(it.value(), probeTotals.value(it.key()));
    removeUnseen(m_memoryAppTree, appSeen);
    m_memoryAppTree->setSortingEnabled(true);

    const QString needle = m_memoryFilter ? m_memoryFilter->text().trimmed()
                                          : QString();
    if (!needle.isEmpty()) {
        applyFilter(m_memoryTree, needle);
        applyFilter(m_memoryAppTree, needle);
    }

    m_memoryStatus->setText(
        QStringLiteral("%1 mappings in %2 groups · read in %3 ms · %4")
            .arg(mappings)
            .arg(snapshot.groups.size())
            .arg(snapshot.sampleCostUs / 1000.0, 0, 'f', 1)
            .arg(QDateTime::fromMSecsSinceEpoch(snapshot.sampledAtMs)
                     .toString(QStringLiteral("HH:mm:ss"))));

    m_memoryPrevious = current;
    m_memoryPreviousAtMs = snapshot.sampledAtMs;

    // Plain-text report for Copy, built from the same snapshot.
    QString report;
    report += QStringLiteral("ForkMesh desktop memory, %1\n")
                  .arg(QDateTime::fromMSecsSinceEpoch(snapshot.sampledAtMs)
                           .toString(Qt::ISODate));
    report += QStringLiteral("resident %1 · private %2 · proportional %3 · "
                             "virtual %4 · swap %5 · peak resident %6 · %7 "
                             "threads\n")
                  .arg(bytes(snapshot.residentBytes), bytes(snapshot.privateBytes),
                       bytes(snapshot.proportionalBytes),
                       bytes(snapshot.virtualBytes), bytes(snapshot.swapBytes),
                       bytes(snapshot.peakResidentBytes))
                  .arg(snapshot.threads);
    if (alloc.valid) {
        report += QStringLiteral("malloc: in use %1, mmap %2, freed but held %3\n")
                      .arg(bytes(alloc.inUseBytes), bytes(alloc.mmapBytes),
                           bytes(alloc.freeBytes));
    }
    report += QLatin1Char('\n');
    for (const MappingGroupSample &group : snapshot.groups) {
        report += QStringLiteral("%1  %2 resident, %3 private, %4 mappings\n")
                      .arg(groupTitle(group.group), bytes(group.residentBytes),
                           bytes(group.privateBytes))
                      .arg(group.mappings);
        const int shown = qMin(10, int(group.items.size()));
        for (int i = 0; i < shown; ++i) {
            const MappingSample &item = group.items.at(i);
            if (item.residentBytes <= 0)
                break;
            report += QStringLiteral("    %1  %2\n")
                          .arg(bytes(item.residentBytes), -10)
                          .arg(item.detail.isEmpty() ? item.label : item.detail);
        }
    }
    if (!snapshot.probes.isEmpty()) {
        report += QStringLiteral("\nInside the app (%1 named)\n").arg(bytes(probeTotal));
        for (const ProbeSample &probe : snapshot.probes) {
            report += QStringLiteral("    %1  %2 / %3  %4\n")
                          .arg(bytes(probe.bytes), -10)
                          .arg(probe.group, probe.name, probe.detail);
        }
    }
    m_memoryLastReport = report;
}

void MainWindow::installMemoryProbes()
{
    if (m_memoryProbesInstalled)
        return;
    m_memoryProbesInstalled = true;
    // Each probe reads a MainWindow member on the GUI thread, from the apply
    // step of a sample. MainWindow outlives the page, and the page is the only
    // reader, so capturing `this` is safe; restart teardown drains the worker
    // before it destroys the window.
    const auto pixmapCache = [this](const QString &name,
                                    const QHash<QString, QPixmap> MainWindow::*member) {
        registerProbe(QStringLiteral("Image caches"), name, [this, name, member] {
            const QHash<QString, QPixmap> &cache = this->*member;
            qint64 total = 0;
            for (const QPixmap &pixmap : cache)
                total += pixmapBytes(pixmap);
            ProbeSample sample;
            sample.name = name;
            sample.bytes = total;
            sample.detail = QStringLiteral("%1 pixmaps").arg(cache.size());
            return sample;
        });
    };
    pixmapCache(QStringLiteral("Favicons"), &MainWindow::m_faviconCache);
    pixmapCache(QStringLiteral("Chat member avatars"),
                &MainWindow::m_chatMemberAvatarCache);
    pixmapCache(QStringLiteral("Pull request avatars"),
                &MainWindow::m_agentPullAvatarCache);

    registerProbe(QStringLiteral("Buffers"), QStringLiteral("Network log"), [this] {
        ProbeSample sample;
        sample.bytes = stringListBytes(m_networkLog);
        sample.detail = QStringLiteral("%1 lines in memory").arg(m_networkLog.size());
        return sample;
    });
    registerProbe(QStringLiteral("Buffers"), QStringLiteral("Log view render queue"),
                  [this] {
                      ProbeSample sample;
                      sample.bytes = stringListBytes(m_logRenderLines);
                      sample.detail =
                          QStringLiteral("%1 lines").arg(m_logRenderLines.size());
                      return sample;
                  });
    registerProbe(QStringLiteral("Caches"), QStringLiteral("Branch diffs"), [this] {
        ProbeSample sample;
        for (const BranchDiffCacheEntry &entry : m_branchDiffCache)
            sample.bytes += entry.patch.capacity() + entry.emptyMessage.capacity() * 2;
        sample.detail = QStringLiteral("%1 patches").arg(m_branchDiffCache.size());
        return sample;
    });
    registerProbe(QStringLiteral("Caches"), QStringLiteral("Network repo mirrors"),
                  [this] {
                      ProbeSample sample;
                      for (const QByteArray &body : m_networkRepoMirrorsCache)
                          sample.bytes += body.capacity();
                      sample.detail = QStringLiteral("%1 responses")
                                          .arg(m_networkRepoMirrorsCache.size());
                      return sample;
                  });
}

void MainWindow::openMemoryMap()
{
    auto *window = new QWidget(this, Qt::Window);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(QStringLiteral("Memory map — ForkMesh"));
    window->setObjectName(QStringLiteral("memoryMapWindow"));
    window->resize(qMax(900, int(width() * 0.85)), qMax(600, int(height() * 0.85)));

    auto *view = new CodeMapView(window);
    view->setObjectName(QStringLiteral("memoryMapView"));
    view->setAccessibleName(QStringLiteral("Memory map"));
    QHash<QString, QColor> colors;
    for (int g = 0; g < int(MappingGroup::GroupCount); ++g)
        colors.insert(groupTitle(MappingGroup(g)), colorFor(MappingGroup(g)));
    view->setCategoryColors(colors);

    auto *crumbs = new QLabel(window);
    crumbs->setTextFormat(Qt::RichText);
    crumbs->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    crumbs->setMinimumWidth(120);
    auto *search = new QLineEdit(window);
    search->setPlaceholderText(QStringLiteral("Find mapping…"));
    search->setClearButtonEnabled(true);
    search->setMaximumWidth(220);
    search->setAccessibleName(QStringLiteral("Find mapping by name"));
    auto *metricBox = new QComboBox(window);
    metricBox->setObjectName(QStringLiteral("memoryMapMetric"));
    for (const MapMetricSpec &spec : kMapMetrics) {
        metricBox->addItem(QString::fromUtf8(spec.label), int(spec.metric));
        metricBox->setItemData(metricBox->count() - 1, QString::fromUtf8(spec.tip), Qt::ToolTipRole);
    }
    metricBox->setToolTip(QStringLiteral("What each cell's size measures"));
    auto *flatButton = new QPushButton(QStringLiteral("2D"), window);
    auto *cityButton = new QPushButton(QStringLiteral("3D"), window);
    flatButton->setCheckable(true);
    cityButton->setCheckable(true);
    flatButton->setChecked(true);
    flatButton->setToolTip(QStringLiteral("Flat treemap: every mapping sized by its bytes"));
    cityButton->setToolTip(QStringLiteral("3D view: group height follows group size"));
    auto *modes = new QButtonGroup(window);
    modes->setExclusive(true);
    modes->addButton(flatButton);
    modes->addButton(cityButton);
    auto *zoomOutButton = new QPushButton(QStringLiteral("−"), window);
    auto *zoomInButton = new QPushButton(QStringLiteral("+"), window);
    auto *resetButton = new QPushButton(QStringLiteral("Reset"), window);
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"), window);
    zoomOutButton->setAccessibleName(QStringLiteral("Zoom out"));
    zoomInButton->setAccessibleName(QStringLiteral("Zoom in"));
    zoomOutButton->setToolTip(QStringLiteral("Zoom out"));
    zoomInButton->setToolTip(QStringLiteral("Zoom in"));
    refreshButton->setToolTip(QStringLiteral("Read the kernel's memory maps again"));
    for (QPushButton *button :
         {flatButton, cityButton, zoomOutButton, zoomInButton, resetButton, refreshButton})
        button->setCursor(Qt::PointingHandCursor);

    auto *legend = new QLabel(window);
    legend->setTextFormat(Qt::RichText);
    auto *status = new QLabel(QStringLiteral("Reading memory maps…"), window);
    auto *hint = new QLabel(window);
    auto *focusLabel = new QLabel(window);
    focusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    for (QLabel *label : {status, hint})
        label->setEnabled(false);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(10, 8, 10, 8);
    top->setSpacing(8);
    top->addWidget(crumbs, 1);
    top->addWidget(search);
    top->addWidget(metricBox);
    top->addWidget(flatButton);
    top->addWidget(cityButton);
    top->addWidget(zoomOutButton);
    top->addWidget(zoomInButton);
    top->addWidget(resetButton);
    top->addWidget(refreshButton);
    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(10, 6, 10, 6);
    bottom->setSpacing(12);
    bottom->addWidget(legend);
    bottom->addWidget(status);
    bottom->addStretch(1);
    bottom->addWidget(hint);
    bottom->addWidget(focusLabel);
    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(top);
    layout->addWidget(view, 1);
    layout->addLayout(bottom);

    const QPointer<CodeMapView> viewGuard(view);
    auto setHint = [hint, view] {
        hint->setText(view->mode() == CodeMapView::Mode::Flat
            ? QStringLiteral("Scroll to zoom · drag to pan · click to zoom in · Esc to zoom out")
            : QStringLiteral("Drag to rotate · right-drag to pan · scroll to zoom · click to fly in"));
    };
    setHint();

    struct MapState {
        ProcessMemorySnapshot snapshot;
        QHash<QString, QString> texts;
        int generation = 0;
        bool sampling = false;
    };
    auto state = std::make_shared<MapState>();

    view->setOnFocusChanged([view, crumbs, focusLabel](int focus) {
        const auto &nodes = view->tree().nodes;
        if (focus < 0 || focus >= nodes.size())
            return;
        QVector<int> chain;
        for (int at = focus; at >= 0; at = nodes.at(at).parent)
            chain.prepend(at);
        QStringList parts;
        for (int index : std::as_const(chain)) {
            parts << QStringLiteral("<a href=\"%1\" style=\"text-decoration:none\">%2</a>")
                         .arg(index).arg(nodes.at(index).name.toHtmlEscaped());
        }
        crumbs->setText(parts.join(QStringLiteral(" / ")));
        focusLabel->setText(focus == 0
            ? QStringLiteral("%1 · %2 mappings").arg(view->describeSize(0).section(QStringLiteral(" · "), 0, 0),
                                                     QLocale().toString(view->tree().fileCount))
            : nodes.at(focus).path + QStringLiteral("  ") + view->describeSize(focus));
    });
    connect(crumbs, &QLabel::linkActivated, view, [view](const QString &link) {
        view->goTo(link.toInt());
        view->setFocus();
    });
    view->setOnCloseRequested([window] { window->close(); });
    view->setOnWantContent([viewGuard, state](const QStringList &paths) {
        QHash<QString, QByteArray> answers;
        for (const QString &path : paths) {
            const auto found = state->texts.constFind(path);
            if (found != state->texts.constEnd())
                answers.insert(path, found.value().toUtf8());
        }
        const int generation = state->generation;
        QTimer::singleShot(0, viewGuard, [viewGuard, state, generation, paths, answers] {
            if (!viewGuard || generation != state->generation)
                return;
            for (const QString &path : paths) {
                const auto found = answers.constFind(path);
                viewGuard->setContent(path, found == answers.constEnd() ? nullptr : &found.value());
            }
            viewGuard->finishContentBatch();
        });
    });

    auto rebuild = [viewGuard, state, metricBox, legend, status] {
        if (!viewGuard)
            return;
        const auto metric = MapMetric(metricBox->currentData().toInt());
        const auto spec = std::find_if(std::begin(kMapMetrics), std::end(kMapMetrics),
                                       [metric](const MapMetricSpec &s) { return s.metric == metric; });
        const auto &nodes = viewGuard->tree().nodes;
        const int focus = viewGuard->focusNode();
        const QString focusPath = focus > 0 && focus < nodes.size() ? nodes.at(focus).path : QString();
        MemoryMapContent content = memoryMapContent(state->snapshot, metric);
        state->texts = std::move(content.texts);
        ++state->generation;
        CodeMapView::Wording wording;
        wording.whole = QString::fromUtf8(spec->whole);
        wording.folder = QStringLiteral("group");
        wording.leaf = metric == MapMetric::App ? QStringLiteral("item") : QStringLiteral("mapping");
        viewGuard->setWording(wording);
        viewGuard->setTree(forkmesh::codemap::buildTreeFromEntries(
            content.entries, QStringLiteral("ForkMesh %1").arg(wording.whole)));
        if (!focusPath.isEmpty())
            viewGuard->goToPath(focusPath);
        QStringList entries;
        for (const auto &language : viewGuard->tree().languages) {
            entries << QStringLiteral("<span style=\"color:%1\">■</span> %2")
                           .arg(viewGuard->languageColor(language.first).name(),
                                language.first.toHtmlEscaped());
        }
        legend->setText(entries.join(QStringLiteral("&nbsp;&nbsp; ")));
        if (!state->snapshot.valid)
            status->setText(QStringLiteral("This platform does not expose per-mapping memory; only Linux reports it."));
        else if (content.entries.isEmpty())
            status->setText(QStringLiteral("Nothing to show for %1.").arg(wording.whole));
        else
            status->setText(QStringLiteral("Sampled %1")
                                .arg(QDateTime::fromMSecsSinceEpoch(state->snapshot.sampledAtMs)
                                         .toString(QStringLiteral("h:mm:ss AP"))));
    };

    auto sample = [this, viewGuard, state, status, rebuild] {
        if (state->sampling)
            return;
        state->sampling = true;
        status->setText(QStringLiteral("Reading memory maps…"));
        runOffThread<ProcessMemorySnapshot>(
            [] { return sampleProcessMemory(); },
            [viewGuard, state, rebuild](ProcessMemorySnapshot snapshot) {
                state->sampling = false;
                if (!viewGuard)
                    return;
                snapshot.probes = sampleProbes();
                state->snapshot = std::move(snapshot);
                rebuild();
            });
    };

    connect(flatButton, &QPushButton::clicked, view, [view, setHint] {
        view->setMode(CodeMapView::Mode::Flat);
        setHint();
        view->setFocus();
    });
    connect(cityButton, &QPushButton::clicked, view, [view, setHint] {
        view->setMode(CodeMapView::Mode::City);
        setHint();
        view->setFocus();
    });
    connect(zoomInButton, &QPushButton::clicked, view, [view] { view->zoomBy(1.6); });
    connect(zoomOutButton, &QPushButton::clicked, view, [view] { view->zoomBy(1 / 1.6); });
    connect(resetButton, &QPushButton::clicked, view, [view] { view->resetView(); });
    connect(refreshButton, &QPushButton::clicked, view, sample);
    connect(metricBox, &QComboBox::currentIndexChanged, view, rebuild);
    connect(search, &QLineEdit::textChanged, view, [view, status](const QString &text) {
        const int count = view->setSearch(text);
        status->setText(text.trimmed().isEmpty()
            ? QString()
            : QStringLiteral("%1 matching mapping%2 · Enter to visit")
                  .arg(QLocale().toString(count), QLatin1String(count == 1 ? "" : "s")));
    });
    connect(search, &QLineEdit::returnPressed, view, [view] { view->visitMatch(false); });

    window->show();
    view->setFocus();
    sample();
}
