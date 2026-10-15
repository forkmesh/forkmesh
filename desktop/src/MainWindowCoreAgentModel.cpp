#include "MainWindow.h"
#include "MainWindowInternal.h"

#include "CoreAgentModel.h"
#include "CoreClient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace {

constexpr int kCoreAgentModelSettleMs = 300;

QByteArray contextFingerprint(QJsonObject context)
{
    context.remove(QStringLiteral("seq"));
    return QCryptographicHash::hash(QJsonDocument(context).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}

} // namespace

bool MainWindow::coreAgentModelAttached() const
{
    return m_coreClient && m_coreClient->isConnected() &&
           m_coreClient->hasNativeRoute(QStringLiteral("core.agents.model.push"));
}

QJsonObject MainWindow::coreAgentModelContext()
{
    QJsonObject context = localControlAgentsContext();
    context.insert(QStringLiteral("seq"), double(++m_coreAgentModelSeq));
    m_coreAgentModelFingerprint = contextFingerprint(context);
    return context;
}

bool MainWindow::pushCoreAgentModel()
{
    if (!coreAgentModelAttached())
        return false;
    m_coreClient->request(QStringLiteral("core.agents.model.push"), coreAgentModelContext(),
                          [](bool ok, const QJsonValue &result, const QString &) {
                              if (!ok)
                                  return;
                              const QString secret =
                                  result.toObject().value(QStringLiteral("confirm")).toString();
                              if (!secret.isEmpty())
                                  forkmesh::coreagentmodel::setConfirmSecret(secret);
                          });
    return true;
}

void MainWindow::scheduleCoreAgentModelPush()
{
    if (!coreAgentModelAttached())
        return;
    if (!m_coreAgentModelTimer) {
        m_coreAgentModelTimer = new QTimer(this);
        m_coreAgentModelTimer->setSingleShot(true);
        m_coreAgentModelTimer->setInterval(kCoreAgentModelSettleMs);
        connect(m_coreAgentModelTimer, &QTimer::timeout, this, [this] {
            if (!coreAgentModelAttached())
                return;
            const QByteArray before = m_coreAgentModelFingerprint;
            const QJsonObject context = localControlAgentsContext();
            if (!before.isEmpty() && contextFingerprint(context) == before)
                return;
            pushCoreAgentModel();
        });
    }
    if (!m_coreAgentModelTimer->isActive())
        m_coreAgentModelTimer->start();
}

void MainWindow::registerCoreAgentModelHandlers()
{
    m_coreClient->handle(QStringLiteral("ui.agents.context"),
                         [this](const QJsonObject &params, const CoreClient::Responder &respond) {
                             const QString secret =
                                 params.value(QStringLiteral("confirm")).toString();
                             if (!secret.isEmpty())
                                 forkmesh::coreagentmodel::setConfirmSecret(secret);
                             respond(coreAgentModelContext(), QString());
                         });
    m_coreClient->handle(QStringLiteral("ui.agents.usage"),
                         [this](const QJsonObject &, const CoreClient::Responder &respond) {
                             respond(localControlAgentOpsUsageInputs(), QString());
                         });
    m_coreClient->handle(QStringLiteral("ui.agents.reload"),
                         [this](const QJsonObject &params, const CoreClient::Responder &respond) {
                             if (params.value(QStringLiteral("sessions")).toBool())
                                 reloadAgents();
                             else if (params.value(QStringLiteral("bin")).toBool())
                                 reloadAgentRecycleBin();
                             notifyLocalControlAgentsChanged();
                             respond({}, QString());
                         });
    m_coreClient->handle(QStringLiteral("ui.agents.opsArm"),
                         [this](const QJsonObject &, const CoreClient::Responder &respond) {
                             armLocalControlAgentOpsBell();
                             respond({}, QString());
                         });
    connect(m_coreClient, &CoreClient::coreEvent, this,
            [this](const QString &event, const QJsonObject &data) {
                if (event == QLatin1String("core.agents.background"))
                    applyCoreBackgroundWork(data);
            });
    connect(m_coreClient, &CoreClient::connectedChanged, this, [this](bool connected) {
        m_coreAgentModelFingerprint.clear();
        if (connected) {
            pushCoreAgentModel();
            return;
        }
        forkmesh::coreagentmodel::setConfirmSecret(QString());
        const QList<int> pending(m_agentCompletionChecks.cbegin(), m_agentCompletionChecks.cend());
        for (const int sessionId : pending)
            QTimer::singleShot(0, this, [this, sessionId] {
                completeAgentSessionWhenSubprocessesExit(sessionId);
            });
    });
}

