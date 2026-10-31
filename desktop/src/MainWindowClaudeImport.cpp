#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"
#include "AgentRuntimeSession.h"
#include "AgentWorktree.h"
#include "AppPaths.h"
#include "ClaudePeerMessaging.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

#include <memory>

namespace {

const QString kImportedClaudeFolderName = QStringLiteral("Imported Claude Code");

struct ClaudeImportListing {
    QList<ExternalClaudeSession> sessions;
    QSet<QString> live;
};

struct ClaudeImportSync {
    int sessionId = 0;
    qint64 fromBytes = 0;
    ClaudeImportBatch batch;
    QString title;
};

QString firstComponentUnder(const QString &path, const QString &root)
{
    if (!path.startsWith(root + QLatin1Char('/')))
        return QString();
    return path.mid(root.size() + 1).section(QLatin1Char('/'), 0, 0);
}

QString relativeAge(qint64 ms)
{
    if (ms <= 0)
        return QString();
    const qint64 minutes = (QDateTime::currentMSecsSinceEpoch() - ms) / 60000;
    if (minutes < 1)
        return QStringLiteral("just now");
    if (minutes < 60)
        return QStringLiteral("%1m ago").arg(minutes);
    if (minutes < 48 * 60)
        return QStringLiteral("%1h ago").arg(minutes / 60);
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms).date(), QLocale::ShortFormat);
}

} // namespace

QHash<QString, int> MainWindow::importedClaudeSessionAgents() const
{
    QHash<QString, int> out;
    for (const AgentSession &s : m_agentSessions)
        if (s.id > 0 && s.claudeImported())
            out.insert(s.claudeImportId, s.id);
    return out;
}

