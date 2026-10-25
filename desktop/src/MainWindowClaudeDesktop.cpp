#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"
#include "ClaudeDesktopSessions.h"

#include <QDesktopServices>
#include <QSettings>
#include <QTimer>

namespace {

constexpr int kClaudeDesktopLinkSpacingMs = 2000;

QStringList claudeAccountConfigDirs()
{
    QStringList dirs;
    for (const AgentAccountProfile &profile :
         agentAccountProfiles(QStringLiteral("claude-code")))
        if (!profile.configDir.trimmed().isEmpty())
            dirs.append(profile.configDir);
    return dirs;
}

struct ClaudeDesktopLookup {
    QString cliSessionId;
    forkmesh::claudedesktop::Exposure exposure;
};

} // namespace

bool MainWindow::claudeDesktopSessionsEnabled() const
{
    return QSettings().value(kClaudeDesktopSessionsSetting, true).toBool();
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
            if (!out.exposure.ok) {
                if (open)
                    flashMessage(QStringLiteral("Could not add this agent to the Claude "
                                                "app: %1.")
                                     .arg(out.exposure.error),
                                 true, QString(), forkmesh::kLocalOperationNoticeKind);
                return;
            }
            bool fresh = true;
            if (AgentSession *session = findAgentSession(sessionId);
                session && !isExternalSession(sessionId)) {
                fresh = session->claudeDesktopSessionId != out.cliSessionId;
                if (fresh && m_agentStore) {
                    session->claudeDesktopSessionId = out.cliSessionId;
                    m_agentStore->saveSession(*session);
                }
            }
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
                    !session.claudeDesktopSessionId.isEmpty())
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
    if (!QDesktopServices::openUrl(forkmesh::claudedesktop::resumeUrl(id)))
        qWarning() << "Claude app did not take session" << id;
    if (!m_claudeDesktopLinkQueue.isEmpty() && m_claudeDesktopLinkTimer)
        m_claudeDesktopLinkTimer->start(kClaudeDesktopLinkSpacingMs);
}
