
#include "MainWindow.h"
#include "ProcessReaper.h"
#include "MainWindowInternal.h"
#include "KebabHeaderView.h"
#include "LocalBackupStore.h"
#include "BackupHostFeed.h"
#include "SafeDelete.h"

#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#include <unistd.h>
#endif
#if defined(Q_OS_LINUX)
#include <sys/syscall.h>
#endif

using namespace forkmesh::ui;

namespace {

#if defined(Q_OS_UNIX)
constexpr int kBackgroundNice = 19;
#if defined(Q_OS_LINUX)
constexpr int kIoprioWhoProcess = 1;
constexpr int kIoprioBestEffortLowest = (2 << 13) | 7;
#endif

void lowerBackgroundPriority()
{
    ::setpriority(PRIO_PROCESS, 0, kBackgroundNice);
#if defined(Q_OS_LINUX)
    ::syscall(SYS_ioprio_set, kIoprioWhoProcess, 0, kIoprioBestEffortLowest);
#endif
}
#endif

QString userDataWipeRefusal(const QString &path)
{
    namespace sd = forkmesh::safedelete;
    if (path.trimmed().isEmpty())
        return QStringLiteral("the path is empty");
    const QString clean = sd::detail::clean(path);
    if (clean == QLatin1String("/"))
        return QStringLiteral("the path is /");
    for (const QString &home : sd::detail::homes()) {
        if (sd::detail::within(home, clean))
            return QStringLiteral("%1 is HOME or contains it").arg(clean);
    }
    if (!sd::detail::gitEntry(clean).isEmpty())
        return QStringLiteral("%1 is a git checkout or worktree").arg(clean);
    for (const auto &root : sd::detail::protectedRoots()) {
        const bool container = root.second == sd::RootKind::Projects ||
                               root.second == sd::RootKind::Project ||
                               root.second == sd::RootKind::Frozen ||
                               root.first.endsWith(QStringLiteral("/src"));
        if (sd::detail::inside(root.first, clean) ||
            (container && root.first == clean))
            return QStringLiteral("%1 is or contains %2").arg(clean, root.first);
    }
    return QString();
}

struct DataDir {
    QString label; // friendly name
    QString path;  // absolute path
    QString hint;  // what lives here (tooltip)
    bool inBackup; // included in export/import (true = configuration, not bulk cache)
    forkmesh::paths::Location::Scope scope = forkmesh::paths::Location::Scope::Project;
    bool isLink = false;    // compatibility link left by the layout migration
    bool deletable = false; // only what this project owns and can regenerate or reset
    bool countInFull = false; // a total row: do not carve nested rows out of it
};

struct DirStat {
    int folders = 0;
    int files = 0;
    qint64 bytes = 0;
};

void scanInto(const QString &path, const QSet<QString> &prune, DirStat &s)
{
    const QFileInfo self(path);
    if (self.isFile() && !self.isSymLink()) {
        s.files++;
        s.bytes += self.size();
        return;
    }
    const QFileInfoList entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                 QDir::Hidden | QDir::System);
    for (const QFileInfo &fi : entries) {
        // Never follow symlinks: they can loop and they point outside the tree.
        if (fi.isSymLink()) {
            s.files++;
            continue;
        }
        if (fi.isDir()) {
            s.folders++;
            const QString abs = fi.absoluteFilePath();
            if (!prune.contains(abs))
                scanInto(abs, prune, s);
        } else {
            s.files++;
            s.bytes += fi.size();
        }
    }
}

QString scopeText(forkmesh::paths::Location::Scope scope)
{
    using Scope = forkmesh::paths::Location::Scope;
    switch (scope) {
    case Scope::Project:
        return QStringLiteral("This project");
    case Scope::Shared:
        return QStringLiteral("Shared by all projects");
    case Scope::Foreign:
        return QStringLiteral("Another project");
    case Scope::Legacy:
        return QStringLiteral("Outside the projects folder");
    }
    return QString();
}

// Every location ForkMesh has on this computer: the open project's roots, the
// shared home folders, sibling projects, and anything still outside the
// projects folder. The settings-driven roots ride along as extras because a
// user may have pointed them anywhere.
QVector<DataDir> resolveDataDirs(const QString &mirrorRoot,
                                 const QString &previewRoot,
                                 const QString &backupRoot)
{
    using forkmesh::paths::Location;
    QVector<Location> extra;
    auto addExtra = [&extra](const QString &id, const QString &label,
                             const QString &path, const QString &hint) {
        if (path.isEmpty())
            return;
        Location location;
        location.id = id;
        location.label = label;
        location.path = path;
        location.hint = hint;
        location.scope = Location::Scope::Project;
        extra.append(location);
    };
    addExtra(QStringLiteral("backups"), QStringLiteral("Backups"), backupRoot,
             QStringLiteral("Hourly snapshots of your configuration data. Never archived "
                            "into an export or another snapshot \xE2\x80\x94 no backups "
                            "of backups."));
    addExtra(QStringLiteral("mirrors"), QStringLiteral("Repository mirrors"), mirrorRoot,
             QStringLiteral("Local copies of every repository you mirror or fork. "
                            "Re-downloadable from the network."));
    addExtra(QStringLiteral("preview"), QStringLiteral("Browse cache"), previewRoot,
             QStringLiteral("Temporary browse-only clones created while exploring the "
                            "network."));

    // Before the layout is active (tests, helper binaries) the map is built
    // from the same accessors every store uses.
    forkmesh::paths::Layout layout = forkmesh::paths::layout();
    const forkmesh::paths::Environment environment =
        forkmesh::paths::Environment::fromProcess(QCoreApplication::arguments());
    if (!forkmesh::paths::isActive()) {
        layout = forkmesh::paths::resolve(environment);
        layout.dataDir = forkmesh::paths::dataDir();
        layout.data = forkmesh::paths::dataRoot();
        layout.cache = forkmesh::paths::cacheRoot();
        layout.config = forkmesh::paths::settingsDir();
        layout.state = forkmesh::paths::stateRoot();
        layout.logs = forkmesh::paths::logsDir();
        layout.ssh = forkmesh::paths::sshDir();
    }

    QVector<DataDir> dirs;
    for (const Location &location :
         forkmesh::paths::storageMap(environment, layout, extra)) {
        DataDir dir;
        dir.label = location.label;
        dir.path = location.path;
        dir.hint = location.hint;
        dir.inBackup = location.inBackup;
        dir.scope = location.scope;
        dir.isLink = location.isCompatLink;
        const bool total = location.id == QLatin1String("project") ||
                           location.id == QLatin1String("home") ||
                           location.scope == Location::Scope::Foreign;
        dir.countInFull = total;
        // Deleting is offered only for this project's own roots. The project
        // folder, the source checkout, other projects and whatever is still
        // outside the projects folder are never one click from gone.
        dir.deletable = location.scope == Location::Scope::Project && !total &&
                        !location.isCompatLink;
        dirs.append(dir);
    }
    return dirs;
}

} // namespace


