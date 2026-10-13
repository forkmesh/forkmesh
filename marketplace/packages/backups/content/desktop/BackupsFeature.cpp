#include "BackupsFeature.h"

#include "AppPaths.h"
#include "LocalBackupStore.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

namespace forkmesh::backups {
namespace {

enum Column { ShelfColumn, TakenColumn, AgeColumn, SizeColumn, LabelColumn, NotesColumn, NameColumn };

constexpr int kIdRole = Qt::UserRole;
constexpr int kShelfRole = Qt::UserRole + 1;
constexpr int kKeyRole = Qt::UserRole + 2;

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

void restyle(QLabel *label, bool error)
{
    label->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    label->style()->unpolish(label);
    label->style()->polish(label);
}

QString sizeText(qint64 bytes)
{
    return bytes < 0 ? QStringLiteral("…") : QLocale().formattedDataSize(bytes);
}

QString notesFor(const BackupItem &item, const QSet<QString> &leaving)
{
    switch (item.shelf) {
    case Shelf::Snapshots:
        return leaving.contains(item.name) ? QStringLiteral("Removed at the next backup")
                                           : QString();
    case Shelf::Kept:
        return QStringLiteral("Out of rotation");
    case Shelf::PackageData:
        return BackupShelf::packageDataId(item.name);
    case Shelf::MovedAside:
        return item.directory ? QStringLiteral("Data folder") : QStringLiteral("Settings file");
    case Shelf::Fleet:
        return item.encrypted ? QStringLiteral("Encrypted (age)")
                              : QStringLiteral("Not encrypted");
    }
    return QString();
}

QHash<QString, qint64> folderSizes(const QStringList &folders)
{
    QHash<QString, qint64> sizes;
    for (const QString &folder : folders) {
        qint64 total = 0;
        QDirIterator it(folder,
                        QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            total += it.fileInfo().size();
        }
        sizes.insert(folder, total);
    }
    return sizes;
}

} // namespace

BackupsFeature::BackupsFeature(const forkmesh::market::NativePackageHost &host,
                               QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_packageId(host.packageId),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("backupsSection"));
    buildPage();
    forkmesh::backupfeed::addListener(this);
}

BackupsFeature::~BackupsFeature()
{
    forkmesh::backupfeed::removeListener(this);
    if (m_verify) {
        m_verify->disconnect();
        m_verify->kill();
        m_verify->waitForFinished(2000);
    }
}

Locations BackupsFeature::desktopLocations()
{
    Locations locations;
    locations.snapshots = forkmesh::backupfeed::hasHost() &&
                                  forkmesh::backupfeed::host().snapshotRoot
                              ? forkmesh::backupfeed::host().snapshotRoot()
                              : forkmesh::defaultBackupRoot();
    locations.dataRoot = forkmesh::paths::dataRoot();
    locations.settingsFile = forkmesh::paths::settingsFilePath(QSettings());
    locations.fleet = BackupShelf::fleetDirectory(
        qEnvironmentVariable("FMNEXT_BACKUPS_DIR"), qEnvironmentVariable("FMNEXT_DATA_DIR"),
        forkmesh::paths::homeDir());
    return locations;
}

