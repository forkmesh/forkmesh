#include "AgentResources.h"
#include "ProcessReaper.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <atomic>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace forkmesh::agentresources {

namespace {

constexpr qint64 kGiB = qint64(1) << 30;
constexpr int kProbeTimeoutMs = 8000;
constexpr char kGradleIdleTimeoutMs[] = "600000";

QMutex &stateMutex()
{
    static QMutex mutex;
    return mutex;
}

Status &storedStatus()
{
    static Status state;
    return state;
}

qint64 physicalMemoryBytes()
{
#if defined(Q_OS_LINUX)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || pageSize <= 0)
        return 0;
    return qint64(pages) * qint64(pageSize);
#else
    return 0;
#endif
}

QString gigabytes(qint64 bytes)
{
    return QStringLiteral("%1 GB").arg(double(bytes) / double(kGiB), 0, 'f', 1);
}

QString shellQuote(const QString &value)
{
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

bool runTool(const QString &program, const QStringList &arguments,
             QString *output, QString *error)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(kProbeTimeoutMs)) {
        if (error)
            *error = process.errorString();
        return false;
    }
    if (!process.waitForFinished(kProbeTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        if (error)
            *error = QStringLiteral("%1 timed out").arg(QFileInfo(program).fileName());
        return false;
    }
    if (output)
        *output = QString::fromUtf8(process.readAllStandardOutput());
    const QString stderrText =
        QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error)
            *error = stderrText.isEmpty()
                         ? QStringLiteral("%1 exited with %2")
                               .arg(QFileInfo(program).fileName())
                               .arg(process.exitCode())
                         : stderrText.section(QLatin1Char('\n'), 0, 0);
        return false;
    }
    return true;
}

QString memoryDelegationProblem()
{
#if defined(Q_OS_UNIX)
    const QString uid = QString::number(getuid());
    QFile file(QStringLiteral("/sys/fs/cgroup/user.slice/user-%1.slice/"
                              "user@%1.service/cgroup.subtree_control")
                   .arg(uid));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QStringList controllers =
        QString::fromLatin1(file.readAll()).split(QLatin1Char(' '),
                                                  Qt::SkipEmptyParts);
    for (const QString &controller : controllers)
        if (controller.trimmed() == QLatin1String("memory"))
            return {};
    return QStringLiteral("the memory controller is not delegated to the "
                          "systemd user manager");
#else
    return {};
#endif
}

bool writeShim(const QString &directory, const QString &realSystemdRun,
               QString *error)
{
    if (!QDir().mkpath(directory)) {
        *error = QStringLiteral("could not create %1").arg(directory);
        return false;
    }
    const QString path = directory + QStringLiteral("/systemd-run");
    const QByteArray wanted = shimScript(realSystemdRun).toUtf8();
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const bool same = file.readAll() == wanted;
        file.close();
        if (same)
            return true;
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(wanted) != wanted.size()) {
        *error = QStringLiteral("could not write %1").arg(path);
        return false;
    }
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                        QFileDevice::ExeOwner);
    return true;
}

} // namespace

QString sliceName()
{
    return QStringLiteral("forkmesh.slice");
}

Budget computeBudget(qint64 totalBytes, int cpuCount, int reservePercent,
                     int cpuReservePercent)
{
    Budget budget;
    if (totalBytes <= 0)
        return budget;
    const int percent =
        qBound(kMinReservePercent, reservePercent, kMaxReservePercent);
    qint64 reserve = qMax(2 * kGiB, totalBytes * percent / 100);
    reserve = qMin(reserve, totalBytes / 2);
    budget.totalBytes = totalBytes;
    budget.reserveBytes = reserve;
    budget.sliceHighBytes = totalBytes - reserve;
    budget.sliceMaxBytes = totalBytes - reserve / 2;
    budget.agentHighBytes = budget.sliceHighBytes * 3 / 5;
    budget.agentMaxBytes = budget.sliceHighBytes * 4 / 5;
    const int cpus = qMax(1, cpuCount);
    const int jobs = int(budget.agentHighBytes / (3 * kGiB));
    budget.jobsPerAgent = qBound(qMin(2, cpus), jobs, cpus);
    const int cpuPercent =
        qBound(kMinReservePercent, cpuReservePercent, kMaxReservePercent);
    budget.cpuCount = cpus;
    budget.cpuReserve = qMin(cpus * 50, qMax(100, cpus * cpuPercent));
    budget.sliceCpuQuota = cpus * 100 - budget.cpuReserve;
    budget.sliceCpuFloor = qMin(budget.sliceCpuQuota, qMax(100, cpus * 25));
    return budget;
}

QString cpuCores(int quotaPercent)
{
    return QStringLiteral("%1 cores").arg(quotaPercent / 100.0, 0, 'f', 1);
}

Budget currentBudget()
{
    const QSettings settings;
    return computeBudget(
        physicalMemoryBytes(), QThread::idealThreadCount(),
        settings.value(kReservePercentSetting, kDefaultReservePercent).toInt(),
        settings.value(kCpuReservePercentSetting, kDefaultCpuReservePercent)
            .toInt());
}

