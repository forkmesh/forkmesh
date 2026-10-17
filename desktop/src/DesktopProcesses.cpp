#include "DesktopProcesses.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSet>
#include <QStringList>

#if defined(Q_OS_LINUX)
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace forkmesh {
namespace desktopprocs {

namespace {

#if defined(Q_OS_LINUX)
QByteArray readSmall(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.read(64 * 1024);
}

qint64 meminfoBytes(const QByteArray &meminfo, const char *key)
{
    const QByteArray needle = QByteArray(key) + ':';
    const int at = meminfo.indexOf(needle);
    if (at < 0 || (at > 0 && meminfo.at(at - 1) != '\n'))
        return -1;
    const int end = meminfo.indexOf('\n', at);
    const QList<QByteArray> fields =
        meminfo.mid(at + needle.size(), end < 0 ? -1 : end - at - needle.size())
            .simplified()
            .split(' ');
    bool ok = false;
    const qint64 value = fields.value(0).toLongLong(&ok);
    return ok ? value * 1024 : -1;
}
#endif

} // namespace

Process parseStatLine(const QByteArray &line, qint64 pageBytes)
{
    Process process;
    const int open = line.indexOf('(');
    const int close = line.lastIndexOf(')');
    if (open <= 0 || close <= open)
        return process;
    bool ok = false;
    process.pid = line.left(open).trimmed().toLongLong(&ok);
    if (!ok)
        return Process();
    process.name = QString::fromUtf8(line.mid(open + 1, close - open - 1));
    const QList<QByteArray> fields = line.mid(close + 1).simplified().split(' ');
    if (fields.size() < 22)
        return Process();
    process.state = QChar::fromLatin1(fields.at(0).isEmpty() ? '?' : fields.at(0).at(0));
    process.parentPid = fields.at(1).toLongLong();
    process.cpuTicks = fields.at(11).toLongLong() + fields.at(12).toLongLong();
    process.threads = fields.at(17).toInt();
    process.startTicks = fields.at(19).toLongLong();
    process.rssBytes = qMax<qint64>(0, fields.at(21).toLongLong()) * pageBytes;
    return process;
}

QString stateWord(QChar state)
{
    switch (state.toLatin1()) {
    case 'R':
        return QStringLiteral("running");
    case 'S':
        return QStringLiteral("sleeping");
    case 'D':
        return QStringLiteral("waiting on disk");
    case 'Z':
        return QStringLiteral("zombie");
    case 'T':
        return QStringLiteral("stopped");
    case 't':
        return QStringLiteral("traced");
    case 'I':
        return QStringLiteral("idle");
    case 'X':
        return QStringLiteral("dead");
    default:
        return QStringLiteral("unknown");
    }
}

QString descriptionFor(const Process &process)
{
    if (process.self)
        return QStringLiteral("ForkMesh desktop application");
    if (process.forkmesh)
        return QStringLiteral("Process started by ForkMesh");
    const QString executable = QFileInfo(process.command.section(QLatin1Char(' '), 0, 0)).fileName();
    const QString name = executable.isEmpty() ? process.name : executable;
    static const QHash<QString, QString> descriptions{
        {QStringLiteral("gnome-shell"), QStringLiteral("GNOME desktop shell")},
        {QStringLiteral("Xwayland"), QStringLiteral("X11 compatibility display server")},
        {QStringLiteral("dockerd"), QStringLiteral("Docker container runtime")},
        {QStringLiteral("containerd"), QStringLiteral("Container runtime service")},
        {QStringLiteral("sshd"), QStringLiteral("Secure Shell server")},
        {QStringLiteral("systemd"), QStringLiteral("System and service manager")},
        {QStringLiteral("pulseaudio"), QStringLiteral("Audio server")},
        {QStringLiteral("pipewire"), QStringLiteral("Audio and video service")},
        {QStringLiteral("chrome"), QStringLiteral("Web browser process")},
        {QStringLiteral("firefox"), QStringLiteral("Web browser process")},
        {QStringLiteral("python"), QStringLiteral("Python program")},
        {QStringLiteral("python3"), QStringLiteral("Python program")},
        {QStringLiteral("node"), QStringLiteral("Node.js program")},
        {QStringLiteral("cc1plus"), QStringLiteral("C++ compiler worker")},
    };
    if (descriptions.contains(name))
        return descriptions.value(name);
    return process.command.isEmpty() ? QStringLiteral("System process")
                                     : QStringLiteral("Runs %1").arg(name);
}

QString detailTextFor(const Process &process)
{
    QStringList lines;
    lines.append(QStringLiteral("Name: %1").arg(process.name));
    lines.append(QStringLiteral("Description: %1").arg(descriptionFor(process)));
    lines.append(QStringLiteral("PID: %1").arg(process.pid));
    lines.append(QStringLiteral("Parent PID: %1").arg(process.parentPid));
    lines.append(QStringLiteral("Owner: %1 (UID %2)").arg(process.user).arg(process.uid));
    lines.append(QStringLiteral("State: %1").arg(stateWord(process.state)));
    lines.append(QStringLiteral("CPU: %1%").arg(process.cpuPercent < 0.0
                                                       ? QStringLiteral("-")
                                                       : QString::number(process.cpuPercent, 'f', 1)));
    lines.append(QStringLiteral("Resident memory: %1").arg(corelogs::compactByteSize(process.rssBytes)));
    lines.append(QStringLiteral("Threads: %1").arg(process.threads));
    if (process.ageSeconds >= 0)
        lines.append(QStringLiteral("Age: %1 seconds").arg(process.ageSeconds));
    if (!process.command.isEmpty())
        lines.append(QStringLiteral("Command: %1").arg(process.command));
#if defined(Q_OS_LINUX)
    const QString base = QStringLiteral("/proc/%1").arg(process.pid);
    const QString executable = QFileInfo(base + QStringLiteral("/exe")).symLinkTarget();
    const QString workingDirectory = QFileInfo(base + QStringLiteral("/cwd")).symLinkTarget();
    if (!executable.isEmpty())
        lines.append(QStringLiteral("Executable: %1").arg(executable));
    if (!workingDirectory.isEmpty())
        lines.append(QStringLiteral("Working directory: %1").arg(workingDirectory));
    const QByteArray cgroup = readSmall(base + QStringLiteral("/cgroup")).trimmed();
    if (!cgroup.isEmpty())
        lines.append(QStringLiteral("Control group: %1").arg(QString::fromUtf8(cgroup)));
    QDir descriptors(base + QStringLiteral("/fd"));
    const int descriptorCount = descriptors.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size();
    if (descriptorCount > 0)
        lines.append(QStringLiteral("Open file descriptors: %1").arg(descriptorCount));
    const QByteArray io = readSmall(base + QStringLiteral("/io"));
    QStringList ioRows;
    for (const QByteArray &row : io.split('\n')) {
        if (row.startsWith("read_bytes:") || row.startsWith("write_bytes:"))
            ioRows.append(QString::fromUtf8(row).simplified());
    }
    if (!ioRows.isEmpty())
        lines.append(QStringLiteral("I/O: %1").arg(ioRows.join(QStringLiteral(", "))));
    QStringList children;
    QDir proc(QStringLiteral("/proc"));
    const long pageBytes = qMax(1L, sysconf(_SC_PAGESIZE));
    for (const QString &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        entry.toLongLong(&numeric);
        if (!numeric)
            continue;
        const Process child = parseStatLine(readSmall(proc.filePath(entry + QStringLiteral("/stat"))),
                                            pageBytes);
        if (child.parentPid == process.pid)
            children.append(QStringLiteral("%1 (PID %2)").arg(child.name).arg(child.pid));
        if (children.size() == 20)
            break;
    }
    if (!children.isEmpty())
        lines.append(QStringLiteral("Children: %1").arg(children.join(QStringLiteral(", "))));
#endif
    return lines.join(QLatin1Char('\n'));
}

void markTree(QList<Process> &processes, qint64 rootPid)
{
    QHash<qint64, QList<int>> children;
    int root = -1;
    for (int i = 0; i < processes.size(); ++i) {
        children[processes.at(i).parentPid].append(i);
        if (processes.at(i).pid == rootPid)
            root = i;
    }
    if (root < 0)
        return;
    processes[root].self = true;
    QList<int> pending{root};
    QSet<int> seen;
    while (!pending.isEmpty()) {
        const int index = pending.takeLast();
        if (seen.contains(index))
            continue;
        seen.insert(index);
        processes[index].forkmesh = true;
        pending.append(children.value(processes.at(index).pid));
    }
}

QString Sampler::userName(uint uid)
{
    auto it = m_users.constFind(uid);
    if (it != m_users.cend())
        return *it;
    QString name = QString::number(uid);
#if defined(Q_OS_LINUX)
    struct passwd entry;
    struct passwd *found = nullptr;
    char buffer[4096];
    if (getpwuid_r(uid, &entry, buffer, sizeof buffer, &found) == 0 && found)
        name = QString::fromLocal8Bit(found->pw_name);
#endif
    m_users.insert(uid, name);
    return name;
}

QString Sampler::commandLine(qint64 pid)
{
    auto it = m_commands.constFind(pid);
    if (it != m_commands.cend())
        return *it;
    QString command;
#if defined(Q_OS_LINUX)
    QByteArray raw = readSmall(QStringLiteral("/proc/%1/cmdline").arg(pid));
    while (raw.endsWith('\0'))
        raw.chop(1);
    raw.replace('\0', ' ');
    command = QString::fromUtf8(raw.left(600));
#else
    Q_UNUSED(pid);
#endif
    m_commands.insert(pid, command);
    return command;
}

Process Sampler::readOne(qint64 pid)
{
#if defined(Q_OS_LINUX)
    if (pid <= 0)
        return Process();
    const qint64 pageBytes = qMax(1L, sysconf(_SC_PAGESIZE));
    const QString base = QStringLiteral("/proc/%1").arg(pid);
    Process process = parseStatLine(readSmall(base + QStringLiteral("/stat")), pageBytes);
    if (process.pid != pid)
        return Process();
    struct stat info;
    if (::stat(base.toLocal8Bit().constData(), &info) == 0) {
        process.uid = info.st_uid;
        process.user = userName(info.st_uid);
    }
    const long ticksPerSecond = qMax(1L, sysconf(_SC_CLK_TCK));
    bool ok = false;
    const double uptime =
        readSmall(QStringLiteral("/proc/uptime")).simplified().split(' ').value(0).toDouble(&ok);
    if (ok)
        process.ageSeconds =
            qMax<qint64>(0, qint64(uptime - double(process.startTicks) / ticksPerSecond));
    m_commands.remove(pid);
    process.command = commandLine(pid);
    process.self = pid == QCoreApplication::applicationPid();
    process.forkmesh = process.self;
    return process;
#else
    Q_UNUSED(pid);
    return Process();
#endif
}

Snapshot Sampler::sample(qint64 nowMs)
{
    Snapshot snapshot;
    snapshot.atMs = nowMs;
#if defined(Q_OS_LINUX)
    if (!m_clock.isValid())
        m_clock.start();
    const qint64 elapsedMs = m_clock.elapsed();
    const double seconds =
        m_lastElapsedMs < 0 ? 0.0 : (elapsedMs - m_lastElapsedMs) / 1000.0;
    m_lastElapsedMs = elapsedMs;
    const long ticksPerSecond = qMax(1L, sysconf(_SC_CLK_TCK));
    const qint64 pageBytes = qMax(1L, sysconf(_SC_PAGESIZE));
    double uptime = -1.0;
    {
        const QList<QByteArray> fields =
            readSmall(QStringLiteral("/proc/uptime")).simplified().split(' ');
        bool ok = false;
        uptime = fields.value(0).toDouble(&ok);
        if (!ok)
            uptime = -1.0;
    }

    QHash<qint64, qint64> ticks;
    QHash<qint64, qint64> starts;
    QHash<qint64, QString> commands;
    DIR *dir = opendir("/proc");
    if (!dir) {
        snapshot.reason = QStringLiteral("/proc could not be read");
        return snapshot;
    }
    while (struct dirent *entry = readdir(dir)) {
        const char *name = entry->d_name;
        if (name[0] < '0' || name[0] > '9')
            continue;
        const QString base = QStringLiteral("/proc/") + QLatin1String(name);
        const QByteArray stat = readSmall(base + QStringLiteral("/stat"));
        Process process = parseStatLine(stat, pageBytes);
        if (process.pid <= 0)
            continue;
        struct stat info;
        if (::stat(base.toLocal8Bit().constData(), &info) == 0) {
            process.uid = info.st_uid;
            process.user = userName(info.st_uid);
        }
        if (uptime >= 0.0)
            process.ageSeconds = qMax<qint64>(
                0, qint64(uptime - double(process.startTicks) / ticksPerSecond));
        const bool samePid = m_lastStart.value(process.pid, -1) == process.startTicks;
        if (samePid && seconds > 0.0) {
            const qint64 delta = process.cpuTicks - m_lastTicks.value(process.pid);
            process.cpuPercent =
                qMax(0.0, 100.0 * double(delta) / ticksPerSecond / seconds);
        }
        if (samePid && m_commands.contains(process.pid))
            commands.insert(process.pid, m_commands.value(process.pid));
        ticks.insert(process.pid, process.cpuTicks);
        starts.insert(process.pid, process.startTicks);
        if (process.state == QLatin1Char('R'))
            ++snapshot.running;
        snapshot.threads += process.threads;
        snapshot.processes.append(process);
    }
    closedir(dir);
    m_lastTicks = ticks;
    m_lastStart = starts;
    m_commands = commands;
    markTree(snapshot.processes, QCoreApplication::applicationPid());
    for (const Process &process : std::as_const(snapshot.processes)) {
        if (!process.forkmesh)
            continue;
        ++snapshot.forkmeshCount;
        snapshot.forkmeshCpu += qMax(0.0, process.cpuPercent);
        snapshot.forkmeshRss += process.rssBytes;
    }

    corelogs::NodeVitals &v = snapshot.vitals;
    v.atMs = nowMs;
    v.cpus = int(qMax(1L, sysconf(_SC_NPROCESSORS_ONLN)));
    v.uptimeSeconds = uptime >= 0.0 ? qint64(uptime) : -1;
    const QByteArray meminfo = readSmall(QStringLiteral("/proc/meminfo"));
    v.memTotal = meminfoBytes(meminfo, "MemTotal");
    v.memAvailable = meminfoBytes(meminfo, "MemAvailable");
    v.swapTotal = meminfoBytes(meminfo, "SwapTotal");
    v.swapFree = meminfoBytes(meminfo, "SwapFree");
    const QList<QByteArray> load =
        readSmall(QStringLiteral("/proc/loadavg")).simplified().split(' ');
    if (load.size() >= 4) {
        v.load1 = load.at(0).toDouble();
        v.load5 = load.at(1).toDouble();
        v.load15 = load.at(2).toDouble();
        const QList<QByteArray> procs = load.at(3).split('/');
        v.procsRunning = procs.value(0).toInt();
        v.procsTotal = procs.value(1).toInt();
    }
    const QList<QByteArray> files =
        readSmall(QStringLiteral("/proc/sys/fs/file-nr")).simplified().split(' ');
    if (files.size() >= 3) {
        v.filesOpen = files.at(0).toLongLong() - files.at(1).toLongLong();
        v.filesMax = files.at(2).toLongLong();
    }
    struct statvfs disk;
    if (statvfs(QDir::homePath().toLocal8Bit().constData(), &disk) == 0) {
        v.diskTotal = qint64(disk.f_blocks) * qint64(disk.f_frsize);
        v.diskFree = qint64(disk.f_bavail) * qint64(disk.f_frsize);
        v.inodesTotal = qint64(disk.f_files);
        v.inodesFree = qint64(disk.f_favail);
    }
    const QByteArray cpu = readSmall(QStringLiteral("/proc/stat"));
    if (cpu.startsWith("cpu ")) {
        const QList<QByteArray> fields =
            cpu.left(cpu.indexOf('\n')).simplified().split(' ');
        double total = 0.0;
        double idle = 0.0;
        for (int i = 1; i < fields.size(); ++i) {
            const double value = fields.at(i).toDouble();
            total += value;
            if (i == 4 || i == 5)
                idle += value;
        }
        const double busy = total - idle;
        if (m_lastTotal >= 0.0 && total > m_lastTotal)
            v.cpuBusy = qBound(0.0,
                               100.0 * (busy - m_lastBusy) / (total - m_lastTotal),
                               100.0);
        m_lastBusy = busy;
        m_lastTotal = total;
    }
    v.valid = true;
    snapshot.valid = true;
#else
    snapshot.reason = QStringLiteral("The process list reads /proc, which only "
                                     "Linux has.");
#endif
    return snapshot;
}

} // namespace desktopprocs
} // namespace forkmesh
