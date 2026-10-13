#include "SystemStats.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>

#include <chrono>
#include <climits>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#if defined(Q_OS_LINUX)
#include <dirent.h>
#endif

namespace {

#if defined(Q_OS_LINUX)
// Count directory entries with opendir/readdir rather than QDir::entryList.
// entryList materialises a QStringList of every name and sorts it by default,
// and under /proc each name costs a kernel round trip — for a process holding
// hundreds of descriptors that is a real getdents64 walk, which the footer
// diagnostics timer was paying on the GUI thread every 15 seconds.
int countProcEntries(const char *path, bool digitsOnly)
{
    DIR *dir = opendir(path);
    if (!dir)
        return 0;
    int count = 0;
    while (const dirent *entry = readdir(dir)) {
        const char *name = entry->d_name;
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
        if (digitsOnly && (name[0] < '0' || name[0] > '9'))
            continue;
        ++count;
    }
    closedir(dir);
    return count;
}
#endif

std::chrono::steady_clock::time_point g_lastWall{};
double g_lastCpuSecs = -1.0;  // < 0 until the first sample establishes a baseline
double g_lastPercent = 0.0;

double g_lastHostBusy = -1.0;   // busy jiffies at the previous host sample
double g_lastHostTotal = -1.0;  // total jiffies at the previous host sample
double g_lastHostPercent = 0.0;

double processCpuSeconds()
{
#if defined(Q_OS_LINUX)
    QFile stat(QStringLiteral("/proc/self/stat"));
    if (stat.open(QIODevice::ReadOnly)) {
        const QByteArray data = stat.readAll();
        const int rp = data.lastIndexOf(')');
        if (rp > 0) {
            const QList<QByteArray> fields = data.mid(rp + 2).split(' ');
            if (fields.size() > 12) {
                bool okU = false, okS = false;
                const qulonglong utime = fields.at(11).toULongLong(&okU);
                const qulonglong stime = fields.at(12).toULongLong(&okS);
                const long hz = sysconf(_SC_CLK_TCK);
                if (okU && okS && hz > 0)
                    return double(utime + stime) / double(hz);
            }
        }
    }
#endif
#if defined(Q_OS_UNIX)
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        return double(ru.ru_utime.tv_sec) + double(ru.ru_utime.tv_usec) / 1e6 +
               double(ru.ru_stime.tv_sec) + double(ru.ru_stime.tv_usec) / 1e6;
#endif
    return -1.0;
}

} // namespace

qint64 SystemStats::residentBytes()
{
#if defined(Q_OS_LINUX)
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (statm.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> parts = statm.readAll().trimmed().split(' ');
        if (parts.size() >= 2) {
            bool ok = false;
            const qulonglong pages = parts.at(1).toULongLong(&ok);
            const long pageSize = sysconf(_SC_PAGESIZE);
            if (ok && pageSize > 0)
                return qint64(pages) * qint64(pageSize);
        }
    }
#endif
#if defined(Q_OS_UNIX)
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#if defined(Q_OS_MACOS)
        return qint64(ru.ru_maxrss);
#else
        return qint64(ru.ru_maxrss) * 1024;
#endif
    }
#endif
    return 0;
}

qint64 SystemStats::totalMemoryBytes()
{
#if defined(Q_OS_UNIX) && defined(_SC_PHYS_PAGES)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0)
        return qint64(pages) * qint64(pageSize);
#endif
    return 0;
}

qint64 SystemStats::availableMemoryBytes()
{
#if defined(Q_OS_LINUX)
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = meminfo.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (!line.startsWith("MemAvailable:"))
                continue;
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() >= 2) {
                bool ok = false;
                const qulonglong kib = parts.at(1).toULongLong(&ok);
                if (ok)
                    return qint64(kib) * 1024;
            }
        }
    }
#endif
#if defined(Q_OS_UNIX) && defined(_SC_AVPHYS_PAGES)
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0)
        return qint64(pages) * qint64(pageSize);
#endif
    return 0;
}

