// The "diag." local control family (WS16): the desktop's logs, health and stall records.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.
// Gate 2 answers "unsupported" for every method; WS16 fills it in.

#include "ActionStore.h"
#include "AppPaths.h"
#include "DesktopPowerAlert.h"
#include "DesktopProcesses.h"
#include "LocalControl.h"
#include "LocalBackupStore.h"
#include "LocalControlDiagnostics.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SystemPressure.h"
#include "SystemStats.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSharedPointer>
#include <QStandardPaths>
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
    QJsonObject data = diagctl::vitalsSampleJson(sample);
    qint64 tokensIn = 0;
    qint64 tokensOut = 0;
    QJsonObject agentTokens;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        const QPair<qint64, qint64> flow = m_sessionTokenFlow.value(session.id);
        const qint64 in = qMax<qint64>(flow.first, session.promptTokens);
        const qint64 out = qMax<qint64>(flow.second, session.completionTokens);
        tokensIn += in;
        tokensOut += out;
        if ((in > 0 || out > 0) && isStoppableAgentSession(session.id))
            agentTokens.insert(QString::number(session.id), QJsonArray{double(in), double(out)});
    }
    data.insert(QStringLiteral("tokensIn"), double(tokensIn));
    data.insert(QStringLiteral("tokensOut"), double(tokensOut));
    data.insert(QStringLiteral("agentTokens"), agentTokens);
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
    input.location = diagctl::backupLocationLabel(root, forkmesh::paths::dataDir());
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
                const qint64 selfPid = QCoreApplication::applicationPid();
                for (const diagctl::ProcessRow &row : rows) {
                    forkmesh::desktopprocs::Process described;
                    described.name = row.name;
                    described.command = row.command;
                    described.self = row.pid == selfPid;
                    described.forkmesh = described.self;
                    QJsonObject item{{QStringLiteral("description"),
                                      forkmesh::desktopprocs::descriptionFor(described)},
                                     {QStringLiteral("pid"), double(row.pid)},
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
    if (method == QLatin1String("diag.process") || method == QLatin1String("diag.processStop")) {
        answerLocalControlProcess(connectionId, requestId, method == QLatin1String("diag.processStop"),
                                  params);
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
        } else if (action == QLatin1String("freeDiskSpace")) {
            label = diagctl::freeDiskSpaceStepLabel(subject);
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
        const hostctl::ConfirmationStore::Result verdict = m_diagControlConfirmations.consume(
            params.value(QStringLiteral("token")).toString(), device.key, action, subject,
            QDateTime::currentMSecsSinceEpoch());
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

void MainWindow::answerLocalControlProcess(int connectionId, int requestId, bool stop,
                                           const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
#if !defined(Q_OS_LINUX)
    Q_UNUSED(stop);
    Q_UNUSED(params);
    server->respondError(connectionId, requestId, QStringLiteral("process_details_unavailable"));
#else
    const qint64 pid = diagctl::processPid(params);
    if (pid <= 0) {
        server->respondError(connectionId, requestId, QStringLiteral("bad_pid"));
        return;
    }
    forkmesh::desktopprocs::Sampler sampler;
    forkmesh::desktopprocs::Process process = sampler.readOne(pid);
    if (process.pid != pid) {
        server->respondError(connectionId, requestId, QStringLiteral("process_exited"));
        return;
    }
    const QString identity = QString::number(process.startTicks);
    const QString asked = params.value(QStringLiteral("identity")).toString();
    if (!asked.isEmpty() && asked != identity) {
        server->respondError(connectionId, requestId, QStringLiteral("process_changed"));
        return;
    }
    const QString deviceKey = server->deviceFor(connectionId).key;
    const QString subject = diagctl::processSubject(pid, identity);
    const bool stoppable = diagctl::processStoppable(pid, QCoreApplication::applicationPid());
    const auto issue = [this, deviceKey, subject] {
        return m_diagControlConfirmations.issue(deviceKey, QStringLiteral("stopProcess"), subject,
                                                QDateTime::currentMSecsSinceEpoch());
    };
    if (!stop) {
        process.command = diagctl::commandSummary(process.command, 32768);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("pid"), double(pid)},
                         {QStringLiteral("name"), process.name},
                         {QStringLiteral("identity"), identity},
                         {QStringLiteral("description"),
                          forkmesh::desktopprocs::descriptionFor(process)},
                         {QStringLiteral("details"),
                          diagctl::processDetailText(
                              forkmesh::desktopprocs::detailTextFor(process))},
                         {QStringLiteral("stoppable"), stoppable},
                         {QStringLiteral("token"), stoppable ? issue() : QString()}});
        return;
    }
    if (!stoppable) {
        server->respondError(connectionId, requestId, QStringLiteral("protected_process"));
        return;
    }
    const hostctl::ConfirmationStore::Result verdict = m_diagControlConfirmations.consume(
        params.value(QStringLiteral("token")).toString(), deviceKey, QStringLiteral("stopProcess"),
        subject, QDateTime::currentMSecsSinceEpoch());
    if (verdict != hostctl::ConfirmationStore::Result::Ok) {
        server->respondError(connectionId, requestId, hostctl::confirmationError(verdict));
        return;
    }
    const QString who = localControlDeviceLabel(connectionId);
    const QJsonObject askPassword{
        {QStringLiteral("passwordRequired"), true},
        {QStringLiteral("message"),
         QStringLiteral("Enter the computer's sudo password to stop this process.")}};
    const auto stopped = [this, server, connectionId, requestId, process, who] {
        logSystem(QStringLiteral("%1 (PID %2) stopped from the memory panel by %3.")
                      .arg(process.name)
                      .arg(process.pid)
                      .arg(who));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("stopped"), true},
                         {QStringLiteral("message"), QStringLiteral("Termination requested.")}});
        server->broadcast(QStringLiteral("diag.changed"));
    };
    QString password = params.value(QStringLiteral("password")).toString();
    if (password.isEmpty()) {
        QString failure;
        switch (SystemStats::requestTermination(pid, &failure)) {
        case SystemStats::Termination::Requested:
            stopped();
            return;
        case SystemStats::Termination::AlreadyExited:
            server->respondError(connectionId, requestId, QStringLiteral("process_exited"));
            return;
        case SystemStats::Termination::PermissionDenied: {
            QJsonObject reply = askPassword;
            reply.insert(QStringLiteral("token"), issue());
            server->respond(connectionId, requestId, reply);
            return;
        }
        case SystemStats::Termination::Unsupported:
            server->respondError(connectionId, requestId,
                                 QStringLiteral("process_details_unavailable"));
            return;
        case SystemStats::Termination::Failed:
            break;
        }
        server->respondError(connectionId, requestId, QStringLiteral("process_stop_failed"));
        return;
    }
    if (!diagctl::sudoPasswordUsable(password)) {
        password.fill(QChar::Null);
        server->respondError(connectionId, requestId, QStringLiteral("bad_password"));
        return;
    }
    const QString kill = QStandardPaths::findExecutable(QStringLiteral("kill"));
    QByteArray secret = password.toUtf8();
    secret.append('\n');
    password.fill(QChar::Null);
    QPointer<LocalControlServer> guard(server);
    auto *elevated = new QProcess(this);
    auto answered = QSharedPointer<bool>::create(false);
    const QString token = issue();
    const auto finish = [guard, elevated, answered, connectionId, requestId, askPassword, token,
                         stopped](bool ok) {
        if (*answered)
            return;
        *answered = true;
        elevated->deleteLater();
        if (!guard)
            return;
        if (ok) {
            stopped();
            return;
        }
        QJsonObject reply = askPassword;
        reply.insert(QStringLiteral("token"), token);
        guard->respond(connectionId, requestId, reply);
    };
    connect(elevated, &QProcess::finished, this,
            [finish](int exitCode, QProcess::ExitStatus status) {
                finish(status == QProcess::NormalExit && exitCode == 0);
            });
    connect(elevated, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false);
    });
    elevated->start(QStringLiteral("sudo"),
                    {QStringLiteral("-k"), QStringLiteral("-S"), QStringLiteral("-p"), QString(),
                     QStringLiteral("--"), kill.isEmpty() ? QStringLiteral("/bin/kill") : kill,
                     QStringLiteral("-TERM"), QString::number(pid)});
    elevated->write(secret);
    secret.fill('\0');
    elevated->closeWriteChannel();
    QTimer::singleShot(15000, elevated, [elevated, finish] {
        if (elevated->state() == QProcess::NotRunning)
            return;
        elevated->kill();
        finish(false);
    });
