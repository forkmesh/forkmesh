#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlWorkflows.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "RepoWorkflow.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>

namespace flowctl = forkmesh::localcontrol::workflows;
namespace hostctl = forkmesh::localcontrol::hosts;
namespace rw = forkmesh::repoworkflows;

void MainWindow::handleLocalControlWorkflowsRequest(int connectionId, int requestId,
                                                    const QString &method,
                                                    const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const QString repo = params.value(QStringLiteral("repo")).toString().trimmed();
    const QString id = params.value(QStringLiteral("id")).toString().trimmed();

    if (method == QLatin1String("workflows.list")) {
        const QJsonObject payload = nativePackageRepositoryWorkflows();
        QJsonArray repos;
        for (const QJsonValue &value : payload.value(QStringLiteral("repositories")).toArray())
            repos.append(value.toObject().value(QStringLiteral("key")));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("desktop"), machineNodeName()},
                         {QStringLiteral("repos"), repos},
                         {QStringLiteral("workflows"),
                          flowctl::listRows(payload.value(QStringLiteral("workflows")).toArray())}});
        return;
    }
    if (method == QLatin1String("workflows.get")) {
        if (!flowctl::validRepo(repo) || !flowctl::validId(id)) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_request"));
            return;
        }
        QPointer<LocalControlServer> guard(server);
        runNativePackageRepositoryWorkflowAction(
            repo, id, QStringLiteral("get"), {},
            [guard, connectionId, requestId](bool ok, const QJsonObject &data, const QString &message) {
                if (!guard)
                    return;
                if (!ok) {
                    guard->respondError(connectionId, requestId, QStringLiteral("unknown_workflow"),
                                        message);
                    return;
                }
                guard->respond(connectionId, requestId, flowctl::detail(data));
            });
        return;
    }
    if (method == QLatin1String("workflows.prepare")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = flowctl::subjectFor(action, params, &error);
        if (subject.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        const int repoIndex = repoIndexForKey(repo);
        if (repoIndex < 0) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_repo"));
            return;
        }
        rw::Workflow workflow;
        if (!findRepoWorkflow(repoIndex, id, &workflow)) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_workflow"));
            return;
        }
        const QString token = m_workflowControlConfirmations.issue(
            device.key, action, subject, QDateTime::currentMSecsSinceEpoch());
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("summary"), flowctl::actionSummary(action, workflow.name, repo)},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }
    if (method == QLatin1String("workflows.run")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = flowctl::subjectFor(action, params, &error);
        const hostctl::ConfirmationStore::Result verdict = m_workflowControlConfirmations.consume(
            params.value(QStringLiteral("token")).toString(), device.key, action, subject,
            QDateTime::currentMSecsSinceEpoch());
        const QString who = localControlDeviceLabel(connectionId);
        if (subject.isEmpty() || verdict != hostctl::ConfirmationStore::Result::Ok) {
            const QString why = subject.isEmpty() ? error : hostctl::confirmationError(verdict);
            logSystem(QStringLiteral("Refused a workflow %1 from %2: %3.").arg(action, who, why));
            server->respondError(connectionId, requestId, why);
            return;
        }
        if (action == QLatin1String("run") && repo != rw::kAccountRepo) {
            const int repoIndex = repoIndexForKey(repo);
            const QString path = repoWorkflowPath(repoIndex, id);
            if (repoIndex < 0 || path.isEmpty()) {
                logSystem(QStringLiteral("Refused a workflow run from %1: %2 has no workflow file on this desktop.")
                              .arg(who, subject));
                server->respondError(connectionId, requestId, QStringLiteral("unknown_workflow"));
                return;
            }
            approveRepoWorkflowFileFromPhone(repoIndex, path, QStringLiteral("Workflow %1").arg(subject), who);
        }
        QJsonObject payload;
        if (action == QLatin1String("run")) {
            payload.insert(QStringLiteral("trigger"), QStringLiteral("By hand · %1").arg(who).left(160));
            const QString assignment = params.value(QStringLiteral("assignment")).toString().trimmed();
            if (!assignment.isEmpty())
                payload.insert(QStringLiteral("assignment"), assignment.left(flowctl::kMaxAssignmentChars));
        }
        QPointer<LocalControlServer> guard(server);
        runNativePackageRepositoryWorkflowAction(
            repo, id, action, payload,
            [this, guard, connectionId, requestId, action, subject, who](bool ok, const QJsonObject &data,
                                                                         const QString &message) {
                if (!guard)
                    return;
                if (!ok) {
                    guard->respondError(connectionId, requestId,
                                        action == QLatin1String("stop") ? QStringLiteral("not_running")
                                                                        : QStringLiteral("not_started"),
                                        message);
                    return;
                }
                logSystem(QStringLiteral("Workflow %1 (%2) from %3.").arg(action, subject, who));
                guard->respond(connectionId, requestId,
                               {{QStringLiteral("started"), true},
                                {QStringLiteral("action"), action},
                                {QStringLiteral("subject"), subject},
                                {QStringLiteral("runId"), data.value(QStringLiteral("runId")).toString()},
                                {QStringLiteral("sessionId"), data.value(QStringLiteral("sessionId")).toInt()},
                                {QStringLiteral("message"), message}});
                guard->broadcast(QStringLiteral("workflows.changed"));
            });
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}