void BackupsFeature::buildPage()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Backups"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(
        QStringLiteral("Every backup ForkMesh keeps on this computer, sorted onto shelves")));
    heading->addStretch();
    auto *refreshButton = ghostButton(QStringLiteral("Refresh"),
                                      QStringLiteral("Scan the backup folders again"));
    auto *openButton = ghostButton(QStringLiteral("Open folder"),
                                   QStringLiteral("Open the snapshot folder"));
    heading->addWidget(refreshButton);
    heading->addWidget(openButton);
    outer->addLayout(heading);
    connect(refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(openButton, &QPushButton::clicked, this, [this] {
        QDir().mkpath(m_locations.snapshots);
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_locations.snapshots));
    });

    m_health = mutedLabel(QString());
    outer->addWidget(m_health);

    auto *controls = new QHBoxLayout;
    m_hourly = new QCheckBox(QStringLiteral("Back up hourly"));
    m_hourly->setCursor(Qt::PointingHandCursor);
    m_hourly->setToolTip(QStringLiteral(
        "Take a snapshot of this desktop's identity, account and settings every hour"));
    m_keep = new QSpinBox;
    m_keep->setRange(forkmesh::kBackupKeepMin, forkmesh::kBackupKeepMax);
    m_keep->setSuffix(QStringLiteral(" in rotation"));
    m_keep->setToolTip(QStringLiteral(
        "How many snapshots rotation keeps. Kept snapshots do not count."));
    m_backUpNow = new QPushButton(QStringLiteral("Back up now"));
    m_backUpNow->setObjectName(QStringLiteral("primaryButton"));
    m_backUpNow->setCursor(Qt::PointingHandCursor);
    controls->addWidget(m_hourly);
    controls->addWidget(m_keep);
    controls->addStretch();
    controls->addWidget(m_backUpNow);
    outer->addLayout(controls);
    connect(m_hourly, &QCheckBox::toggled, this, [this](bool on) {
        if (m_syncing || !forkmesh::backupfeed::hasHost())
            return;
        forkmesh::backupfeed::host().setHourlyEnabled(on);
        refresh();
    });
    connect(m_keep, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int keep) {
        if (m_syncing || !forkmesh::backupfeed::hasHost())
            return;
        forkmesh::backupfeed::host().setKeepCount(keep);
        refresh();
    });
    connect(m_backUpNow, &QPushButton::clicked, this, [this] {
        if (!forkmesh::backupfeed::hasHost())
            return;
        forkmesh::backupfeed::host().backUpNow();
        syncControls();
    });

    auto *splitter = new QSplitter(Qt::Horizontal);
    m_shelves = new QListWidget;
    m_shelves->setObjectName(QStringLiteral("backupsShelves"));
    m_shelves->setMinimumWidth(200);
    splitter->addWidget(m_shelves);

    auto *itemsPane = new QWidget;
    auto *itemsLayout = new QVBoxLayout(itemsPane);
    itemsLayout->setContentsMargins(0, 0, 0, 0);
    itemsLayout->setSpacing(8);
    m_shelfNote = mutedLabel(QString());
    itemsLayout->addWidget(m_shelfNote);
    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("backupsTable"));
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setHeaderLabels({QStringLiteral("Shelf"), QStringLiteral("Taken"),
                             QStringLiteral("Age"), QStringLiteral("Size"),
                             QStringLiteral("Label"), QStringLiteral("Notes"),
                             QStringLiteral("Name")});
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    itemsLayout->addWidget(m_tree, 1);

    auto *actions = new QHBoxLayout;
    m_keepButton = ghostButton(QStringLiteral("Keep"),
                               QStringLiteral("Move this snapshot out of rotation so it is "
                                              "never removed automatically"));
    m_releaseButton = ghostButton(QStringLiteral("Return to rotation"),
                                  QStringLiteral("Let rotation remove this snapshot again"));
    m_labelButton = ghostButton(QStringLiteral("Label…"),
                                QStringLiteral("Name this backup so it is easy to find"));
    m_verifyButton = ghostButton(QStringLiteral("Verify"),
                                 QStringLiteral("Read the whole backup and check it is intact"));
    m_restoreButton = ghostButton(QStringLiteral("Restore…"),
                                  QStringLiteral("Replace the live identity, account and "
                                                 "settings with this snapshot and restart"));
    m_copyButton = ghostButton(QStringLiteral("Copy to…"),
                               QStringLiteral("Save a copy somewhere else, such as a USB drive"));
    m_revealButton = ghostButton(QStringLiteral("Show in folder"),
                                 QStringLiteral("Open the folder that holds this backup"));
    m_deleteButton = new QPushButton(QStringLiteral("Delete…"));
    m_deleteButton->setObjectName(QStringLiteral("dangerButton"));
    m_deleteButton->setCursor(Qt::PointingHandCursor);
    for (QPushButton *button : {m_keepButton, m_releaseButton, m_labelButton, m_verifyButton,
                                m_restoreButton, m_copyButton, m_revealButton})
        actions->addWidget(button);
    actions->addStretch();
    actions->addWidget(m_deleteButton);
    itemsLayout->addLayout(actions);
    splitter->addWidget(itemsPane);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter, 1);

    m_status = mutedLabel(QString());
    m_status->setVisible(false);
    outer->addWidget(m_status);

    connect(m_shelves, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0)
            return;
        m_shelfFilter = m_shelves->item(row)->data(kShelfRole).toInt();
        renderItems();
    });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] { updateActions(); });
    connect(m_keepButton, &QPushButton::clicked, this, [this] { keepSelected(); });
    connect(m_releaseButton, &QPushButton::clicked, this, [this] { releaseSelected(); });
    connect(m_labelButton, &QPushButton::clicked, this, [this] { labelSelected(); });
    connect(m_verifyButton, &QPushButton::clicked, this, [this] { verifySelected(); });
    connect(m_restoreButton, &QPushButton::clicked, this, [this] { restoreSelected(); });
    connect(m_copyButton, &QPushButton::clicked, this, [this] { copySelected(); });
    connect(m_revealButton, &QPushButton::clicked, this, [this] { revealSelected(); });
    connect(m_deleteButton, &QPushButton::clicked, this, [this] { deleteSelected(); });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this] { labelSelected(); });
}

