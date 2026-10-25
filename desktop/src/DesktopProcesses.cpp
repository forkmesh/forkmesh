#include "DesktopProcesses.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

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

QByteArray readSmall(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.read(64 * 1024);
}

QString readText(const QString &path)
{
    return QString::fromUtf8(readSmall(path)).trimmed();
}

qint64 readNumber(const QString &path)
{
    bool ok = false;
    const qint64 value = readSmall(path).trimmed().toLongLong(&ok);
    return ok ? value : -1;
}

QString summarize(const QMap<QString, int> &counts)
{
    if (counts.isEmpty())
        return QString();
    if (counts.size() == 1)
        return counts.firstKey();
    QStringList parts;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        parts.append(QStringLiteral("%1 \u00d7%2").arg(it.key()).arg(it.value()));
    return QStringLiteral("mixed: ") + parts.join(QStringLiteral(", "));
}

qint64 cacheSizeBytes(const QString &text)
{
    QString value = text.trimmed().toUpper();
    qint64 scale = 1;
    if (value.endsWith(QLatin1Char('K')))
        scale = 1024;
    else if (value.endsWith(QLatin1Char('M')))
        scale = 1024 * 1024;
    if (scale > 1)
        value.chop(1);
    bool ok = false;
    const qint64 number = value.toLongLong(&ok);
    return ok ? number * scale : -1;
}

#if defined(Q_OS_LINUX)

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

QList<int> parseCpuList(const QString &text)
{
    QList<int> cpus;
    for (const QString &part : text.trimmed().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int dash = part.indexOf(QLatin1Char('-'));
        bool firstOk = false;
        bool lastOk = true;
        const int first = (dash < 0 ? part : part.left(dash)).trimmed().toInt(&firstOk);
        const int last = dash < 0 ? first : part.mid(dash + 1).trimmed().toInt(&lastOk);
        if (!firstOk || !lastOk || first < 0 || last < first || last - first > 4096)
            continue;
        for (int cpu = first; cpu <= last; ++cpu)
            cpus.append(cpu);
    }
    return cpus;
}

QHash<int, CpuTicks> parsePerCpuTicks(const QByteArray &procStat)
{
    QHash<int, CpuTicks> ticks;
    for (const QByteArray &line : procStat.split('\n')) {
        if (!line.startsWith("cpu"))
            continue;
        if (line.size() < 4 || line.at(3) < '0' || line.at(3) > '9')
            continue;
        const QList<QByteArray> fields = line.simplified().split(' ');
        bool ok = false;
        const int cpu = fields.at(0).mid(3).toInt(&ok);
        if (!ok || fields.size() < 5)
            continue;
        CpuTicks entry;
        qint64 idle = 0;
        for (int i = 1; i < qMin(fields.size(), 9); ++i) {
            const qint64 value = fields.at(i).toLongLong();
            entry.total += value;
            if (i == 4 || i == 5)
                idle += value;
        }
        entry.busy = entry.total - idle;
        ticks.insert(cpu, entry);
    }
    return ticks;
}

QString selectedChoice(const QString &text)
{
    const int open = text.indexOf(QLatin1Char('['));
    const int close = text.indexOf(QLatin1Char(']'), open + 1);
    if (open >= 0 && close > open)
        return text.mid(open + 1, close - open - 1).trimmed();
    return text.trimmed();
}

double pressureSome(const QByteArray &text)
{
    for (const QByteArray &line : text.split('\n')) {
        if (!line.startsWith("some "))
            continue;
        for (const QByteArray &field : line.simplified().split(' ')) {
            if (!field.startsWith("avg10="))
                continue;
            bool ok = false;
            const double value = field.mid(6).toDouble(&ok);
            return ok ? value : -1.0;
        }
    }
    return -1.0;
}

QString shortCpuModel(const QString &model)
{
    QString text = model;
    static const QRegularExpression noise(
        QStringLiteral("\\((R|TM|tm|r)\\)|\\bCPU\\b|\\bProcessor\\b|\\b\\d+-Core\\b|@.*$"));
    text.remove(noise);
    return text.simplified();
}

QList<SwapDevice> parseSwaps(const QByteArray &text)
{
    QList<SwapDevice> swaps;
    const QList<QByteArray> lines = text.split('\n');
    for (int i = 1; i < lines.size(); ++i) {
        const QList<QByteArray> fields = lines.at(i).simplified().split(' ');
        if (fields.size() < 4)
            continue;
        SwapDevice device;
        device.type = fields.at(0).contains("/zram") ? QStringLiteral("zram")
                                                     : QString::fromUtf8(fields.at(1));
        device.sizeBytes = fields.at(2).toLongLong() * 1024;
        device.usedBytes = fields.at(3).toLongLong() * 1024;
        swaps.append(device);
    }
    return swaps;
}

