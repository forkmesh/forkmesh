#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlPulls.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "PullStore.h"

#include <QDateTime>
#include <QJsonObject>

namespace pullsctl = forkmesh::localcontrol::pulls;
namespace hostctl = forkmesh::localcontrol::hosts;

void MainWindow::handleLocalControlPullsRequest(int connectionId, int requestId,
                                                const QString &method,
                                                const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const QString owner = params.value(QStringLiteral("owner")).toString().trimmed();
    const QString name = params.value(QStringLiteral("name")).toString().trimmed();
    const QString action = params.value(QStringLiteral("action")).toString();
    if (method != QLatin1String("pulls.prepare") && method != QLatin1String("pulls.run")) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_method"));
        return;
    }
    if (!pullsctl::validRequest(owner, name, action)) {
        server->respondError(connectionId, requestId, QStringLiteral("bad_request"));
        return;
    }
    const int index = repoIndexFor(owner, name);
    if (index < 0 || index >= m_repositories.size()) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_repo"));
        return;
    }
    const QString token = params.value(QStringLiteral("token")).toString();
    whenPullOwnershipKnown(index, [this, connectionId, requestId, method, owner, name,
                                   action, token] {
        answerLocalControlPullsRequest(connectionId, requestId, method, owner, name,
                                       action, token);
    });
}

void MainWindow::answerLocalControlPullsRequest(int connectionId, int requestId,
                                                const QString &method,
                                                const QString &owner,
                                                const QString &name,
                                                const QString &action,
                                                const QString &token)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const int index = repoIndexFor(owner, name);
    if (index < 0 || index >= m_repositories.size()) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_repo"));
        return;
    }
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const QString target = pullsctl::confirmationTarget(owner, name, action);

    if (method == QLatin1String("pulls.prepare")) {
        const QString blocker = pullMergeAllBlocker(index);
        if (!blocker.isEmpty()) {
            server->respondError(connectionId, requestId, blocker);
            return;
        }
        const QString issued = m_pullsConfirmations.issue(
            device.key, action, target, QDateTime::currentMSecsSinceEpoch());
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), issued},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("summary"),
                          pullsctl::actionSummary(owner, name, action)},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }

    const hostctl::ConfirmationStore::Result verdict = m_pullsConfirmations.consume(
        token, device.key, action, target, QDateTime::currentMSecsSinceEpoch());
    if (verdict != hostctl::ConfirmationStore::Result::Ok) {
        logSystem(QStringLiteral("Refused a pull request %1 on %2/%3 from %4: %5.")
                      .arg(action, owner, name, localControlDeviceLabel(connectionId),
                           hostctl::confirmationError(verdict)));
        server->respondError(connectionId, requestId, hostctl::confirmationError(verdict));
        return;
    }
    runLocalControlPullsMergeAll(connectionId, requestId, index);
}

void MainWindow::runLocalControlPullsMergeAll(int connectionId, int requestId, int index)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const QString blocker = pullMergeAllBlocker(index);
    if (!blocker.isEmpty()) {
        server->respondError(connectionId, requestId, blocker);
        return;
    }
    const QString owner = m_repositories.at(index).owner;
    const QString name = m_repositories.at(index).name;
    const QString device = localControlDeviceLabel(connectionId);
    const PullStore store = pullStoreForRepoIndex(index);
    m_pullMergeAllPreparing = true;
    runOffThread<QList<PullRequest>>(
        [store] { return openPullsToMergeAll(store.loadAll()); },
        [this, connectionId, requestId, owner, name, device](QList<PullRequest> open) {
            m_pullMergeAllPreparing = false;
            startPullMergeAll(
                owner, name, open, false,
                [this, connectionId, requestId, owner, name,
                 device](const forkmesh::pullmergeall::Launch &launch) {
                    logSystem(QStringLiteral("Merge all with AI on %1/%2 from %3: %4")
                                  .arg(owner, name, device,
                                       forkmesh::pullmergeall::launchMessage(launch)));
                    if (!m_localControl)
                        return;
                    if (!launch.error.isEmpty()) {
                        m_localControl->respondError(connectionId, requestId,
                                                     launch.error);
                        return;
                    }
                    m_localControl->respond(connectionId, requestId,
                                            pullsctl::launchReply(launch));
                });
        });
}