void BackupsFeature::refresh()
{
    m_locations = desktopLocations();
    m_items = BackupShelf::scan(m_locations, loadLabels());
    const int keep = forkmesh::backupfeed::hasHost() ? forkmesh::backupfeed::host().keepCount()
                                                     : forkmesh::kBackupKeepDefault;
    m_leaving = BackupShelf::leavingAtNextBackup(m_items, keep);
    m_loaded = true;
    ++m_generation;
    syncControls();
    renderShelves();
    renderItems();
    measureFolders();
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

void BackupsFeature::prepareSearch()
{
    if (!m_loaded)
        refresh();
}

void BackupsFeature::syncControls()
{
    const bool host = forkmesh::backupfeed::hasHost();
    const bool hourly = host && forkmesh::backupfeed::host().hourlyEnabled();
    m_syncing = true;
    {
        const QSignalBlocker hourlyBlock(m_hourly);
        const QSignalBlocker keepBlock(m_keep);
        m_hourly->setChecked(hourly);
        m_keep->setValue(host ? forkmesh::backupfeed::host().keepCount()
                              : forkmesh::kBackupKeepDefault);
    }
    m_syncing = false;
    m_hourly->setEnabled(host);
    m_keep->setEnabled(host);
    const bool running = host && forkmesh::backupfeed::host().running();
    m_backUpNow->setEnabled(host && !running);
    m_backUpNow->setText(running ? QStringLiteral("Backing up…") : QStringLiteral("Back up now"));

    const Health health = BackupShelf::health(m_items, hourly, QDateTime::currentDateTime());
    m_health->setText(health.text);
    restyle(m_health, health.warning);
}

void BackupsFeature::renderShelves()
{
    const QMap<Shelf, ShelfTotals> totals = BackupShelf::totals(m_items);
    const QSignalBlocker block(m_shelves);
    m_shelves->clear();
    qint64 allBytes = 0;
    for (const ShelfTotals &total : totals)
        allBytes += total.bytes;
    auto *all = new QListWidgetItem(QStringLiteral("All backups\n%1 · %2")
                                        .arg(m_items.size())
                                        .arg(QLocale().formattedDataSize(allBytes)));
    all->setData(kShelfRole, -1);
    m_shelves->addItem(all);
    int current = 0;
    for (Shelf shelf : BackupShelf::shelves()) {
        const ShelfTotals total = totals.value(shelf);
        if (shelf == Shelf::Fleet && total.count == 0)
            continue;
        auto *row = new QListWidgetItem(QStringLiteral("%1\n%2 · %3")
                                            .arg(BackupShelf::title(shelf))
                                            .arg(total.count)
                                            .arg(QLocale().formattedDataSize(total.bytes)));
        row->setData(kShelfRole, static_cast<int>(shelf));
        row->setToolTip(BackupShelf::description(shelf));
        m_shelves->addItem(row);
        if (static_cast<int>(shelf) == m_shelfFilter)
            current = m_shelves->count() - 1;
    }
    if (current == 0)
        m_shelfFilter = -1;
    m_shelves->setCurrentRow(current);
}

void BackupsFeature::renderItems()
{
    const QList<QTreeWidgetItem *> selected = m_tree->selectedItems();
    const QString previous =
        selected.isEmpty() ? QString() : selected.first()->data(0, kKeyRole).toString();
    const QDateTime now = QDateTime::currentDateTime();
    m_shelfNote->setText(m_shelfFilter < 0
                             ? QStringLiteral("Everything on every shelf, newest first within "
                                              "each shelf. Pick a shelf to see what it is for.")
                             : BackupShelf::description(static_cast<Shelf>(m_shelfFilter)));
    m_tree->setColumnHidden(ShelfColumn, m_shelfFilter >= 0);
    {
        const QSignalBlocker block(m_tree);
        m_tree->clear();
        for (int i = 0; i < m_items.size(); ++i) {
            const BackupItem &item = m_items.at(i);
            if (m_shelfFilter >= 0 && static_cast<int>(item.shelf) != m_shelfFilter)
                continue;
            if (!BackupShelf::matches(item, m_query))
                continue;
            auto *row = new QTreeWidgetItem;
            row->setText(ShelfColumn, BackupShelf::title(item.shelf));
            row->setText(TakenColumn, QLocale().toString(item.taken, QLocale::ShortFormat));
            row->setText(AgeColumn, BackupShelf::ageText(item.taken, now));
            row->setText(SizeColumn, sizeText(item.bytes));
            row->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
            row->setText(LabelColumn, item.label);
            row->setText(NotesColumn, notesFor(item, m_leaving));
            row->setText(NameColumn, item.name);
            row->setToolTip(NameColumn, QDir::toNativeSeparators(item.path));
            row->setData(0, kIdRole, i);
            row->setData(0, kKeyRole, BackupShelf::itemId(item));
            m_tree->addTopLevelItem(row);
        }
    }
    if (!previous.isEmpty())
        selectItem(previous);
    if (m_tree->topLevelItemCount() == 0)
        m_shelfNote->setText(m_shelfNote->text() + QStringLiteral("\n\n") +
                             (m_query.isEmpty()
                                  ? QStringLiteral("Nothing on this shelf.")
                                  : QStringLiteral("No backup matches \"%1\".").arg(m_query)));
    updateActions();
}

void BackupsFeature::updateActions()
{
    const BackupItem *item = selectedItem();
    const bool host = forkmesh::backupfeed::hasHost();
    m_keepButton->setVisible(!item || BackupShelf::canKeep(*item));
    m_releaseButton->setVisible(item && BackupShelf::canRelease(*item));
    m_keepButton->setEnabled(item && BackupShelf::canKeep(*item));
    m_releaseButton->setEnabled(item && BackupShelf::canRelease(*item));
    m_labelButton->setEnabled(item != nullptr);
    m_verifyButton->setEnabled(item && !m_verify &&
                               BackupShelf::verifyKind(*item) != VerifyKind::None);
    m_restoreButton->setEnabled(host && item && BackupShelf::canRestore(*item));
    m_copyButton->setEnabled(item && !m_copying && BackupShelf::canCopy(*item));
    m_revealButton->setEnabled(item != nullptr);
    m_deleteButton->setEnabled(item && BackupShelf::canDelete(*item, m_locations));
    if (item && item->shelf == Shelf::Fleet)
        m_deleteButton->setToolTip(QStringLiteral(
            "Fleet backups are removed only by tools/next_backup.py prune"));
    else
        m_deleteButton->setToolTip(QStringLiteral("Delete this backup permanently"));
}

void BackupsFeature::measureFolders()
{
    QStringList folders;
    for (const BackupItem &item : m_items) {
        if (item.directory)
            folders.append(item.path);
    }
    if (folders.isEmpty())
        return;
    const quint64 generation = m_generation;
    auto *watcher = new QFutureWatcher<QHash<QString, qint64>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, qint64>>::finished, this,
            [this, watcher, generation] {
                const QHash<QString, qint64> sizes = watcher->result();
                watcher->deleteLater();
                if (generation != m_generation)
                    return;
                for (BackupItem &item : m_items) {
                    if (item.directory && sizes.contains(item.path))
                        item.bytes = sizes.value(item.path);
                }
                renderShelves();
                renderItems();
            });
    watcher->setFuture(QtConcurrent::run(folderSizes, folders));
}

