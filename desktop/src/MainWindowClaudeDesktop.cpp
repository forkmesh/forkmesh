#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"
#include "AgentResumeIdentity.h"
#include "ClaudeDesktopSessions.h"
#include "ClaudeSessionImport.h"
#include "ClaudeStreamSession.h"
#include "ProcessReaper.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QTimer>

namespace {

constexpr int kClaudeDesktopLinkSpacingMs = 2000;

bool openClaudeUrl(const QUrl &url)
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    if (forkmesh::processreaper::startReleasedQuietly(QStringLiteral("xdg-open"), {url.toString()}))
        return true;
#elif defined(Q_OS_MACOS)
    if (forkmesh::processreaper::startReleasedQuietly(QStringLiteral("/usr/bin/open"), {url.toString()}))
        return true;
#endif
    return QDesktopServices::openUrl(url);
}

QStringList claudeAccountConfigDirs()
{
    QStringList dirs;
    for (const AgentAccountProfile &profile :
         agentAccountProfiles(QStringLiteral("claude-code")))
        if (!profile.configDir.trimmed().isEmpty())
            dirs.append(profile.configDir);
    return dirs;
}

struct ClaudeAppAccount {
    QString id;
    QString configDir;
};

QList<ClaudeAppAccount> claudeAppAccounts()
{
    QList<ClaudeAppAccount> out;
    for (const AgentAccountProfile &profile :
         agentAccountProfiles(QStringLiteral("claude-code")))
        if (!profile.configDir.trimmed().isEmpty())
            out.append({profile.id, profile.configDir});
    out.append({QString(), forkmesh::claudedesktop::defaultClaudeHome()});
    return out;
}

bool underDir(const QString &path, const QString &dir)
{
    const QString root = QFileInfo(dir).canonicalFilePath();
    return !root.isEmpty() && path.startsWith(root + QLatin1Char('/'));
}

struct ClaudeAppJob {
    int sessionId = 0;
    QString cliSessionId;
    QString path;
    qint64 bytes = 0;
};

struct ClaudeAppSync {
    int sessionId = 0;
    QString seenPath;
    qint64 seenBytes = 0;
    QString path;
    qint64 fromBytes = 0;
    bool reset = false;
    bool missing = false;
    QString accountId;
    ClaudeImportBatch batch;
};

struct ClaudeDesktopLookup {
    QString cliSessionId;
    forkmesh::claudedesktop::Exposure exposure;
};

} // namespace

bool MainWindow::claudeDesktopSessionsEnabled() const
{
    return QSettings().value(kClaudeDesktopSessionsSetting, false).toBool();
}

QString MainWindow::claudeDesktopBlocker(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return QStringLiteral("Select a session first.");
    if (session->cloud || isCloudSession(sessionId))
        return QStringLiteral("This agent runs on another machine, so its Claude "
                              "Code session is not on this computer.");
    if (isExternalSession(sessionId))
        return QString();
    if (session->provider != QLatin1String("claude-code"))
        return QStringLiteral("Only Claude Code agents are Claude Code sessions; "
                              "%1 agents stay in ForkMesh.")
            .arg(agentProviderName(session->provider));
    return QString();
}

void MainWindow::noteClaudeDesktopCandidate(const AgentSession &session)
{
#ifdef FORKMESH_WINDOW_TESTS
    Q_UNUSED(session);
    return;
#else
    if (!AgentStatus::isTerminal(session.status) || session.cloud ||
        isExternalSession(session.id) ||
        session.provider != QLatin1String("claude-code") ||
        !claudeDesktopSessionsEnabled())
        return;
    const QString key =
        QStringLiteral("%1:%2").arg(session.id).arg(session.finishedAtMs);
    if (m_claudeDesktopChecked.contains(key))
        return;
    m_claudeDesktopChecked.insert(key);
    addAgentToClaudeDesktop(session.id, false);
#endif
}

