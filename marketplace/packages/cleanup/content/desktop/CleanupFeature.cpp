#include "CleanupFeature.h"

#include "AppPaths.h"
#include "CleanupHostFeed.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace forkmesh::cleanup {
namespace {

enum Column { NameColumn, SizeColumn, AgeColumn, StatusColumn, ReasonColumn, PathColumn };
enum ConfirmColumn { ConfirmDelete, ConfirmItem, ConfirmSize, ConfirmWhy, ConfirmDiscard, ConfirmRetire };

constexpr int kIdRole = Qt::UserRole;
constexpr int kSortRole = Qt::UserRole + 1;

class SortItem final : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant left = data(column, kSortRole);
        const QVariant right = other.data(column, kSortRole);
        if (left.isValid() && right.isValid())
            return left.toLongLong() < right.toLongLong();
        return text(column).localeAwareCompare(other.text(column)) < 0;
    }
};

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QPushButton *ghostButton(const QString &text, const QString &tip)
{
    auto *button = new QPushButton(text);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    return button;
}

QColor statusColor(Status status)
{
    switch (status) {
    case Status::InUse:
        return QColor(0xd9, 0x77, 0x06);
    case Status::Attached:
        return QColor(0x3b, 0x82, 0xf6);
    case Status::Orphaned:
        return QColor(0x16, 0xa3, 0x4a);
    case Status::Protected:
        return QColor(0x8b, 0x94, 0x9e);
    }
    return QColor();
}

bool containsText(const Item &item, const QString &query)
{
    if (query.isEmpty())
        return true;
    return item.title.contains(query, Qt::CaseInsensitive) ||
           item.path.contains(query, Qt::CaseInsensitive) ||
           item.reasonText().contains(query, Qt::CaseInsensitive) ||
           item.branch.contains(query, Qt::CaseInsensitive);
}

struct DeletionBatch {
    QList<Outcome> outcomes;
    ProcessSnapshot processes;
};

} // namespace

CleanupFeature::CleanupFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_packageId(host.packageId),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("cleanupSection"));
    buildPage();
}

CleanupFeature::~CleanupFeature()
{
    if (m_cancel)
        m_cancel->store(true);
    if (m_trim) {
        m_trim->disconnect();
        m_trim->kill();
        m_trim->waitForFinished(2000);
    }
}

Context CleanupFeature::desktopContext()
{
    Context context;
    context.home = QDir::homePath();
    context.tmpRoots << QDir::cleanPath(QDir::tempPath());
#if defined(Q_OS_UNIX)
    if (QFileInfo(QStringLiteral("/tmp")).isDir() && !context.tmpRoots.contains(QStringLiteral("/tmp")))
        context.tmpRoots << QStringLiteral("/tmp");
    context.uid = QString::number(::getuid());
#endif
    if (forkmesh::paths::isActive()) {
        const forkmesh::paths::Layout &layout = forkmesh::paths::layout();
        context.projectHome = layout.homeDir;
        context.buildsDir = layout.buildsDir();
        context.worktreesDir = layout.worktreesDir();
    }
    context.dataDir = forkmesh::paths::dataDir();
    context.agentAccountsDir = forkmesh::paths::dataPath(QStringLiteral("agent-accounts"));
    context.claudeBinary = QStandardPaths::findExecutable(QStringLiteral("claude"));
    if (forkmesh::cleanupfeed::hasHost()) {
        const forkmesh::cleanupfeed::Host &host = forkmesh::cleanupfeed::host();
        if (host.repositories)
            context.repositories = host.repositories();
        if (host.agents)
            context.agents = host.agents();
        if (host.taskLinks)
            context.tasks = host.taskLinks();
        if (host.actionsSandboxDir)
            context.sandboxDir = host.actionsSandboxDir();
        if (host.claimedSandboxes)
            context.claimedSandboxes = host.claimedSandboxes();
    }
    return context;
}

QString CleanupFeature::logPath()
{
    return forkmesh::paths::logsDir() + QStringLiteral("/cleanup.log");
}