void MainWindow::showClaudeSessionImportDialog()
{
    if (!m_agentStore)
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size() ||
        m_repositories.at(m_repoDetailIndex).localPath.isEmpty()) {
        flashMessage(QStringLiteral("Open a repository with a local checkout to import "
                                    "its Claude Code sessions."),
                     true);
        return;
    }
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString repoPath = QDir(repo.localPath).absolutePath();
    const QString wtRoot = QDir(forkmesh::agentwt::root(repoPath)).absolutePath();

    QHash<QString, int> ownedWorktrees;
    QSet<int> ownIds;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.id <= 0 || s.claudeImported() || s.owner != repo.owner || s.name != repo.name)
            continue;
        ownIds.insert(s.id);
        if (!s.worktreeName.isEmpty())
            ownedWorktrees.insert(s.worktreeName, s.id);
    }

    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("claudeImportDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Import Claude Code sessions"));
    dialog->resize(860, 520);

    auto *hint = new QLabel(dialog);
    hint->setObjectName(QStringLiteral("modeHint"));
    hint->setWordWrap(true);
    hint->setText(
        QStringLiteral("Sessions from Claude Code and Claude Desktop that ran in %1. "
                       "Each one you import becomes an agent here, named after the "
                       "session, with its whole conversation. New turns made in Claude "
                       "keep arriving until you send it a message from ForkMesh, which "
                       "continues the same conversation as a ForkMesh agent and leaves "
                       "Claude's own copy as it was.")
            .arg(QDir::toNativeSeparators(repoPath)));

    auto *filter = new QLineEdit(dialog);
    filter->setObjectName(QStringLiteral("claudeImportFilter"));
    filter->setPlaceholderText(QStringLiteral("Filter by name, prompt, branch or folder"));
    filter->setClearButtonEnabled(true);
    auto *everyFolder = new QCheckBox(QStringLiteral("Every folder"), dialog);
    everyFolder->setObjectName(QStringLiteral("claudeImportEveryFolder"));
    everyFolder->setToolTip(
        QStringLiteral("Also list sessions that ran outside this repository; they "
                       "continue in this repository's checkout"));
    auto *agentWorktrees = new QCheckBox(QStringLiteral("Headless runs"), dialog);
    agentWorktrees->setObjectName(QStringLiteral("claudeImportHeadless"));
    agentWorktrees->setToolTip(
        QStringLiteral("Also list sessions run without a window (claude -p, the SDK, "
                       "ForkMesh's own agents) and sessions in agent worktrees. Those of "
                       "agents still in the list are always left out."));
    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(filter, 1);
    filterRow->addWidget(agentWorktrees);
    filterRow->addWidget(everyFolder);

    auto *table = new QTableWidget(0, 4, dialog);
    table->setObjectName(QStringLiteral("claudeImportTable"));
    table->setHorizontalHeaderLabels({QStringLiteral("Session"), QStringLiteral("Last active"),
                                      QStringLiteral("Where"), QStringLiteral("Status")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        table->horizontalHeader()->setSectionResizeMode(column,
                                                        QHeaderView::ResizeToContents);

    auto *status = new QLabel(dialog);
    status->setObjectName(QStringLiteral("claudeImportStatus"));
    status->setWordWrap(true);

    auto *importButton = new QPushButton(QStringLiteral("Import"), dialog);
    importButton->setObjectName(QStringLiteral("claudeImportButton"));
    importButton->setEnabled(false);
    auto *importAllButton = new QPushButton(QStringLiteral("Import all new"), dialog);
    importAllButton->setObjectName(QStringLiteral("claudeImportAllButton"));
    importAllButton->setEnabled(false);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->addButton(importButton, QDialogButtonBox::AcceptRole);
    buttons->addButton(importAllButton, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(hint);
    layout->addLayout(filterRow);
    layout->addWidget(table, 1);
    layout->addWidget(status);
    layout->addWidget(buttons);

    auto listing = std::make_shared<ClaudeImportListing>();
    const auto ownerOf = [repoPath, wtRoot, ownedWorktrees,
                          ownIds](const ExternalClaudeSession &s) -> int {
        const QString folder = firstComponentUnder(QDir(s.cwd).absolutePath(), wtRoot);
        if (folder.isEmpty())
            return 0;
        if (const int id = ownedWorktrees.value(folder))
            return id;
        static const QRegularExpression numbered(QStringLiteral("^(?:agent-)?(\\d+)(?:-|$)"));
        const QRegularExpressionMatch m = numbered.match(folder);
        const int id = m.hasMatch() ? m.captured(1).toInt() : 0;
        return ownIds.contains(id) ? id : 0;
    };

    const auto populate = [this, listing, table, filter, agentWorktrees, status,
                           importAllButton, repoPath, ownerOf] {
        const QHash<QString, int> imported = importedClaudeSessionAgents();
        const QString needle = filter->text().trimmed();
        table->setRowCount(0);
        int fresh = 0;
        for (int i = 0; i < listing->sessions.size(); ++i) {
            const ExternalClaudeSession &s = listing->sessions.at(i);
            if (!agentWorktrees->isChecked() &&
                s.entrypoint.startsWith(QLatin1String("sdk")))
                continue;
            const QString cwd = QDir(s.cwd).absolutePath();
            QString where = cwd == repoPath
                                ? QString()
                                : (cwd.startsWith(repoPath + QLatin1Char('/'))
                                       ? cwd.mid(repoPath.size() + 1)
                                       : QDir::toNativeSeparators(cwd));
            if (!s.gitBranch.isEmpty())
                where = where.isEmpty() ? s.gitBranch
                                        : s.gitBranch + QStringLiteral(" \xC2\xB7 ") + where;
            if (!needle.isEmpty() && !s.title.contains(needle, Qt::CaseInsensitive) &&
                !s.firstPrompt.contains(needle, Qt::CaseInsensitive) &&
                !where.contains(needle, Qt::CaseInsensitive))
                continue;
            const int agent = imported.value(s.uuid);
            if (!agent && ownerOf(s))
                continue;
            QStringList state;
            if (agent)
                state << QStringLiteral("Imported as #%1").arg(agent);
            if (listing->live.contains(s.uuid))
                state << QStringLiteral("open in Claude");
            if (!agent)
                ++fresh;
            const int row = table->rowCount();
            table->insertRow(row);
            auto *title = new QTableWidgetItem(s.title);
            title->setData(Qt::UserRole, i);
            title->setData(Qt::UserRole + 1, agent == 0);
            QString tip = s.title;
            if (!s.firstPrompt.isEmpty() && s.firstPrompt != s.title)
                tip += QStringLiteral("\n\n") + s.firstPrompt.left(600);
            tip += QStringLiteral("\n\n%1 \xC2\xB7 %2")
                       .arg(s.entrypoint.isEmpty() ? QStringLiteral("claude") : s.entrypoint,
                            s.uuid);
            title->setToolTip(tip);
            table->setItem(row, 0, title);
            auto *age = new QTableWidgetItem(relativeAge(s.lastActivityMs));
            age->setToolTip(QLocale().toString(
                QDateTime::fromMSecsSinceEpoch(s.lastActivityMs), QLocale::LongFormat));
            table->setItem(row, 1, age);
            table->setItem(row, 2, new QTableWidgetItem(where));
            table->setItem(row, 3, new QTableWidgetItem(state.join(QStringLiteral(", "))));
        }
        importAllButton->setEnabled(fresh > 0);
        status->setText(table->rowCount() == 0
                            ? QStringLiteral("No Claude Code sessions to import here.")
                            : QStringLiteral("%1 sessions, %2 not imported yet.")
                                  .arg(table->rowCount())
                                  .arg(fresh));
    };

    auto loading = std::make_shared<quint64>(0);
    const auto load = [this, dialogGuard = QPointer<QDialog>(dialog), listing, populate,
                       everyFolder, agentWorktrees, status, table, repoPath, wtRoot,
                       loading] {
        const quint64 generation = ++*loading;
        status->setText(QStringLiteral("Reading Claude Code sessions\xE2\x80\xA6"));
        table->setRowCount(0);
        const bool anyFolder = everyFolder->isChecked();
        const QStringList skipRoots =
            agentWorktrees->isChecked() ? QStringList() : QStringList{wtRoot};
        const QString home = QDir::homePath();
        const QStringList configDirs{home + QStringLiteral("/.claude"),
                                     agentAccountDefaultConfigDir(QStringLiteral("claude-code"))};
        runOffThread<ClaudeImportListing>(
            [configDirs, repoPath, anyFolder, skipRoots] {
                const forkmesh::BackgroundScope activity(
                    QStringLiteral("agents"), QStringLiteral("list Claude Code sessions"),
                    forkmesh::ActionTelemetry::Execution::Worker);
                ClaudeImportListing out;
                out.sessions =
                    ClaudeSessionScan::listAll(configDirs, repoPath, anyFolder, skipRoots);
                for (const ClaudePeerSession &peer : ClaudePeerMessaging::liveSessions(configDirs))
                    out.live.insert(peer.sessionId);
                return out;
            },
            [dialogGuard, listing, populate, generation, loading](ClaudeImportListing out) {
                if (!dialogGuard || generation != *loading)
                    return;
                *listing = std::move(out);
                populate();
            });
    };

    const auto chosen = [listing, table](bool everyNew) {
        QList<ExternalClaudeSession> picked;
        QList<int> rows;
        if (everyNew) {
            for (int row = 0; row < table->rowCount(); ++row)
                rows << row;
        } else {
            for (const QModelIndex &index : table->selectionModel()->selectedRows(0))
                rows << index.row();
        }
        for (const int row : std::as_const(rows)) {
            const QTableWidgetItem *item = table->item(row, 0);
            if (!item)
                continue;
            const bool fresh = item->data(Qt::UserRole + 1).toBool();
            if (everyNew && !fresh)
                continue;
            const int index = item->data(Qt::UserRole).toInt();
            if (index >= 0 && index < listing->sessions.size())
                picked << listing->sessions.at(index);
        }
        return picked;
    };

    connect(filter, &QLineEdit::textChanged, dialog, [populate] { populate(); });
    connect(everyFolder, &QCheckBox::toggled, dialog, [load] { load(); });
    connect(agentWorktrees, &QCheckBox::toggled, dialog, [load] { load(); });
    connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, dialog,
            [table, importButton] {
                importButton->setEnabled(table->selectionModel()->hasSelection());
            });
    connect(importButton, &QPushButton::clicked, dialog, [this, dialog, chosen] {
        const QList<ExternalClaudeSession> picked = chosen(false);
        if (picked.isEmpty())
            return;
        dialog->close();
        importClaudeSessions(picked);
    });
    connect(importAllButton, &QPushButton::clicked, dialog, [this, dialog, chosen] {
        const QList<ExternalClaudeSession> picked = chosen(true);
        if (picked.isEmpty())
            return;
        dialog->close();
        importClaudeSessions(picked);
    });
    connect(table, &QTableWidget::itemDoubleClicked, dialog,
            [this, dialog, chosen](QTableWidgetItem *) {
                const QList<ExternalClaudeSession> picked = chosen(false);
                if (picked.isEmpty())
                    return;
                dialog->close();
                importClaudeSessions(picked);
            });

    dialog->show();
    load();
}

void MainWindow::importClaudeSessions(const QList<ExternalClaudeSession> &sessions,
                                      const QString &followUp)
{
    if (!m_agentStore || sessions.isEmpty())
        return;
    const QHash<QString, int> imported = importedClaudeSessionAgents();
    QList<ExternalClaudeSession> fresh;
    int refreshed = 0;
    int lastId = 0;
    for (const ExternalClaudeSession &ext : sessions) {
        if (!ClaudeSessionImport::validSessionId(ext.uuid) ||
            m_claudeImportsInFlight.contains(ext.uuid))
            continue;
        if (const int id = imported.value(ext.uuid)) {
            if (AgentSession *s = findAgentSession(id)) {
                if (!ext.title.isEmpty() && s->issueTitle != ext.title) {
                    s->issueTitle = ext.title;
                    m_agentStore->saveSession(*s);
                }
                ++refreshed;
                lastId = id;
            }
            continue;
        }
        m_claudeImportsInFlight.insert(ext.uuid);
        fresh << ext;
    }
    if (refreshed > 0) {
        syncImportedClaudeSessions();
        reloadAgents(AgentReloadScan::FromCache);
    }
    if (fresh.isEmpty()) {
        if (lastId)
            switchToAgentsTab(lastId);
        if (lastId && !followUp.isEmpty()) {
            if (!m_streamEvents.contains(lastId))
                if (const AgentSession *s = findAgentSession(lastId))
                    m_streamEvents[lastId] = m_agentStore->loadEvents(*s);
            m_agentTaskTrackers.remove(lastId);
            sendPromptToAgentSession(lastId, followUp);
            return;
        }
        if (refreshed == 0)
            return;
        flashMessage(refreshed == 1
                         ? QStringLiteral("That session is already imported; it was refreshed.")
                         : QStringLiteral("Those sessions are already imported; they were "
                                          "refreshed."));
        return;
    }
    flashMessage(fresh.size() == 1
                     ? QStringLiteral("Importing the Claude Code session\xE2\x80\xA6")
                     : QStringLiteral("Importing %1 Claude Code sessions\xE2\x80\xA6")
                           .arg(fresh.size()));
    struct Converted {
        ExternalClaudeSession ext;
        ClaudeImportBatch batch;
    };
    runOffThread<QList<Converted>>(
        [fresh] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("agents"), QStringLiteral("import Claude Code sessions"),
                forkmesh::ActionTelemetry::Execution::Worker);
            QList<Converted> out;
            for (const ExternalClaudeSession &ext : fresh) {
                Converted c;
                c.ext = ext;
                c.batch = ClaudeSessionImport::convert(ext.path, 0, ext.uuid);
                out << c;
            }
            return out;
        },
        [this, fresh, followUp](QList<Converted> converted) {
            for (const ExternalClaudeSession &ext : fresh)
                m_claudeImportsInFlight.remove(ext.uuid);
            const QHash<QString, int> already = importedClaudeSessionAgents();
            int first = 0;
            int made = 0;
            for (const Converted &c : std::as_const(converted)) {
                if (already.contains(c.ext.uuid))
                    continue;
                const int id = createImportedClaudeAgent(c.ext, c.batch);
                if (id <= 0)
                    continue;
                ++made;
                if (!first) {
                    first = id;
                    if (!followUp.isEmpty())
                        m_streamEvents[id] = c.batch.events;
                    m_agentTaskTrackers.remove(id);
                }
            }
            saveAgentFolders();
            reloadAgents();
            scheduleAgentSessionsPush();
            if (first)
                switchToAgentsTab(first);
            if (first && !followUp.isEmpty()) {
                sendPromptToAgentSession(first, followUp);
                return;
            }
            flashMessage(made == 1 ? QStringLiteral("Imported 1 Claude Code session.")
                                   : QStringLiteral("Imported %1 Claude Code sessions.")
                                         .arg(made),
                         made == 0);
        });
}