QWidget *MainWindow::buildDataSection()
{
    auto *page = new QWidget;
    auto *col = new QVBoxLayout(page);
    col->setContentsMargins(2, 14, 2, 14);
    col->setSpacing(10);

    auto *storageLabel = new QLabel("STORAGE MAP");
    storageLabel->setObjectName("sectionLabel");
    auto *storageHint = new QLabel(
        "Every folder and file ForkMesh keeps on this computer, and which "
        "project it belongs to. Everything lives under the projects folder; "
        "anything listed as outside it is either still waiting to be moved or "
        "a link left so older tools keep working. Open a row to explore it, or "
        "delete one to reclaim space \xE2\x80\x94 mirrors and caches come back "
        "on their own, but deleting your application data removes the identity "
        "key that is your account.");
    storageHint->setObjectName("statusLine");
    storageHint->setWordWrap(true);

    m_dataDirTable = new QTableWidget(0, 7);
    installColumnHeaderMenu(m_dataDirTable);
    m_dataDirTable->setHorizontalHeaderLabels(
        {"Location", "Path", "Folders", "Files", "Size", "", "Belongs to"});
    m_dataDirTable->verticalHeader()->setVisible(false);
    m_dataDirTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_dataDirTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_dataDirTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_dataDirTable->setWordWrap(false);
    m_dataDirTable->setTextElideMode(Qt::ElideMiddle);
    QHeaderView *dataHeader = m_dataDirTable->horizontalHeader();
    dataHeader->setStretchLastSection(false);
    for (int c = 0; c < m_dataDirTable->columnCount(); ++c)
        dataHeader->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    dataHeader->setSectionResizeMode(1, QHeaderView::Stretch); // path takes slack

    auto *refreshButton = new QPushButton("Refresh");
    refreshButton->setObjectName("ghostButton");
    refreshButton->setCursor(Qt::PointingHandCursor);
    connect(refreshButton, &QPushButton::clicked, this,
            &MainWindow::refreshDataDirTable);

    auto *logsLabel = new QLabel("LOG FILES");
    logsLabel->setObjectName("sectionLabel");
    auto *logsHint = new QLabel(
        "Where ForkMesh writes its diagnostics on this computer. Both are plain "
        "text \xE2\x80\x94 open them, or hand the paths to a coding agent when "
        "reporting a problem.");
    logsHint->setObjectName("statusLine");
    logsHint->setWordWrap(true);

    auto *logsGrid = new QGridLayout;
    logsGrid->setContentsMargins(0, 0, 0, 0);
    logsGrid->setHorizontalSpacing(10);
    logsGrid->setColumnStretch(1, 1);
    int logRow = 0;
    const auto addLogRow = [&](const QString &name, const QString &path,
                               const QString &hint) {
        if (path.isEmpty())
            return;
        auto *nameLabel = new QLabel(name);
        nameLabel->setToolTip(hint);
        auto *pathLabel = new QLabel(QDir::toNativeSeparators(path));
        pathLabel->setObjectName("statusLine");
        pathLabel->setToolTip(hint);
        pathLabel->setTextFormat(Qt::PlainText);
        pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *openButton = new QPushButton("Open");
        openButton->setObjectName("ghostButton");
        openButton->setCursor(Qt::PointingHandCursor);
        openButton->setEnabled(QFileInfo::exists(path));
        openButton->setToolTip(openButton->isEnabled()
                                   ? QStringLiteral("Open this log in your text editor")
                                   : QStringLiteral("Nothing written to this log yet"));
        connect(openButton, &QPushButton::clicked, this, [path] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        auto *copyButton = new QPushButton("Copy path");
        copyButton->setObjectName("ghostButton");
        copyButton->setCursor(Qt::PointingHandCursor);
        connect(copyButton, &QPushButton::clicked, this, [this, path] {
            QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(path));
            setDataStatus(
                QStringLiteral("Copied %1").arg(QDir::toNativeSeparators(path)));
        });
        logsGrid->addWidget(nameLabel, logRow, 0);
        logsGrid->addWidget(pathLabel, logRow, 1);
        logsGrid->addWidget(openButton, logRow, 2);
        logsGrid->addWidget(copyButton, logRow, 3);
        ++logRow;
    };
    addLogRow(QStringLiteral("App log"), networkLogPath(),
              QStringLiteral("Everything the Log view shows: network calls, sync, "
                             "agents and system messages. Kept for %1 days - once "
                             "this file fills it is rotated into network_log.d "
                             "next to it, never overwritten.")
                  .arg(forkmesh::netlog::kRetentionDays));
    addLogRow(QStringLiteral("UI-stall log"), stallLogPath(),
              QStringLiteral("Backtraces for every GUI-thread freeze the watchdog "
                             "records; the footer's stall badge drafts a fix-it "
                             "prompt from these."));

    auto *autoLabel = new QLabel("AUTOMATIC BACKUPS");
    autoLabel->setObjectName("sectionLabel");
    auto *autoHint = new QLabel(
        "With this on, every hour ForkMesh writes a snapshot of the live "
        "database \xE2\x80\x94 your identity key, account, chat history, agents, "
        "issues and pull requests \xE2\x80\x94 to this computer's drive. It "
        "starts on only for control nodes (the ones holding a Cloudflare API "
        "token), since a day of snapshots can run to gigabytes; tick the box to "
        "turn it on here. Restore any snapshot below to roll the whole database "
        "back to that moment. Each one is an ordinary .tar.gz, so it can also "
        "be recovered by hand with \"tar xzf\".");
    autoHint->setObjectName("statusLine");
    autoHint->setWordWrap(true);

    m_backupEnabledCheck = new QCheckBox("Back up hourly to this computer");
    m_backupEnabledCheck->setChecked(autoBackupEnabled());
    m_backupEnabledCheck->setCursor(Qt::PointingHandCursor);
    connect(m_backupEnabledCheck, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(kAutoBackupEnabledSetting, on);
        startAutoBackups(); // arms or stops the hourly timer
        setBackupStatus(on ? QStringLiteral("Hourly backups are on.")
                           : QString::fromUtf8("Hourly backups are off \xE2\x80\x94 "
                                            "existing snapshots are kept."));
    });

    m_backupKeepSpin = new QSpinBox;
    m_backupKeepSpin->setRange(forkmesh::kBackupKeepMin, forkmesh::kBackupKeepMax);
    m_backupKeepSpin->setValue(backupKeepCount());
    m_backupKeepSpin->setSuffix(" kept");
    m_backupKeepSpin->setToolTip(
        "How many snapshots to keep before the oldest is deleted. 24 hourly "
        "snapshots cover a full day.");
    connect(m_backupKeepSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int keep) {
                QSettings().setValue(kAutoBackupKeepSetting, keep);
                pruneOldBackups();
                refreshBackupTable();
            });

    m_backupNowButton = new QPushButton("Back up now");
    m_backupNowButton->setObjectName("primaryButton");
    m_backupNowButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_backupNowButton, "history", 16);
    connect(m_backupNowButton, &QPushButton::clicked, this,
            [this] { takeBackupNow(false); });

    auto *openBackupsButton = new QPushButton("Open backup folder");
    openBackupsButton->setObjectName("ghostButton");
    openBackupsButton->setCursor(Qt::PointingHandCursor);
    setOcticon(openBackupsButton, "file-directory", 16);
    connect(openBackupsButton, &QPushButton::clicked, this, [this] {
        QDir().mkpath(backupRoot());
        QDesktopServices::openUrl(QUrl::fromLocalFile(backupRoot()));
    });

    auto *autoRow = new QHBoxLayout;
    autoRow->setContentsMargins(0, 0, 0, 0);
    autoRow->addWidget(m_backupEnabledCheck);
    autoRow->addSpacing(12);
    autoRow->addWidget(m_backupKeepSpin);
    autoRow->addSpacing(12);
    autoRow->addWidget(m_backupNowButton);
    autoRow->addWidget(openBackupsButton);
    autoRow->addStretch();

    m_backupTable = new QTableWidget(0, 5);
    installColumnHeaderMenu(m_backupTable);
    m_backupTable->setHorizontalHeaderLabels(
        {"Taken", "Age", "Size", "File", ""});
    m_backupTable->verticalHeader()->setVisible(false);
    m_backupTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_backupTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_backupTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_backupTable->setWordWrap(false);
    m_backupTable->setTextElideMode(Qt::ElideMiddle);
    QHeaderView *backupHeader = m_backupTable->horizontalHeader();
    backupHeader->setStretchLastSection(false);
    for (int c = 0; c < m_backupTable->columnCount(); ++c)
        backupHeader->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    backupHeader->setSectionResizeMode(3, QHeaderView::Stretch); // file name

    m_backupStatus = new QLabel;
    m_backupStatus->setObjectName("statusLine");
    m_backupStatus->setWordWrap(true);
    m_backupStatus->setVisible(false);

    auto *backupLabel = new QLabel("EXPORT & IMPORT");
    backupLabel->setObjectName("sectionLabel");
    auto *backupHint = new QLabel(
        "Same archive as an hourly snapshot, saved wherever you choose \xE2\x80\x94 "
        "for moving your configuration to another computer or keeping a copy "
        "off this drive. Importing replaces the current configuration and "
        "restarts ForkMesh.");
    backupHint->setObjectName("statusLine");
    backupHint->setWordWrap(true);

    auto *exportButton = new QPushButton("Export configuration\xE2\x80\xA6");
    m_exportConfigButton = exportButton;
    exportButton->setObjectName("primaryButton");
    exportButton->setCursor(Qt::PointingHandCursor);
    setOcticon(exportButton, "upload", 16);
    connect(exportButton, &QPushButton::clicked, this,
            &MainWindow::exportConfigData);

    auto *importButton = new QPushButton("Import configuration\xE2\x80\xA6");
    m_importConfigButton = importButton;
    importButton->setObjectName("ghostButton");
    importButton->setCursor(Qt::PointingHandCursor);
    setOcticon(importButton, "download", 16);
    connect(importButton, &QPushButton::clicked, this,
            &MainWindow::importConfigData);
    // The page can be rebuilt while a tar is still running.
    setConfigArchiveBusy(m_configArchiveProcess != nullptr);

    auto *backupRow = new QHBoxLayout;
    backupRow->setContentsMargins(0, 0, 0, 0);
    backupRow->addWidget(exportButton);
    backupRow->addWidget(importButton);
    backupRow->addStretch();

    m_dataStatus = new QLabel;
    m_dataStatus->setObjectName("statusLine");
    m_dataStatus->setWordWrap(true);
    m_dataStatus->setVisible(false);

    auto *dangerLabel = new QLabel("DELETE EVERYTHING");
    dangerLabel->setObjectName("sectionLabel");
    auto *dangerHint = new QLabel(
        "Erase all ForkMesh data at once and start fresh. This removes your "
        "identity, settings, mirrors and caches, then restarts ForkMesh. The "
        "app itself stays installed. This cannot be undone.");
    dangerHint->setObjectName("statusLine");
    dangerHint->setWordWrap(true);
    auto *deleteAllButton = new QPushButton("Delete all ForkMesh data\xE2\x80\xA6");
    deleteAllButton->setObjectName("dangerButton");
    deleteAllButton->setCursor(Qt::PointingHandCursor);
    connect(deleteAllButton, &QPushButton::clicked, this,
            &MainWindow::deleteAllData);

    col->addWidget(storageLabel);
    col->addWidget(storageHint);
    if (forkmesh::paths::isActive() && !forkmesh::paths::layout().rerootAllowed) {
        // Say why files are still in the old places, and what unblocks it.
        auto *layoutNote = new QLabel(forkmesh::paths::layout().rerootBlockedReason);
        layoutNote->setObjectName("statusLine");
        layoutNote->setWordWrap(true);
        layoutNote->setTextInteractionFlags(Qt::TextSelectableByMouse);
        col->addWidget(layoutNote);
    }
    col->addWidget(m_dataDirTable, 1);
    col->addWidget(refreshButton, 0, Qt::AlignLeft);
    col->addSpacing(6);
    col->addWidget(logsLabel);
    col->addWidget(logsHint);
    col->addLayout(logsGrid);
    col->addSpacing(6);
    col->addWidget(autoLabel);
    col->addWidget(autoHint);
    col->addLayout(autoRow);
    col->addWidget(m_backupTable, 1);
    col->addWidget(m_backupStatus);
    col->addSpacing(6);
    col->addWidget(backupLabel);
    col->addWidget(backupHint);
    col->addLayout(backupRow);
    col->addWidget(m_dataStatus);
    col->addSpacing(6);
    col->addWidget(dangerLabel);
    col->addWidget(dangerHint);
    col->addWidget(deleteAllButton, 0, Qt::AlignLeft);
    col->addStretch();

    refreshDataDirTable();
    refreshBackupTable();
    return page;
}