void CleanupFeature::buildPage()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Disk Cleanup"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral(
        "What Claude Code, Codex and agent runs left on this computer, and what still uses it")));
    heading->addStretch();
    m_refreshButton = ghostButton(QStringLiteral("Refresh"), QStringLiteral("Scan again"));
    m_cancelButton = ghostButton(QStringLiteral("Stop scan"), QStringLiteral("Stop the scan in progress"));
    m_cancelButton->hide();
    heading->addWidget(m_refreshButton);
    heading->addWidget(m_cancelButton);
    outer->addLayout(heading);
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] { cancelScan(); });

    m_summary = mutedLabel(QStringLiteral("Not scanned yet."));
    outer->addWidget(m_summary);

    auto *filters = new QHBoxLayout;
    m_orphanedOnly = new QCheckBox(QStringLiteral("Orphaned only"));
    m_orphanedOnly->setCursor(Qt::PointingHandCursor);
    m_orphanedOnly->setToolTip(QStringLiteral("Show only what nothing references"));
    m_olderDays = new QSpinBox;
    m_olderDays->setRange(0, 3650);
    m_olderDays->setSpecialValueText(QStringLiteral("Any age"));
    m_olderDays->setPrefix(QStringLiteral("Older than "));
    m_olderDays->setSuffix(QStringLiteral(" days"));
    m_largerMb = new QSpinBox;
    m_largerMb->setRange(0, 10000000);
    m_largerMb->setSingleStep(100);
    m_largerMb->setSpecialValueText(QStringLiteral("Any size"));
    m_largerMb->setPrefix(QStringLiteral("Larger than "));
    m_largerMb->setSuffix(QStringLiteral(" MB"));
    filters->addWidget(m_orphanedOnly);
    filters->addWidget(m_olderDays);
    filters->addWidget(m_largerMb);
    filters->addStretch();
    outer->addLayout(filters);
    connect(m_orphanedOnly, &QCheckBox::toggled, this, [this] { render(); });
    connect(m_olderDays, &QSpinBox::valueChanged, this, [this] { render(); });
    connect(m_largerMb, &QSpinBox::valueChanged, this, [this] { render(); });

    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("cleanupTree"));
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels({QStringLiteral("Item"), QStringLiteral("Size"), QStringLiteral("Age"),
                             QStringLiteral("Status"), QStringLiteral("Attached to"),
                             QStringLiteral("Path")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSortingEnabled(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(NameColumn, QHeaderView::Interactive);
    m_tree->setColumnWidth(NameColumn, 320);
    m_tree->setColumnWidth(SizeColumn, 90);
    m_tree->setColumnWidth(AgeColumn, 70);
    m_tree->setColumnWidth(StatusColumn, 100);
    m_tree->setColumnWidth(ReasonColumn, 360);
    m_tree->sortByColumn(SizeColumn, Qt::DescendingOrder);
    outer->addWidget(m_tree, 1);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *row, int column) {
        if (m_rendering || column != NameColumn)
            return;
        const QString id = row->data(NameColumn, kIdRole).toString();
        if (id.isEmpty())
            return;
        if (row->checkState(NameColumn) == Qt::Checked)
            m_checked.insert(id);
        else
            m_checked.remove(id);
        updateSummary();
        updateButtons();
    });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] { updateButtons(); });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        QTreeWidgetItem *row = m_tree->itemAt(point);
        const Item *item = row ? itemById(row->data(NameColumn, kIdRole).toString()) : nullptr;
        if (!item)
            return;
        QMenu menu(this);
        const QString path = item->path;
        menu.addAction(QStringLiteral("Open folder"), this, [path] {
            const QFileInfo info(path);
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(info.isDir() ? path : info.absolutePath()));
        });
        menu.addAction(QStringLiteral("Copy path"), this,
                       [path] { QApplication::clipboard()->setText(path); });
        menu.exec(m_tree->viewport()->mapToGlobal(point));
    });

    auto *actions = new QHBoxLayout;
    m_selectOrphaned = ghostButton(QStringLiteral("Select orphaned"),
                                   QStringLiteral("Tick every visible orphaned item"));
    m_clearButton = ghostButton(QStringLiteral("Clear"), QStringLiteral("Untick everything"));
    m_openButton = ghostButton(QStringLiteral("Open folder"),
                               QStringLiteral("Open the selected item's folder"));
    m_trimButton = ghostButton(QStringLiteral("Trim ccache…"),
                               QStringLiteral("Evict compiler cache entries unused for 14 days"));
    m_deleteButton = new QPushButton(QStringLiteral("Delete selected…"));
    m_deleteButton->setObjectName(QStringLiteral("primaryButton"));
    m_deleteButton->setCursor(Qt::PointingHandCursor);
    actions->addWidget(m_selectOrphaned);
    actions->addWidget(m_clearButton);
    actions->addWidget(m_openButton);
    actions->addWidget(m_trimButton);
    actions->addStretch();
    actions->addWidget(m_deleteButton);
    outer->addLayout(actions);
    connect(m_selectOrphaned, &QPushButton::clicked, this, [this] { checkOrphaned(); });
    connect(m_clearButton, &QPushButton::clicked, this, [this] { clearChecks(); });
    connect(m_openButton, &QPushButton::clicked, this, [this] { openSelectedFolder(); });
    connect(m_trimButton, &QPushButton::clicked, this, [this] { trimCcache(); });
    connect(m_deleteButton, &QPushButton::clicked, this, [this] { deleteChecked(); });

    m_status = mutedLabel(QString());
    m_status->hide();
    outer->addWidget(m_status);
    updateButtons();
}