void BackupsFeature::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    restyle(m_status, error);
    m_status->setVisible(!text.isEmpty());
}

const BackupItem *BackupsFeature::selectedItem() const
{
    const QList<QTreeWidgetItem *> selected = m_tree ? m_tree->selectedItems()
                                                     : QList<QTreeWidgetItem *>();
    if (selected.isEmpty())
        return nullptr;
    const int index = selected.first()->data(0, kIdRole).toInt();
    return index >= 0 && index < m_items.size() ? &m_items.at(index) : nullptr;
}

void BackupsFeature::selectItem(const QString &id)
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *row = m_tree->topLevelItem(i);
        if (row->data(0, kKeyRole).toString() == id) {
            m_tree->setCurrentItem(row);
            m_tree->scrollToItem(row);
            return;
        }
    }
}

QString BackupsFeature::labelsKey() const
{
    return QStringLiteral("market/packages/%1/data/labels").arg(m_packageId);
}

QHash<QString, QString> BackupsFeature::loadLabels() const
{
    return BackupShelf::parseLabels(QSettings().value(labelsKey()).toString().toUtf8());
}

void BackupsFeature::saveLabels(const QHash<QString, QString> &labels)
{
    QSettings settings;
    if (labels.isEmpty())
        settings.remove(labelsKey());
    else
        settings.setValue(labelsKey(),
                          QString::fromUtf8(BackupShelf::serializeLabels(labels)));
}

