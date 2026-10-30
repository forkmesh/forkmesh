#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentCliTasks.h"
#include "AgentWorktree.h"
#include "ClaudeControlProtocol.h"
#include "ClaudeElicitationDialog.h"
#include "ClaudeStreamSession.h"
#include "MainWindowNextAccess.h"
#include "SessionSidePanels.h"
#include "WorkspaceLayout.h"

#include <QInputDialog>
#include <QSplitter>
#include <QTimer>

using namespace forkmesh::ui;

namespace {

const QString kTitledByUser = QStringLiteral("You");
const QString kTitledByClaudeSession = QStringLiteral("Claude Code session");
const QString kTitledByClaudeRename = QStringLiteral("Claude Code rename");

QString compactCount(qint64 value)
{
    if (value >= 1'000'000)
        return QStringLiteral("%1M").arg(value / 1'000'000.0, 0, 'f', 1);
    if (value >= 1'000)
        return QStringLiteral("%1k").arg(value / 1'000.0, 0, 'f', 1);
    return QString::number(value);
}

} // namespace

QString MainWindow::claudeInitialSessionTitle(const AgentSession &session) const
{
    if (session.issueNumber > 0 || session.titledBy == kTitledByUser ||
        forkmesh::agentwt::isPromptNamerLabel(session.titledBy,
                                              agentProviderName(session.provider)))
        return session.issueTitle.trimmed();
    return QString();
}

void MainWindow::handleClaudeControlEvent(int sessionId, const QJsonObject &event)
{
    namespace cc = forkmesh::claudecontrol;
    ClaudeStreamSession *stream = m_streamSessions.value(sessionId);
    if (!stream || !stream->hostControl())
        return;
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("system")) {
        const QString subtype = event.value(QStringLiteral("subtype")).toString();
        if (subtype == QLatin1String("init")) {
            if (stream->claimTitleRequest())
                requestClaudeSessionTitle(sessionId);
        } else if (subtype == QLatin1String("session_title_changed")) {
            applyClaudeSessionTitle(sessionId, cc::titleFromEvent(event), true);
        } else if (forkmesh::agentclitasks::isTaskEvent(event) ||
                   subtype == QLatin1String("background_tasks_changed")) {
            if (sessionId == m_selectedAgentSessionId)
                refreshAgentSubSessions();
        }
        return;
    }
    if (cc::isControlCancel(event)) {
        const QString key = event.value(cc::kPromptKeyField).toString();
        if (key.isEmpty())
            return;
        closeClaudeElicitation(key);
        applyTranscriptEvent(
            sessionId,
            QJsonObject{{QStringLiteral("type"), QStringLiteral("_local_ask_answer")},
                        {QStringLiteral("tool_use_id"), key},
                        {QStringLiteral("text"),
                         QStringLiteral("Claude Code withdrew this question")}});
        if (const AgentSession *waiting = findAgentSession(sessionId);
            waiting && waiting->status == AgentStatus::Waiting)
            markAgentSessionRunning(sessionId);
        return;
    }
    if (!cc::isControlRequest(event))
        return;
    const cc::Prompt prompt = cc::promptFor(event);
    if (prompt.kind == cc::PromptKind::None || prompt.kind == cc::PromptKind::Unanswerable)
        return;
    QTimer::singleShot(0, this, [this] { ringLocalControlAgentOpsBell(); });
    if (prompt.kind == cc::PromptKind::Elicitation) {
        showClaudeElicitation(sessionId, event);
        return;
    }
    const QJsonObject card = cc::cardEvent(prompt);
    if (!card.isEmpty())
        applyTranscriptEvent(sessionId, card);
}

void MainWindow::requestClaudeSessionTitle(int sessionId)
{
    ClaudeStreamSession *stream = m_streamSessions.value(sessionId);
    const AgentSession *session = findAgentSession(sessionId);
    if (!stream || !session || session->issueNumber > 0)
        return;
    if (session->titledBy == kTitledByUser ||
        forkmesh::agentwt::isPromptNamerLabel(session->titledBy,
                                              agentProviderName(session->provider))) {
        stream->renameSession(session->issueTitle);
        return;
    }
    if (!QSettings().value(kAgentTitleFromAgentSetting, true).toBool())
        return;
    const QString prompt = session->prompt.trimmed();
    const bool promptTitled =
        session->issueTitle == forkmesh::agentwt::promptTitle(session->prompt) ||
        session->issueTitle == QLatin1String("Ad-hoc agent run");
    if (prompt.isEmpty() || (!promptTitled && session->titledBy.isEmpty()))
        return;
    const QPointer<ClaudeStreamSession> guard(stream);
    stream->generateSessionTitle(prompt, [this, sessionId, guard](const QString &title) {
        if (!guard || m_streamSessions.value(sessionId) != guard || title.isEmpty())
            return;
        applyClaudeSessionTitle(sessionId, title, false);
    });
}

bool MainWindow::setAgentSessionTitle(int sessionId, const QString &title,
                                      const QString &titledBy)
{
    const QString clean = forkmesh::claudecontrol::cleanTitle(title);
    if (clean.isEmpty() || !m_agentStore)
        return false;
    {
        AgentSession *session = findAgentSession(sessionId);
        if (!session)
            return false;
        if (session->issueTitle == clean && session->titledBy == titledBy)
            return false;
        session->issueTitle = clean;
        session->titledBy = titledBy;
        m_agentStore->saveSession(*session);
        m_agentStore->appendLog(*session, QStringLiteral("==> %1 titled this run \"%2\".\n")
                                              .arg(titledBy, clean));
    }
    if (m_streamSessionInfo.contains(sessionId)) {
        AgentSession info = m_streamSessionInfo.value(sessionId);
        info.issueTitle = clean;
        info.titledBy = titledBy;
        m_streamSessionInfo[sessionId] = info;
    }
    reloadAgents();
    scheduleAgentSessionsPush();
    return true;
}

void MainWindow::applyClaudeSessionTitle(int sessionId, const QString &title, bool renamed)
{
    const AgentSession *session = findAgentSession(sessionId);
    const QString clean = forkmesh::claudecontrol::cleanTitle(title);
    if (!session || clean.isEmpty() || session->issueNumber > 0 ||
        session->issueTitle == clean)
        return;
    if (!renamed && session->titledBy == kTitledByUser)
        return;
    if (!setAgentSessionTitle(sessionId, clean,
                              renamed ? kTitledByClaudeRename : kTitledByClaudeSession))
        return;
    if (renamed)
        noteAgentSessionNotice(
            sessionId, QStringLiteral("Claude Code renamed this session \"%1\".").arg(clean));
}

void MainWindow::renameAgentSessionInteractive(int sessionId)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session || isCloudSession(sessionId) || isExternalSession(sessionId))
        return;
    auto *dialog = new QInputDialog(this);
    dialog->setObjectName(QStringLiteral("agentRenameDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Rename agent"));
    dialog->setLabelText(QStringLiteral("Session name:"));
    dialog->setTextValue(session->issueTitle);
    connect(dialog, &QInputDialog::textValueSelected, this,
            [this, sessionId](const QString &text) {
                const QString clean = forkmesh::claudecontrol::cleanTitle(text);
                if (clean.isEmpty())
                    return;
                setAgentSessionTitle(sessionId, clean, kTitledByUser);
                if (ClaudeStreamSession *stream = m_streamSessions.value(sessionId);
                    stream && stream->running() && stream->hostControl())
                    stream->renameSession(clean);
            });
    dialog->open();
}