void CleanupFeature::refresh()
{
    if (m_scanning || m_deleting)
        return;
    m_scanning = true;
    const quint64 generation = ++m_generation;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const CancelFlag cancel = m_cancel;
    Context context = desktopContext();
    m_refreshButton->setEnabled(false);
    m_cancelButton->show();
    m_summary->setText(QStringLiteral("Scanning…"));
    updateButtons();
    auto *watcher = new QFutureWatcher<ScanResult>(this);
    connect(watcher, &QFutureWatcher<ScanResult>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        m_scanning = false;
        m_refreshButton->setEnabled(true);
        m_cancelButton->hide();
        applyScan(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([context, cancel]() mutable {
        context.processes = readProcesses();
        context.nowMs = QDateTime::currentMSecsSinceEpoch();
        return scan(context, cancel);
    }));
}

void CleanupFeature::cancelScan()
{
    if (m_cancel)
        m_cancel->store(true);
}

void CleanupFeature::prepareSearch() {}

void CleanupFeature::applyScan(const ScanResult &result)
{
    m_items = result.items;
    m_notes = result.notes;
    m_scannedAtMs = QDateTime::currentMSecsSinceEpoch();
    QSet<QString> ids;
    for (const Item &item : std::as_const(m_items)) {
        if (item.deletable())
            ids.insert(item.id);
    }
    m_checked.intersect(ids);
    render();
    if (result.cancelled)
        setStatus(QStringLiteral("Scan stopped; the list is incomplete."), true);
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

bool CleanupFeature::visible(const Item &item) const
{
    if (m_orphanedOnly->isChecked() && item.status != Status::Orphaned)
        return false;
    const int days = m_olderDays->value();
    if (days > 0 && item.modifiedMs > 0 &&
        m_scannedAtMs - item.modifiedMs < qint64(days) * 24 * 60 * 60 * 1000)
        return false;
    const qint64 minimum = qint64(m_largerMb->value()) * 1024 * 1024;
    if (minimum > 0 && std::max(item.bytes, item.keptBytes) < minimum)
        return false;
    return containsText(item, m_query);
}

void CleanupFeature::render()
{
    m_rendering = true;
    const QSignalBlocker blocker(m_tree);
    const QString current =
        m_tree->currentItem() ? m_tree->currentItem()->data(NameColumn, kIdRole).toString() : QString();
    m_tree->setSortingEnabled(false);
    m_tree->clear();
    QTreeWidgetItem *restore = nullptr;
    for (const Category category : categories()) {
        auto *parent = new SortItem(m_tree);
        qint64 total = 0;
        qint64 orphaned = 0;
        int count = 0;
        for (const Item &item : std::as_const(m_items)) {
            if (item.category != category || !visible(item))
                continue;
            ++count;
            const qint64 size = item.bytes + item.keptBytes;
            total += size;
            if (item.status == Status::Orphaned)
                orphaned += item.bytes;
            auto *row = new SortItem(parent);
            row->setText(NameColumn, item.title);
            row->setData(NameColumn, kIdRole, item.id);
            row->setToolTip(NameColumn, item.path);
            row->setText(SizeColumn, sizeText(size));
            row->setData(SizeColumn, kSortRole, size);
            if (item.keptBytes > 0 && item.bytes > 0)
                row->setToolTip(SizeColumn, QStringLiteral("%1 can go, %2 is kept")
                                                .arg(sizeText(item.bytes), sizeText(item.keptBytes)));
            row->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
            row->setText(AgeColumn, ageText(item.modifiedMs, m_scannedAtMs));
            row->setData(AgeColumn, kSortRole, m_scannedAtMs - item.modifiedMs);
            row->setText(StatusColumn, QStringLiteral("● %1").arg(statusName(item.status)));
            row->setData(StatusColumn, kSortRole, int(item.status));
            row->setForeground(StatusColumn, statusColor(item.status));
            row->setText(ReasonColumn, item.reasonText());
            row->setToolTip(ReasonColumn, item.reasonText());
            row->setText(PathColumn, item.path);
            row->setToolTip(PathColumn, item.path);
            if (item.deletable()) {
                row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
                row->setCheckState(NameColumn,
                                   m_checked.contains(item.id) ? Qt::Checked : Qt::Unchecked);
            } else {
                row->setFlags(row->flags() & ~Qt::ItemIsUserCheckable);
            }
            if (item.id == current)
                restore = row;
        }
        parent->setText(NameColumn, categoryName(category));
        parent->setText(SizeColumn, sizeText(total));
        parent->setData(SizeColumn, kSortRole, total);
        parent->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
        parent->setText(StatusColumn, QStringLiteral("%1 item%2").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")));
        parent->setText(ReasonColumn, orphaned > 0 ? QStringLiteral("%1 orphaned").arg(sizeText(orphaned))
                                                   : QString());
        QFont font = parent->font(NameColumn);
        font.setBold(true);
        parent->setFont(NameColumn, font);
        parent->setFlags(parent->flags() & ~Qt::ItemIsUserCheckable);
        parent->setExpanded(count > 0 && count <= 60);
    }
    m_tree->setSortingEnabled(true);
    if (restore)
        m_tree->setCurrentItem(restore);
    m_rendering = false;
    updateSummary();
    updateButtons();
}

void CleanupFeature::updateSummary()
{
    if (m_scanning)
        return;
    if (m_scannedAtMs == 0) {
        m_summary->setText(QStringLiteral("Not scanned yet."));
        return;
    }
    qint64 total = 0;
    qint64 orphaned = 0;
    qint64 selected = 0;
    int selectedCount = 0;
    for (const Item &item : std::as_const(m_items)) {
        total += item.bytes + item.keptBytes;
        if (item.status == Status::Orphaned)
            orphaned += item.bytes;
        if (m_checked.contains(item.id)) {
            selected += item.bytes;
            ++selectedCount;
        }
    }
    QString text = QStringLiteral("%1 in %2 items · %3 orphaned")
                       .arg(sizeText(total))
                       .arg(m_items.size())
                       .arg(sizeText(orphaned));
    if (selectedCount > 0)
        text += QStringLiteral(" · %1 selected (%2)").arg(selectedCount).arg(sizeText(selected));
    m_summary->setText(text);
}

void CleanupFeature::updateButtons()
{
    const bool idle = !m_scanning && !m_deleting;
    m_deleteButton->setEnabled(idle && !m_checked.isEmpty());
    m_selectOrphaned->setEnabled(idle && !m_items.isEmpty());
    m_clearButton->setEnabled(idle && !m_checked.isEmpty());
    m_openButton->setEnabled(m_tree->currentItem() &&
                             itemById(m_tree->currentItem()->data(NameColumn, kIdRole).toString()));
    bool ccache = false;
    for (const Item &item : std::as_const(m_items))
        ccache = ccache || (item.kind == Kind::Report && item.title.startsWith(QLatin1String("ccache")));
    m_trimButton->setEnabled(idle && ccache && !m_trim);
}

void CleanupFeature::setStatus(const QString &text, bool error)
{
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

const Item *CleanupFeature::itemById(const QString &id) const
{
    for (const Item &item : m_items) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

QList<Item> CleanupFeature::checkedItems() const
{
    QList<Item> items;
    for (const Item &item : m_items) {
        if (m_checked.contains(item.id) && item.deletable())
            items << item;
    }
    return items;
}

void CleanupFeature::checkOrphaned()
{
    for (const Item &item : std::as_const(m_items)) {
        if (item.status == Status::Orphaned && item.deletable() && !item.needsConfirm() &&
            visible(item))
            m_checked.insert(item.id);
    }
    render();
}

void CleanupFeature::clearChecks()
{
    m_checked.clear();
    render();
}

void CleanupFeature::openSelectedFolder()
{
    QTreeWidgetItem *row = m_tree->currentItem();
    const Item *item = row ? itemById(row->data(NameColumn, kIdRole).toString()) : nullptr;
    if (!item)
        return;
    const QFileInfo info(item->path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? item->path : info.absolutePath()));
}

void CleanupFeature::setHostSearchQuery(const QString &query)
{
    if (m_query == query.trimmed())
        return;
    m_query = query.trimmed();
    render();
}

QList<forkmesh::market::NativePackageSearchHit>
CleanupFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return hits;
    for (const Item &item : m_items) {
        if (hits.size() >= limit)
            break;
        if (!containsText(item, needle))
            continue;
        hits.append({item.id, item.title,
                     QStringLiteral("%1 · %2 · %3")
                         .arg(categoryName(item.category), sizeText(item.bytes + item.keptBytes),
                              statusName(item.status))});
    }
    return hits;
}

void CleanupFeature::activateSearchHit(const QString &id)
{
    const Item *item = itemById(id);
    if (!item)
        return;
    if (!visible(*item)) {
        const QSignalBlocker a(m_orphanedOnly);
        const QSignalBlocker b(m_olderDays);
        const QSignalBlocker c(m_largerMb);
        m_orphanedOnly->setChecked(false);
        m_olderDays->setValue(0);
        m_largerMb->setValue(0);
        m_query.clear();
        render();
    }
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *parent = m_tree->topLevelItem(i);
        for (int j = 0; j < parent->childCount(); ++j) {
            QTreeWidgetItem *row = parent->child(j);
            if (row->data(NameColumn, kIdRole).toString() != id)
                continue;
            parent->setExpanded(true);
            m_tree->setCurrentItem(row);
            m_tree->scrollToItem(row);
            return;
        }
    }
}

bool CleanupFeature::confirmDeletion(const QList<Item> &items, QList<Item> *chosen,
                                     QHash<QString, Choice> *choices)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Delete from disk"));
    dialog.resize(900, 520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral(
        "These will be removed from disk. Attached items need their own tick in Delete.")));
    auto *table = new QTreeWidget(&dialog);
    table->setRootIsDecorated(false);
    table->setColumnCount(6);
    table->setHeaderLabels({QStringLiteral("Delete"), QStringLiteral("Item"), QStringLiteral("Size"),
                            QStringLiteral("Why it is kept around"), QStringLiteral("Discard changes"),
                            QStringLiteral("Retire branch")});
    table->setColumnWidth(ConfirmDelete, 70);
    table->setColumnWidth(ConfirmItem, 260);
    table->setColumnWidth(ConfirmSize, 80);
    table->setColumnWidth(ConfirmWhy, 280);
    for (const Item &item : items) {
        auto *row = new QTreeWidgetItem(table);
        row->setData(ConfirmItem, kIdRole, item.id);
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        row->setCheckState(ConfirmDelete, item.needsConfirm() ? Qt::Unchecked : Qt::Checked);
        row->setText(ConfirmItem, item.title);
        row->setToolTip(ConfirmItem, item.path);
        row->setText(ConfirmSize, sizeText(item.bytes));
        row->setData(ConfirmSize, kSortRole, item.bytes);
        row->setText(ConfirmWhy, item.needsConfirm() ? item.reasonText() : QStringLiteral("nothing"));
        row->setToolTip(ConfirmWhy, item.reasonText());
        if (item.kind == Kind::Worktree && item.dirtyFiles > 0) {
            row->setCheckState(ConfirmDiscard, Qt::Unchecked);
            row->setText(ConfirmDiscard, QStringLiteral("%1 files").arg(item.dirtyFiles));
        }
        if (item.kind == Kind::Worktree && !item.branch.isEmpty()) {
            row->setCheckState(ConfirmRetire, Qt::Unchecked);
            row->setText(ConfirmRetire, item.ahead > 0
                                            ? QStringLiteral("%1 unmerged").arg(item.ahead)
                                            : QStringLiteral("merged"));
        }
    }
    layout->addWidget(table, 1);
    layout->addWidget(mutedLabel(QStringLiteral(
        "Worktrees are removed with git worktree remove, so git stays consistent. A worktree with "
        "uncommitted changes is kept unless Discard changes is ticked; its tracked edits are then "
        "saved as refs/forkmesh/trash/cleanup/<date>/<branch>-wip (untracked files are lost). "
        "Branches are kept unless Retire branch is ticked, which moves the branch to "
        "refs/forkmesh/trash/cleanup/<date>/<branch>; nothing is hard-deleted.")));
    auto *summary = new QLabel(&dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton *go = buttons->addButton(QStringLiteral("Delete"), QDialogButtonBox::DestructiveRole);
    go->setObjectName(QStringLiteral("primaryButton"));
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(go, &QPushButton::clicked, &dialog, &QDialog::accept);

    const auto collect = [&](QList<Item> *picked, QHash<QString, Choice> *picks) {
        for (int i = 0; i < table->topLevelItemCount(); ++i) {
            QTreeWidgetItem *row = table->topLevelItem(i);
            if (row->checkState(ConfirmDelete) != Qt::Checked)
                continue;
            const Item &item = items.at(i);
            Choice choice;
            choice.confirmed = true;
            choice.discardChanges = row->data(ConfirmDiscard, Qt::CheckStateRole).isValid() &&
                                    row->checkState(ConfirmDiscard) == Qt::Checked;
            choice.retireBranch = row->data(ConfirmRetire, Qt::CheckStateRole).isValid() &&
                                  row->checkState(ConfirmRetire) == Qt::Checked;
            picked->append(item);
            picks->insert(item.id, choice);
        }
    };
    const auto recount = [&] {
        QList<Item> picked;
        QHash<QString, Choice> picks;
        collect(&picked, &picks);
        const Plan plan = buildPlan(picked, picks);
        QString text = QStringLiteral("Will remove %1 item%2 and reclaim %3.")
                           .arg(plan.count())
                           .arg(plan.count() == 1 ? QString() : QStringLiteral("s"), sizeText(plan.bytes));
        if (!plan.skipped.isEmpty())
            text += QStringLiteral(" %1 kept: %2.")
                        .arg(plan.skipped.size())
                        .arg(plan.skipped.first().item.title + QStringLiteral(" ") +
                             plan.skipped.first().why);
        summary->setText(text);
        go->setEnabled(plan.count() > 0);
    };
    connect(table, &QTreeWidget::itemChanged, &dialog, [&] { recount(); });
    recount();
    if (dialog.exec() != QDialog::Accepted)
        return false;
    collect(chosen, choices);
    return !chosen->isEmpty();
}

void CleanupFeature::deleteChecked()
{
    if (m_scanning || m_deleting)
        return;
    const QList<Item> items = checkedItems();
    if (items.isEmpty())
        return;
    QList<Item> chosen;
    QHash<QString, Choice> choices;
    if (!confirmDeletion(items, &chosen, &choices))
        return;
    Plan plan = buildPlan(chosen, choices);
    if (forkmesh::cleanupfeed::hasHost() && forkmesh::cleanupfeed::host().agents) {
        const QList<forkmesh::cleanupfeed::Agent> agents = forkmesh::cleanupfeed::host().agents();
        QList<Step> worktrees;
        for (const Step &step : std::as_const(plan.worktrees)) {
            bool live = false;
            for (const forkmesh::cleanupfeed::Agent &agent : agents)
                live = live || (agent.live && (QDir::cleanPath(agent.worktree) == step.item.path ||
                                               (!agent.branch.isEmpty() && agent.branch == step.item.branch)));
            if (live) {
                plan.skipped.append({step.item, QStringLiteral("an agent started using it")});
                plan.bytes -= step.item.bytes;
            } else {
                worktrees << step;
            }
        }
        plan.worktrees = worktrees;
    }
    runPlan(plan);
}

void CleanupFeature::runPlan(const Plan &plan)
{
    m_deleting = true;
    m_outcomes.clear();
    m_planSkipped = int(plan.skipped.size());
    for (const Skip &skip : plan.skipped)
        m_outcomes.append({skip.item.path, skip.item.bytes, skip.item.reasonText(), false, skip.why});
    m_pendingWorktrees = plan.worktrees;
    setStatus(QStringLiteral("Deleting %1 item%2…")
                  .arg(plan.count())
                  .arg(plan.count() == 1 ? QString() : QStringLiteral("s")));
    updateButtons();
    Context context = desktopContext();
    const QList<Step> files = plan.files;
    const QList<Step> sandboxes = plan.sandboxes;
    const SandboxRemover remover = forkmesh::cleanupfeed::hasHost()
                                       ? forkmesh::cleanupfeed::host().removeActionSandbox
                                       : SandboxRemover();
    auto *watcher = new QFutureWatcher<DeletionBatch>(this);
    connect(watcher, &QFutureWatcher<DeletionBatch>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const DeletionBatch batch = watcher->result();
        m_outcomes += batch.outcomes;
        m_deleteProcesses = batch.processes;
        removeNextWorktree();
    });
    watcher->setFuture(QtConcurrent::run([context, files, sandboxes, remover]() mutable {
        context.processes = readProcesses();
        context.nowMs = QDateTime::currentMSecsSinceEpoch();
        Recheck recheck;
        recheck.processes = context.processes;
        recheck.live = liveClaudeSessions(context);
        recheck.nowMs = context.nowMs;
        recheck.liveMinutes = context.liveMinutes;
        DeletionBatch batch;
        batch.outcomes = deleteFiles(files, recheck);
        batch.outcomes += deleteSandboxes(sandboxes, recheck, remover);
        batch.processes = readProcesses();
        return batch;
    }));
}

