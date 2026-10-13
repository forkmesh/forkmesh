#include "ProcessReaper.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <memory>

#if defined(Q_OS_UNIX)
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace forkmesh::processreaper {

namespace {

QByteArray &instanceStorage()
{
    static QByteArray value;
    return value;
}

QByteArray readSmallFile(const QString &path, qint64 limit = 1 << 20)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
        return {};
    return file.read(limit);
}

bool inPlace(const QString &controlGroup, const QString &place)
{
    if (place.isEmpty())
        return true;
    return controlGroup == place ||
           controlGroup.startsWith(place + QLatin1Char('/'));
}

#if defined(Q_OS_UNIX)
// code-health: allow(blocking-wait) runs after the event loop has ended, bounded by timeoutMs
QString runTool(const QString &program, const QStringList &arguments,
                int timeoutMs, qint64 *pidOut = nullptr)
{
    if (program.isEmpty())
        return {};
    QProcess process;
    process.setProcessEnvironment(releasedEnvironment());
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(timeoutMs))
        return {};
    if (pidOut)
        *pidOut = process.processId();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        return {};
    }
    return QString::fromLocal8Bit(process.readAllStandardOutput());
}
#endif

} // namespace

QByteArray initializeInstance()
{
    QByteArray &value = instanceStorage();
    if (!value.isEmpty())
        return value;
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    value = QByteArray(reinterpret_cast<const char *>(words), sizeof(words))
                .toHex();
    qputenv(kInstanceVariable, value);
    return value;
}

QByteArray instance()
{
    return instanceStorage();
}

QStringList defaultProtectedUnitPrefixes()
{
    return {QStringLiteral("fmnext-"),
            QStringLiteral("fm-"),
            QStringLiteral("forkmesh-node-deploy"),
            QStringLiteral("forkmesh-source-publish"),
            QStringLiteral("forkmesh-source-guard"),
            QStringLiteral("forkmesh-source-cores"),
            QStringLiteral("forkmesh-mirror"),
            QStringLiteral("forkmesh-release"),
            QStringLiteral("forkmesh-backup"),
            QStringLiteral("forkmesh-dns")};
}

Policy currentPolicy()
{
    return {instance(), ownedUnits(), defaultProtectedUnitPrefixes()};
}

QString unitOfControlGroup(const QString &controlGroup)
{
    const QStringList parts =
        controlGroup.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (auto it = parts.crbegin(); it != parts.crend(); ++it) {
        if (it->endsWith(QLatin1String(".scope")) ||
            it->endsWith(QLatin1String(".service")))
            return *it;
    }
    return {};
}

bool isOwnedUnitName(const QString &unit)
{
    static const QRegularExpression pattern(
        QStringLiteral("^forkmesh-(agent|action)-[A-Za-z0-9-]+\\.scope$"));
    return pattern.match(unit).hasMatch();
}

bool isProtectedUnit(const QString &unit, const QStringList &prefixes)
{
    if (unit.isEmpty())
        return false;
    for (const QString &prefix : prefixes) {
        if (unit.startsWith(prefix))
            return true;
    }
    return false;
}