void MainWindow::refreshDataDirTable()
{
    if (!m_dataDirTable)
        return;
    const QVector<DataDir> dirs = resolveDataDirs(
        repositoryMirrorRoot(), repositoryPreviewRoot(), backupRoot());
    // A row nested inside another is carved out of its parent, so sizes add
    // up instead of counting the same bytes twice. Total rows opt out.
    QSet<QString> nested;
    for (const DataDir &dir : dirs) {
        if (!dir.countInFull && !dir.isLink)
            nested.insert(dir.path);
    }

    const quint64 generation = ++m_dataDirScanGeneration;
    m_dataDirTable->setRowCount(dirs.size());
    const QColor muted(0x8b, 0x94, 0x9e);
    for (int r = 0; r < dirs.size(); ++r) {
        const DataDir &d = dirs.at(r);
        const QFileInfo info(d.path);
        const bool exists = info.exists() || info.isSymLink();

        auto *nameItem = new QTableWidgetItem(d.label);
        nameItem->setToolTip(d.hint);
        m_dataDirTable->setItem(r, 0, nameItem);

        auto *pathItem = new QTableWidgetItem(d.path);
        pathItem->setToolTip(d.path);
        QStringList prune;
        if (!d.countInFull) {
            for (const QString &other : std::as_const(nested)) {
                if (other != d.path)
                    prune.append(other);
            }
        }
        pathItem->setData(Qt::UserRole, prune);
        pathItem->setData(Qt::UserRole + 1, exists && !d.isLink);
        m_dataDirTable->setItem(r, 1, pathItem);

        auto note = [&muted](const QString &text, bool right) {
            auto *it = new QTableWidgetItem(text);
            if (right)
                it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            it->setForeground(muted);
            return it;
        };
        const QString dash = QString::fromUtf8("\xE2\x80\x94");
        if (d.isLink) {
            m_dataDirTable->setItem(r, 2, note(dash, true));
            m_dataDirTable->setItem(r, 3, note(dash, true));
            auto *target = note(QStringLiteral("link \xE2\x86\x92 %1")
                                    .arg(info.symLinkTarget()),
                                false);
            target->setToolTip(
                QStringLiteral("Nothing is stored here. This link points at %1 so tools "
                               "that still use the old location keep working. Remove it "
                               "once nothing needs it.")
                    .arg(info.symLinkTarget()));
            m_dataDirTable->setItem(r, 4, target);
        } else if (exists) {
            m_dataDirTable->setItem(r, 2, note(QStringLiteral("Calculating\xE2\x80\xA6"), true));
            m_dataDirTable->setItem(r, 3, note(QStringLiteral("Calculating\xE2\x80\xA6"), true));
            m_dataDirTable->setItem(r, 4, note(QStringLiteral("Calculating\xE2\x80\xA6"), true));
        } else {
            m_dataDirTable->setItem(r, 2, note(dash, true));
            m_dataDirTable->setItem(r, 3, note(dash, true));
            m_dataDirTable->setItem(r, 4, note(QStringLiteral("not created yet"), false));
        }
        auto *scopeItem = new QTableWidgetItem(scopeText(d.scope));
        if (d.scope == forkmesh::paths::Location::Scope::Legacy)
            scopeItem->setForeground(d.isLink ? muted : QColor(0xd2, 0x99, 0x22));
        m_dataDirTable->setItem(r, 6, scopeItem);

        auto *actions = new QWidget;
        auto *actionRow = new QHBoxLayout(actions);
        actionRow->setContentsMargins(4, 2, 4, 2);
        actionRow->setSpacing(6);
        auto *openButton = new QPushButton("Open");
        openButton->setObjectName("ghostButton");
        openButton->setCursor(Qt::PointingHandCursor);
        openButton->setEnabled(exists);
        openButton->setToolTip("Open this location in your file manager");
        const QString path = d.path;
        const bool isFile = info.isFile();
        connect(openButton, &QPushButton::clicked, this, [path, isFile] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                isFile ? QFileInfo(path).absolutePath() : path));
        });
        actionRow->addWidget(openButton);
        if (d.isLink) {
            auto *unlinkButton = new QPushButton("Remove link");
            unlinkButton->setObjectName("ghostButton");
            unlinkButton->setCursor(Qt::PointingHandCursor);
            unlinkButton->setToolTip(
                "Removes only the link. The files it points at stay where they are.");
            connect(unlinkButton, &QPushButton::clicked, this, [this, path] {
                // Re-checked at click time: only ever unlink a link.
                if (QFileInfo(path).isSymLink() && QFile::remove(path))
                    setDataStatus(QStringLiteral("Removed the link at %1.").arg(path));
                else
                    setDataStatus(QStringLiteral("Could not remove %1.").arg(path), true);
                refreshDataDirTable();
            });
            actionRow->addWidget(unlinkButton);
        } else if (d.deletable) {
            auto *deleteButton = new QPushButton("Delete");
            deleteButton->setObjectName("ghostButton");
            deleteButton->setCursor(Qt::PointingHandCursor);
            deleteButton->setEnabled(exists);
            const QString label = d.label;
            const bool critical = d.inBackup; // identity/preferences => restart after
            connect(deleteButton, &QPushButton::clicked, this,
                    [this, label, path, critical] {
                        deleteDataDir(label, path, critical);
                    });
            actionRow->addWidget(deleteButton);
        }
        actionRow->addStretch();
        m_dataDirTable->setCellWidget(r, 5, actions);
    }
    scanDataDirRow(generation, 0);
}