void CleanupFeature::removeNextWorktree()
{
    if (m_pendingWorktrees.isEmpty()) {
        finishDeletion();
        return;
    }
    const Step step = m_pendingWorktrees.takeFirst();
    Recheck recheck;
    recheck.processes = m_deleteProcesses;
    const QString refusal = worktreeRefusal(step.item, recheck);
    const bool hosted = forkmesh::cleanupfeed::hasHost() &&
                        forkmesh::cleanupfeed::host().removeWorktree;
    if (!refusal.isEmpty() || !hosted) {
        m_outcomes.append({step.item.path, step.item.bytes, step.item.reasonText(), false,
                           refusal.isEmpty() ? QStringLiteral("the desktop is not available to remove it")
                                             : refusal});
        removeNextWorktree();
        return;
    }
    forkmesh::cleanupfeed::WorktreeRemoval request;
    request.repoPath = step.item.repoPath;
    request.worktreePath = step.item.path;
    request.branch = step.item.branch;
    request.discardChanges = step.choice.discardChanges;
    request.retireBranch = step.choice.retireBranch;
    QPointer<CleanupFeature> self(this);
    forkmesh::cleanupfeed::host().removeWorktree(
        request, [self, step](const forkmesh::cleanupfeed::WorktreeRemovalResult &result) {
            if (!self)
                return;
            QString note = result.message;
            if (result.branchRetired)
                note = QStringLiteral("branch saved as %1").arg(result.trashRef);
            if (!result.wipRef.isEmpty())
                note += (note.isEmpty() ? QString() : QStringLiteral("; ")) +
                        QStringLiteral("uncommitted work saved as %1").arg(result.wipRef);
            Outcome outcome{step.item.path, step.item.bytes, step.item.reasonText(), result.removed,
                            result.removed ? QString() : result.message};
            if (result.removed && !note.isEmpty())
                outcome.reason += QStringLiteral(" · ") + note;
            self->m_outcomes.append(outcome);
            self->removeNextWorktree();
        });
}