QList<qint64> selectTargets(const ProcessTable &table, const Policy &policy)
{
    QHash<qint64, const ProcessEntry *> byPid;
    QHash<qint64, QList<qint64>> children;
    for (const ProcessEntry &entry : table.processes) {
        byPid.insert(entry.pid, &entry);
        children[entry.parentPid].append(entry.pid);
    }

    QSet<qint64> ancestors;
    qint64 selfGroup = 0;
    if (const ProcessEntry *self = byPid.value(table.selfPid)) {
        selfGroup = self->groupId;
        qint64 parent = self->parentPid;
        while (parent > 0 && !ancestors.contains(parent)) {
            ancestors.insert(parent);
            const ProcessEntry *up = byPid.value(parent);
            parent = up ? up->parentPid : 0;
        }
    }

    QSet<qint64> descendants;
    QList<qint64> pending{table.selfPid};
    while (!pending.isEmpty()) {
        const qint64 parent = pending.takeLast();
        for (qint64 child : children.value(parent)) {
            if (child == table.selfPid || descendants.contains(child))
                continue;
            descendants.insert(child);
            pending.append(child);
        }
    }

    const auto eligible = [&](const ProcessEntry &entry) {
        if (entry.pid <= 1 || entry.pid == table.selfPid || entry.zombie ||
            ancestors.contains(entry.pid))
            return false;
        if (table.selfUid < 0 || entry.uid != table.selfUid)
            return false;
        return true;
    };
    const auto ownedPlace = [&](const ProcessEntry &entry) {
        const QString unit = unitOfControlGroup(entry.controlGroup);
        return isOwnedUnitName(unit) && policy.ownedUnits.contains(unit) &&
               !isProtectedUnit(unit, policy.protectedUnitPrefixes);
    };
    const auto samePlace = [&](const ProcessEntry &entry) {
        if (!inPlace(entry.controlGroup, table.selfControlGroup))
            return false;
        if (table.selfControlGroup.isEmpty() || entry.controlGroup.isEmpty())
            return true;
        const QString unit = unitOfControlGroup(entry.controlGroup);
        return unit == unitOfControlGroup(table.selfControlGroup) ||
               !isProtectedUnit(unit, policy.protectedUnitPrefixes);
    };
    const auto marked = [&](const ProcessEntry &entry) {
        return !policy.instance.isEmpty() && entry.instance == policy.instance;
    };

    QSet<qint64> relatedGroups;
    for (const ProcessEntry &entry : table.processes) {
        if (!eligible(entry) || !samePlace(entry))
            continue;
        if ((marked(entry) || descendants.contains(entry.pid)) &&
            entry.groupId > 1 && entry.groupId != selfGroup)
            relatedGroups.insert(entry.groupId);
    }

    QList<qint64> targets;
    for (const ProcessEntry &entry : table.processes) {
        if (!eligible(entry))
            continue;
        const bool related = marked(entry) ||
                             descendants.contains(entry.pid) ||
                             relatedGroups.contains(entry.groupId);
        if (ownedPlace(entry) || (related && samePlace(entry)))
            targets.append(entry.pid);
    }
    std::sort(targets.begin(), targets.end());
    return targets;
}

QStringList liveOwnedUnits(const ProcessTable &table, const Policy &policy)
{
    QSet<QString> seen;
    for (const ProcessEntry &entry : table.processes) {
        if (entry.uid != table.selfUid || entry.zombie)
            continue;
        const QString unit = unitOfControlGroup(entry.controlGroup);
        if (isOwnedUnitName(unit) && policy.ownedUnits.contains(unit) &&
            !isProtectedUnit(unit, policy.protectedUnitPrefixes))
            seen.insert(unit);
    }
    QStringList units(seen.cbegin(), seen.cend());
    units.sort();
    return units;
}

bool parseProcStat(const QByteArray &stat, ProcessEntry *entry)
{
    const int open = stat.indexOf('(');
    const int close = stat.lastIndexOf(')');
    if (open <= 0 || close < open)
        return false;
    bool ok = false;
    const qint64 pid = stat.left(open).trimmed().toLongLong(&ok);
    if (!ok)
        return false;
    const QList<QByteArray> fields =
        stat.mid(close + 1).simplified().split(' ');
    if (fields.size() < 3)
        return false;
    bool parentOk = false;
    bool groupOk = false;
    entry->pid = pid;
    entry->zombie = fields.at(0) == "Z" || fields.at(0) == "X";
    entry->parentPid = fields.at(1).toLongLong(&parentOk);
    entry->groupId = fields.at(2).toLongLong(&groupOk);
    return parentOk && groupOk;
}

QString parseProcControlGroup(const QByteArray &cgroup)
{
    QString legacy;
    for (const QByteArray &line : cgroup.split('\n')) {
        if (line.startsWith("0::"))
            return QString::fromUtf8(line.mid(3)).trimmed();
        const int marker = line.indexOf(":name=systemd:");
        if (marker >= 0)
            legacy = QString::fromUtf8(line.mid(marker + 14)).trimmed();
    }
    return legacy;
}

QByteArray instanceFromEnviron(const QByteArray &environment)
{
    const QByteArray key = QByteArray(kInstanceVariable) + '=';
    for (const QByteArray &item : environment.split('\0')) {
        if (item.startsWith(key))
            return item.mid(key.size());
    }
    return {};
}

