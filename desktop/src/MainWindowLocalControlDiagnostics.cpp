// The "diag." local control family (WS16): the desktop's logs, health and stall records.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.
// Gate 2 answers "unsupported" for every method; WS16 fills it in.

#include "ActionStore.h"
#include "AppPaths.h"
#include "CoreAgentModel.h"
#include "DesktopPowerAlert.h"
#include "LocalControl.h"
#include "LocalBackupStore.h"
#include "LocalControlDiagnostics.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SystemPressure.h"
#include "SystemStats.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSharedPointer>
#include <QTimer>

#include <utility>

namespace diagctl = forkmesh::localcontrol::diag;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

constexpr int kMaxProcessRows = 30;
constexpr int kProcessQueryTimeoutMs = 4000;
constexpr int kMaxStallChars = 64000;
constexpr int kBuildMacProbeTimeoutMs = 12000;
constexpr qint64 kBuildMacCacheMs = 20000;

struct BuildMacWaiter {
    QPointer<LocalControlServer> server;
    int connectionId = 0;
    int requestId = 0;
};

QList<BuildMacWaiter> &buildMacWaiters()
{
    static QList<BuildMacWaiter> waiters;
    return waiters;
}

QJsonObject &buildMacLast()
{
    static QJsonObject last;
    return last;
}

void answerBuildMac(QObject *context, LocalControlServer *server, int connectionId, int requestId)
{
    const QString dir = forkmesh::paths::sshDir() + QStringLiteral("/mac-build");
    const QString key = dir + QStringLiteral("/id_ed25519");
    const QString knownHosts = dir + QStringLiteral("/known_hosts");
    if (!QFileInfo::exists(key) || !QFileInfo::exists(knownHosts)) {
        server->respond(connectionId, requestId, {{QStringLiteral("configured"), false}});
        return;
    }
    const QJsonObject &last = buildMacLast();
    if (!last.isEmpty() &&
        QDateTime::currentMSecsSinceEpoch() - qint64(last.value(QStringLiteral("checkedAtMs")).toDouble()) <
            kBuildMacCacheMs) {
        server->respond(connectionId, requestId, last);
        return;
    }
    QList<BuildMacWaiter> &waiters = buildMacWaiters();
    waiters.append({QPointer<LocalControlServer>(server), connectionId, requestId});
    if (waiters.size() > 1)
        return;
    QString host = ActionStore::variables().value(QStringLiteral("MAC_BUILD_HOST")).trimmed();
    if (host.isEmpty()) {
        server->respond(connectionId, requestId, {{QStringLiteral("configured"), false}});
        return;
    }
    auto *probe = new QProcess(context);
    auto answered = QSharedPointer<bool>::create(false);
    auto finish = [probe, answered](bool reachable) {
        if (*answered)
            return;
        *answered = true;
        const QJsonObject status = diagctl::buildMacStatus(
            reachable, reachable ? probe->readAllStandardOutput() : QByteArray(),
            QDateTime::currentMSecsSinceEpoch());
        buildMacLast() = status;
        const QList<BuildMacWaiter> waiting = std::exchange(buildMacWaiters(), {});
        for (const BuildMacWaiter &waiter : waiting)
            if (waiter.server)
                waiter.server->respond(waiter.connectionId, waiter.requestId, status);
        probe->deleteLater();
    };
    QObject::connect(probe, &QProcess::finished, context,
                     [finish](int exitCode, QProcess::ExitStatus exitStatus) {
                         finish(exitStatus == QProcess::NormalExit && exitCode == 0);
                     });
    QObject::connect(probe, &QProcess::errorOccurred, context,
                     [finish](QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart)
                             finish(false);
                     });
    probe->setStandardInputFile(QProcess::nullDevice());
    probe->start(QStringLiteral("ssh"),
                 {QStringLiteral("-i"), key, QStringLiteral("-o"), QStringLiteral("IdentitiesOnly=yes"),
                  QStringLiteral("-o"), QStringLiteral("BatchMode=yes"), QStringLiteral("-o"),
                  QStringLiteral("ConnectTimeout=5"), QStringLiteral("-o"),
                  QStringLiteral("StrictHostKeyChecking=yes"), QStringLiteral("-o"),
                  QStringLiteral("UserKnownHostsFile=") + knownHosts, QStringLiteral("-o"),
                  QStringLiteral("HostKeyAlias=forkmesh-mac"),
                  QStringLiteral("build@") + host, diagctl::buildMacProbeScript()});
    QTimer::singleShot(kBuildMacProbeTimeoutMs, probe, [probe, finish] {
        if (probe->state() == QProcess::NotRunning)
            return;
        probe->kill();
        finish(false);
    });
}

}

