#include "CoreClient.h"
#include "CoreGit.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>

#include <functional>
#include <utility>

using namespace forkmesh::ui;

namespace {

constexpr qint64 kCoreMirrorCallMs = 5000;
constexpr qint64 kCoreUnwireCallMs = 20000;

using MirrorFinished = std::function<void(int, int)>;

QHash<QString, MirrorFinished> &waitingMirrorPushes()
{
    static QHash<QString, MirrorFinished> waiting;
    return waiting;
}

QByteArray &lastSentMirrorRepos()
{
    static QByteArray digest;
    return digest;
}

QJsonObject mirrorRepoJson(const RepositoryRecord &repo)
{
    return QJsonObject{{QStringLiteral("owner"), repo.owner},
                       {QStringLiteral("name"), repo.name},
                       {QStringLiteral("localPath"), repo.localPath},
                       {QStringLiteral("mirrorPath"), repo.mirrorPath},
                       {QStringLiteral("previewOnly"), repo.previewOnly},
                       {QStringLiteral("private"), repo.isPrivate}};
}

QJsonArray mirrorRepoList(const QList<RepositoryRecord> &repositories)
{
    QJsonArray list;
    for (const RepositoryRecord &repo : repositories) {
        if (repo.owner.trimmed().isEmpty() || repo.name.trimmed().isEmpty())
            continue;
        list.append(mirrorRepoJson(repo));
    }
    return list;
}

bool callCoreMirror(const QString &method, const QJsonObject &params, qint64 timeoutMs,
                    QJsonObject *result, bool *sent)
{
    QString error;
    return forkmesh::coregit::callCore(method, params, timeoutMs, result, &error, sent);
}

void releaseWaitingMirrorPushes()
{
    const QHash<QString, MirrorFinished> waiting = std::exchange(waitingMirrorPushes(), {});
    for (const MirrorFinished &finished : waiting)
        if (finished)
            finished(0, 1);
}

}

bool MainWindow::coreMirrorPushAttached() const
{
    return m_coreClient && m_coreClient->isConnected() &&
           m_coreClient->hasNativeRoute(QStringLiteral("core.mirror.push"));
}

void MainWindow::syncCoreMirrorRepos()
{
    if (!coreMirrorPushAttached())
        return;
    const QJsonArray list = mirrorRepoList(m_repositories);
    const QByteArray encoded = QJsonDocument(list).toJson(QJsonDocument::Compact);
    if (encoded == lastSentMirrorRepos())
        return;
    lastSentMirrorRepos() = encoded;
    m_coreClient->request(QStringLiteral("core.mirror.repos"), {{QStringLiteral("repos"), list}},
                          [](bool ok, const QJsonValue &, const QString &) {
                              if (!ok)
                                  lastSentMirrorRepos().clear();
                          });
}

bool MainWindow::pushToSshMirrorRemotesThroughCore(int index, bool userInitiated,
                                                   const QString &releaseTag, QString *idleReason,
                                                   bool pruneRefs, int *targets)
{
    if (!coreMirrorPushAttached() || index < 0 || index >= m_repositories.size())
        return false;
    syncCoreMirrorRepos();
    const QJsonObject params{{QStringLiteral("repo"), mirrorRepoJson(m_repositories.at(index))},
                             {QStringLiteral("userInitiated"), userInitiated},
                             {QStringLiteral("releaseTag"), releaseTag},
                             {QStringLiteral("pruneRefs"), pruneRefs}};
    QJsonObject result;
    bool sent = false;
    if (!callCoreMirror(QStringLiteral("core.mirror.push"), params, kCoreMirrorCallMs, &result,
                        &sent)) {
        if (!sent)
            return false;
        if (idleReason)
            *idleReason = QStringLiteral("forkmesh-core did not answer the mirror push request");
        *targets = 0;
        return true;
    }
    if (idleReason)
        *idleReason = result.value(QStringLiteral("idleReason")).toString();
    *targets = result.value(QStringLiteral("targets")).toInt();
    return true;
}