#endif
}

void MainWindow::runLocalControlDiagnosticsAction(int connectionId, int requestId,
                                                  const QString &action, const QString &subject)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const QString who = localControlDeviceLabel(connectionId);
    if (action == QLatin1String("freeDiskSpace")) {
        runLocalControlFreeDiskSpace(connectionId, requestId, subject, who);
        return;
    }
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
        if (const int openSessionId = openStallRepairSessionId(repoIndex);
            openSessionId > 0) {
            sendPromptToAgentSession(openSessionId, prompt);
            logSystem(QStringLiteral("Added a UI stall report from %1 to agent "
                                     "session #%2.")
                          .arg(who)
                          .arg(openSessionId));
            server->respond(connectionId, requestId,
                            {{QStringLiteral("started"), true},
                             {QStringLiteral("action"), action},
                             {QStringLiteral("subject"), subject},
                             {QStringLiteral("sessionId"), openSessionId},
                             {QStringLiteral("message"),
                              QStringLiteral("Added to the agent already "
                                             "working on UI stalls.")}});
            server->broadcast(QStringLiteral("agents.changed"));
            return;
        }
        const int sessionId = startAdHocAgentForRepo(
            repoIndex, prompt, forkmesh::ui::defaultAgentProvider(), false);
        if (sessionId > 0)
            rememberStallRepairSession(sessionId);
        if (sessionId <= 0) {
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

void MainWindow::runLocalControlFreeDiskSpace(int connectionId, int requestId,
                                              const QString &step, const QString &who)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const QString processName = QStringLiteral("forkmeshFreeDiskSpaceProcess");
    if (findChild<QProcess *>(processName, Qt::FindDirectChildrenOnly)) {
        server->respondError(connectionId, requestId, QStringLiteral("busy"),
                             QStringLiteral("Another disk cleanup step is still running on "
                                            "the desktop."));
        return;
    }
    QString root;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        const QString dir = writableRecordFor(repo).localPath.trimmed();
        if (dir.isEmpty() ||
            !QFileInfo(QDir(dir).filePath(diagctl::freeDiskSpaceScript())).isFile())
            continue;
        if (repo.owner == QLatin1String("forkmesh") && repo.name == QLatin1String("forkmesh")) {
            root = dir;
            break;
        }
        if (root.isEmpty())
            root = dir;
    }
    if (root.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("no_checkout"),
                             QStringLiteral("Add the forkmesh/forkmesh repository with a local "
                                            "checkout on this desktop: the cleanup runs its "
                                            "tools/free_disk_space.py."));
        return;
    }
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("no_python"),
                             QStringLiteral("python3 is not installed on the desktop."));
        return;
    }
    auto *process = new QProcess(this);
    process->setObjectName(processName);
    process->setWorkingDirectory(root);
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
    QPointer<LocalControlServer> guard(server);
    const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
    logSystem(QStringLiteral("Free disk space: %1, started from %2.")
                  .arg(diagctl::freeDiskSpaceStepLabel(step), who));
    connect(process, &QProcess::finished, this,
            [this, process, guard, connectionId, requestId, step,
             startedMs](int exitCode, QProcess::ExitStatus status) {
                process->setObjectName(QString());
                process->deleteLater();
                const QByteArray output = process->readAllStandardOutput();
                QString error;
                const QJsonObject reply =
                    status == QProcess::NormalExit && exitCode == 0
                        ? diagctl::freeDiskSpaceReply(
                              step, output, QDateTime::currentMSecsSinceEpoch() - startedMs,
                              &error)
                        : QJsonObject();
                if (reply.isEmpty()) {
                    const QString last =
                        QString::fromUtf8(output).trimmed().section(QLatin1Char('\n'), -1).trimmed();
                    const QString message =
                        last.isEmpty() ? QStringLiteral("The cleanup step stopped before it finished.")
                                       : last.left(diagctl::kFreeDiskSpaceMaxLineChars);
                    logSystem(QStringLiteral("Free disk space: %1 failed: %2")
                                  .arg(diagctl::freeDiskSpaceStepLabel(step), message));
                    if (guard)
                        guard->respondError(connectionId, requestId,
                                            QStringLiteral("cleanup_failed"), message);
                    return;
                }
                if (!guard)
                    return;
                guard->respond(connectionId, requestId, reply);
                guard->broadcast(QStringLiteral("diag.changed"));
            });
    connect(process, &QProcess::errorOccurred, this,
            [process, guard, connectionId, requestId](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->setObjectName(QString());
                process->deleteLater();
                if (guard)
                    guard->respondError(connectionId, requestId, QStringLiteral("cleanup_failed"),
                                        QStringLiteral("python3 could not start."));
            });
    timer->start(diagctl::kFreeDiskSpaceTimeoutMs);
    process->start(python, diagctl::freeDiskSpaceArguments(root, step));
}