QJsonObject MainWindow::localControlHealthSummary()
{
    const forkmesh::SystemPressureSnapshot pressure = forkmesh::cachedSystemPressure(2000);
    diagctl::HealthInput input;
    input.pressureValid = pressure.valid;
    input.memoryFullAvg10 = pressure.memoryFullAvg10;
    input.memorySomeAvg10 = pressure.memorySomeAvg10;
    input.ioFullAvg10 = pressure.ioFullAvg10;
    input.cpuSomeAvg10 = pressure.cpuSomeAvg10;
    input.loadAverage1 = pressure.loadAverage1;
    input.cpuCount = pressure.cpuCount;
    input.cpuPercent = SystemStats::hostCpuPercent();
    input.memoryTotal = SystemStats::totalMemoryBytes();
    input.memoryAvailable = SystemStats::availableMemoryBytes();
    input.swapTotal = SystemStats::totalSwapBytes();
    input.swapFree = SystemStats::freeSwapBytes();
    input.selfResident = SystemStats::residentBytes();
    const QString dataRoot = forkmesh::paths::dataRoot();
    input.diskTotal = SystemStats::diskTotalBytes(dataRoot);
    input.diskFree = SystemStats::diskFreeBytes(dataRoot);
    input.stalls = int(m_stallLog.size());
    input.openFiles = SystemStats::openFileCount();
    input.openFileLimit = SystemStats::openFileSoftLimit();
    input.threads = SystemStats::threadCount();
    const forkmesh::poweralert::Reading power = currentPowerReading();
    input.powerKnown = power.source != forkmesh::poweralert::Source::Unknown;
    input.onBattery = power.source == forkmesh::poweralert::Source::Battery;
    input.batteryPercent = power.percent;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        ++input.agentsTotal;
        if (isStoppableAgentSession(session.id))
            ++input.agentsRunning;
        else if (session.status.contains(QLatin1String("queue"), Qt::CaseInsensitive) ||
                 session.status.contains(QLatin1String("scheduled"), Qt::CaseInsensitive))
            ++input.agentsQueued;
    }
    QJsonObject summary = diagctl::healthSummary(input);
    summary.insert(QStringLiteral("desktop"), machineNodeName());
    summary.insert(QStringLiteral("sampledAtMs"), double(QDateTime::currentMSecsSinceEpoch()));
    summary.insert(QStringLiteral("pressureLine"), forkmesh::describeSystemPressure(pressure));
    return summary;
}

void MainWindow::sampleHostVitals()
{
    if (m_localControl && m_localControl->coreAttached()) {
        m_hostVitalsWatchers.clear();
        return;
    }
    diagctl::VitalsSample sample;
    sample.memory = diagctl::usedPercent(SystemStats::totalMemoryBytes(),
                                         SystemStats::availableMemoryBytes());
    sample.cpu = SystemStats::hostCpuPercent();
    const QString dataRoot = forkmesh::paths::dataRoot();
    sample.disk = diagctl::usedPercent(SystemStats::diskTotalBytes(dataRoot),
                                       SystemStats::diskFreeBytes(dataRoot));
    sample.swap = diagctl::usedPercent(SystemStats::totalSwapBytes(), SystemStats::freeSwapBytes());
    diagctl::appendVitals(m_hostVitals, sample);
    if (m_hostVitalsWatchers.isEmpty() || !m_localControl)
        return;
    const QJsonObject data = diagctl::vitalsSampleJson(sample);
    for (auto it = m_hostVitalsWatchers.begin(); it != m_hostVitalsWatchers.end();) {
        if (m_localControl->sendEventTo(*it, QStringLiteral("diag.vitals"), data))
            ++it;
        else
            it = m_hostVitalsWatchers.erase(it);
    }
}