Machine readMachine(const QString &root)
{
    Machine machine;
    const QString cpuRoot = root + QStringLiteral("/sys/devices/system/cpu");
    QList<int> online = parseCpuList(readText(cpuRoot + QStringLiteral("/online")));
    if (online.isEmpty()) {
        const QStringList entries = QDir(cpuRoot).entryList(
            {QStringLiteral("cpu*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            bool ok = false;
            const int cpu = entry.mid(3).toInt(&ok);
            if (ok)
                online.append(cpu);
        }
        std::sort(online.begin(), online.end());
    }
    if (online.isEmpty())
        return machine;
    const QList<int> pList = parseCpuList(readText(root + QStringLiteral("/sys/devices/cpu_core/cpus")));
    const QList<int> eList = parseCpuList(readText(root + QStringLiteral("/sys/devices/cpu_atom/cpus")));
    QSet<QPair<int, int>> physical;
    for (const int cpu : std::as_const(online)) {
        const QString base = cpuRoot + QStringLiteral("/cpu%1").arg(cpu);
        Core core;
        core.cpu = cpu;
        core.coreId = int(readNumber(base + QStringLiteral("/topology/core_id")));
        core.package = int(qMax<qint64>(0, readNumber(base + QStringLiteral("/topology/physical_package_id"))));
        if (pList.contains(cpu))
            core.kind = QStringLiteral("P");
        else if (eList.contains(cpu))
            core.kind = QStringLiteral("E");
        core.maxKhz = readNumber(base + QStringLiteral("/cpufreq/cpuinfo_max_freq"));
        physical.insert({core.package, core.coreId < 0 ? cpu : core.coreId});
        machine.cores.append(core);
    }
    machine.physicalCores = physical.size();
    const QString cpu0 = cpuRoot + QStringLiteral("/cpu%1").arg(online.first());
    machine.governors = readText(cpu0 + QStringLiteral("/cpufreq/scaling_available_governors"))
                            .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    machine.driver = readText(cpu0 + QStringLiteral("/cpufreq/scaling_driver"));
    machine.baseKhz = readNumber(cpu0 + QStringLiteral("/cpufreq/base_frequency"));
    for (int index = 0; index < 10; ++index) {
        const QString cache = cpu0 + QStringLiteral("/cache/index%1").arg(index);
        if (readNumber(cache + QStringLiteral("/level")) == 3) {
            machine.cacheL3Bytes = cacheSizeBytes(readText(cache + QStringLiteral("/size")));
            break;
        }
    }
    const QString cpuinfo = QString::fromUtf8(readSmall(root + QStringLiteral("/proc/cpuinfo")));
    for (const QString &line : cpuinfo.split(QLatin1Char('\n'))) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString key = line.left(colon).trimmed();
        if (key == QLatin1String("model name") || key == QLatin1String("Model")) {
            machine.cpuModel = shortCpuModel(line.mid(colon + 1));
            break;
        }
    }
    machine.kernel = readText(root + QStringLiteral("/proc/sys/kernel/osrelease"));
    const QStringList nodes = QDir(root + QStringLiteral("/sys/devices/system/node"))
                                  .entryList({QStringLiteral("node*")}, QDir::Dirs);
    machine.numaNodes = nodes.size();

    QHash<QPair<int, int>, QString> coreTemps;
    const QDir hwmon(root + QStringLiteral("/sys/class/hwmon"));
    for (const QString &entry : hwmon.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = hwmon.filePath(entry);
        const QString name = readText(base + QStringLiteral("/name"));
        if (name != QLatin1String("coretemp") && name != QLatin1String("k10temp") &&
            name != QLatin1String("zenpower") && name != QLatin1String("cpu_thermal"))
            continue;
        int package = 0;
        QHash<int, QString> labelled;
        const QStringList inputs = QDir(base).entryList({QStringLiteral("temp*_input")}, QDir::Files);
        for (const QString &input : inputs) {
            const QString path = base + QLatin1Char('/') + input;
            QString label = readText(QString(path).replace(QStringLiteral("_input"), QStringLiteral("_label")));
            if (label.startsWith(QLatin1String("Package id "))) {
                package = label.mid(11).toInt();
                if (machine.packageTempPath.isEmpty())
                    machine.packageTempPath = path;
            } else if (label.startsWith(QLatin1String("Core "))) {
                labelled.insert(label.mid(5).toInt(), path);
            } else if ((label == QLatin1String("Tctl") || label == QLatin1String("Tdie") ||
                        label.isEmpty()) &&
                       machine.packageTempPath.isEmpty()) {
                machine.packageTempPath = path;
            }
        }
        for (auto it = labelled.cbegin(); it != labelled.cend(); ++it)
            coreTemps.insert({package, it.key()}, it.value());
    }
    for (Core &core : machine.cores)
        core.tempPath = coreTemps.value({core.package, core.coreId});

    machine.valid = true;
    refreshMachine(machine, root);
    return machine;
}

void refreshMachine(Machine &machine, const QString &root)
{
    const QString cpuRoot = root + QStringLiteral("/sys/devices/system/cpu");
    QMap<QString, int> governors;
    QMap<QString, int> preferences;
    machine.minKhz = -1;
    machine.maxKhz = -1;
    for (Core &core : machine.cores) {
        const QString freq = cpuRoot + QStringLiteral("/cpu%1/cpufreq").arg(core.cpu);
        core.curKhz = readNumber(freq + QStringLiteral("/scaling_cur_freq"));
        const QString governor = readText(freq + QStringLiteral("/scaling_governor"));
        if (!governor.isEmpty())
            ++governors[governor];
        const QString preference = readText(freq + QStringLiteral("/energy_performance_preference"));
        if (!preference.isEmpty())
            ++preferences[preference];
        const qint64 low = readNumber(freq + QStringLiteral("/scaling_min_freq"));
        const qint64 high = readNumber(freq + QStringLiteral("/scaling_max_freq"));
        if (low > 0)
            machine.minKhz = machine.minKhz < 0 ? low : qMin(machine.minKhz, low);
        if (high > 0)
            machine.maxKhz = qMax(machine.maxKhz, high);
        const qint64 temp = core.tempPath.isEmpty() ? -1 : readNumber(core.tempPath);
        core.tempC = temp >= 0 ? temp / 1000.0 : -1.0;
    }
    machine.governor = summarize(governors);
    machine.energyPreference = summarize(preferences);
    machine.driverStatus = readText(cpuRoot + QStringLiteral("/intel_pstate/status"));
    if (machine.driverStatus.isEmpty())
        machine.driverStatus = readText(cpuRoot + QStringLiteral("/amd_pstate/status"));
    const qint64 noTurbo = readNumber(cpuRoot + QStringLiteral("/intel_pstate/no_turbo"));
    const qint64 boost = readNumber(cpuRoot + QStringLiteral("/cpufreq/boost"));
    machine.turbo = noTurbo >= 0 ? int(noTurbo == 0) : boost >= 0 ? int(boost != 0) : -1;
    machine.smt = readText(cpuRoot + QStringLiteral("/smt/control"));
    const qint64 package = machine.packageTempPath.isEmpty() ? -1 : readNumber(machine.packageTempPath);
    machine.packageTempC = package >= 0 ? package / 1000.0 : -1.0;
    const QString pressure = root + QStringLiteral("/proc/pressure/");
    machine.pressureCpu = pressureSome(readSmall(pressure + QStringLiteral("cpu")));
    machine.pressureMemory = pressureSome(readSmall(pressure + QStringLiteral("memory")));
    machine.pressureIo = pressureSome(readSmall(pressure + QStringLiteral("io")));
    const QString thp = root + QStringLiteral("/sys/kernel/mm/transparent_hugepage/");
    machine.hugePages = selectedChoice(readText(thp + QStringLiteral("enabled")));
    machine.hugePagesDefrag = selectedChoice(readText(thp + QStringLiteral("defrag")));
    const QString vm = root + QStringLiteral("/proc/sys/vm/");
    machine.swappiness = int(readNumber(vm + QStringLiteral("swappiness")));
    machine.overcommit = int(readNumber(vm + QStringLiteral("overcommit_memory")));
    machine.dirtyRatio = int(readNumber(vm + QStringLiteral("dirty_ratio")));
    machine.dirtyBackgroundRatio = int(readNumber(vm + QStringLiteral("dirty_background_ratio")));
    machine.cachePressure = int(readNumber(vm + QStringLiteral("vfs_cache_pressure")));
    const QString zswap = readText(root + QStringLiteral("/sys/module/zswap/parameters/enabled"));
    machine.zswap = zswap.isEmpty() ? QString()
                    : zswap == QLatin1String("Y") || zswap == QLatin1String("1")
                        ? QStringLiteral("on")
                        : QStringLiteral("off");
    machine.swaps = parseSwaps(readSmall(root + QStringLiteral("/proc/swaps")));
}

void applyCpuTicks(Machine &machine, const QHash<int, CpuTicks> &previous,
                   const QHash<int, CpuTicks> &current)
{
    for (Core &core : machine.cores) {
        core.busy = -1.0;
        const auto before = previous.constFind(core.cpu);
        const auto now = current.constFind(core.cpu);
        if (before == previous.cend() || now == current.cend())
            continue;
        const qint64 total = now->total - before->total;
        if (total <= 0)
            continue;
        core.busy = qBound(0.0, 100.0 * double(now->busy - before->busy) / double(total), 100.0);
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
    const QHash<int, CpuTicks> cpuTicks = parsePerCpuTicks(cpu);
    if (m_machine.valid)
        refreshMachine(m_machine, QString());
    else
        m_machine = readMachine(QString());
    applyCpuTicks(m_machine, m_lastCpuTicks, cpuTicks);
    m_lastCpuTicks = cpuTicks;
    snapshot.machine = m_machine;
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