QStringList sliceProperties(const Budget &budget)
{
    QStringList properties{
        QStringLiteral("MemoryHigh=%1").arg(budget.sliceHighBytes),
        QStringLiteral("MemoryMax=%1").arg(budget.sliceMaxBytes),
        QStringLiteral("CPUWeight=50")};
    if (budget.sliceCpuQuota > 0)
        properties << QStringLiteral("CPUQuota=%1%").arg(budget.sliceCpuQuota);
    return properties;
}

QString scopeUnitName(const QString &label, qint64 pid, int serial)
{
    QString clean = label.toLower();
    clean.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                  QStringLiteral("-"));
    clean = clean.left(32);
    while (clean.startsWith(QLatin1Char('-')))
        clean.remove(0, 1);
    while (clean.endsWith(QLatin1Char('-')))
        clean.chop(1);
    if (clean.isEmpty())
        clean = QStringLiteral("agent");
    return QStringLiteral("forkmesh-agent-%1-%2-%3.scope")
        .arg(clean)
        .arg(pid)
        .arg(serial);
}

QStringList scopeArguments(const QString &unit, const QString &description,
                           const Budget &budget, bool oomPolicy)
{
    QStringList arguments{
        QStringLiteral("--user"),
        QStringLiteral("--scope"),
        QStringLiteral("--quiet"),
        QStringLiteral("--collect"),
        QStringLiteral("--slice=") + sliceName(),
        QStringLiteral("--unit=") + unit,
        QStringLiteral("--description=") + description,
        QStringLiteral("--property=MemoryHigh=%1").arg(budget.agentHighBytes),
        QStringLiteral("--property=MemoryMax=%1").arg(budget.agentMaxBytes),
    };
    if (oomPolicy)
        arguments << QStringLiteral("--property=OOMPolicy=continue");
    return arguments;
}

QString shimScript(const QString &realSystemdRun)
{
    const QString real = shellQuote(realSystemdRun);
    return QStringLiteral(
               "#!/bin/sh\n"
               "for arg in \"$@\"; do\n"
               "    case \"$arg\" in\n"
               "        --slice|--slice=*|--slice-inherit|Slice=*|--property=Slice=*)"
               " exec %1 \"$@\" ;;\n"
               "    esac\n"
               "done\n"
               "for arg in \"$@\"; do\n"
               "    [ \"$arg\" = --user ] && exec %1 --slice=%2 \"$@\"\n"
               "done\n"
               "exec %1 \"$@\"\n")
        .arg(real, sliceName());
}

QStringList environmentDefaults(const Budget &budget)
{
    const QString jobs = QString::number(qMax(1, budget.jobsPerAgent));
    return {
        QStringLiteral("CMAKE_BUILD_PARALLEL_LEVEL=") + jobs,
        QStringLiteral("CARGO_BUILD_JOBS=") + jobs,
        QStringLiteral("PYTEST_XDIST_AUTO_NUM_WORKERS=") + jobs,
        QStringLiteral("GRADLE_OPTS=-Dorg.gradle.workers.max=%1 "
                       "-Dorg.gradle.daemon.idletimeout=%2")
            .arg(jobs, QLatin1String(kGradleIdleTimeoutMs)),
        QStringLiteral("FORKMESH_AGENT_SLICE=") + sliceName(),
    };
}

void mergeEnvironment(QProcessEnvironment &env, const QStringList &defaults)
{
    for (const QString &entry : defaults) {
        const int equals = entry.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = entry.left(equals);
        const QString value = entry.mid(equals + 1);
        if (!env.contains(key)) {
            env.insert(key, value);
            continue;
        }
        if (key != QLatin1String("GRADLE_OPTS"))
            continue;
        QString merged = env.value(key);
        for (const QString &token :
             value.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const QString name = token.section(QLatin1Char('='), 0, 0);
            if (!merged.contains(name))
                merged += (merged.isEmpty() ? QString() : QStringLiteral(" ")) +
                          token;
        }
        env.insert(key, merged);
    }
}

bool enabled()
{
    return QSettings().value(kEnabledSetting, true).toBool();
}