QJsonObject MainWindow::localControlBackupStatus() const
{
    diagctl::BackupStatusInput input;
    input.enabled = autoBackupEnabled();
    input.keep = backupKeepCount();
    input.running = m_backupProcess != nullptr;
    const QString root = backupRoot();
    input.location = diagctl::backupLocationLabel(root, forkmesh::paths::dataRoot());
    for (const forkmesh::BackupSnapshot &snapshot : forkmesh::listBackups(root)) {
        if (input.snapshots.size() >= diagctl::kMaxBackupSnapshots)
            break;
        input.snapshots.append({snapshot.taken.toMSecsSinceEpoch(), snapshot.bytes});
    }
    input.failure = m_lastBackupFailure;
    input.failureAtMs = m_lastBackupResultMs;
    input.intervalMs = forkmesh::kBackupIntervalMs;
    input.nowMs = QDateTime::currentMSecsSinceEpoch();
    return diagctl::backupStatus(input);
}

void MainWindow::noteBackupResult(const QString &failure)
{
    m_lastBackupFailure = failure;
    m_lastBackupResultMs = QDateTime::currentMSecsSinceEpoch();
}

void MainWindow::answerLocalControlProcesses(int connectionId, int requestId)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    QPointer<LocalControlServer> guard(server);
    auto *query = new QProcess(this);
    auto answered = QSharedPointer<bool>::create(false);
    connect(query, &QProcess::finished, this,
            [this, guard, query, connectionId, requestId, answered](int exitCode,
                                                                    QProcess::ExitStatus status) {
                query->deleteLater();
                if (*answered || !guard)
                    return;
                *answered = true;
                if (status != QProcess::NormalExit || exitCode != 0) {
                    guard->respondError(connectionId, requestId, QStringLiteral("ps_failed"));
                    return;
                }
                QHash<qint64, qint64> parents;
                const QList<diagctl::ProcessRow> rows = diagctl::parseProcessTable(
                    query->readAllStandardOutput(), kMaxProcessRows, &parents);
                struct Owner {
                    int session = 0;
                    bool stoppable = false;
                    QString label;
                };
                QHash<qint64, Owner> roots;
                for (const AgentSession &session : std::as_const(m_agentSessions)) {
                    const qint64 pid = agentSessionProcessId(session.id);
                    if (pid <= 0)
                        continue;
                    QString title = session.issueTitle.trimmed();
                    if (title.isEmpty())
                        title = session.prompt.section(QLatin1Char('\n'), 0, 0).trimmed();
                    if (title.isEmpty())
                        title = QStringLiteral("Agent run #%1").arg(session.id);
                    roots.insert(pid, Owner{session.id, isStoppableAgentSession(session.id),
                                            title.left(120)});
                }
                QJsonArray out;
                for (const diagctl::ProcessRow &row : rows) {
                    QJsonObject item{{QStringLiteral("pid"), double(row.pid)},
                                     {QStringLiteral("name"), row.name},
                                     {QStringLiteral("user"), row.user},
                                     {QStringLiteral("rssBytes"), double(row.rssKb) * 1024.0},
                                     {QStringLiteral("percent"), row.percent},
                                     {QStringLiteral("command"), diagctl::commandSummary(row.command)}};
                    qint64 pid = row.pid;
                    for (int hops = 0; pid > 1 && hops < 64; ++hops) {
                        const auto found = roots.constFind(pid);
                        if (found != roots.constEnd()) {
                            item.insert(QStringLiteral("agent"),
                                        QJsonObject{{QStringLiteral("session"), found->session},
                                                    {QStringLiteral("label"), found->label},
                                                    {QStringLiteral("stoppable"), found->stoppable}});
                            break;
                        }
                        const qint64 parent = parents.value(pid, 0);
                        if (parent <= 0 || parent == pid)
                            break;
                        pid = parent;
                    }
                    out.append(item);
                }
                guard->respond(connectionId, requestId,
                               {{QStringLiteral("processes"), out},
                                {QStringLiteral("memoryTotal"),
                                 double(SystemStats::totalMemoryBytes())}});
            });
    connect(query, &QProcess::errorOccurred, this,
            [guard, query, connectionId, requestId, answered](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || *answered)
                    return;
                *answered = true;
                query->deleteLater();
                if (guard)
                    guard->respondError(connectionId, requestId, QStringLiteral("ps_unavailable"));
            });
    query->start(QStringLiteral("ps"), {QStringLiteral("-eo"),
                                        QStringLiteral("pid=,ppid=,user=,rss=,%mem=,args="),
                                        QStringLiteral("--sort=-rss")});
    QTimer::singleShot(kProcessQueryTimeoutMs, query, [guard, query, connectionId, requestId,
                                                       answered] {
        if (*answered || query->state() == QProcess::NotRunning)
            return;
        *answered = true;
        query->kill();
        if (guard)
            guard->respondError(connectionId, requestId, QStringLiteral("timeout"));
    });
}