qint64 uidFromProcStatus(const QByteArray &status)
{
    for (const QByteArray &line : status.split('\n')) {
        if (!line.startsWith("Uid:"))
            continue;
        const QList<QByteArray> fields = line.mid(4).simplified().split(' ');
        bool ok = false;
        const qint64 uid = fields.value(0).toLongLong(&ok);
        return ok ? uid : -1;
    }
    return -1;
}

ProcessTable readProcTable(const QString &procRoot, qint64 selfPid)
{
    ProcessTable table;
    table.selfPid = selfPid;
    const QDir root(procRoot);
    const QStringList names = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QList<ProcessEntry> entries;
    for (const QString &name : names) {
        bool numeric = false;
        name.toLongLong(&numeric);
        if (!numeric)
            continue;
        const QString base = root.filePath(name) + QLatin1Char('/');
        ProcessEntry entry;
        if (!parseProcStat(readSmallFile(base + QStringLiteral("stat"), 4096),
                           &entry))
            continue;
        entry.uid = uidFromProcStatus(
            readSmallFile(base + QStringLiteral("status"), 16384));
        entry.controlGroup = parseProcControlGroup(
            readSmallFile(base + QStringLiteral("cgroup"), 16384));
        entries.append(entry);
        if (entry.pid == selfPid) {
            table.selfUid = entry.uid;
            table.selfControlGroup = entry.controlGroup;
        }
    }
    for (ProcessEntry &entry : entries) {
        if (entry.uid < 0 || entry.uid != table.selfUid || entry.zombie)
            continue;
        entry.instance = instanceFromEnviron(readSmallFile(
            root.filePath(QString::number(entry.pid)) +
            QStringLiteral("/environ")));
    }
    table.processes = entries;
    return table;
}

ProcessTable parsePsTable(const QByteArray &psOutput, qint64 selfPid,
                          qint64 selfUid)
{
    ProcessTable table;
    table.selfPid = selfPid;
    table.selfUid = selfUid;
    for (const QByteArray &line : psOutput.split('\n')) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.size() < 5)
            continue;
        bool pidOk = false;
        bool parentOk = false;
        bool groupOk = false;
        bool uidOk = false;
        ProcessEntry entry;
        entry.pid = fields.at(0).toLongLong(&pidOk);
        entry.parentPid = fields.at(1).toLongLong(&parentOk);
        entry.groupId = fields.at(2).toLongLong(&groupOk);
        entry.uid = fields.at(3).toLongLong(&uidOk);
        entry.zombie = fields.at(4).startsWith('Z');
        if (pidOk && parentOk && groupOk && uidOk)
            table.processes.append(entry);
    }
    return table;
}

Report reap(const Policy &policy, const Hooks &hooks, int graceMs,
            int deadlineMs)
{
    Report report;
    if (!hooks.snapshot || !hooks.send || !hooks.nowMs || !hooks.sleepMs)
        return report;
    const qint64 start = hooks.nowMs();
    const qint64 deadlineAt = start + qMax(0, deadlineMs);
    const qint64 graceAt = start + qBound(0, graceMs, qMax(0, deadlineMs));

    ProcessTable table = hooks.snapshot();
    QList<qint64> targets = selectTargets(table, policy);
    QStringList units = liveOwnedUnits(table, policy);
    if (!units.isEmpty() && hooks.stopUnits) {
        hooks.stopUnits(units, Signal::Terminate);
        report.stoppedUnits = units;
    }
    QSet<qint64> terminated;
    QSet<qint64> killed;
    for (qint64 pid : std::as_const(targets)) {
        hooks.send(pid, Signal::Terminate);
        terminated.insert(pid);
        report.terminated.append(pid);
    }
    bool unitsKilled = false;

    while (!targets.isEmpty()) {
        const qint64 now = hooks.nowMs();
        if (now >= deadlineAt) {
            report.timedOut = true;
            report.survivors = targets;
            break;
        }
        hooks.sleepMs(int(qMin<qint64>(100, deadlineAt - now)));
        ++report.rounds;
        table = hooks.snapshot();
        targets = selectTargets(table, policy);
        if (targets.isEmpty())
            break;
        if (hooks.nowMs() >= graceAt) {
            units = liveOwnedUnits(table, policy);
            if (!unitsKilled && !units.isEmpty() && hooks.stopUnits) {
                hooks.stopUnits(units, Signal::Kill);
                unitsKilled = true;
            }
            for (qint64 pid : std::as_const(targets)) {
                if (killed.contains(pid))
                    continue;
                hooks.send(pid, Signal::Kill);
                killed.insert(pid);
                report.killed.append(pid);
            }
        } else {
            for (qint64 pid : std::as_const(targets)) {
                if (terminated.contains(pid))
                    continue;
                hooks.send(pid, Signal::Terminate);
                terminated.insert(pid);
                report.terminated.append(pid);
            }
        }
    }
    return report;
}

