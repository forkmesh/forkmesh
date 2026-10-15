#include "ActionStore.h"
#include "DesktopPackageExtension.h"
#include "IssueStore.h"
#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlStats.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "RepoSecurity.h"
#include "RepoStatsStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QSet>
#include <QUrl>

#include <utility>

namespace statsctl = forkmesh::localcontrol::stats;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

class StatsControlState : public QObject
{
public:
    explicit StatsControlState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshStatsControlState"));
    }

    statsctl::RatchetConfirmations confirmations;
    QSet<QString> qualityInFlight;
    QSet<QString> ratchetInFlight;
    QJsonObject codemagic;
    QString codemagicToken;
    qint64 codemagicAtMs = 0;
    QList<QPair<int, int>> codemagicWaiting;
    QJsonObject appStore;
    qint64 appStoreAtMs = 0;
    QList<QPair<int, int>> appStoreWaiting;
};

StatsControlState *statsControlState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(QStringLiteral("forkmeshStatsControlState"),
                                                  Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<StatsControlState *>(found))
        return state;
    return new StatsControlState(window);
}

void fetchCodemagicUsage(QNetworkAccessManager *network, StatsControlState *state,
                         LocalControlServer *server, const QString &token, int connectionId,
                         int requestId)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!state->codemagic.isEmpty() && state->codemagicToken == token &&
        now - state->codemagicAtMs < statsctl::kCodemagicCacheMs) {
        server->respond(connectionId, requestId, state->codemagic);
        return;
    }
    state->codemagicWaiting.append({connectionId, requestId});
    if (state->codemagicWaiting.size() > 1)
        return;
    QNetworkRequest request(QUrl(QStringLiteral("https://api.codemagic.io/user")));
    request.setRawHeader("x-auth-token", token.toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(15000);
    QNetworkReply *reply = network->get(request);
    QPointer<StatsControlState> stateGuard(state);
    QPointer<LocalControlServer> guard(server);
    QObject::connect(reply, &QNetworkReply::finished, state, [reply, stateGuard, guard, token] {
        reply->deleteLater();
        if (!stateGuard)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const qint64 at = QDateTime::currentMSecsSinceEpoch();
        const QJsonObject usage =
            status >= 200 && status < 300
                ? statsctl::codemagicUsage(QJsonDocument::fromJson(reply->readAll()).object(), at)
                : QJsonObject();
        const QList<QPair<int, int>> waiting = std::exchange(stateGuard->codemagicWaiting, {});
        if (!usage.isEmpty()) {
            stateGuard->codemagic = usage;
            stateGuard->codemagicToken = token;
            stateGuard->codemagicAtMs = at;
        }
        if (!guard)
            return;
        for (const auto &[connection, request] : waiting) {
            if (usage.isEmpty())
                guard->respondError(connection, request, statsctl::codemagicError(status));
            else
                guard->respond(connection, request, usage);
        }
    });
}

void answerAppStoreWaiting(StatsControlState *state, LocalControlServer *server,
                           const QJsonObject &reply, const QString &error,
                           const QString &message)
{
    const QList<QPair<int, int>> waiting = std::exchange(state->appStoreWaiting, {});
    if (!server)
        return;
    for (const auto &[connection, request] : waiting) {
        if (reply.isEmpty())
            server->respondError(connection, request, error, message);
        else
            server->respond(connection, request, reply);
    }
}