qint64 SystemStats::totalSwapBytes()
{
#if defined(Q_OS_LINUX)
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = meminfo.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (!line.startsWith("SwapTotal:"))
                continue;
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() >= 2) {
                bool ok = false;
                const qulonglong kib = parts.at(1).toULongLong(&ok);
                if (ok)
                    return qint64(kib) * 1024;
            }
        }
    }
#endif
    return 0;
}

qint64 SystemStats::freeSwapBytes()
{
#if defined(Q_OS_LINUX)
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = meminfo.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (!line.startsWith("SwapFree:"))
                continue;
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() >= 2) {
                bool ok = false;
                const qulonglong kib = parts.at(1).toULongLong(&ok);
                if (ok)
                    return qint64(kib) * 1024;
            }
        }
    }
#endif
    return 0;
}

qint64 SystemStats::diskTotalBytes(const QString &path)
{
#if defined(Q_OS_UNIX)
    struct statvfs vfs{};
    if (!path.isEmpty() && statvfs(path.toLocal8Bit().constData(), &vfs) == 0)
        return qint64(vfs.f_blocks) * qint64(vfs.f_frsize);
#else
    Q_UNUSED(path);
#endif
    return 0;
}

qint64 SystemStats::diskFreeBytes(const QString &path)
{
#if defined(Q_OS_UNIX)
    struct statvfs vfs{};
    if (!path.isEmpty() && statvfs(path.toLocal8Bit().constData(), &vfs) == 0)
        return qint64(vfs.f_bavail) * qint64(vfs.f_frsize);
#else
    Q_UNUSED(path);
#endif
    return 0;
}

double SystemStats::hostCpuPercent()
{
#if defined(Q_OS_LINUX)
    QFile stat(QStringLiteral("/proc/stat"));
    if (stat.open(QIODevice::ReadOnly)) {
        const QByteArray first = stat.readLine();
        if (first.startsWith("cpu ")) {
            const QList<QByteArray> fields = first.simplified().split(' ');
            double total = 0.0, idle = 0.0;
            for (int i = 1; i < fields.size(); ++i) {
                bool ok = false;
                const double v = double(fields.at(i).toLongLong(&ok));
                if (!ok)
                    continue;
                total += v;
                if (i == 4 || i == 5) // idle + iowait
                    idle += v;
            }
            if (total > 0.0) {
                const double busy = total - idle;
                if (g_lastHostTotal < 0.0) {
                    g_lastHostBusy = busy;
                    g_lastHostTotal = total;
                    g_lastHostPercent = 0.0;
                    return g_lastHostPercent;
                }
                const double dTotal = total - g_lastHostTotal;
                const double dBusy = busy - g_lastHostBusy;
                g_lastHostBusy = busy;
                g_lastHostTotal = total;
                if (dTotal > 0.0) {
                    const double pct = dBusy / dTotal * 100.0;
                    g_lastHostPercent = pct < 0.0 ? 0.0 : (pct > 100.0 ? 100.0 : pct);
                }
                return g_lastHostPercent;
            }
        }
    }
#endif
    return -1.0;
}

double SystemStats::cpuPercent()
{
    const double cpu = processCpuSeconds();
    if (cpu < 0.0)
        return -1.0;
    const auto now = std::chrono::steady_clock::now();
    if (g_lastCpuSecs < 0.0) {
        g_lastCpuSecs = cpu;
        g_lastWall = now;
        g_lastPercent = 0.0;
        return g_lastPercent;
    }
    const double wallSecs =
        std::chrono::duration<double>(now - g_lastWall).count();
    if (wallSecs < 0.05)
        return g_lastPercent; // too soon to resample without amplifying noise
    const double pct = (cpu - g_lastCpuSecs) / wallSecs * 100.0;
    g_lastCpuSecs = cpu;
    g_lastWall = now;
    g_lastPercent = pct < 0.0 ? 0.0 : pct;
    return g_lastPercent;
}

QList<SystemStats::DescendantProcess>
SystemStats::descendantProcesses(qint64 rootPid)
{
    if (rootPid <= 0)
        return {};
    return descendantProcesses(rootPid, processTable());
}

