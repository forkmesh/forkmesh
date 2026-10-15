#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowAgentsInternal.h"

#include "AgentRunner.h"
#include "AgentStore.h"

#include <QJsonObject>

void MainWindow::finishCoreRunnerReattach(int sessionId, const QString &key, const QJsonObject &meta,
                                          qint64 pid)
{
    AgentSession *session = findAgentSession(sessionId);
    if (!session || runnerForSession(sessionId)) {
        stopUnadoptedCoreAgent(key, true);
        return;
    }
    AgentRunner::Config config = agentConfigForProvider(session->provider, session->model);
    if (!session->model.isEmpty() && agentModelMatchesProvider(session->provider, session->model))
        config.model = session->model;
    if (!session->mode.isEmpty())
        config.mode = session->mode;
    if (!session->strength.isEmpty())
        config.strength = session->strength;
    AgentRunner *runner = acquireAgentRunner();
    if (!runner->attachCoreProcess(*session, config, key, meta, pid)) {
        requeueUnattachedCoreAgent(sessionId);
        return;
    }
    logSystem(QStringLiteral("Agent #%1: re-attached to its %2 process (pid %3) after the restart; "
                             "forkmesh-core kept writing its log.")
                  .arg(sessionId)
                  .arg(AgentRunner::providerDisplayName(session->provider))
                  .arg(pid));
    updateAgentStatusCell(sessionId);
    if (sessionId == m_selectedAgentSessionId)
        showAgentSession(sessionId);
}