void MainWindow::syncClaudePermissionMode(int sessionId)
{
    ClaudeStreamSession *stream = m_streamSessions.value(sessionId);
    if (!stream || !stream->hostControl())
        return;
    const QString label =
        forkmesh::claudecontrol::permissionModeLabel(stream->permissionMode());
    AgentSession *session = findAgentSession(sessionId);
    if (!session || label.isEmpty() ||
        session->mode.compare(label, Qt::CaseInsensitive) == 0)
        return;
    session->mode = label;
    if (m_agentStore)
        m_agentStore->saveSession(*session);
    if (m_streamSessionInfo.contains(sessionId))
        m_streamSessionInfo[sessionId].mode = label;
    noteAgentSessionNotice(sessionId,
                           QStringLiteral("Permission mode is now %1.").arg(label));
}

void MainWindow::showClaudeElicitation(int sessionId, const QJsonObject &event)
{
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;
    auto *dialog = new ClaudeElicitationDialog(session->issueTitle, event, this);
    const QString key = dialog->promptKey();
    closeClaudeElicitation(key);
    m_claudeElicitations.insert(key, dialog);
    connect(dialog, &ClaudeElicitationDialog::answered, this,
            [this, sessionId](const QString &answeredKey, const QString &action,
                              const QJsonObject &content) {
                m_claudeElicitations.remove(answeredKey);
                answerClaudeElicitation(sessionId, answeredKey, action, content);
            });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::answerClaudeElicitation(int sessionId, const QString &key,
                                         const QString &action, const QJsonObject &content)
{
    ClaudeStreamSession *stream = m_streamSessions.value(sessionId);
    if (!stream || !stream->running() || !stream->respondToElicitation(key, action, content)) {
        noteAgentSessionNotice(
            sessionId,
            QStringLiteral("The answer could not reach Claude Code; the request was "
                           "already closed or the session has stopped."),
            true);
        return;
    }
    noteAgentSessionNotice(
        sessionId,
        action == QLatin1String("accept")
            ? QStringLiteral("Sent your answer to the MCP server's request.")
            : (action == QLatin1String("decline")
                   ? QStringLiteral("Declined the MCP server's request.")
                   : QStringLiteral("Cancelled the MCP server's request.")));
    markAgentSessionRunning(sessionId);
}

void MainWindow::closeClaudeElicitation(const QString &key)
{
    const QPointer<ClaudeElicitationDialog> dialog = m_claudeElicitations.take(key);
    if (!dialog)
        return;
    dialog->blockSignals(true);
    dialog->close();
    dialog->deleteLater();
}

QWidget *MainWindow::buildAgentTowerPanels()
{
    m_agentTowerPanels = new SessionSidePanels;
    m_agentTowerPanels->setMinimumWidth(220);
    m_agentTowerPanels->hide();
    m_agentTowerPanelsTimer = new QTimer(this);
    m_agentTowerPanelsTimer->setInterval(1000);
    connect(m_agentTowerPanelsTimer, &QTimer::timeout, this,
            &MainWindow::refreshAgentTowerPanels);
    m_agentTowerPanelsTimer->start();
    return m_agentTowerPanels;
}

void MainWindow::refreshAgentTowerPanels()
{
    if (!m_agentTowerPanels)
        return;
    const bool want = !workspaceLayoutNew() && m_selectedAgentSessionId != 0 &&
                      findAgentSession(m_selectedAgentSessionId);
    if (m_agentTowerPanels->isHidden() == want)
        m_agentTowerPanels->setVisible(want);
    if (!want || !m_agentTowerPanels->isVisible())
        return;
    if (!m_agentTowerPanelsSized && m_agentCodeSplit && m_agentCodeSplit->width() > 0) {
        m_agentTowerPanelsSized = true;
        const int index = m_agentCodeSplit->indexOf(m_agentTowerPanels);
        QList<int> sizes = m_agentCodeSplit->sizes();
        if (index >= 0 && index < sizes.size()) {
            const int total = m_agentCodeSplit->width();
            const int panel = qBound(220, total / 4, 320);
            int rest = 0;
            for (int at = 0; at < sizes.size(); ++at)
                if (at != index)
                    rest += sizes.at(at);
            const int room = qMax(0, total - panel);
            for (int at = 0; at < sizes.size(); ++at)
                if (at != index)
                    sizes[at] = rest > 0 ? sizes.at(at) * room / rest : 0;
            sizes[index] = panel;
            m_agentCodeSplit->setSizes(sizes);
        }
    }
    NextAccess::feedSessionSidePanels(*this, m_agentTowerPanels, m_selectedAgentSessionId);
}

void MainWindow::refreshAgentSubSessions()
{
    if (m_agentTowerPanels && m_agentTowerPanels->isVisible())
        fillAgentSubSessions(m_agentTowerPanels, m_selectedAgentSessionId);
}

void MainWindow::fillAgentSubSessions(SessionSidePanels *panels, int sessionId)
{
    if (!panels)
        return;
    const auto ledger = m_agentCliTasks.constFind(sessionId);
    const QList<forkmesh::agentclitasks::Task> tasks =
        ledger == m_agentCliTasks.constEnd() ? QList<forkmesh::agentclitasks::Task>()
                                             : ledger->rows();
    const ClaudeStreamSession *stream = m_streamSessions.value(sessionId);
    const bool canStop = stream && stream->running() && stream->hostControl();
    QList<SessionSidePanels::SubSession> rows;
    for (const forkmesh::agentclitasks::Task &task : tasks) {
        SessionSidePanels::SubSession row;
        row.id = task.id;
        row.depth = task.depth;
        QString kind;
        if (!task.subagentType.isEmpty())
            kind = task.subagentType;
        else if (task.taskType == QLatin1String("local_bash"))
            kind = QStringLiteral("shell");
        else if (task.taskType == QLatin1String("local_workflow"))
            kind = QStringLiteral("workflow");
        row.icon = task.taskType == QLatin1String("local_bash")       ? QStringLiteral("terminal")
                   : task.taskType == QLatin1String("local_workflow") ? QStringLiteral("workflow")
                                                                      : QStringLiteral("smart-toy");
        row.text = task.description.isEmpty() ? QStringLiteral("Unnamed task")
                                              : task.description.left(160);
        row.trailing = task.tokens > 0 ? compactCount(task.tokens) : kind;
        QStringList facts;
        if (!kind.isEmpty())
            facts << kind;
        if (task.backgrounded)
            facts << QStringLiteral("background");
        if (task.tokens > 0)
            facts << QStringLiteral("%1 tokens").arg(compactCount(task.tokens));
        if (task.toolUses > 0)
            facts << (task.toolUses == 1 ? QStringLiteral("1 tool call")
                                         : QStringLiteral("%1 tool calls").arg(task.toolUses));
        const QString status = !task.summary.isEmpty()
                                   ? task.summary
                                   : (task.lastTool.isEmpty()
                                          ? QString()
                                          : QStringLiteral("using %1").arg(task.lastTool));
        QStringList tip{row.text};
        if (!facts.isEmpty())
            tip << facts.join(QStringLiteral(" · "));
        if (!status.isEmpty())
            tip << status.left(240);
        row.tip = tip.join(QLatin1Char('\n'));
        rows << row;
    }
    MainWindow *self = this;
    panels->setSubSessions(rows, canStop, [self, sessionId](const QString &taskId) {
        self->stopAgentSubSession(sessionId, taskId);
    });
}

void MainWindow::stopAgentSubSession(int sessionId, const QString &taskId)
{
    ClaudeStreamSession *stream = m_streamSessions.value(sessionId);
    if (!stream || !stream->running() || !stream->stopTask(taskId)) {
        noteAgentSessionNotice(sessionId,
                               QStringLiteral("Could not ask Claude Code to stop that "
                                              "sub-session."),
                               true);
        return;
    }
    noteAgentSessionNotice(sessionId, QStringLiteral("Asked Claude Code to stop a sub-session."));
}