bool MainWindow::watchCoreBackgroundWork(int sessionId, qint64 processId)
{
    if (!m_coreClient || !m_coreClient->isConnected() ||
        !m_coreClient->hasNativeRoute(QStringLiteral("core.agents.background.watch")))
        return false;
    m_coreClient->request(QStringLiteral("core.agents.background.watch"),
                          {{QStringLiteral("id"), sessionId},
                           {QStringLiteral("pid"), double(processId)},
                           {QStringLiteral("announced"),
                            m_agentBackgroundWork.value(sessionId).announced}});
    return true;
}

void MainWindow::cancelCoreBackgroundWork(int sessionId)
{
    if (m_coreClient && m_coreClient->isConnected() &&
        m_coreClient->hasNativeRoute(QStringLiteral("core.agents.background.cancel")))
        m_coreClient->notify(QStringLiteral("core.agents.background.cancel"),
                             {{QStringLiteral("id"), sessionId}});
}

void MainWindow::applyCoreBackgroundWork(const QJsonObject &data)
{
    const int sessionId = data.value(QStringLiteral("id")).toInt();
    if (sessionId <= 0)
        return;
    if (!m_agentCompletionChecks.contains(sessionId)) {
        if (!data.value(QStringLiteral("cleared")).toBool() &&
            !data.value(QStringLiteral("done")).toBool())
            cancelCoreBackgroundWork(sessionId);
        return;
    }
    if (const AgentSession *live = findAgentSession(sessionId);
        !live || live->status != AgentStatus::Running) {
        m_agentCompletionChecks.remove(sessionId);
        m_agentBackgroundWork.remove(sessionId);
        applyAgentBackgroundWork();
        cancelCoreBackgroundWork(sessionId);
        return;
    }
    if (data.value(QStringLiteral("cleared")).toBool()) {
        watchCoreBackgroundWork(sessionId, agentSessionProcessId(sessionId));
        return;
    }
    const QString notice = data.value(QStringLiteral("notice")).toString();
    if (data.value(QStringLiteral("done")).toBool()) {
        m_agentCompletionChecks.remove(sessionId);
        m_agentBackgroundWork.remove(sessionId);
        applyAgentBackgroundWork();
        if (!notice.isEmpty())
            noteAgentSessionNotice(sessionId, notice, /*error=*/true);
        AgentSession *session = findAgentSession(sessionId);
        if (!session || session->status != AgentStatus::Running)
            return;
        session->status = AgentStatus::Success;
        session->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
        session->lastError.clear();
        if (m_agentStore && !isExternalSession(sessionId))
            m_agentStore->saveSession(*session);
        updateAgentStatusCell(sessionId);
        notifyAgentDone(sessionId);
        return;
    }
    AgentBackgroundWork &work = m_agentBackgroundWork[sessionId];
    work.count = data.value(QStringLiteral("count")).toInt();
    work.busy = data.value(QStringLiteral("busy")).toBool();
    work.summary = data.value(QStringLiteral("summary")).toString();
    work.announced = true;
    applyAgentBackgroundWork();
    if (!notice.isEmpty())
        noteAgentSessionNotice(sessionId, notice);
    if (!m_agentCompletionChecks.contains(sessionId))
        return;
    if (data.value(QStringLiteral("changed")).toBool())
        updateAgentStatusCell(sessionId);
    else if (sessionId == m_selectedAgentSessionId)
        refreshAgentDetailMeta(sessionId);
}