int MainWindow::createImportedClaudeAgent(const ExternalClaudeSession &ext,
                                          const ClaudeImportBatch &batch)
{
    if (!m_agentStore || m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return 0;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString repoPath = QDir(repo.localPath).absolutePath();
    const QString wtRoot = QDir(forkmesh::agentwt::root(repoPath)).absolutePath();
    const QString cwd = QDir(ext.cwd).absolutePath();

    AgentSession session;
    session.owner = repo.owner;
    session.name = repo.name;
    session.issueNumber = 0;
    session.issueTitle = ext.title.trimmed().isEmpty() ? QStringLiteral("Claude Code session")
                                                       : ext.title.trimmed();
    session.prompt = ext.firstPrompt.isEmpty() ? session.issueTitle : ext.firstPrompt;
    session.provider = QStringLiteral("claude-code");
    session.runtimeId = agentRuntimeIdForLegacyProvider(session.provider);
    session.model = ext.model;
    session.status = AgentStatus::Success;
    session.originInstanceId = activeServerInstanceId();
    session.originInstanceUrl = activeServerInstanceUrl();
    session.claudeImportId = ext.uuid;
    session.claudeImportPath = ext.path;
    session.claudeImportCwd = cwd;
    session.claudeImportBytes = batch.newOffset;

    const QString folder = firstComponentUnder(cwd, wtRoot);
    const QString worktree = folder.isEmpty() ? QString() : QDir(wtRoot).filePath(folder);
    QString branch;
    if (!worktree.isEmpty() && QFileInfo(worktree + QStringLiteral("/.git")).isFile()) {
        QFile dotGit(worktree + QStringLiteral("/.git"));
        if (dotGit.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString gitDir = QString::fromUtf8(dotGit.readLine()).trimmed();
            if (gitDir.startsWith(QLatin1String("gitdir:"))) {
                gitDir = gitDir.mid(7).trimmed();
                if (QDir::isRelativePath(gitDir))
                    gitDir = QDir(worktree).absoluteFilePath(gitDir);
                QFile head(gitDir + QStringLiteral("/HEAD"));
                if (head.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    const QString ref = QString::fromUtf8(head.readLine()).trimmed();
                    if (ref.startsWith(QLatin1String("ref: refs/heads/")))
                        branch = ref.mid(16);
                }
            }
        }
    }
    if (!branch.isEmpty()) {
        session.branchName = branch;
        session.worktreeName = folder;
    } else {
        session.directCheckout = true;
        session.baseBranch = ext.gitBranch;
    }

    session = m_agentStore->createSession(session);
    if (session.id <= 0)
        return 0;
    session.createdAtMs = ext.createdMs > 0 ? ext.createdMs : ext.lastActivityMs;
    session.startedAtMs = session.createdAtMs;
    session.finishedAtMs = qMax(batch.lastMs, ext.lastActivityMs);
    m_agentStore->saveSession(session);
    m_agentStore->appendLog(
        session, QStringLiteral("==> Imported Claude Code session %1 from %2 (ran in %3).\n")
                     .arg(ext.uuid, QDir::toNativeSeparators(ext.path),
                          QDir::toNativeSeparators(cwd)));
    for (const QJsonObject &ev : batch.events)
        m_agentStore->appendEvent(session, ev);
    if (!batch.log.isEmpty())
        m_agentStore->appendLog(session, batch.log);
    m_streamEventsAbsent.remove(session.id);
    m_agentSessions.append(session);

    const QString repoKey = AgentFolders::repoKey(repo.owner, repo.name);
    const QString folderId = m_agentFolders.findOrCreate(repoKey, kImportedClaudeFolderName);
    if (!folderId.isEmpty())
        m_agentFolders.assign(repoKey, agentReadKey(session), folderId);

    const auto external = m_externalTempId.constFind(ext.uuid);
    if (external != m_externalTempId.constEnd() && m_externalSurfaced.contains(*external))
        unsurfaceExternalSession(*external);
    m_externalClaude.erase(std::remove_if(m_externalClaude.begin(), m_externalClaude.end(),
                                          [&ext](const ExternalClaudeSession &e) {
                                              return e.uuid == ext.uuid;
                                          }),
                           m_externalClaude.end());
    return session.id;
}

void MainWindow::syncImportedClaudeSessions()
{
    if (m_claudeImportSyncInFlight || !m_agentStore)
        return;
    struct Job {
        int sessionId = 0;
        QString path;
        QString uuid;
        qint64 bytes = 0;
    };
    QList<Job> jobs;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.id <= 0 || !s.claudeImportMirroring() || s.claudeImportPath.isEmpty())
            continue;
        if (s.status == AgentStatus::Running || s.status == AgentStatus::Queued ||
            s.status == AgentStatus::Waiting || agentSessionHasLiveTransport(s.id))
            continue;
        jobs << Job{s.id, s.claudeImportPath, s.claudeImportId, s.claudeImportBytes};
    }
    if (jobs.isEmpty())
        return;
    m_claudeImportSyncInFlight = true;
    runOffThread<QList<ClaudeImportSync>>(
        [jobs] {
            QList<ClaudeImportSync> out;
            QHash<QString, QString> desktopTitles;
            bool desktopRead = false;
            for (const Job &job : jobs) {
                const QFileInfo info(job.path);
                if (!info.isFile() || info.size() <= job.bytes)
                    continue;
                ClaudeImportSync sync;
                sync.sessionId = job.sessionId;
                sync.fromBytes = job.bytes;
                sync.batch = ClaudeSessionImport::convert(job.path, job.bytes, job.uuid);
                if (sync.batch.newOffset <= job.bytes)
                    continue;
                ExternalClaudeSession meta;
                meta.uuid = job.uuid;
                meta.path = job.path;
                ClaudeSessionScan::readMeta(meta);
                if (!desktopRead) {
                    desktopTitles = ClaudeSessionScan::desktopTitles(
                        ClaudeSessionScan::desktopConfigRoot());
                    desktopRead = true;
                }
                sync.title = desktopTitles.value(job.uuid, meta.title);
                out << sync;
            }
            return out;
        },
        [this](QList<ClaudeImportSync> synced) {
            m_claudeImportSyncInFlight = false;
            bool retitled = false;
            for (const ClaudeImportSync &sync : std::as_const(synced)) {
                const AgentSession *s = findAgentSession(sync.sessionId);
                if (!s || !s->claudeImportMirroring() || s->claudeImportBytes != sync.fromBytes)
                    continue;
                retitled |= !sync.title.isEmpty() && sync.title != s->issueTitle;
                appendImportedClaudeEvents(sync.sessionId, sync.batch, sync.title);
            }
            if (retitled)
                reloadAgents(AgentReloadScan::FromCache);
        });
}