void MainWindow::handleLocalControlDiagnosticsRequest(int connectionId, int requestId,
                                                      const QString &method,
                                                      const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);

    if (method == QLatin1String("diag.health")) {
        server->respond(connectionId, requestId, localControlHealthSummary());
        return;
    }
    if (method == QLatin1String("diag.watch")) {
        m_hostVitalsWatchers.insert(connectionId);
        server->respond(connectionId, requestId, diagctl::vitalsHistoryJson(m_hostVitals));
        return;
    }
    if (method == QLatin1String("diag.unwatch")) {
        m_hostVitalsWatchers.remove(connectionId);
        server->respond(connectionId, requestId, {});
        return;
    }
    if (method == QLatin1String("diag.backups")) {
        server->respond(connectionId, requestId, localControlBackupStatus());
        return;
    }
    if (method == QLatin1String("diag.buildMac")) {
        answerBuildMac(this, server, connectionId, requestId);
        return;
    }
    if (method == QLatin1String("diag.log")) {
        const qint64 before = params.contains(QStringLiteral("before"))
                                  ? qint64(params.value(QStringLiteral("before")).toDouble(-1))
                                  : -1;
        const int lines = params.value(QStringLiteral("lines")).toInt(200);
        const diagctl::LogPage page = diagctl::logPage(
            networkLogPath(), before, lines,
            params.value(QStringLiteral("query")).toString(),
            params.value(QStringLiteral("severity")).toString());
        server->respond(connectionId, requestId,
                        {{QStringLiteral("lines"), page.lines},
                         {QStringLiteral("start"), double(page.start)},
                         {QStringLiteral("end"), double(page.end)},
                         {QStringLiteral("more"), page.more},
                         {QStringLiteral("scanned"), page.scanned}});
        return;
    }
    if (method == QLatin1String("diag.stalls")) {
        QJsonArray rows;
        for (int i = int(m_stallLog.size()) - 1; i >= 0; --i)
            rows.append(diagctl::stallRow(i, m_stallLog.at(i)));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("stalls"), rows},
                         {QStringLiteral("canSend"), !m_stallLog.isEmpty() &&
                                                         stallReportRepoIndex() >= 0}});
        return;
    }
    if (method == QLatin1String("diag.stall")) {
        const int index = params.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= m_stallLog.size()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_stall"));
            return;
        }
        server->respond(connectionId, requestId,
                        diagctl::stallDetail(index, m_stallLog.at(index), kMaxStallChars));
        return;
    }
    if (method == QLatin1String("diag.processes")) {
        answerLocalControlProcesses(connectionId, requestId);
        return;
    }
    if (method == QLatin1String("diag.prepare")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = diagctl::subjectFor(action, params, &error);
        if (subject.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        QString label;
        if (action == QLatin1String("stopAgent")) {
            const int session = subject.toInt();
            if (!isStoppableAgentSession(session)) {
                server->respondError(connectionId, requestId, QStringLiteral("not_running"));
                return;
            }
            for (const AgentSession &item : std::as_const(m_agentSessions))
                if (item.id == session)
                    label = item.issueTitle.trimmed().isEmpty()
                                ? item.prompt.section(QLatin1Char('\n'), 0, 0).trimmed().left(120)
                                : item.issueTitle.trimmed().left(120);
        } else {
            if (m_stallLog.isEmpty() || stallReportRepoIndex() < 0) {
                server->respondError(connectionId, requestId, QStringLiteral("nothing_to_send"));
                return;
            }
            if (action == QLatin1String("sendStall")) {
                const int index = subject.toInt();
                if (index >= m_stallLog.size()) {
                    server->respondError(connectionId, requestId, QStringLiteral("unknown_stall"));
                    return;
                }
                label = diagctl::stallRow(index, m_stallLog.at(index))
                            .value(QStringLiteral("when"))
                            .toString();
            }
        }
        const QString token = m_diagControlConfirmations.issue(
            device.key, action, subject, QDateTime::currentMSecsSinceEpoch());
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("summary"), diagctl::actionSummary(action, subject, label)},
                         {QStringLiteral("expiresInMs"),
                          double(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }
    if (method == QLatin1String("diag.run")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = diagctl::subjectFor(action, params, &error);
        const hostctl::ConfirmationStore::Result verdict =
            forkmesh::coreagentmodel::confirmedByCore(connectionId, params)
                ? hostctl::ConfirmationStore::Result::Ok
                : m_diagControlConfirmations.consume(
                      params.value(QStringLiteral("token")).toString(), device.key, action,
                      subject, QDateTime::currentMSecsSinceEpoch());
        if (subject.isEmpty() || verdict != hostctl::ConfirmationStore::Result::Ok) {
            const QString why = subject.isEmpty() ? error : hostctl::confirmationError(verdict);
            logSystem(QStringLiteral("Refused a diagnostics %1 from %2: %3.")
                          .arg(action, localControlDeviceLabel(connectionId), why));
            server->respondError(connectionId, requestId, why);
            return;
        }
        runLocalControlDiagnosticsAction(connectionId, requestId, action, subject);
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

void MainWindow::runLocalControlDiagnosticsAction(int connectionId, int requestId,
                                                  const QString &action, const QString &subject)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const QString who = localControlDeviceLabel(connectionId);
    if (action == QLatin1String("stopAgent")) {
        const int session = subject.toInt();
        if (!isStoppableAgentSession(session) || !stopAgentSessionById(session)) {
            server->respondError(connectionId, requestId, QStringLiteral("not_running"));
            return;
        }
        logSystem(QStringLiteral("Agent run #%1 stopped from the memory panel by %2.")
                      .arg(session)
                      .arg(who));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("started"), true},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("message"), QStringLiteral("Stop requested.")}});
        server->broadcast(QStringLiteral("diag.changed"));
        return;
    }
    const int repoIndex = stallReportRepoIndex();
    if (m_stallLog.isEmpty() || repoIndex < 0) {
        server->respondError(connectionId, requestId, QStringLiteral("nothing_to_send"));
        return;
    }
    if (action == QLatin1String("sendStalls")) {
        if (!sendStallLogToAgent()) {
            server->respondError(connectionId, requestId, QStringLiteral("agent_not_started"));
            return;
        }
    } else {
        const int index = subject.toInt();
        if (index < 0 || index >= m_stallLog.size()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_stall"));
            return;
        }
        const QString prompt =
            QStringLiteral("ForkMesh's GUI thread stalled: the event loop was blocked, which "
                           "makes the window freeze. Find the blocking call in the backtrace "
                           "below and fix it so the UI stays responsive (move the slow work off "
                           "the main thread, or skip it when nothing changed).\n\n%1\nRecorded "
                           "stall:\n\n%2")
                .arg(stallLogLocationsBlock(), m_stallLog.at(index).left(kMaxStallChars / 4));
        if (startAdHocAgentForRepo(repoIndex, prompt, forkmesh::ui::defaultAgentProvider(), false) <= 0) {
            server->respondError(connectionId, requestId, QStringLiteral("agent_not_started"));
            return;
        }
    }
    logSystem(QStringLiteral("Stall fix agent started from %1.").arg(who));
    server->respond(connectionId, requestId,
                    {{QStringLiteral("started"), true},
                     {QStringLiteral("action"), action},
                     {QStringLiteral("subject"), subject},
                     {QStringLiteral("message"), QStringLiteral("An agent is working on it.")}});
    server->broadcast(QStringLiteral("agents.changed"));
}