void CleanupFeature::finishDeletion()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool logged = appendLog(logPath(), m_outcomes, now);
    const qint64 freed = freedBytes(m_outcomes);
    int removed = 0;
    QStringList problems;
    for (const Outcome &outcome : std::as_const(m_outcomes)) {
        if (outcome.removed)
            ++removed;
        else if (problems.size() < 3)
            problems << QStringLiteral("%1: %2").arg(QFileInfo(outcome.path).fileName(), outcome.error);
    }
    const int kept = int(m_outcomes.size()) - removed;
    QString text = QStringLiteral("Freed %1 (%2 removed").arg(sizeText(freed)).arg(removed);
    if (kept > 0)
        text += QStringLiteral(", %1 kept: %2").arg(kept).arg(problems.join(QStringLiteral("; ")));
    text += QStringLiteral(").");
    if (!logged)
        text += QStringLiteral(" The cleanup log could not be written.");
    if (forkmesh::cleanupfeed::hasHost() && forkmesh::cleanupfeed::host().log)
        forkmesh::cleanupfeed::host().log(QStringLiteral("Cleanup: freed %1, %2 removed, %3 kept; see %4")
                                              .arg(sizeText(freed))
                                              .arg(removed)
                                              .arg(kept)
                                              .arg(logPath()));
    m_deleting = false;
    m_checked.clear();
    m_outcomes.clear();
    m_deleteProcesses = ProcessSnapshot();
    refresh();
    setStatus(text, kept > 0 && removed == 0);
}