Status prepare()
{
    QMutexLocker lock(&stateMutex());
    Status state;
    state.enabled = enabled();
    state.budget = currentBudget();
    const auto finish = [&state](const QString &reason) {
        state.reason = reason;
        storedStatus() = state;
        return state;
    };
#if !defined(Q_OS_LINUX)
    return finish(QStringLiteral("memory containment needs Linux with systemd"));
#else
    if (!state.enabled)
        return finish(QStringLiteral("switched off in Settings > Agents"));
    if (!state.budget.valid())
        return finish(QStringLiteral("the physical memory size is unknown"));
    if (!QFile::exists(QStringLiteral("/sys/fs/cgroup/cgroup.controllers")))
        return finish(QStringLiteral("cgroup v2 is not mounted"));
    state.systemdRun = QStandardPaths::findExecutable(QStringLiteral("systemd-run"));
    state.systemctl = QStandardPaths::findExecutable(QStringLiteral("systemctl"));
    if (state.systemdRun.isEmpty() || state.systemctl.isEmpty())
        return finish(QStringLiteral("systemd-run or systemctl is missing"));
    const QString delegation = memoryDelegationProblem();
    if (!delegation.isEmpty())
        return finish(delegation);

    QString error;
    if (!runTool(state.systemctl,
                 QStringList{QStringLiteral("--user"),
                             QStringLiteral("set-property"),
                             QStringLiteral("--runtime"), sliceName()} +
                     sliceProperties(state.budget),
                 nullptr, &error))
        return finish(QStringLiteral("could not configure %1: %2")
                          .arg(sliceName(), error));

    const qint64 pid = QCoreApplication::applicationPid();
    for (const bool oomPolicy : {true, false}) {
        const QString unit = QStringLiteral("forkmesh-agent-probe-%1-%2.scope")
                                 .arg(pid)
                                 .arg(oomPolicy ? 1 : 0);
        QStringList arguments =
            scopeArguments(unit, QStringLiteral("ForkMesh agent scope probe"),
                           state.budget, oomPolicy);
        arguments << QStringLiteral("--") << QStringLiteral("true");
        if (runTool(state.systemdRun, arguments, nullptr, &error)) {
            state.oomPolicy = oomPolicy;
            state.available = true;
            break;
        }
    }
    if (!state.available)
        return finish(QStringLiteral("systemd-run could not start an agent "
                                     "scope: %1")
                          .arg(error));

    QString controlGroup;
    if (runTool(state.systemctl,
                {QStringLiteral("--user"), QStringLiteral("show"), sliceName(),
                 QStringLiteral("--property=ControlGroup"),
                 QStringLiteral("--value")},
                &controlGroup, nullptr) &&
        !controlGroup.trimmed().isEmpty())
        state.controlGroup =
            QStringLiteral("/sys/fs/cgroup") + controlGroup.trimmed();

    const QString runtime =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtime.isEmpty()) {
        const QString directory = runtime + QStringLiteral("/forkmesh-agent-bin");
        QString shimError;
        if (writeShim(directory, state.systemdRun, &shimError))
            state.shimDirectory = directory;
        else
            state.reason = shimError;
    }
    storedStatus() = state;
    return state;
#endif
}

Status status()
{
    QMutexLocker lock(&stateMutex());
    return storedStatus();
}

QString describe(const Status &state)
{
    if (!state.available)
        return QStringLiteral("Agent memory containment is off: %1.")
            .arg(state.reason.isEmpty() ? QStringLiteral("not prepared")
                                        : state.reason);
    const Budget &b = state.budget;
    return QStringLiteral(
               "Agents run in %1: throttled above %2, hard limit %3, %4 kept "
               "for the desktop and the rest of the system; each agent is "
               "throttled above %5 and capped at %6; %7 build jobs per agent. "
               "Agent CPU is capped at %8 of %9, lower while other programs "
               "need it.")
        .arg(sliceName(), gigabytes(b.sliceHighBytes),
             gigabytes(b.sliceMaxBytes), gigabytes(b.reserveBytes),
             gigabytes(b.agentHighBytes), gigabytes(b.agentMaxBytes))
        .arg(b.jobsPerAgent)
        .arg(cpuCores(b.sliceCpuQuota), cpuCores(b.cpuCount * 100));
}

void applyEnvironment(QProcessEnvironment &env)
{
    const Status state = status();
    if (!state.enabled || !state.budget.valid())
        return;
    mergeEnvironment(env, environmentDefaults(state.budget));
}

QString shimDirectory()
{
    const Status state = status();
    return state.available ? state.shimDirectory : QString();
}

QString wrapShellCommand(const QString &command)
{
    const QString directory = shimDirectory();
    if (directory.isEmpty() || command.trimmed().isEmpty())
        return command;
    return QStringLiteral("export PATH=%1:\"$PATH\"; %2")
        .arg(shellQuote(directory), command);
}

Launch contain(const QString &program, const QStringList &arguments,
               const QString &label)
{
    static std::atomic<int> serial{0};
    Launch launch{program, arguments, QString(), false};
    const Status state = status();
    if (!state.available || program.isEmpty())
        return launch;
    launch.unit = scopeUnitName(label, QCoreApplication::applicationPid(),
                                ++serial);
    launch.program = state.systemdRun;
    launch.arguments =
        scopeArguments(launch.unit,
                       QStringLiteral("ForkMesh agent %1").arg(label),
                       state.budget, state.oomPolicy);
    launch.arguments << QStringLiteral("--") << program;
    launch.arguments += arguments;
    launch.contained = true;
    forkmesh::processreaper::registerOwnedUnit(launch.unit);
    return launch;
}

QString launchLabel(const QString &provider, const QProcessEnvironment &env)
{
    const QString session =
        env.value(QStringLiteral("FORKMESH_AGENT_GATE_SESSION"))
            .section(QLatin1Char(':'), 0, 0)
            .trimmed();
    return session.isEmpty() ? provider
                             : QStringLiteral("%1 %2").arg(provider, session);
}

} // namespace forkmesh::agentresources
