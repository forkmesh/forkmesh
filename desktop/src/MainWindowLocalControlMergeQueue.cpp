// The "mergequeue." local control family (WS11): the merge queue and code review actions that run on this desktop.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.

#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlMergeQueue.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "PullStore.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

namespace mqctl = forkmesh::localcontrol::mergequeue;
namespace hostctl = forkmesh::localcontrol::hosts;

QJsonObject MainWindow::localControlMergeQueueRow(int index) const
{
    if (index < 0 || index >= m_repositories.size())
        return {};
    const RepositoryRecord &record = m_repositories.at(index);
    const RepositoryRecord &writable = writableRecordFor(record);
    mqctl::RepoState state;
    state.owner = record.owner;
    state.name = record.name;
    state.enabled = record.mergeQueueEnabled;
    state.paused = record.mergeQueuePaused;
    state.open = index == m_repoDetailIndex;
    state.writable = PullStore(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                               m_userName)
                         .canWrite();
    state.queue = MergeQueue::fromRows(record.mergeQueue);
    QHash<int, QString> titles;
    if (state.open) {
        for (const PullRequest &pr : m_currentPulls)
            titles.insert(pr.number, pr.title);
    }
    return mqctl::repoRow(state, titles);
}

void MainWindow::handleLocalControlMergeQueueRequest(int connectionId, int requestId,
                                                     const QString &method,
                                                     const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const QString owner = params.value(QStringLiteral("owner")).toString().trimmed();
    const QString name = params.value(QStringLiteral("name")).toString().trimmed();

    if (method == QLatin1String("mergequeue.list")) {
        QJsonArray repos;
        for (int i = 0; i < m_repositories.size() && repos.size() < mqctl::kMaxRepos; ++i) {
            const RepositoryRecord &record = m_repositories.at(i);
            if (!owner.isEmpty() || !name.isEmpty()) {
                if (record.owner.compare(owner, Qt::CaseInsensitive) != 0 ||
                    record.name.compare(name, Qt::CaseInsensitive) != 0)
                    continue;
            } else if (!record.mergeQueueEnabled && record.mergeQueue.isEmpty()) {
                continue;
            }
            repos.append(localControlMergeQueueRow(i));
        }
        server->respond(connectionId, requestId, {{QStringLiteral("repos"), repos}});
        return;
    }

    const QString action = params.value(QStringLiteral("action")).toString();
    const int number = params.value(QStringLiteral("number")).toInt();
    const int delta = params.value(QStringLiteral("delta")).toInt();
    if (method != QLatin1String("mergequeue.prepare") &&
        method != QLatin1String("mergequeue.run")) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_method"));
        return;
    }
    if (!mqctl::validRequest(owner, name, action, number, delta)) {
        server->respondError(connectionId, requestId, QStringLiteral("bad_request"));
        return;
    }
    const int index = repoIndexFor(owner, name);
    if (index < 0 || index >= m_repositories.size()) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_repo"));
        return;
    }
    const QString target = mqctl::confirmationTarget(owner, name, action, number, delta);

    if (method == QLatin1String("mergequeue.prepare")) {
        const QString token = m_mergeQueueConfirmations.issue(
            device.key, action, target, QDateTime::currentMSecsSinceEpoch());
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("summary"),
                          mqctl::actionSummary(owner, name, action, number, delta)},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }

    const hostctl::ConfirmationStore::Result verdict = m_mergeQueueConfirmations.consume(
        params.value(QStringLiteral("token")).toString(), device.key, action, target,
        QDateTime::currentMSecsSinceEpoch());
    if (verdict != hostctl::ConfirmationStore::Result::Ok) {
        logSystem(QStringLiteral("Refused a merge queue %1 on %2/%3 from %4: %5.")
                      .arg(action, owner, name, localControlDeviceLabel(connectionId),
                           hostctl::confirmationError(verdict)));
        server->respondError(connectionId, requestId, hostctl::confirmationError(verdict));
        return;
    }
    runLocalControlMergeQueueAction(connectionId, requestId, index, action, number, delta);
}

void MainWindow::runLocalControlMergeQueueAction(int connectionId, int requestId, int index,
                                                 const QString &action, int number,
                                                 int delta)
{
    LocalControlServer *server = m_localControl;
    if (!server || index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord record = m_repositories.at(index);
    const QJsonObject row = localControlMergeQueueRow(index);
    mqctl::RepoState state;
    state.owner = record.owner;
    state.name = record.name;
    state.enabled = record.mergeQueueEnabled;
    state.paused = record.mergeQueuePaused;
    state.open = index == m_repoDetailIndex;
    state.writable = row.value(QStringLiteral("writable")).toBool();
    state.queue = MergeQueue::fromRows(record.mergeQueue);
    QString pullStatus;
    if (state.open) {
        for (const PullRequest &pr : m_currentPulls)
            if (pr.number == number)
                pullStatus = pr.status;
    }
    QString error = mqctl::precheck(state, action, number, pullStatus);
    if (error.isEmpty() && action == QLatin1String("process") && m_mergeQueueBusy)
        error = QStringLiteral("busy");
    if (!error.isEmpty()) {
        server->respondError(connectionId, requestId, error);
        return;
    }

    if (action == QLatin1String("enable") || action == QLatin1String("disable") ||
        action == QLatin1String("pause") || action == QLatin1String("resume")) {
        const bool on = action == QLatin1String("enable") || action == QLatin1String("pause");
        const bool enableFlag =
            action == QLatin1String("enable") || action == QLatin1String("disable");
        if (index == m_repoDetailIndex) {
            if (enableFlag)
                setRepoMergeQueueEnabled(on);
            else
                setMergeQueuePaused(on);
        } else {
            if (enableFlag)
                m_repositories[index].mergeQueueEnabled = on;
            else
                m_repositories[index].mergeQueuePaused = on;
            saveRepositories();
        }
    } else if (action == QLatin1String("process")) {
        scheduleMergeQueueRun(0);
    } else {
        MergeQueue queue = state.queue;
        error = mqctl::applyToQueue(queue, action, number, delta,
                                    QDateTime::currentMSecsSinceEpoch());
        if (!error.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        saveMergeQueue(record.owner, record.name, queue);
        if (action == QLatin1String("enqueue") && index == m_repoDetailIndex)
            scheduleMergeQueueRun(1500);
    }

    const MergeQueue after = MergeQueue::fromRows(m_repositories.at(index).mergeQueue);
    logSystem(QStringLiteral("Merge queue %1 on %2/%3%4 from %5.")
                  .arg(action, record.owner, record.name,
                       number > 0 ? QStringLiteral(" #%1").arg(number) : QString(),
                       localControlDeviceLabel(connectionId)));
    server->respond(connectionId, requestId,
                    {{QStringLiteral("message"), mqctl::resultMessage(action, number, after)},
                     {QStringLiteral("queue"), localControlMergeQueueRow(index)}});
    server->sendEventTo(connectionId, QStringLiteral("mergequeue.changed"),
                        {{QStringLiteral("owner"), record.owner},
                         {QStringLiteral("name"), record.name}});
}