SystemStats::ProcessTable SystemStats::processTable()
{
    ProcessTable table;
#if defined(Q_OS_LINUX)
    const QStringList pids =
        QDir(QStringLiteral("/proc"))
            .entryList(QStringList() << QStringLiteral("[0-9]*"), QDir::Dirs);
    table.entries.reserve(pids.size());
    for (const QString &name : pids) {
        bool pidOk = false;
        const qint64 pid = name.toLongLong(&pidOk);
        if (!pidOk || pid <= 0)
            continue;
        QFile stat(QStringLiteral("/proc/%1/stat").arg(name));
        if (!stat.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = stat.readAll();
        const int lp = data.indexOf('(');
        const int rp = data.lastIndexOf(')');
        if (lp < 0 || rp <= lp)
            continue;
        const QList<QByteArray> fields = data.mid(rp + 2).split(' ');
        if (fields.size() < 2 || fields.at(0).isEmpty())
            continue;
        bool parentOk = false;
        const qint64 parentPid = fields.at(1).toLongLong(&parentOk);
        if (!parentOk)
            continue;
        const bool zombie = fields.at(0).at(0) == 'Z';
        // /proc/<pid>/stat fields 14 (utime) and 15 (stime); this split starts at
        // field 3 (state), so they land at indexes 11 and 12.
        qint64 cpuTicks = 0;
        if (fields.size() > 12)
            cpuTicks = fields.at(11).toLongLong() + fields.at(12).toLongLong();
        table.entries.insert(pid, {parentPid,
                                   QString::fromLocal8Bit(data.mid(lp + 1, rp - lp - 1)),
                                   zombie, cpuTicks});
        table.children[parentPid].append(pid);
    }
#endif
    return table;
}

QList<SystemStats::DescendantProcess>
SystemStats::descendantProcesses(qint64 rootPid, const ProcessTable &table)
{
    QList<DescendantProcess> result;
    if (rootPid <= 0)
        return result;
    QList<qint64> queue{rootPid};
    QSet<qint64> seen{rootPid}; // a racing PID reuse must not make a cycle
    while (!queue.isEmpty()) {
        const qint64 pid = queue.takeLast();
        const auto entry = table.entries.constFind(pid);
        if (entry != table.entries.constEnd() && pid != rootPid && !entry->zombie)
            result.append({pid, entry->parentPid, entry->command, entry->cpuTicks});
        for (qint64 child : table.children.value(pid)) {
            if (!seen.contains(child)) {
                seen.insert(child);
                queue.append(child);
            }
        }
    }
    return result;
}

SystemStats::DescendantLoad SystemStats::descendantsNamed(qint64 rootPid,
                                                          const QString &comm)
{
    DescendantLoad load;
    if (rootPid <= 0 || comm.isEmpty())
        return load;
#if defined(Q_OS_LINUX)
    const QByteArray wanted = comm.toLocal8Bit();
    struct Entry {
        qint64 ppid = 0;
        bool matches = false;
    };
    QHash<qint64, Entry> entries;
    QHash<qint64, QList<qint64>> children;
    const QStringList pids =
        QDir(QStringLiteral("/proc"))
            .entryList(QStringList() << QStringLiteral("[0-9]*"), QDir::Dirs);
    for (const QString &name : pids) {
        bool pidOk = false;
        const qint64 pid = name.toLongLong(&pidOk);
        if (!pidOk || pid <= 0)
            continue;
        QFile stat(QStringLiteral("/proc/%1/stat").arg(name));
        if (!stat.open(QIODevice::ReadOnly))
            continue; // the process exited between listing and reading
        const QByteArray data = stat.readAll();
        const int lp = data.indexOf('(');
        const int rp = data.lastIndexOf(')');
        if (lp < 0 || rp <= lp)
            continue;
        const QList<QByteArray> fields = data.mid(rp + 2).split(' ');
        if (fields.size() < 2)
            continue;
        bool ppidOk = false;
        const qint64 ppid = fields.at(1).toLongLong(&ppidOk);
        if (!ppidOk)
            continue;
        entries.insert(pid, {ppid, data.mid(lp + 1, rp - lp - 1) == wanted});
        children[ppid].append(pid);
    }
    QList<qint64> queue{rootPid};
    QSet<qint64> seen{rootPid}; // PID reuse can't turn the walk into a cycle
    while (!queue.isEmpty()) {
        const qint64 pid = queue.takeLast();
        const auto entry = entries.constFind(pid);
        if (entry != entries.constEnd() && entry->matches && pid != rootPid) {
            ++load.count;
            QFile statm(QStringLiteral("/proc/%1/statm").arg(pid));
            if (statm.open(QIODevice::ReadOnly)) {
                const QList<QByteArray> fields =
                    statm.readAll().simplified().split(' ');
                bool rssOk = false;
                const qulonglong pages =
                    fields.size() > 1 ? fields.at(1).toULongLong(&rssOk) : 0;
                const long pageSize = sysconf(_SC_PAGESIZE);
                if (rssOk && pageSize > 0)
                    load.residentBytes += qint64(pages) * qint64(pageSize);
            }
        }
        for (qint64 child : children.value(pid)) {
            if (!seen.contains(child)) {
                seen.insert(child);
                queue.append(child);
            }
        }
    }
#endif
    return load;
}

int SystemStats::openFileCount()
{
#if defined(Q_OS_LINUX)
    // Every entry is a symlink, and most of them (sockets, pipes, eventfds,
    // deleted files) dangle, so nothing here may stat them: readdir only.
    return countProcEntries("/proc/self/fd", /*digitsOnly=*/false);
#else
    return 0;
#endif
}

int SystemStats::openFileSoftLimit()
{
#if defined(Q_OS_UNIX)
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
        limit.rlim_cur != RLIM_INFINITY)
        return int(qMin<qint64>(qint64(limit.rlim_cur), INT_MAX));
#endif
    return 0;
}