void CleanupFeature::trimCcache()
{
    QString dir;
    for (const Item &item : std::as_const(m_items)) {
        if (item.kind == Kind::Report && item.title.startsWith(QLatin1String("ccache")))
            dir = item.path;
    }
    if (dir.isEmpty() || m_trim)
        return;
    QMessageBox box(QMessageBox::Question, QStringLiteral("Trim ccache"),
                    QStringLiteral("Evict compiler cache entries not used in the last 14 days from\n%1?\n\n"
                                   "The next build recompiles what it needs.")
                        .arg(dir),
                    QMessageBox::Cancel, this);
    QPushButton *go = box.addButton(QStringLiteral("Trim"), QMessageBox::AcceptRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go)
        return;
    auto *process = new QProcess(this);
    m_trim = process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("CCACHE_DIR"), dir);
    process->setProcessEnvironment(env);
    connect(process, &QProcess::finished, this, [this, process, dir](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        const bool ok = status == QProcess::NormalExit && code == 0;
        if (forkmesh::cleanupfeed::hasHost() && forkmesh::cleanupfeed::host().log)
            forkmesh::cleanupfeed::host().log(
                QStringLiteral("Cleanup: ccache trim of %1 %2").arg(dir, ok ? QStringLiteral("finished")
                                                                           : QStringLiteral("failed")));
        refresh();
        setStatus(ok ? QStringLiteral("Trimmed ccache in %1.").arg(dir)
                     : QStringLiteral("ccache could not trim %1: %2")
                           .arg(dir, QString::fromUtf8(process->readAllStandardError()).trimmed()),
                  !ok);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        setStatus(QStringLiteral("ccache is not installed."), true);
        updateButtons();
    });
    process->start(QStringLiteral("ccache"), {QStringLiteral("--evict-older-than"), QStringLiteral("14d")});
    setStatus(QStringLiteral("Trimming ccache…"));
    updateButtons();
}

} // namespace forkmesh::cleanup

namespace {
const forkmesh::market::NativePackageRegistration cleanupRegistration({
    QStringLiteral("builtin:cleanup"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Cleanup"),
    QStringLiteral("trash"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::cleanup::CleanupFeature(host, parent);
    },
});
} // namespace