// One row at a time, each on a worker: a small folder shows its size at once
// instead of waiting behind tens of gigabytes of build trees, and the disk is
// never asked to walk twenty trees in parallel.
void MainWindow::scanDataDirRow(quint64 generation, int row)
{
    if (generation != m_dataDirScanGeneration || !m_dataDirTable)
        return;
    while (row < m_dataDirTable->rowCount()) {
        const QTableWidgetItem *item = m_dataDirTable->item(row, 1);
        if (item && item->data(Qt::UserRole + 1).toBool())
            break;
        ++row;
    }
    if (row >= m_dataDirTable->rowCount())
        return;
    const QTableWidgetItem *item = m_dataDirTable->item(row, 1);
    const QString path = item->text();
    const QStringList pruneList = item->data(Qt::UserRole).toStringList();
    runOffThread<DirStat>(
        [path, pruneList] {
            DirStat stat;
            if (QFileInfo::exists(path)) {
                scanInto(path, QSet<QString>(pruneList.begin(), pruneList.end()), stat);
            }
            return stat;
        },
        [this, generation, row, path](DirStat stat) {
            if (generation != m_dataDirScanGeneration || !m_dataDirTable ||
                row >= m_dataDirTable->rowCount())
                return;
            const QTableWidgetItem *current = m_dataDirTable->item(row, 1);
            if (current && current->text() == path) {
                auto numberItem = [](qint64 number) {
                    auto *it = new QTableWidgetItem(QLocale().toString(number));
                    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    return it;
                };
                m_dataDirTable->setItem(row, 2, numberItem(stat.folders));
                m_dataDirTable->setItem(row, 3, numberItem(stat.files));
                auto *size =
                    new QTableWidgetItem(QLocale().formattedDataSize(stat.bytes));
                size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                m_dataDirTable->setItem(row, 4, size);
            }
            scanDataDirRow(generation, row + 1);
        });
}