int SystemStats::openFileHardLimit()
{
#if defined(Q_OS_UNIX)
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
        limit.rlim_max != RLIM_INFINITY)
        return int(qMin<qint64>(qint64(limit.rlim_max), INT_MAX));
#endif
    return 0;
}

int SystemStats::threadCount()
{
#if defined(Q_OS_LINUX)
    // Task ids are all numeric, which is what the old "[0-9]*" name filter
    // selected; "." and ".." never matched it either.
    const int threads = countProcEntries("/proc/self/task", /*digitsOnly=*/true);
    if (threads > 0)
        return threads;
#endif
    return 1;
}

SystemStats::Termination SystemStats::requestTermination(qint64 pid,
                                                         QString *error)
{
#if defined(Q_OS_UNIX)
    if (pid <= 0)
        return Termination::Failed;
    if (::kill(static_cast<pid_t>(pid), SIGTERM) == 0)
        return Termination::Requested;
    const int failure = errno;
    if (failure == ESRCH)
        return Termination::AlreadyExited;
    if (error)
        *error = QString::fromLocal8Bit(std::strerror(failure));
    return Termination::Failed;
#else
    Q_UNUSED(pid);
    Q_UNUSED(error);
    return Termination::Unsupported;
#endif
}

int SystemStats::raiseOpenFileLimit()
{
#if defined(Q_OS_UNIX)
    constexpr rlim_t kTarget = 65536;

    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return 0;
    if (limit.rlim_cur == RLIM_INFINITY)
        return 0; // already unbounded; openFileSoftLimit() reports 0 for that
    const rlim_t ceiling = limit.rlim_max == RLIM_INFINITY
                               ? kTarget
                               : qMin<rlim_t>(limit.rlim_max, kTarget);
    for (rlim_t want = ceiling; want > limit.rlim_cur; want /= 2) {
        rlimit raised = limit;
        raised.rlim_cur = want;
        if (::setrlimit(RLIMIT_NOFILE, &raised) == 0)
            break;
    }
#endif
    return openFileSoftLimit();
}

QString SystemStats::formatBytes(qint64 bytes)
{
    if (bytes <= 0)
        return QStringLiteral("0 B");
    const double kib = double(bytes) / 1024.0;
    if (kib < 1024.0)
        return QStringLiteral("%1 KB").arg(kib, 0, 'f', 0);
    const double mib = kib / 1024.0;
    if (mib < 1024.0)
        return QStringLiteral("%1 MB").arg(mib, 0, 'f', 1);
    const double gib = mib / 1024.0;
    return QStringLiteral("%1 GB").arg(gib, 0, 'f', 2);
}