void MainWindow::appendImportedClaudeEvents(int sessionId, const ClaudeImportBatch &batch,
                                            const QString &title)
{
    AgentSession *s = findAgentSession(sessionId);
    if (!s || !m_agentStore)
        return;
    s->claudeImportBytes = batch.newOffset;
    if (!title.isEmpty())
        s->issueTitle = title;
    if (batch.lastMs > s->finishedAtMs)
        s->finishedAtMs = batch.lastMs;
    m_agentStore->saveSession(*s);
    const AgentSession snapshot = *s;
    for (const QJsonObject &ev : batch.events)
        m_agentStore->appendEvent(snapshot, ev);
    if (!batch.log.isEmpty())
        m_agentStore->appendLog(snapshot, batch.log);
    if (batch.events.isEmpty())
        return;
    noteAgentActivity(sessionId, int(batch.events.size()) * 200);
    if (m_streamEvents.contains(sessionId)) {
        m_streamEvents[sessionId] += batch.events;
        if (sessionId == m_selectedAgentSessionId && !renderTranscriptTail(sessionId))
            renderTranscriptForSession(sessionId);
    } else {
        m_streamEventsAbsent.remove(sessionId);
        if (sessionId == m_selectedAgentSessionId)
            ensureStreamEventsLoadedAsync(sessionId);
    }
}