void fetchAppStoreStatus(StatsControlState *state, LocalControlServer *server,
                         const QString &root, int connectionId, int requestId)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!state->appStore.isEmpty() && now - state->appStoreAtMs < statsctl::kAppStoreStatusCacheMs) {
        server->respond(connectionId, requestId, state->appStore);
        return;
    }
    state->appStoreWaiting.append({connectionId, requestId});
    if (state->appStoreWaiting.size() > 1)
        return;
    auto *process = new QProcess(state);
    process->setWorkingDirectory(root);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QMap<QString, QString> variables = ActionStore::variables();
    for (const QString &name : statsctl::appStoreReportVariables()) {
        const QString value = variables.value(name).trimmed();
        if (!value.isEmpty())
            environment.insert(name, value);
    }
    process->setProcessEnvironment(environment);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
    QPointer<StatsControlState> stateGuard(state);
    QPointer<LocalControlServer> guard(server);
    QObject::connect(
        process, &QProcess::finished, state,
        [process, stateGuard, guard](int exitCode, QProcess::ExitStatus status) {
            process->deleteLater();
            if (!stateGuard)
                return;
            const QByteArray output = process->readAllStandardOutput();
            const QByteArray errors = process->readAllStandardError();
            if (status != QProcess::NormalExit || exitCode != 0) {
                answerAppStoreWaiting(stateGuard, guard, {},
                                      QStringLiteral("app_store_report_failed"),
                                      statsctl::appStoreReportError(errors));
                return;
            }
            QString error;
            const QJsonObject reply = statsctl::appStoreStatusReply(
                output, QDateTime::currentMSecsSinceEpoch(), &error);
            if (!reply.isEmpty()) {
                stateGuard->appStore = reply;
                stateGuard->appStoreAtMs = QDateTime::currentMSecsSinceEpoch();
            }
            answerAppStoreWaiting(stateGuard, guard, reply, error,
                                  statsctl::appStoreReportError(errors));
        });
    QObject::connect(process, &QProcess::errorOccurred, state,
                     [process, stateGuard, guard](QProcess::ProcessError error) {
                         if (error != QProcess::FailedToStart)
                             return;
                         process->deleteLater();
                         if (stateGuard)
                             answerAppStoreWaiting(stateGuard, guard, {},
                                                   QStringLiteral("app_store_report_failed"),
                                                   QStringLiteral("python3 could not start."));
                     });
    timer->start(statsctl::kAppStoreStatusTimeoutMs);
    process->start(QStringLiteral("python3"),
                   {QStringLiteral("-u"), statsctl::appStoreReportScript(), QStringLiteral("--json")});
}

QString appStoreReportRoot(const QList<RepositoryRecord> &repositories)
{
    QString fallback;
    for (const RepositoryRecord &repo : repositories) {
        const QString root = repo.localPath.trimmed();
        if (root.isEmpty() ||
            !QFileInfo(QDir(root).filePath(statsctl::appStoreReportScript())).isFile())
            continue;
        if (repo.owner == QLatin1String("forkmesh") && repo.name == QLatin1String("forkmesh"))
            return root;
        if (fallback.isEmpty())
            fallback = root;
    }
    return fallback;
}

struct RepoFactsResult {
    statsctl::RepoFacts facts;
    QString error;
    bool changed = false;
};

RepoFactsResult readRepoFacts(const QString &dir)
{
    RepoFactsResult result;
    QString error;
    const QVector<RepoStatsSample> days = RepoStatsStore::captureDaily(dir, &error);
    if (!days.isEmpty()) {
        const RepoStatsSample &latest = days.last();
        result.facts.files = latest.files;
        result.facts.lines = latest.lines;
        result.facts.bytes = latest.bytes;
        result.facts.day = latest.day;
    }
    const RepoRatchetReading ratchet = RepoStatsStore::ratchetReading(dir);
    result.facts.ratchetEnabled = ratchet.enabled;
    result.facts.ceilingBytes = ratchet.ceilingBytes;
    result.facts.ceilingDay = ratchet.ceilingDay;
    QString count;
    if (forkmesh::ui::gitOutput(dir, {QStringLiteral("rev-list"), QStringLiteral("--count"),
                                      QStringLiteral("HEAD")},
                                &count))
        result.facts.commits = count.trimmed().toLongLong();
    if (days.isEmpty() && !error.isEmpty())
        result.error = QStringLiteral("stats_unavailable");
    return result;
}

}

QString MainWindow::localControlStatsRepoDir(int repoIndex) const
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return {};
    const RepositoryRecord &writable = writableRecordFor(m_repositories.at(repoIndex));
    const QString dir = writable.localPath;
    if (dir.isEmpty() || !QFileInfo::exists(QDir(dir).filePath(QStringLiteral(".git"))))
        return {};
    return dir;
}

void MainWindow::answerLocalControlStatsRepo(int connectionId, int requestId, int repoIndex)
{
    const QString dir = localControlStatsRepoDir(repoIndex);
    if (dir.isEmpty()) {
        if (m_localControl)
            m_localControl->respondError(connectionId, requestId, QStringLiteral("no_checkout"));
        return;
    }
    QPointer<LocalControlServer> guard(m_localControl);
    runOffThread<RepoFactsResult>([dir] { return readRepoFacts(dir); },
                                  [guard, connectionId, requestId](RepoFactsResult result) {
                                      if (!guard)
                                          return;
                                      if (!result.error.isEmpty()) {
                                          guard->respondError(connectionId, requestId,
                                                              result.error);
                                          return;
                                      }
                                      guard->respond(connectionId, requestId,
                                                     statsctl::repoStats(result.facts));
                                  });
}

