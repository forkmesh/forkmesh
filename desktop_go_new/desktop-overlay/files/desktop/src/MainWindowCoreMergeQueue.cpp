#include "CoreClient.h"
#include "CorePulls.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MergeQueue.h"
#include "PullStore.h"
#include "PullStoreCore.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace {

QStringList stringList(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &item : value.toArray())
        out.append(item.toString());
    return out;
}

} // namespace

bool MainWindow::coreMergeQueueAttached() const
{
    return !m_closingDown && m_coreClient && m_coreClient->isConnected() &&
           m_coreClient->hasNativeRoute(QStringLiteral("core.mergequeue.schedule"));
}

QJsonObject MainWindow::coreMergeQueueView() const
{
    QJsonArray repos;
    for (const RepositoryRecord &record : m_repositories) {
        const RepositoryRecord &writable = writableRecordFor(record);
        repos.append(QJsonObject{
            {QStringLiteral("owner"), record.owner},
            {QStringLiteral("name"), record.name},
            {QStringLiteral("enabled"), record.mergeQueueEnabled},
            {QStringLiteral("paused"), record.mergeQueuePaused},
            {QStringLiteral("rows"), QJsonArray::fromStringList(record.mergeQueue)},
            {QStringLiteral("requirePeerApproval"), record.requirePeerApproval},
            {QStringLiteral("previewOnly"), record.previewOnly},
            {QStringLiteral("activeRelay"), repositoryBelongsToActiveRelay(record)},
            {QStringLiteral("localPath"), writable.localPath},
            {QStringLiteral("mirrorPath"), writable.mirrorPath},
        });
    }
    const bool open = m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    const bool conflictMerge =
        open && PullStore::conflictSessionLive(
                    writableRecordFor(m_repositories.at(m_repoDetailIndex)).localPath);
    return QJsonObject{
        {QStringLiteral("identity"), m_profileIdentity.isValid()},
        {QStringLiteral("open"), open ? m_repoDetailIndex : -1},
        {QStringLiteral("busy"), m_mergeQueueBusy},
        {QStringLiteral("aiFix"), m_aiFix != nullptr},
        {QStringLiteral("deleting"), m_pullDeleteInProgress},
        {QStringLiteral("conflictMerge"), conflictMerge},
        {QStringLiteral("repos"), repos},
    };
}

bool MainWindow::handOffMergeQueueToCore(int delayMs)
{
    if (!coreMergeQueueAttached())
        return false;
    if (m_mergeQueueTimer)
        m_mergeQueueTimer->stop();
    m_coreClient->request(QStringLiteral("core.mergequeue.schedule"),
                          {{QStringLiteral("delayMs"), qMax(0, delayMs)}});
    return true;
}