void BackupsFeature::setHostSearchQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    if (m_loaded)
        renderItems();
}

QList<forkmesh::market::NativePackageSearchHit>
BackupsFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    if (query.trimmed().isEmpty())
        return hits;
    const QDateTime now = QDateTime::currentDateTime();
    for (const BackupItem &item : m_items) {
        if (hits.size() >= limit)
            break;
        if (!BackupShelf::matches(item, query))
            continue;
        hits.append({BackupShelf::itemId(item),
                     item.label.isEmpty() ? item.name : item.label,
                     QStringLiteral("%1 · %2")
                         .arg(BackupShelf::title(item.shelf),
                              BackupShelf::ageText(item.taken, now))});
    }
    return hits;
}

void BackupsFeature::activateSearchHit(const QString &id)
{
    if (!m_loaded)
        refresh();
    m_query.clear();
    m_shelfFilter = -1;
    renderShelves();
    renderItems();
    selectItem(id);
}

void BackupsFeature::backupStatus(const QString &text, bool error)
{
    if (!m_loaded)
        return;
    setStatus(text, error);
    refresh();
}

void BackupsFeature::keepSelected()
{
    const BackupItem *item = selectedItem();
    if (!item || !BackupShelf::canKeep(*item))
        return;
    const QString target = BackupShelf::keptPath(*item, m_locations);
    const QString name = item->name;
    QDir().mkpath(m_locations.kept());
    if (QFileInfo::exists(target) || !QFile::rename(item->path, target)) {
        setStatus(QStringLiteral("Could not keep %1.").arg(name), true);
        return;
    }
    if (forkmesh::backupfeed::hasHost())
        forkmesh::backupfeed::host().snapshotsChanged();
    setStatus(QStringLiteral("Kept %1. Rotation will not remove it.").arg(name));
    m_shelfFilter = static_cast<int>(Shelf::Kept);
    refresh();
    selectItem(BackupShelf::key(Shelf::Kept) + QLatin1Char(':') + name);
}