void MainWindow::setDataStatus(const QString &text, bool error)
{
    forkmesh::backupfeed::publishStatus(text, error);
    if (!m_dataStatus)
        return;
    m_dataStatus->setText(text);
    m_dataStatus->setStyleSheet(Theme::remapSheet(error ? QStringLiteral("color:#f85149;")
                                       : QString()));
    m_dataStatus->setVisible(!text.isEmpty());
}


void MainWindow::setConfigArchiveBusy(bool busy)
{
    if (m_exportConfigButton)
        m_exportConfigButton->setEnabled(!busy);
    if (m_importConfigButton)
        m_importConfigButton->setEnabled(!busy);
}

void MainWindow::exportConfigData()
{
    if (m_configArchiveProcess) {
        setDataStatus("A configuration export or import is already "
                      "running\xE2\x80\xA6");
        return;
    }

    const QString tarExe = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (tarExe.isEmpty()) {
        setDataStatus("Can't export: the 'tar' tool was not found on this "
                      "system.",
                      true);
        return;
    }

    const QString suggested =
        QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
            .filePath("forkmesh-config-" +
                      QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") +
                      ".tar.gz");
    QString out = QFileDialog::getSaveFileName(
        this, "Export ForkMesh configuration", suggested,
        "Backup archive (*.tar.gz)");
    if (out.isEmpty())
        return;
    if (!out.endsWith(QStringLiteral(".tar.gz")) &&
        !out.endsWith(QStringLiteral(".tgz")))
        out += QStringLiteral(".tar.gz");

    const QStringList args = forkmesh::configArchiveTarArgs(
        out, forkmesh::paths::dataDir(),
        QSettings().fileName(),
        backupExcludeRoots());
    if (args.isEmpty()) {
        setDataStatus("There's no configuration to export yet.", true);
        return;
    }

    // The archive covers the whole live database, which the hint above warns
    // can run to gigabytes; gzipping it took minutes with the GUI thread parked
    // in waitForFinished(-1). Run it the way the hourly snapshot already does
    // and let the window keep painting.
    setConfigArchiveBusy(true);
    setDataStatus("Exporting configuration to " + out +
                  "\xE2\x80\xA6 a large database can take a few minutes; "
                  "ForkMesh stays usable while it runs.");

    auto *tar = new QProcess(this);
    m_configArchiveProcess = tar;
    auto settled = std::make_shared<bool>(false);
    const auto retire = [this, tar, out, settled](bool ok, const QString &why) {
        if (*settled)
            return;
        *settled = true;
        m_configArchiveProcess = nullptr;
        setConfigArchiveBusy(false);
        if (ok) {
            setDataStatus("Exported configuration to " + out + " (" +
                          QLocale().formattedDataSize(QFileInfo(out).size()) +
                          ").");
        } else {
            setDataStatus("Export failed" + (why.isEmpty() ? QString()
                                                           : ": " + why),
                          true);
        }
        tar->deleteLater();
    };
    connect(tar, &QProcess::finished, this,
            [tar, retire](int code, QProcess::ExitStatus status) {
                const QString err =
                    QString::fromUtf8(tar->readAllStandardError()).trimmed();
                retire(status == QProcess::NormalExit && code == 0, err);
            });
    // finished() never arrives when the process cannot be spawned, so the
    // buttons would stay greyed out forever without this.
    connect(tar, &QProcess::errorOccurred, this,
            [retire](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    retire(false, QStringLiteral("'tar' could not be started."));
            });
    tar->start(tarExe, args);
}

void MainWindow::importConfigData()
{
    if (m_configArchiveProcess) {
        setDataStatus("A configuration export or import is already "
                      "running\xE2\x80\xA6");
        return;
    }

    const QString in = QFileDialog::getOpenFileName(
        this, "Import ForkMesh configuration",
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        "Backup archive (*.tar.gz *.tgz)");
    if (in.isEmpty())
        return;
    restoreConfigArchive(in);
}

void MainWindow::restoreConfigArchive(const QString &in)
{
    if (m_configArchiveProcess) {
        setDataStatus("A configuration export or import is already "
                      "running\xE2\x80\xA6");
        return;
    }

    const QString tarExe = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (tarExe.isEmpty()) {
        setDataStatus("Can't restore: the 'tar' tool was not found on this "
                      "system.",
                      true);
        return;
    }
    if (!QFileInfo::exists(in)) {
        setDataStatus("That backup is no longer on disk: " + in, true);
        refreshBackupTable();
        return;
    }

    QMessageBox box(
        QMessageBox::Warning, QStringLiteral("Restore configuration"),
        "Restoring replaces ForkMesh's current identity, account and "
        "preferences with the contents of:\n\n" +
            in +
            "\n\nYour current configuration is moved aside to a timestamped "
            "backup first. ForkMesh will restart to finish.",
        QMessageBox::Cancel, this);
    QPushButton *go = box.addButton(QString::fromUtf8("Restore & restart\xE2\x80\xA6"),
                                    QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go)
        return;

    const QString appData =
        forkmesh::paths::dataDir();
    const QString appParent = QFileInfo(appData).absolutePath();
    const QString appBase = QFileInfo(appData).fileName();
    // Empty when the settings live in the registry: nothing to restore by file.
    const QString configFile = forkmesh::paths::settingsFilePath(QSettings());
    const QString configDir =
        configFile.isEmpty() ? QString() : QFileInfo(configFile).absolutePath();
    const QString configBase =
        configFile.isEmpty() ? QString() : QFileInfo(configFile).fileName();
    const QString stamp =
        QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");

    const QString workRoot =
        appParent + QStringLiteral("/tmp-forkmesh-import-") + stamp;
    QDir().mkpath(workRoot);

    // Unpacking is the mirror image of the export: gigabytes of gzip, minutes
    // of work. Run it asynchronously and pick the restore back up in
    // finishConfigRestore() once tar has exited.
    setConfigArchiveBusy(true);
    setDataStatus("Unpacking the backup\xE2\x80\xA6 this can take a few "
                  "minutes.");

    auto *tar = new QProcess(this);
    m_configArchiveProcess = tar;
    auto settled = std::make_shared<bool>(false);
    const auto retire = [this, tar, workRoot, stamp, settled](
                            bool ok, const QString &why) {
        if (*settled)
            return;
        *settled = true;
        m_configArchiveProcess = nullptr;
        setConfigArchiveBusy(false);
        tar->deleteLater();
        if (!ok) {
            forkmesh::safedelete::removeTree(workRoot, QFileInfo(workRoot).absolutePath());
            setDataStatus("Restore failed to unpack the archive" +
                              (why.isEmpty() ? QString() : ": " + why),
                          true);
            return;
        }
        finishConfigRestore(workRoot, stamp);
    };
    connect(tar, &QProcess::finished, this,
            [tar, retire](int code, QProcess::ExitStatus status) {
                const QString err =
                    QString::fromUtf8(tar->readAllStandardError()).trimmed();
                retire(status == QProcess::NormalExit && code == 0, err);
            });
    connect(tar, &QProcess::errorOccurred, this,
            [retire](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    retire(false, QStringLiteral("'tar' could not be started."));
            });
    tar->start(tarExe,
               {QStringLiteral("-xzf"), in, QStringLiteral("-C"), workRoot});
}

