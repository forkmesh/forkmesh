#include "CoreClient.h"
#include "CoreDiag.h"
#include "CoreVitals.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QJsonArray>
#include <QJsonObject>

void MainWindow::registerCoreDiagHandlers()
{
    if (!m_coreClient)
        return;
    m_coreClient->handle(
        QStringLiteral("ui.diag.state"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            int running = 0;
            int queued = 0;
            int total = 0;
            for (const AgentSession &session : std::as_const(m_agentSessions)) {
                ++total;
                if (isStoppableAgentSession(session.id))
                    ++running;
                else if (session.status.contains(QLatin1String("queue"), Qt::CaseInsensitive) ||
                         session.status.contains(QLatin1String("scheduled"), Qt::CaseInsensitive))
                    ++queued;
            }
            QJsonObject result{{QStringLiteral("desktop"), machineNodeName()},
                               {QStringLiteral("canSendStalls"), stallReportRepoIndex() >= 0},
                               {QStringLiteral("agents"),
                                QJsonObject{{QStringLiteral("running"), running},
                                            {QStringLiteral("queued"), queued},
                                            {QStringLiteral("total"), total}}}};
            if (params.value(QStringLiteral("roots")).toBool()) {
                QJsonArray roots;
                for (const AgentSession &session : std::as_const(m_agentSessions)) {
                    const qint64 pid = agentSessionProcessId(session.id);
                    if (pid <= 0)
                        continue;
                    QString title = session.issueTitle.trimmed();
                    if (title.isEmpty())
                        title = session.prompt.section(QLatin1Char('\n'), 0, 0).trimmed();
                    if (title.isEmpty())
                        title = QStringLiteral("Agent run #%1").arg(session.id);
                    roots.append(QJsonObject{{QStringLiteral("pid"), double(pid)},
                                             {QStringLiteral("session"), session.id},
                                             {QStringLiteral("label"), title.left(120)},
                                             {QStringLiteral("stoppable"),
                                              isStoppableAgentSession(session.id)}});
                }
                result.insert(QStringLiteral("roots"), roots);
            }
            respond(result, QString());
        });
    forkmesh::corediag::follow(m_coreClient, [this] { return m_stallLog; });
    forkmesh::corevitals::follow(m_coreClient);
}