void MainWindow::answerLocalControlStatsQuality(int connectionId, int requestId, int repoIndex)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size()) {
        if (m_localControl)
            m_localControl->respondError(connectionId, requestId, QStringLiteral("unknown_repo"));
        return;
    }
    const RepositoryRecord &selected = m_repositories.at(repoIndex);
    const RepositoryRecord &writable = writableRecordFor(selected);
    const QString key = selected.owner + QLatin1Char('/') + selected.name;
    StatsControlState *state = statsControlState(this);
    if (state->qualityInFlight.contains(key)) {
        if (m_localControl)
            m_localControl->respondError(connectionId, requestId, QStringLiteral("busy"));
        return;
    }
    RepoSecurityInput input;
    input.owner = selected.owner;
    input.name = selected.name;
    input.localPath = writable.localPath;
    input.mirrorPath = writable.mirrorPath.isEmpty() ? selected.mirrorPath : writable.mirrorPath;
    input.publishToNetwork = selected.publishToNetwork;
    input.isPrivate = selected.isPrivate;
    input.previewOnly = selected.previewOnly;
    input.actionsEnabled = selected.actionsEnabled;
    input.issues =
        IssueStore(writable.localPath, input.mirrorPath, &m_profileIdentity, m_userName).loadAll();
    input.workflows = availableWorkflowsForRepo(writable);
    for (const ActionRun &run : std::as_const(m_actionRuns))
        if (run.owner == selected.owner && run.name == selected.name)
            input.actionRuns.append(run);
    const QString root = input.localPath.isEmpty() ? input.mirrorPath : input.localPath;
    state->qualityInFlight.insert(key);
    QPointer<LocalControlServer> guard(m_localControl);
    QPointer<StatsControlState> stateGuard(state);
    runOffThread<QJsonObject>(
        [input, root] {
            const RepoSecuritySnapshot snapshot = RepoQuality::scan(input);
            QList<statsctl::QualityFinding> findings;
            for (const RepoSecurityFinding &finding : snapshot.findings)
                findings.append({statsctl::relativeFindingPath(finding.path, root),
                                 int(finding.severity), finding.title});
            QList<int> signalSeverities;
            for (const RepoSecuritySignal &signal : snapshot.signalList)
                signalSeverities.append(int(signal.severity));
            const qint64 at = snapshot.generatedAtMs > 0 ? snapshot.generatedAtMs
                                                         : QDateTime::currentMSecsSinceEpoch();
            return statsctl::qualityReport(findings, signalSeverities, at);
        },
        [guard, stateGuard, key, connectionId, requestId](QJsonObject report) {
            if (stateGuard)
                stateGuard->qualityInFlight.remove(key);
            if (guard)
                guard->respond(connectionId, requestId, report);
        });
}