void MainWindow::registerCoreMergeQueueHandlers()
{
    m_coreClient->handle(QStringLiteral("ui.mergequeue.view"),
                         [this](const QJsonObject &, const CoreClient::Responder &respond) {
                             respond(coreMergeQueueView(), QString());
                         });
    m_coreClient->handle(
        QStringLiteral("ui.mergequeue.apply"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const QString owner = params.value(QStringLiteral("owner")).toString();
            const QString name = params.value(QStringLiteral("name")).toString();
            const int index = repoIndexFor(owner, name);
            if (index < 0 || index >= m_repositories.size()) {
                respond({}, QStringLiteral("unknown_repo"));
                return;
            }
            if (params.contains(QStringLiteral("rows"))) {
                if (stringList(params.value(QStringLiteral("expect"))) !=
                    m_repositories.at(index).mergeQueue) {
                    respond({}, QStringLiteral("conflict"));
                    return;
                }
                saveMergeQueue(owner, name,
                               MergeQueue::fromRows(stringList(params.value(QStringLiteral("rows")))));
                respond({{QStringLiteral("ok"), true}}, QString());
                return;
            }
            const bool enableFlag = params.contains(QStringLiteral("enabled"));
            const bool on = enableFlag ? params.value(QStringLiteral("enabled")).toBool()
                                       : params.value(QStringLiteral("paused")).toBool();
            if (!enableFlag && !params.contains(QStringLiteral("paused"))) {
                respond({}, QStringLiteral("bad_request"));
                return;
            }
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
            respond({{QStringLiteral("ok"), true}}, QString());
        });
    forkmesh::corepulls::install();
    m_coreClient->handle(
        QStringLiteral("ui.pulls.rewrite"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const QString workTree = params.value(QStringLiteral("workTree")).toString();
            const QString mirror = params.value(QStringLiteral("mirror")).toString();
            const int number = params.value(QStringLiteral("number")).toInt();
            const QStringList paths = stringList(params.value(QStringLiteral("paths")));
            if (workTree.isEmpty() || !forkmesh::pullstorecore::validRewritePaths(number, paths)) {
                respond({}, QStringLiteral("bad_request"));
                return;
            }
            runOffThread<QString>(
                [workTree, mirror, number, paths] {
                    QString error;
                    if (forkmesh::pullstorecore::rewriteLedgerHistory(workTree, mirror, number, paths, &error))
                        return QString();
                    return error.isEmpty() ? QStringLiteral("rewrite_failed") : error;
                },
                [respond](QString error) {
                    respond(error.isEmpty() ? QJsonObject{{QStringLiteral("ok"), true}} : QJsonObject(), error);
                });
        });
    m_coreClient->handle(
        QStringLiteral("ui.mergequeue.purge"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const int index = repoIndexFor(params.value(QStringLiteral("owner")).toString(),
                                           params.value(QStringLiteral("name")).toString());
            const int number = params.value(QStringLiteral("number")).toInt();
            if (index < 0 || index >= m_repositories.size() || number <= 0) {
                respond({}, QStringLiteral("unknown_repo"));
                return;
            }
            const RepositoryRecord &writable = writableRecordFor(m_repositories.at(index));
            const PullStore store(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                                  m_userName);
            runOffThread<QString>(
                [store, number] {
                    QString error;
                    if (store.purgeMergedPullPayload(number, &error))
                        return QString();
                    return error.isEmpty() ? QStringLiteral("purge_failed") : error;
                },
                [respond](QString error) {
                    respond(error.isEmpty() ? QJsonObject{{QStringLiteral("ok"), true}}
                                            : QJsonObject(),
                            error);
                });
        });
    connect(m_coreClient, &CoreClient::connectedChanged, this, [this](bool connected) {
        if (!connected || !coreMergeQueueAttached())
            return;
        m_coreClient->request(QStringLiteral("core.mergequeue.view"), coreMergeQueueView());
        if (mergeQueueEnabledForOpenRepo() && !openRepoMergeQueue().isEmpty())
            handOffMergeQueueToCore(1500);
    });
    connect(m_coreClient, &CoreClient::coreEvent, this,
            [this](const QString &event, const QJsonObject &data) {
                if (event == QLatin1String("core.mergequeue.state")) {
                    refreshMergeQueuePanel();
                    return;
                }
                if (event != QLatin1String("core.mergequeue.merged"))
                    return;
                const int number = data.value(QStringLiteral("number")).toInt();
                const bool open =
                    m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size() &&
                    m_repositories.at(m_repoDetailIndex).owner ==
                        data.value(QStringLiteral("owner")).toString() &&
                    m_repositories.at(m_repoDetailIndex).name ==
                        data.value(QStringLiteral("name")).toString();
                if (!open) {
                    logSystem(QStringLiteral("Merged pull request #%1.").arg(number));
                    refreshMergeQueuePanel();
                    return;
                }
                PullRequest merged =
                    PullRequest::fromJson(data.value(QStringLiteral("pull")).toObject());
                for (const PullRequest &pr : std::as_const(m_currentPulls)) {
                    if (pr.number == number) {
                        merged = pr;
                        break;
                    }
                }
                setRepoDetailNotice(
                    QStringLiteral("Merge queue merged pull request #%1.").arg(number));
                afterPullMerged(merged);
            });
}