void MainWindow::finishConfigRestore(const QString &workRoot,
                                     const QString &stamp)
{
    const QString appData =
        forkmesh::paths::dataDir();
    const QString appParent = QFileInfo(appData).absolutePath();
    const QString appBase = QFileInfo(appData).fileName();
    // Empty when the settings live in the registry: nothing to restore by file.
    const QString configFile = forkmesh::paths::settingsFilePath(QSettings());
    const QString configDir =
        configFile.isEmpty() ? QString() : QFileInfo(configFile).absolutePath();
    const QString configBase =
        configFile.isEmpty() ? QString() : QFileInfo(configFile).fileName();

    const QString importedApp = QDir(workRoot).filePath(appBase);
    const QString importedConfig =
        configBase.isEmpty() ? QString() : QDir(workRoot).filePath(configBase);
    if (!QFileInfo::exists(importedApp) && !QFileInfo::exists(importedConfig)) {
        forkmesh::safedelete::removeTree(workRoot, QFileInfo(workRoot).absolutePath());
        setDataStatus("That archive doesn't look like a ForkMesh "
                      "configuration backup.",
                      true);
        return;
    }

    stopLiveServicesForDataOp();

    if (QFileInfo::exists(importedApp)) {
        QDir().mkpath(appParent);
        const QString displaced = appData + ".bak-" + stamp;
        if (QFileInfo::exists(appData))
            QDir().rename(appData, displaced);
        QDir().rename(importedApp, appData);
        for (const QString &kept :
             {backupRoot(), forkmesh::paths::sshDir(), forkmesh::paths::stateRoot(),
              forkmesh::paths::logsDir(), forkmesh::paths::cacheRoot()}) {
            const QString live = QDir(kept).absolutePath();
            const QString rel = QDir(appData).relativeFilePath(live);
            if (rel.isEmpty() || rel.startsWith(QStringLiteral("..")) ||
                QFileInfo::exists(live) || !QFileInfo::exists(QDir(displaced).filePath(rel)))
                continue;
            QDir().mkpath(QFileInfo(live).absolutePath());
            QDir().rename(QDir(displaced).filePath(rel), live);
        }
    }
    if (!importedConfig.isEmpty() && QFileInfo::exists(importedConfig)) {
        QDir().mkpath(configDir);
        if (QFileInfo::exists(configFile))
            QFile::rename(configFile, configFile + ".bak-" + stamp);
        if (!QFile::rename(importedConfig, configFile))
            QFile::copy(importedConfig, configFile);
    }
    forkmesh::safedelete::removeTree(workRoot, QFileInfo(workRoot).absolutePath());

    relaunchForkMesh();
}


QString MainWindow::backupRoot() const
{
    return forkmesh::defaultBackupRoot();
}

bool MainWindow::autoBackupEnabled() const
{
    // Unset is off on every node. A stored deploy credential must not silently
    // opt a machine into a recurring multi-gigabyte disk workload.
    return QSettings()
        .value(kAutoBackupEnabledSetting,
               forkmesh::autoBackupDefault(resolvedCloudflareApiToken()))
        .toBool();
}

int MainWindow::backupKeepCount() const
{
    return qBound(forkmesh::kBackupKeepMin,
                  QSettings()
                      .value(kAutoBackupKeepSetting, forkmesh::kBackupKeepDefault)
                      .toInt(),
                  forkmesh::kBackupKeepMax);
}

void MainWindow::setBackupStatus(const QString &text, bool error)
{
    forkmesh::backupfeed::publishStatus(text, error);
    if (!m_backupStatus)
        return;
    m_backupStatus->setText(text);
    m_backupStatus->setStyleSheet(Theme::remapSheet(error ? QStringLiteral("color:#f85149;")
                                        : QString()));
    m_backupStatus->setVisible(!text.isEmpty());
}

void MainWindow::installBackupHost()
{
    forkmesh::backupfeed::Host host;
    host.snapshotRoot = [this] { return backupRoot(); };
    host.hourlyEnabled = [this] { return autoBackupEnabled(); };
    host.setHourlyEnabled = [this](bool on) {
        if (m_backupEnabledCheck && m_backupEnabledCheck->isChecked() != on) {
            m_backupEnabledCheck->setChecked(on);
            return;
        }
        QSettings().setValue(kAutoBackupEnabledSetting, on);
        startAutoBackups();
        setBackupStatus(on ? QStringLiteral("Hourly backups are on.")
                           : QStringLiteral("Hourly backups are off."));
    };
    host.keepCount = [this] { return backupKeepCount(); };
    host.setKeepCount = [this](int keep) {
        keep = qBound(forkmesh::kBackupKeepMin, keep, forkmesh::kBackupKeepMax);
        if (m_backupKeepSpin && m_backupKeepSpin->value() != keep) {
            m_backupKeepSpin->setValue(keep);
            return;
        }
        QSettings().setValue(kAutoBackupKeepSetting, keep);
        pruneOldBackups();
        refreshBackupTable();
    };
    host.running = [this] { return m_backupProcess != nullptr; };
    host.backUpNow = [this] { takeBackupNow(false); };
    host.restore = [this](const QString &path) { restoreConfigArchive(path); };
    host.snapshotsChanged = [this] { refreshBackupTable(); };
    forkmesh::backupfeed::setHost(this, std::move(host));
}

void MainWindow::startAutoBackups()
{
    if (!m_backupTimer) {
        m_backupTimer = new QTimer(this);
        m_backupTimer->setInterval(
            static_cast<int>(forkmesh::kBackupIntervalMs));
        connect(m_backupTimer, &QTimer::timeout, this,
                [this] { takeBackupNow(true); });
    }
    if (!autoBackupEnabled()) {
        m_backupTimer->stop();
        return;
    }
    m_backupTimer->start();

    const QList<forkmesh::BackupSnapshot> snapshots =
        forkmesh::listBackups(backupRoot());
    const QDateTime last =
        snapshots.isEmpty() ? QDateTime() : snapshots.first().taken;
    if (forkmesh::backupIsDue(last, QDateTime::currentDateTime()))
        QTimer::singleShot(15000, this, [this] { takeBackupNow(true); });
}