void MainWindow::handleLocalControlStatsRequest(int connectionId, int requestId,
                                                const QString &method, const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    StatsControlState *state = statsControlState(this);
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto repoIndex = [this, &params](QString *slug) {
        QString owner;
        QString name;
        const QString text = params.value(QStringLiteral("repo")).toString().trimmed();
        if (!statsctl::parseRepoSlug(text, &owner, &name))
            return -2;
        if (slug)
            *slug = owner + QLatin1Char('/') + name;
        return repoIndexFor(owner, name);
    };
    const auto failRepo = [server, connectionId, requestId](int index) {
        server->respondError(connectionId, requestId,
                             index == -2 ? QStringLiteral("bad_request")
                                         : QStringLiteral("unknown_repo"));
    };

    if (method == QLatin1String("stats.timeline")) {
        server->respond(connectionId, requestId,
                        statsctl::timeline(m_networkLogDigest, now,
                                           statsctl::clampDays(params.value(QStringLiteral("days")))));
        return;
    }
    if (method == QLatin1String("stats.quality")) {
        const int index = repoIndex(nullptr);
        if (index < 0) {
            failRepo(index);
            return;
        }
        answerLocalControlStatsQuality(connectionId, requestId, index);
        return;
    }
    if (method == QLatin1String("stats.repo")) {
        const int index = repoIndex(nullptr);
        if (index < 0) {
            failRepo(index);
            return;
        }
        answerLocalControlStatsRepo(connectionId, requestId, index);
        return;
    }
    if (method == QLatin1String("stats.ratchet.prepare")) {
        QString slug;
        const int index = repoIndex(&slug);
        const QJsonValue enabled = params.value(QStringLiteral("enabled"));
        if (index < 0 || !enabled.isBool()) {
            failRepo(index < 0 ? index : -2);
            return;
        }
        if (localControlStatsRepoDir(index).isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("no_checkout"));
            return;
        }
        const statsctl::RatchetConfirmations::Issued issued =
            state->confirmations.issue(device.key, slug, enabled.toBool(), now);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), issued.token},
                         {QStringLiteral("action"), statsctl::ratchetAction(enabled.toBool())},
                         {QStringLiteral("summary"),
                          statsctl::ratchetSummary(slug, enabled.toBool())},
                         {QStringLiteral("expiresAt"), double(issued.expiresAtMs)},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }
    if (method == QLatin1String("stats.ratchet.run")) {
        QString slug;
        bool enabled = false;
        const auto verdict = state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), device.key, now, &slug, &enabled);
        if (verdict != hostctl::ConfirmationStore::Result::Ok) {
            const QString why = hostctl::confirmationError(verdict);
            logSystem(QStringLiteral("Refused a Ratchet Mode change from %1: %2.")
                          .arg(localControlDeviceLabel(connectionId), why));
            server->respondError(connectionId, requestId, why);
            return;
        }
        QString owner;
        QString name;
        statsctl::parseRepoSlug(slug, &owner, &name);
        const QString dir = localControlStatsRepoDir(repoIndexFor(owner, name));
        if (dir.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("no_checkout"));
            return;
        }
        if (state->ratchetInFlight.contains(dir)) {
            server->respondError(connectionId, requestId, QStringLiteral("busy"));
            return;
        }
        state->ratchetInFlight.insert(dir);
        const QString who = localControlDeviceLabel(connectionId);
        QPointer<LocalControlServer> guard(server);
        QPointer<StatsControlState> stateGuard(state);
        runOffThread<RepoFactsResult>(
            [dir, enabled] {
                QString error;
                if (!RepoStatsStore::setRatchetEnabled(dir, enabled, &error)) {
                    RepoFactsResult failed;
                    failed.error = QStringLiteral("ratchet_failed");
                    return failed;
                }
                RepoFactsResult result = readRepoFacts(dir);
                result.changed = true;
                return result;
            },
            [this, guard, stateGuard, dir, slug, who, enabled, connectionId,
             requestId](RepoFactsResult result) {
                if (stateGuard)
                    stateGuard->ratchetInFlight.remove(dir);
                if (result.changed)
                    logSystem(QStringLiteral("Ratchet Mode %1 for %2, requested by %3.")
                                  .arg(enabled ? QStringLiteral("enabled")
                                               : QStringLiteral("disabled"),
                                       slug, who));
                if (!guard)
                    return;
                if (!result.changed) {
                    guard->respondError(connectionId, requestId,
                                        result.error.isEmpty() ? QStringLiteral("ratchet_failed")
                                                               : result.error);
                    return;
                }
                guard->respond(connectionId, requestId, statsctl::repoStats(result.facts));
                guard->broadcast(QStringLiteral("stats.changed"),
                                 {{QStringLiteral("what"), QStringLiteral("ratchet")},
                                  {QStringLiteral("repo"), slug}});
            });
        return;
    }
    if (method == QLatin1String("stats.sentry")) {
        const auto found = m_desktopPackageExtensions.find(QStringLiteral("sentry"));
        if (found == m_desktopPackageExtensions.end() || !found->second) {
            server->respondError(connectionId, requestId, QStringLiteral("sentry_not_installed"));
            return;
        }
        const QJsonObject snapshot = found->second->remoteSnapshot();
        if (!snapshot.value(QStringLiteral("connected")).toBool()) {
            server->respondError(connectionId, requestId, QStringLiteral("sentry_not_connected"));
            return;
        }
        server->respond(connectionId, requestId,
                        {{QStringLiteral("issues"),
                          statsctl::sentryIssues(snapshot.value(QStringLiteral("issues")).toArray())},
                         {QStringLiteral("lastSyncMs"),
                          snapshot.value(QStringLiteral("lastSyncMs")).toDouble()}});
        return;
    }
    if (method == QLatin1String("stats.codemagic")) {
        const QString token =
            ActionStore::variables().value(statsctl::codemagicTokenVariable()).trimmed();
        if (token.isEmpty() || !m_networkAccess) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("codemagic_not_configured"));
            return;
        }
        fetchCodemagicUsage(m_networkAccess, state, server, token, connectionId, requestId);
        return;
    }
    if (method == QLatin1String("stats.appStoreStatus")) {
        const QString root = appStoreReportRoot(m_repositories);
        if (root.isEmpty()) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("app_store_report_missing"),
                                 QStringLiteral("This desktop has no ForkMesh checkout with the "
                                                "App Store report."));
            return;
        }
        fetchAppStoreStatus(state, server, root, connectionId, requestId);
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}