void BackupsFeature::releaseSelected()
{
    const BackupItem *item = selectedItem();
    if (!item || !BackupShelf::canRelease(*item))
        return;
    const QString target = BackupShelf::releasedPath(*item, m_locations);
    const QString name = item->name;
    QDir().mkpath(m_locations.snapshots);
    if (QFileInfo::exists(target) || !QFile::rename(item->path, target)) {
        setStatus(QStringLiteral("Could not return %1 to rotation.").arg(name), true);
        return;
    }
    if (forkmesh::backupfeed::hasHost())
        forkmesh::backupfeed::host().snapshotsChanged();
    setStatus(QStringLiteral("%1 is back in rotation; it is removed once it is older than "
                             "the snapshots rotation keeps.")
                  .arg(name));
    refresh();
}

void BackupsFeature::labelSelected()
{
    const BackupItem *item = selectedItem();
    if (!item)
        return;
    const QString name = item->name;
    const QString id = BackupShelf::itemId(*item);
    bool ok = false;
    const QString text = QInputDialog::getText(
        this, QStringLiteral("Label backup"),
        QStringLiteral("A label for %1 (leave empty to remove it):").arg(name), QLineEdit::Normal,
        item->label, &ok);
    if (!ok)
        return;
    QHash<QString, QString> labels = loadLabels();
    const QString label = BackupShelf::normalizedLabel(text);
    if (label.isEmpty())
        labels.remove(name);
    else
        labels.insert(name, label);
    saveLabels(labels);
    refresh();
    selectItem(id);
}

void BackupsFeature::verifySelected()
{
    const BackupItem *item = selectedItem();
    if (!item || m_verify)
        return;
    const QString name = item->name;
    const VerifyKind kind = BackupShelf::verifyKind(*item);
    if (kind == VerifyKind::PackageData) {
        QFile file(item->path);
        QString packageId;
        int keys = 0;
        if (file.size() <= 16 * 1024 * 1024 && file.open(QIODevice::ReadOnly) &&
            BackupShelf::packageDataValid(file.readAll(), &packageId, &keys)) {
            setStatus(QStringLiteral("%1 is intact: %2 setting(s) for %3.")
                          .arg(name)
                          .arg(keys)
                          .arg(packageId));
        } else {
            setStatus(QStringLiteral("%1 is damaged or is not a package data copy.").arg(name),
                      true);
        }
        return;
    }
    const QString program = QStandardPaths::findExecutable(
        kind == VerifyKind::Gzip ? QStringLiteral("gzip") : QStringLiteral("tar"));
    if (program.isEmpty()) {
        setStatus(QStringLiteral("Can't verify: '%1' was not found on this system.")
                      .arg(kind == VerifyKind::Gzip ? QStringLiteral("gzip")
                                                    : QStringLiteral("tar")),
                  true);
        return;
    }
    auto *process = new QProcess(this);
    m_verify = process;
    auto entries = std::make_shared<qint64>(0);
    connect(process, &QProcess::readyReadStandardOutput, this, [process, entries] {
        *entries += process->readAllStandardOutput().count('\n');
    });
    connect(process, &QProcess::finished, this,
            [this, process, entries, name, kind](int code, QProcess::ExitStatus status) {
                *entries += process->readAllStandardOutput().count('\n');
                const QString err =
                    QString::fromUtf8(process->readAllStandardError()).trimmed().left(300);
                process->deleteLater();
                m_verify = nullptr;
                if (status == QProcess::NormalExit && code == 0) {
                    setStatus(kind == VerifyKind::TarGzip
                                  ? QStringLiteral("%1 is intact: %2 entries read.")
                                        .arg(name)
                                        .arg(*entries)
                                  : QStringLiteral("%1 is intact.").arg(name));
                } else {
                    setStatus(QStringLiteral("%1 failed verification%2")
                                  .arg(name, err.isEmpty() ? QStringLiteral(".")
                                                           : QStringLiteral(": ") + err),
                              true);
                }
                updateActions();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                m_verify = nullptr;
                setStatus(QStringLiteral("Can't verify: the checker could not be started."),
                          true);
                updateActions();
            });
    setStatus(QStringLiteral("Verifying %1… a large snapshot can take a minute.").arg(name));
    process->start(program, BackupShelf::verifyArguments(*item));
    updateActions();
}