void MainWindow::takeBackupNow(bool automatic)
{
    if (automatic && !autoBackupEnabled())
        return;
    if (m_backupProcess) {
        if (!automatic)
            setBackupStatus("A backup is already running\xE2\x80\xA6");
        return;
    }

    const QString tarExe = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (tarExe.isEmpty()) {
        setBackupStatus("Can't back up: the 'tar' tool was not found on this "
                        "system.",
                        true);
        noteBackupResult(QStringLiteral("tar_missing"));
        logSystem(QString::fromUtf8(
            "Backup: skipped \xE2\x80\x94 'tar' was not found on this system."));
        return;
    }

    QDir().mkpath(backupRoot());
    removePartialBackups();
    const QString root = backupRoot();
    const qint64 freeBytes = QStorageInfo(root).bytesAvailable();
    if (!forkmesh::backupLeavesFreeReserve(freeBytes)) {
        const QString why =
            QStringLiteral("only %1 is free on the backup disk and ForkMesh "
                           "leaves at least %2 free")
                .arg(QLocale().formattedDataSize(freeBytes),
                     QLocale().formattedDataSize(
                         forkmesh::kBackupFreeReserveBytes));
        setBackupStatus("Backup skipped: " + why + ".", true);
        noteBackupResult(QStringLiteral("low_disk"));
        logSystem(QStringLiteral("Backup: skipped (%1).").arg(why));
        return;
    }
    const QString finalPath = QDir(root).filePath(
        forkmesh::backupFileName(QDateTime::currentDateTime()));
    const QString partPath = finalPath + QStringLiteral(".part");
    const QStringList args = forkmesh::configArchiveTarArgs(
        partPath,
        forkmesh::paths::dataDir(),
        QSettings().fileName(),
        backupExcludeRoots());
    if (args.isEmpty()) {
        setBackupStatus("There's nothing to back up yet.", true);
        noteBackupResult(QStringLiteral("nothing_to_back_up"));
        return;
    }

    auto *tar = new QProcess(this);
    m_backupProcess = tar;
#if defined(Q_OS_UNIX)
    tar->setChildProcessModifier([] { lowerBackgroundPriority(); });
#endif
    auto lowDisk = std::make_shared<bool>(false);
    auto *diskWatch = new QTimer(tar);
    diskWatch->setInterval(5000);
    connect(diskWatch, &QTimer::timeout, tar, [tar, root, lowDisk] {
        if (*lowDisk || forkmesh::backupLeavesFreeReserve(
                            QStorageInfo(root).bytesAvailable()))
            return;
        *lowDisk = true;
        tar->kill();
    });
    diskWatch->start();
    if (m_backupNowButton)
        m_backupNowButton->setEnabled(false);
    if (!automatic)
        setBackupStatus("Backing up\xE2\x80\xA6");
    connect(tar, &QProcess::finished, this,
            [this, tar, partPath, finalPath, automatic, lowDisk](
                int code, QProcess::ExitStatus status) {
                m_backupProcess = nullptr;
                if (m_backupNowButton)
                    m_backupNowButton->setEnabled(true);
                const bool ok = status == QProcess::NormalExit &&
                                (code == 0 || code == 1);
                if (ok && QFile::rename(partPath, finalPath)) {
                    pruneOldBackups();
                    noteBackupResult(QString());
                    refreshBackupTable();
                    const QString name = QFileInfo(finalPath).fileName();
                    const QString size = QLocale().formattedDataSize(
                        QFileInfo(finalPath).size());
                    setBackupStatus((automatic ? QStringLiteral("Hourly backup ")
                                               : QStringLiteral("Backup ")) +
                                    "saved: " + name + " (" + size + ").");
                    logSystem(QStringLiteral("Backup: wrote %1 (%2).")
                                  .arg(name, size));
                } else {
                    QFile::remove(partPath);
                    const QString err =
                        QString::fromUtf8(tar->readAllStandardError())
                            .trimmed()
                            .left(300);
                    const QString why =
                        *lowDisk
                            ? QStringLiteral(
                                  "stopped before the disk fell below %1 free")
                                  .arg(QLocale().formattedDataSize(
                                      forkmesh::kBackupFreeReserveBytes))
                        : err.isEmpty()
                            ? QStringLiteral("tar exited with %1").arg(code)
                            : err;
                    setBackupStatus("Backup failed: " + why, true);
                    noteBackupResult(*lowDisk ? QStringLiteral("low_disk")
                                     : ok     ? QStringLiteral("rename_failed")
                                              : QStringLiteral("tar_failed"));
                    logSystem(QStringLiteral("Backup: failed (%1).").arg(why));
                }
                tar->deleteLater();
            });
    tar->start(tarExe, args);
}

void MainWindow::pruneOldBackups()
{
    const QList<forkmesh::BackupSnapshot> stale = forkmesh::backupsToPrune(
        forkmesh::listBackups(backupRoot()), backupKeepCount());
    for (const forkmesh::BackupSnapshot &snapshot : stale)
        QFile::remove(snapshot.path);
    removePartialBackups();
}

void MainWindow::removePartialBackups()
{
    if (m_backupProcess)
        return;
    const QFileInfoList partials =
        QDir(backupRoot())
            .entryInfoList({QStringLiteral("forkmesh-backup-*.tar.gz.part")},
                           QDir::Files);
    for (const QFileInfo &fi : partials)
        QFile::remove(fi.absoluteFilePath());
}

QStringList MainWindow::backupExcludeRoots() const
{
    QStringList roots{repositoryMirrorRoot(), repositoryPreviewRoot(),
                      backupRoot(), forkmesh::paths::cacheRoot(),
                      forkmesh::paths::stateRoot(), forkmesh::paths::logsDir(),
                      forkmesh::paths::sshDir()};
    if (m_actionStore)
        roots << m_actionStore->cacheRoot() << m_actionStore->sandboxDir();
    return roots;
}

