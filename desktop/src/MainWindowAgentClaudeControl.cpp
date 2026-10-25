#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentCliTasks.h"
#include "AgentWorktree.h"
#include "ClaudeControlProtocol.h"
#include "ClaudeElicitationDialog.h"
#include "ClaudeStreamSession.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

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

QWidget *MainWindow::buildAgentSubSessionStrip()
{
    auto *strip = new QFrame;
    strip->setObjectName(QStringLiteral("agentSubSessionStrip"));
    strip->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(strip);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(4);
    m_agentSubSessionHeader = new QLabel(strip);
    m_agentSubSessionHeader->setObjectName(QStringLiteral("agentSubSessionHeader"));
    QFont bold = m_agentSubSessionHeader->font();
    bold.setBold(true);
    m_agentSubSessionHeader->setFont(bold);
    layout->addWidget(m_agentSubSessionHeader);
    m_agentSubSessionRows = new QWidget(strip);
    m_agentSubSessionRows->setObjectName(QStringLiteral("agentSubSessionRows"));
    auto *rows = new QVBoxLayout(m_agentSubSessionRows);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    layout->addWidget(m_agentSubSessionRows);
    strip->hide();
    m_agentSubSessionStrip = strip;
    return strip;
}

void MainWindow::refreshAgentSubSessions()
{
    if (!m_agentSubSessionStrip || !m_agentSubSessionRows)
        return;
    const int sessionId = m_selectedAgentSessionId;
    const auto ledger = m_agentCliTasks.constFind(sessionId);
    const QList<forkmesh::agentclitasks::Task> tasks =
        ledger == m_agentCliTasks.constEnd() ? QList<forkmesh::agentclitasks::Task>()
                                             : ledger->rows();
    auto *rowsLayout = qobject_cast<QVBoxLayout *>(m_agentSubSessionRows->layout());
    while (QLayoutItem *item = rowsLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    if (tasks.isEmpty()) {
        m_agentSubSessionStrip->hide();
        return;
    }
    const ClaudeStreamSession *stream = m_streamSessions.value(sessionId);
    const bool canStop = stream && stream->running() && stream->hostControl();
    m_agentSubSessionHeader->setText(
        tasks.size() == 1 ? QStringLiteral("1 sub-session running")
                          : QStringLiteral("%1 sub-sessions running").arg(tasks.size()));
    for (const forkmesh::agentclitasks::Task &task : tasks) {
        auto *row = new QWidget(m_agentSubSessionRows);
        row->setObjectName(QStringLiteral("agentSubSessionRow"));
        row->setProperty("taskId", task.id);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(task.depth * 16, 0, 0, 0);
        h->setSpacing(8);
        QStringList facts;
        if (!task.subagentType.isEmpty())
            facts << task.subagentType;
        else if (task.taskType == QLatin1String("local_bash"))
            facts << QStringLiteral("shell");
        else if (task.taskType == QLatin1String("local_workflow"))
            facts << QStringLiteral("workflow");
        if (task.backgrounded)
            facts << QStringLiteral("background");
        if (task.tokens > 0)
            facts << QStringLiteral("%1 tokens").arg(compactCount(task.tokens));
        if (task.toolUses > 0)
            facts << (task.toolUses == 1 ? QStringLiteral("1 tool call")
                                         : QStringLiteral("%1 tool calls").arg(task.toolUses));
        QString text = QStringLiteral("<b>%1</b>")
                           .arg((task.description.isEmpty() ? QStringLiteral("Unnamed task")
                                                            : task.description)
                                    .left(160)
                                    .toHtmlEscaped());
        if (!facts.isEmpty())
            text += QStringLiteral(" · ") + facts.join(QStringLiteral(" · ")).toHtmlEscaped();
        const QString status = !task.summary.isEmpty()
                                   ? task.summary
                                   : (task.lastTool.isEmpty()
                                          ? QString()
                                          : QStringLiteral("using %1").arg(task.lastTool));
        if (!status.isEmpty())
            text += QStringLiteral("<br>%1").arg(status.left(240).toHtmlEscaped());
        auto *label = new QLabel(text, row);
        label->setObjectName(QStringLiteral("agentSubSessionLabel"));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setToolTip(task.id);
        h->addWidget(label, 1);
        if (canStop) {
            auto *stop = new QToolButton(row);
            stop->setObjectName(QStringLiteral("agentSubSessionStopButton"));
            stop->setText(QStringLiteral("Stop"));
            stop->setToolTip(QStringLiteral("Stop this sub-session; the main session keeps going"));
            const QString taskId = task.id;
            connect(stop, &QToolButton::clicked, this, [this, sessionId, taskId] {
                stopAgentSubSession(sessionId, taskId);
            });
            h->addWidget(stop, 0, Qt::AlignTop);
        }
        rowsLayout->addWidget(row);
    }
    m_agentSubSessionStrip->show();
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