void MainWindow::addAgentToClaudeDesktop(int sessionId, bool open)
{
    const QString blocker = claudeDesktopBlocker(sessionId);
    if (!blocker.isEmpty()) {
        if (open)
            flashMessage(blocker, true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    if (m_claudeDesktopPending.contains(sessionId))
        return;
    const AgentSession snapshot = *findAgentSession(sessionId);
    QString known;
    if (isExternalSession(sessionId))
        known = m_externalSurfaced.value(sessionId).uuid;
    else if (m_streamEvents.contains(sessionId))
        known = forkmesh::claudedesktop::latestSessionId(m_streamEvents.value(sessionId));
    const bool external = isExternalSession(sessionId);
    AgentStore *store = external ? nullptr : m_agentStore;
    const QStringList accounts = claudeAccountConfigDirs();
    const QString home = forkmesh::claudedesktop::defaultClaudeHome();
    m_claudeDesktopPending.insert(sessionId);
    runOffThread<ClaudeDesktopLookup>(
        [store, snapshot, known, accounts, home] {
            ClaudeDesktopLookup out;
            out.cliSessionId = known;
            if (out.cliSessionId.isEmpty() && store) {
                out.cliSessionId = forkmesh::claudedesktop::latestSessionId(
                    store->loadRecentEvents(snapshot, AgentStore::kLatestOutputTailBytes));
                if (out.cliSessionId.isEmpty())
                    out.cliSessionId =
                        forkmesh::claudedesktop::latestSessionId(store->loadEvents(snapshot));
            }
            if (out.cliSessionId.isEmpty())
                out.exposure.error = QStringLiteral("this agent has no Claude Code "
                                                    "session id yet");
            else
                out.exposure =
                    forkmesh::claudedesktop::exposeSession(out.cliSessionId, accounts, home);
            return out;
        },
        [this, sessionId, open](ClaudeDesktopLookup out) {
            m_claudeDesktopPending.remove(sessionId);
            const AgentSession *now = findAgentSession(sessionId);
            if (!now)
                return;
            if (!open && (!AgentStatus::isTerminal(now->status) ||
                          isStoppableAgentSession(sessionId)))
                return;
            if (!out.exposure.ok) {
                if (open)
                    flashMessage(QStringLiteral("Could not add this agent to the Claude "
                                                "app: %1.")
                                     .arg(out.exposure.error),
                                 true, QString(), forkmesh::kLocalOperationNoticeKind);
                return;
            }
            const bool fresh = isExternalSession(sessionId) ||
                               now->claudeAppSessionId != out.cliSessionId;
            if (!isExternalSession(sessionId))
                m_claudeDesktopLinkOwners.insert(out.cliSessionId, sessionId);
            if (open || fresh)
                queueClaudeDesktopLink(out.cliSessionId, open);
        });
}

void MainWindow::backfillClaudeDesktopSessions()
{
#ifdef FORKMESH_WINDOW_TESTS
    return;
#endif
    if (!claudeDesktopSessionsEnabled() || m_claudeDesktopBackfillRunning)
        return;
    m_claudeDesktopBackfillRunning = true;
    const QStringList accounts = claudeAccountConfigDirs();
    const QString home = forkmesh::claudedesktop::defaultClaudeHome();
    runOffThread<int>(
        [accounts, home] {
            int linked = 0;
            for (const QString &account : accounts)
                linked += forkmesh::claudedesktop::exposeAccount(account, home);
            return linked;
        },
        [this](int) {
            m_claudeDesktopBackfillRunning = false;
            const QList<AgentSession> sessions = m_agentSessions;
            for (const AgentSession &session : sessions) {
                if (session.deletedAtMs > 0 || session.associationOnly ||
                    !session.claudeAppSessionId.isEmpty())
                    continue;
                noteClaudeDesktopCandidate(session);
            }
        });
}

void MainWindow::queueClaudeDesktopLink(const QString &cliSessionId, bool front)
{
    const QUrl url = forkmesh::claudedesktop::resumeUrl(cliSessionId);
    if (url.isEmpty())
        return;
    m_claudeDesktopLinkQueue.removeAll(cliSessionId);
    if (front)
        m_claudeDesktopLinkQueue.prepend(cliSessionId);
    else
        m_claudeDesktopLinkQueue.append(cliSessionId);
    if (!m_claudeDesktopLinkTimer) {
        m_claudeDesktopLinkTimer = new QTimer(this);
        m_claudeDesktopLinkTimer->setSingleShot(true);
        connect(m_claudeDesktopLinkTimer, &QTimer::timeout, this,
                &MainWindow::pumpClaudeDesktopLinks);
    }
    if (!m_claudeDesktopLinkTimer->isActive())
        m_claudeDesktopLinkTimer->start(front ? 0 : kClaudeDesktopLinkSpacingMs);
}

void MainWindow::pumpClaudeDesktopLinks()
{
    if (m_claudeDesktopLinkQueue.isEmpty())
        return;
    const QString id = m_claudeDesktopLinkQueue.takeFirst();
    const int owner = m_claudeDesktopLinkOwners.take(id);
    if (!openClaudeUrl(forkmesh::claudedesktop::resumeUrl(id))) {
        qWarning() << "Claude app did not take session" << id;
    } else if (AgentSession *session = owner ? findAgentSession(owner) : nullptr;
               session && m_agentStore && session->claudeAppSessionId != id) {
        session->claudeAppSessionId = id;
        m_agentStore->saveSession(*session);
    }
    if (!m_claudeDesktopLinkQueue.isEmpty() && m_claudeDesktopLinkTimer)
        m_claudeDesktopLinkTimer->start(kClaudeDesktopLinkSpacingMs);
}

void MainWindow::syncClaudeAppSessions()
{
    if (m_claudeAppSyncInFlight || !m_agentStore)
        return;
    QList<ClaudeAppJob> jobs;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.id <= 0 || s.claudeAppSessionId.isEmpty() || s.cloud || s.deletedAtMs > 0 ||
            s.associationOnly || s.provider != QLatin1String("claude-code") ||
            m_claudeAppTranscriptMissing.contains(s.id) || isExternalSession(s.id) ||
            s.status == AgentStatus::Running ||
            s.status == AgentStatus::Queued || s.status == AgentStatus::Waiting)
            continue;
        jobs << ClaudeAppJob{s.id, s.claudeAppSessionId, s.claudeAppPath, s.claudeAppBytes};
    }
    if (jobs.isEmpty())
        return;
    m_claudeAppSyncInFlight = true;
    const QList<ClaudeAppAccount> accounts = claudeAppAccounts();
    runOffThread<QList<ClaudeAppSync>>(
        [jobs, accounts] {
            QList<ClaudeAppSync> out;
            for (const ClaudeAppJob &job : jobs) {
                ClaudeAppSync sync;
                sync.sessionId = job.sessionId;
                sync.seenPath = job.path;
                sync.seenBytes = job.bytes;
                sync.fromBytes = job.bytes;
                sync.path = job.path;
                const QString fileName = job.cliSessionId.toLower() + QStringLiteral(".jsonl");
                if (QFileInfo(sync.path).fileName() != fileName || !QFileInfo(sync.path).isFile()) {
                    sync.path.clear();
                    for (const ClaudeAppAccount &account : accounts) {
                        const QString found = forkmesh::claudedesktop::findTranscript(
                            QDir(account.configDir).filePath(QStringLiteral("projects")),
                            job.cliSessionId);
                        if (!found.isEmpty()) {
                            sync.path = QFileInfo(found).canonicalFilePath();
                            break;
                        }
                    }
                    sync.reset = true;
                    sync.fromBytes = 0;
                    if (sync.path.isEmpty()) {
                        sync.missing = true;
                        out << sync;
                        continue;
                    }
                }
                const qint64 size = QFileInfo(sync.path).size();
                if (size == sync.fromBytes && !sync.reset)
                    continue;
                for (const ClaudeAppAccount &account : accounts)
                    if (underDir(sync.path, account.configDir)) {
                        sync.accountId = account.id;
                        break;
                    }
                sync.batch = ClaudeSessionImport::convertForeign(
                    sync.path, size < sync.fromBytes ? 0 : sync.fromBytes, job.cliSessionId);
                out << sync;
            }
            return out;
        },
        [this](QList<ClaudeAppSync> synced) {
            m_claudeAppSyncInFlight = false;
            for (ClaudeAppSync &sync : synced) {
                AgentSession *s = findAgentSession(sync.sessionId);
                if (!s || !m_agentStore)
                    continue;
                if (sync.missing) {
                    m_claudeAppTranscriptMissing.insert(sync.sessionId);
                    continue;
                }
                if (s->claudeAppPath != sync.seenPath || s->claudeAppBytes != sync.seenBytes ||
                    s->status == AgentStatus::Running || s->status == AgentStatus::Waiting ||
                    s->status == AgentStatus::Queued)
                    continue;
                s->claudeAppPath = sync.path;
                s->claudeAppBytes = sync.batch.newOffset;
                if (sync.batch.events.isEmpty()) {
                    m_agentStore->saveSession(*s);
                    continue;
                }
                if (!sync.accountId.isEmpty())
                    for (QJsonObject &ev : sync.batch.events)
                        if (!ev.value(QStringLiteral("session_id")).toString().isEmpty())
                            ev.insert(forkmesh::agents::resumeAccountKey(), sync.accountId);
                if (ClaudeStreamSession *idle = m_streamSessions.take(sync.sessionId)) {
                    idle->disconnect(this);
                    idle->stop();
                    idle->deleteLater();
                }
                appendClaudeTranscriptEvents(sync.sessionId, sync.batch);
            }
        });
}