void MainWindow::refreshBackupTable()
{
    if (!m_backupTable)
        return;
    const QList<forkmesh::BackupSnapshot> snapshots =
        forkmesh::listBackups(backupRoot());
    m_backupTable->setRowCount(snapshots.size());
    for (int r = 0; r < snapshots.size(); ++r) {
        const forkmesh::BackupSnapshot &snapshot = snapshots.at(r);

        auto *takenItem = new QTableWidgetItem(
            snapshot.taken.toString(QStringLiteral("yyyy-MM-dd hh:mm")));
        m_backupTable->setItem(r, 0, takenItem);

        m_backupTable->setItem(
            r, 1,
            new QTableWidgetItem(formatIssueRelativeTime(
                snapshot.taken.toMSecsSinceEpoch())));

        auto *sizeItem = new QTableWidgetItem(
            QLocale().formattedDataSize(snapshot.bytes));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_backupTable->setItem(r, 2, sizeItem);

        auto *fileItem = new QTableWidgetItem(snapshot.fileName);
        fileItem->setToolTip(snapshot.path);
        m_backupTable->setItem(r, 3, fileItem);

        auto *actions = new QWidget;
        auto *actionRow = new QHBoxLayout(actions);
        actionRow->setContentsMargins(4, 2, 4, 2);
        actionRow->setSpacing(6);
        const QString path = snapshot.path;
        const QString name = snapshot.fileName;
        auto *restoreButton = new QPushButton("Restore");
        restoreButton->setObjectName("ghostButton");
        restoreButton->setCursor(Qt::PointingHandCursor);
        restoreButton->setToolTip(
            "Replace the live database with this snapshot and restart");
        connect(restoreButton, &QPushButton::clicked, this,
                [this, path] { restoreConfigArchive(path); });
        auto *deleteButton = new QPushButton("Delete");
        deleteButton->setObjectName("ghostButton");
        deleteButton->setCursor(Qt::PointingHandCursor);
        connect(deleteButton, &QPushButton::clicked, this, [this, path, name] {
            QMessageBox box(QMessageBox::Warning,
                            QStringLiteral("Delete backup"),
                            "Permanently delete this snapshot?\n\n" + name,
                            QMessageBox::Cancel, this);
            QPushButton *go = box.addButton(QStringLiteral("Delete"),
                                            QMessageBox::DestructiveRole);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() != go)
                return;
            QFile::remove(path);
            refreshBackupTable();
            setBackupStatus("Deleted " + name + ".");
        });
        actionRow->addWidget(restoreButton);
        actionRow->addWidget(deleteButton);
        actionRow->addStretch();
        m_backupTable->setCellWidget(r, 4, actions);
    }

    if (snapshots.isEmpty())
        setBackupStatus(
            autoBackupEnabled()
                ? QString::fromUtf8("No snapshots yet \xE2\x80\x94 the first one is "
                                 "taken shortly after startup, then every hour.")
                : QStringLiteral("Hourly backups are off. \"Back up now\" still "
                                 "takes one on demand."));
}


void MainWindow::deleteDataDir(const QString &label, const QString &path,
                               bool critical)
{
    if (!QFileInfo::exists(path))
        return;
    // A link is never deleted through: removing "it" recursively would empty
    // the folder it points at. Links have their own Remove link button.
    if (QFileInfo(path).isSymLink())
        return;
    const QString refused = userDataWipeRefusal(path);
    if (!refused.isEmpty()) {
        setDataStatus("Not deleting " + label + ": " + refused + ".", true);
        return;
    }

    QString detail = "Permanently delete " + label + "?\n\n" + path;
    if (critical)
        detail += "\n\nThis removes your identity and account \xE2\x80\x94 it "
                  "cannot be undone. ForkMesh will restart afterwards.";
    else
        detail += "\n\nThis cannot be undone.";

    QMessageBox box(QMessageBox::Warning, QStringLiteral("Delete data"), detail,
                    QMessageBox::Cancel, this);
    QPushButton *go = box.addButton(QString::fromUtf8("Delete\xE2\x80\xA6"),
                                    QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go)
        return;

    if (critical) {
        stopLiveServicesForDataOp();
        QDir(path).removeRecursively();
        relaunchForkMesh();
        return;
    }

    if (QDir(path).removeRecursively())
        setDataStatus("Deleted " + label + ".");
    else
        setDataStatus("Couldn't fully delete " + label + " \xE2\x80\x94 some "
                      "files may be in use.",
                      true);
    refreshDataDirTable();
    refreshBackupTable(); // the Backups row is one of the deletable locations
}

void MainWindow::deleteAllData()
{
    const QVector<DataDir> dirs = resolveDataDirs(
        repositoryMirrorRoot(), repositoryPreviewRoot(), backupRoot());
    QStringList present;
    for (const DataDir &d : dirs) {
        if (d.deletable && !d.isLink && QFileInfo::exists(d.path) &&
            userDataWipeRefusal(d.path).isEmpty() && !present.contains(d.path))
            present << d.path;
    }
    if (present.isEmpty()) {
        setDataStatus("There's no ForkMesh data on this computer to delete.");
        return;
    }

    QString detail = QStringLiteral(
        "This permanently erases every ForkMesh data folder on this computer, "
        "including your identity key (your account cannot be recovered), all "
        "settings, mirrored repositories and caches.\n\nFolders removed:\n");
    for (const QString &p : std::as_const(present))
        detail += "    " + p + "/\n";
    detail += QStringLiteral(
        "\nThe ForkMesh app stays installed and will restart fresh.");

    QMessageBox box(QMessageBox::Warning,
                    QStringLiteral("Delete all ForkMesh data"), detail,
                    QMessageBox::Cancel, this);
    QPushButton *go = box.addButton(QString::fromUtf8("Delete everything\xE2\x80\xA6"),
                                    QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go)
        return;

    bool ok = false;
    const QString typed = QInputDialog::getText(
        this, QStringLiteral("Confirm delete"),
        QStringLiteral("Type DELETE to erase all ForkMesh data:"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || typed.trimmed().compare(QStringLiteral("DELETE"),
                                       Qt::CaseInsensitive) != 0)
        return;

    stopLiveServicesForDataOp();
    // code-health: allow(dir-walk-in-loop) at most the 7 fixed data folders, erased after the typed DELETE confirmation right before the app relaunches
    for (const QString &p : std::as_const(present))
        QDir(p).removeRecursively();
    relaunchForkMesh();
}

void MainWindow::stopLiveServicesForDataOp()
{
    if (m_heartbeatTimer)
        m_heartbeatTimer->stop();
    if (m_backupTimer)
        m_backupTimer->stop();
    if (m_backupProcess) {
        // code-health: allow(blocking-wait) stop path before a confirmed data wipe or config restore; SIGKILL then a reap bounded at 2 s
        m_backupProcess->disconnect(this);
        m_backupProcess->kill();
        m_backupProcess->waitForFinished(2000);
        m_backupProcess->deleteLater();
        m_backupProcess = nullptr;
    }
    stopNodeEventSocket();
    if (m_backend) {
        m_backend->disconnect(this);
        m_backend->shutdown();
        m_backend->deleteLater();
        m_backend = nullptr;
    }
}

void MainWindow::relaunchForkMesh()
{
    if (requestSupervisingUserServiceRestart(
            QCoreApplication::applicationFilePath())) {
        const QStringList arguments = systemdUserServiceCloseArguments(
            supervisingSystemdUserServiceUnit(), true);
        if (!arguments.isEmpty() &&
            forkmesh::processreaper::startReleased(QStringLiteral("systemctl"), arguments)) {
            QCoreApplication::exit(0);
            return;
        }
    }
    const QString launchDir = forkmesh::vm::workspaceRoot().isEmpty()
                                  ? QDir::currentPath()
                                  : forkmesh::vm::workspaceRoot();
    forkmesh::releaseSingleInstance();
    forkmesh::processreaper::startReleased(QCoreApplication::applicationFilePath(),
                            QCoreApplication::arguments().mid(1), launchDir);
    QCoreApplication::exit(0);
}