void MainWindow::sendPromptToExternalClaudeSession(int sessionId, const QString &prompt)
{
    const auto surfaced = m_externalSurfaced.constFind(sessionId);
    if (surfaced == m_externalSurfaced.constEnd() || prompt.trimmed().isEmpty())
        return;
    const ExternalClaudeSession ext = *surfaced;
    if (!externalIsLive(ext.uuid)) {
        importClaudeSessions({ext}, prompt);
        return;
    }
    struct Sent {
        bool ok = false;
        bool unreachable = false;
        bool held = false;
        QString error;
    };
    const QString home = QDir::homePath();
    const QString accountsRoot = forkmesh::paths::dataPath(QStringLiteral("agent-accounts"));
    flashMessage(QStringLiteral("Sending to the Claude Code session\xE2\x80\xA6"));
    runOffThread<Sent>(
        [ext, prompt, home, accountsRoot] {
            Sent sent;
            const QList<qint64> pids = ClaudeSessionScan::findSessionPids(ext.uuid, ext.cwd);
            const QList<ClaudePeerSession> peers = ClaudePeerMessaging::liveSessions(
                ClaudePeerMessaging::configDirs(home, accountsRoot, {}));
            const ClaudePeerSession *session = ClaudePeerMessaging::find(peers, pids, ext.uuid);
            if (!session) {
                sent.unreachable = true;
                sent.error = QStringLiteral("it has no messaging socket");
                return sent;
            }
            QByteArrayList argv;
            QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(session->pid));
            if (cmdline.open(QIODevice::ReadOnly))
                argv = cmdline.readAll().split('\0');
            const ClaudeInboundPolicy policy = ClaudePeerMessaging::predictInbound(
                *session, argv, ClaudePeerMessaging::permissionClass(argv, ext.path),
                ClaudePeerMessaging::managedSettingsPath());
            if (policy.policy == QLatin1String("refuse")) {
                sent.unreachable = true;
                sent.error = QStringLiteral("it refuses messages from other sessions (%1)")
                                 .arg(policy.cause);
                return sent;
            }
            sent.held = policy.policy == QLatin1String("hold");
            if (sent.held && session->entrypoint != QLatin1String("cli")) {
                sent.unreachable = true;
                sent.error = QStringLiteral("it would hold the message for approval and has "
                                            "no terminal to approve it in (%1)")
                                 .arg(policy.cause);
                return sent;
            }
            sent.ok = ClaudePeerMessaging::deliverFromDesktop(*session, prompt, &sent.error);
            sent.unreachable = !sent.ok;
            return sent;
        },
        [this, ext, prompt](Sent sent) {
            if (sent.ok) {
                logSystem(QStringLiteral("Message sent to Claude Code in %1.").arg(ext.cwd));
                flashMessage(sent.held
                                 ? QStringLiteral("Sent. Claude Code holds it until you "
                                                  "approve it in its terminal.")
                                 : QStringLiteral("Sent to the Claude Code session."));
                return;
            }
            if (QMessageBox::question(
                    this, QStringLiteral("Continue in ForkMesh?"),
                    QStringLiteral("ForkMesh could not message this Claude Code session: "
                                   "%1.\n\nContinue the conversation as a ForkMesh agent "
                                   "instead? The session in Claude stays as it is.")
                        .arg(sent.error),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
                return;
            importClaudeSessions({ext}, prompt);
        });
}