void BackupsFeature::restoreSelected()
{
    const BackupItem *item = selectedItem();
    if (!item || !BackupShelf::canRestore(*item) || !forkmesh::backupfeed::hasHost())
        return;
    forkmesh::backupfeed::host().restore(item->path);
}

void BackupsFeature::copySelected()
{
    const BackupItem *item = selectedItem();
    if (!item || m_copying || !BackupShelf::canCopy(*item))
        return;
    const QString source = item->path;
    const QString name = item->name;
    const QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("Copy backup"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(name));
    if (target.isEmpty())
        return;
    if (QDir::cleanPath(QFileInfo(target).absoluteFilePath()) ==
        QDir::cleanPath(QFileInfo(source).absoluteFilePath()))
        return;
    m_copying = true;
    updateActions();
    setStatus(QStringLiteral("Copying %1…").arg(name));
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, name, target] {
        const bool ok = watcher->result();
        watcher->deleteLater();
        m_copying = false;
        if (ok)
            setStatus(QStringLiteral("Copied %1 to %2.").arg(name, QDir::toNativeSeparators(target)));
        else
            setStatus(QStringLiteral("Could not copy %1 to %2.")
                          .arg(name, QDir::toNativeSeparators(target)),
                      true);
        updateActions();
    });
    watcher->setFuture(QtConcurrent::run([source, target] {
        const QString part = target + QStringLiteral(".part");
        QFile::remove(part);
        if (!QFile::copy(source, part))
            return false;
        if (QFileInfo::exists(target) && !QFile::remove(target)) {
            QFile::remove(part);
            return false;
        }
        return QFile::rename(part, target);
    }));
}

void BackupsFeature::revealSelected()
{
    const BackupItem *item = selectedItem();
    if (!item)
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(item->path).absolutePath()));
}

void BackupsFeature::deleteSelected()
{
    const BackupItem *item = selectedItem();
    if (!item || !BackupShelf::canDelete(*item, m_locations))
        return;
    const BackupItem chosen = *item;
    QString warning = QStringLiteral("Permanently delete this backup?\n\n%1").arg(chosen.name);
    if (!chosen.label.isEmpty())
        warning += QStringLiteral(" (%1)").arg(chosen.label);
    if (chosen.shelf == Shelf::MovedAside && chosen.directory)
        warning += QStringLiteral("\n\nThis is a whole data folder a restore moved aside, "
                                  "including the identity key it held.");
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Delete backup"), warning,
                    QMessageBox::Cancel, this);
    QPushButton *go = box.addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go)
        return;
    m_locations = desktopLocations();
    if (!BackupShelf::canDelete(chosen, m_locations)) {
        setStatus(QStringLiteral("%1 changed on disk; nothing was deleted.").arg(chosen.name),
                  true);
        refresh();
        return;
    }
    const bool removed = chosen.directory ? QDir(chosen.path).removeRecursively()
                                          : QFile::remove(chosen.path);
    QHash<QString, QString> labels = loadLabels();
    if (removed && labels.remove(chosen.name) > 0)
        saveLabels(labels);
    if (chosen.shelf == Shelf::Snapshots && forkmesh::backupfeed::hasHost())
        forkmesh::backupfeed::host().snapshotsChanged();
    setStatus(removed ? QStringLiteral("Deleted %1.").arg(chosen.name)
                      : QStringLiteral("Could not delete all of %1.").arg(chosen.name),
              !removed);
    refresh();
}

} // namespace forkmesh::backups

namespace {
const forkmesh::market::NativePackageRegistration backupsRegistration({
    QStringLiteral("builtin:backups"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Backups"),
    QStringLiteral("history"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::backups::BackupsFeature(host, parent);
    },
});
} // namespace