bool MainWindow::pushRefsToManagedHostsThroughCore(int index, bool wakeIfUnchanged,
                                                   const QString &releaseTag,
                                                   const QStringList &releaseArtifactHashes,
                                                   std::function<void(int, int)> onFinished,
                                                   bool reconcileArtifacts, bool pruneRefs,
                                                   int *targets)
{
    if (!coreMirrorPushAttached())
        return false;
    if (index < 0 || index >= m_repositories.size())
        return false;
    syncCoreMirrorRepos();
    QString token;
    if (onFinished) {
        token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        waitingMirrorPushes().insert(token, onFinished);
    }
    const QJsonObject params{{QStringLiteral("repo"), mirrorRepoJson(m_repositories.at(index))},
                             {QStringLiteral("wakeIfUnchanged"), wakeIfUnchanged},
                             {QStringLiteral("releaseTag"), releaseTag},
                             {QStringLiteral("artifactHashes"),
                              QJsonArray::fromStringList(releaseArtifactHashes)},
                             {QStringLiteral("reconcileArtifacts"), reconcileArtifacts},
                             {QStringLiteral("pruneRefs"), pruneRefs},
                             {QStringLiteral("token"), token}};
    QJsonObject result;
    bool sent = false;
    if (!callCoreMirror(QStringLiteral("core.mirror.managed"), params, kCoreMirrorCallMs, &result,
                        &sent)) {
        if (!token.isEmpty())
            waitingMirrorPushes().remove(token);
        if (!sent)
            return false;
        logSystem(QStringLiteral(
            "Mirror: forkmesh-core did not answer the managed-host push request."));
        if (onFinished)
            QTimer::singleShot(0, this, [onFinished] { onFinished(0, 1); });
        *targets = 0;
        return true;
    }
    *targets = result.value(QStringLiteral("targets")).toInt();
    return true;
}

bool MainWindow::removeSshMirrorRemotesForNodeThroughCore(const QString &node, int *removed)
{
    if (!coreMirrorPushAttached())
        return false;
    const QJsonArray list = mirrorRepoList(m_repositories);
    QJsonObject result;
    bool sent = false;
    if (!callCoreMirror(QStringLiteral("core.mirror.unwire"),
                        {{QStringLiteral("node"), node}, {QStringLiteral("repos"), list}},
                        kCoreUnwireCallMs, &result, &sent)) {
        if (!sent)
            return false;
        *removed = 0;
        return true;
    }
    lastSentMirrorRepos() = QJsonDocument(list).toJson(QJsonDocument::Compact);
    *removed = result.value(QStringLiteral("removed")).toInt();
    return true;
}

void MainWindow::registerCoreMirrorPushHandlers()
{
    if (!m_coreClient)
        return;
    connect(m_coreClient, &CoreClient::connectedChanged, this, [this](bool connected) {
        lastSentMirrorRepos().clear();
        if (!connected) {
            releaseWaitingMirrorPushes();
            return;
        }
        syncCoreMirrorRepos();
    });
    connect(m_coreClient, &CoreClient::coreEvent, this,
            [this](const QString &event, const QJsonObject &data) {
                if (event == QLatin1String("core.mirror.finished")) {
                    const MirrorFinished finished = waitingMirrorPushes().take(
                        data.value(QStringLiteral("token")).toString());
                    if (finished)
                        finished(data.value(QStringLiteral("succeeded")).toInt(),
                                 data.value(QStringLiteral("failed")).toInt());
                    return;
                }
                if (event == QLatin1String("core.mirror.unwired")) {
                    if (m_repoRemotesTable)
                        reloadRepoRemotesTable();
                    return;
                }
                if (event == QLatin1String("core.hosts.variablesChanged"))
                    reloadVariablesList();
            });
}