Report reapOnQuit(int graceMs, int deadlineMs)
{
#if defined(Q_OS_UNIX)
    const qint64 self = QCoreApplication::applicationPid();
    Hooks hooks;
#if defined(Q_OS_LINUX)
    hooks.snapshot = [self] { return readProcTable(QStringLiteral("/proc"), self); };
#else
    const qint64 uid = qint64(::getuid());
    const QString ps = QStringLiteral("/bin/ps");
    hooks.snapshot = [self, uid, ps] {
        qint64 psPid = 0;
        const QString output = runTool(
            ps, {QStringLiteral("-axo"), QStringLiteral("pid=,ppid=,pgid=,uid=,stat=")},
            1000, &psPid);
        ProcessTable table = parsePsTable(output.toLocal8Bit(), self, uid);
        table.processes.removeIf(
            [psPid](const ProcessEntry &entry) { return entry.pid == psPid; });
        return table;
    };
#endif
    hooks.send = [](qint64 pid, Signal signal) {
        if (pid > 1)
            ::kill(pid_t(pid), signal == Signal::Kill ? SIGKILL : SIGTERM);
    };
#if defined(Q_OS_LINUX)
    const QString systemctl = QStringLiteral("systemctl");
    hooks.stopUnits = [systemctl](const QStringList &units, Signal signal) {
        const QStringList arguments =
            signal == Signal::Kill
                ? QStringList{QStringLiteral("--user"), QStringLiteral("kill"),
                              QStringLiteral("--signal=SIGKILL"),
                              QStringLiteral("--kill-whom=all")} +
                      units
                : QStringList{QStringLiteral("--user"), QStringLiteral("stop"),
                              QStringLiteral("--no-block")} +
                      units;
        runTool(systemctl, arguments, 1000);
    };
#endif
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    hooks.nowMs = [clock] { return clock->elapsed(); };
    hooks.sleepMs = [](int ms) {
        if (ms > 0)
            QThread::msleep(ulong(ms));
    };
    return reap(currentPolicy(), hooks, graceMs, deadlineMs);
#else
    Q_UNUSED(graceMs)
    Q_UNUSED(deadlineMs)
    return {};
#endif
}

QString describe(const Report &report)
{
    if (report.terminated.isEmpty() && report.stoppedUnits.isEmpty())
        return QStringLiteral("Quit: no leftover child processes.");
    const auto pids = [](const QList<qint64> &list) {
        QStringList out;
        for (qint64 pid : list)
            out << QString::number(pid);
        return out.join(QLatin1Char(' '));
    };
    QString text = QStringLiteral("Quit: stopped %1 leftover process(es)")
                       .arg(report.terminated.size());
    if (!report.stoppedUnits.isEmpty())
        text += QStringLiteral(" and unit(s) %1")
                    .arg(report.stoppedUnits.join(QLatin1Char(' ')));
    if (!report.terminated.isEmpty())
        text += QStringLiteral("; SIGTERM: %1").arg(pids(report.terminated));
    if (!report.killed.isEmpty())
        text += QStringLiteral("; SIGKILL after the grace period: %1")
                    .arg(pids(report.killed));
    if (!report.survivors.isEmpty())
        text += QStringLiteral("; still running at the deadline: %1")
                    .arg(pids(report.survivors));
    return text + QLatin1Char('.');
}

} // namespace forkmesh::processreaper
