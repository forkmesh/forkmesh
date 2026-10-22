#include "NodeDiagnostics.h"
#include "AppPaths.h"

#include "LogSource.h"
#include "SystemStats.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QLocale>
#include <QMutexLocker>
#include <algorithm>

#if defined(Q_OS_UNIX)
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace NodeDiagnostics {

namespace {

constexpr qint64 kDayMs = 24 * 60 * 60 * 1000;
constexpr qint64 kMinTrendBytesPerDay = 64LL * 1024 * 1024;
constexpr double kDiskTrendCriticalDays = 2.0;
constexpr double kDiskTrendWarningDays = 14.0;
constexpr double kDiskTrendInfoDays = 60.0;
constexpr double kInodeCriticalPct = 95.0;
constexpr double kInodeWarningPct = 85.0;
constexpr double kFdCriticalPct = 90.0;
constexpr double kFdWarningPct = 75.0;
constexpr int kZombieCritical = 100;
constexpr int kZombieWarning = 25;
constexpr int kLogErrorWarning = 10;
constexpr int kStallWarning = 10;
constexpr int kStallInfo = 3;
constexpr int kRelayDropWarning = 5;
constexpr int kRelayDropInfo = 2;
// Signed relay/worker requests are rejected once a clock drifts this far.
constexpr qint64 kClockSkewCriticalMs = 5 * 60 * 1000;
constexpr qint64 kClockSkewWarningMs = 2 * 60 * 1000;
constexpr qint64 kMaxLogTailBytes = 256 * 1024;
constexpr int kMaxLogMatchesPerScan = 200;

QString formatCount(qint64 n)
{
    static const QLocale locale(QLocale::English, QLocale::UnitedStates);
    return locale.toString(n);
}

QString formatHorizon(double days)
{
    if (days >= 2.0)
        return QStringLiteral("~%1 days").arg(days, 0, 'f', 0);
    const double hours = days * 24.0;
    if (hours >= 2.0)
        return QStringLiteral("~%1 hours").arg(hours, 0, 'f', 0);
    return QStringLiteral("~%1 minutes").arg(qMax(1.0, hours * 60.0), 0, 'f', 0);
}

QString formatDuration(qint64 ms)
{
    const qint64 seconds = qAbs(ms) / 1000;
    if (seconds < 90)
        return QStringLiteral("%1s").arg(seconds);
    if (seconds < 90 * 60)
        return QStringLiteral("%1m").arg(seconds / 60);
    return QStringLiteral("%1h").arg(seconds / 3600);
}

QString formatWindow(qint64 windowMs)
{
    return formatDuration(windowMs > 0 ? windowMs : kWindowMs);
}

void add(QList<Finding> &out, const QString &id, int severity, const QString &message)
{
    Finding f;
    f.id = id;
    f.severity = severity;
    f.message = message.left(kMaxMessageChars);
    out.append(f);
}

bool lineLooksLikeError(const QString &line)
{
    forkmesh::LogSeverity severity = forkmesh::LogSeverity::Automatic;
    QString body;
    if (forkmesh::splitLogSource(
            line, &body, nullptr, nullptr, &severity)) {
        if (severity == forkmesh::LogSeverity::Informational)
            return false;
        if (severity == forkmesh::LogSeverity::Error)
            return true;
    }
    static const char *kNeedles[] = {"error",  "failed",    "failure", "fatal",
                                     "crash",  "exception", "traceback",
                                     "denied", "refused"};
    for (const char *needle : kNeedles) {
        if (line.contains(QLatin1String(needle), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// Turn the compact, heartbeat-safe finding into useful operator guidance. The
// wire message stays deliberately small; every client can expand the stable
// check id locally without making presence frames larger.
QString remediationFor(const QString &id)
{
    if (id == QLatin1String("disk-trend"))
        return QStringLiteral(
            "Check which repositories, logs, artifacts, or temporary packs are "
            "growing on the node; confirm the trend with filesystem usage over "
            "time, then free space or expand the volume before syncing again.");
    if (id == QLatin1String("disk-inodes"))
        return QStringLiteral(
            "Check inode usage by directory (not only free bytes), remove safe "
            "small-file caches or stale worktrees, and verify the node can create "
            "a file in its data directory afterward.");
    if (id == QLatin1String("data-dir"))
        return QStringLiteral(
            "Inspect the data-directory owner, mode, mount flags, and available "
            "space. Restore write access for the ForkMesh service account, then "
            "verify a mirror fetch and served-repository update both succeed.");
    if (id == QLatin1String("file-descriptors"))
        return QStringLiteral(
            "Inspect open descriptors by type and repeated target, check for "
            "leaked sockets/files or an unexpectedly low process limit, and "
            "verify the count remains stable during reconnect and clone traffic.");
    if (id == QLatin1String("zombie-processes"))
        return QStringLiteral(
            "Identify the unreaped child commands and their parent launch path; "
            "ensure every Git or agent helper is waited for on success, failure, "
            "timeout, and cancellation, then verify the zombie count returns to zero.");
    if (id == QLatin1String("log-errors"))
        return QStringLiteral(
            "Read the surrounding node log entries, preserve the first causal "
            "error rather than only the final failure, reproduce that operation, "
            "and verify a clean retry no longer adds matching errors.");
    if (id == QLatin1String("ui-stalls"))
        return QStringLiteral(
            "Inspect the UI-stall backtraces and app log, find blocking work on "
            "the GUI thread, move slow Git/network/disk work off that thread, and "
            "verify the operation remains responsive under the same load.");
    if (id == QLatin1String("relay-flap"))
        return QStringLiteral(
            "Correlate disconnect times with relay, DNS, TLS, proxy, sleep, and "
            "network logs; fix the first failing layer and verify heartbeats stay "
            "connected through several refresh intervals.");
    if (id == QLatin1String("relay-backpressure"))
        return QStringLiteral(
            "Inspect outbound queue growth and slow socket writes, bound or "
            "coalesce bursty messages, and verify sustained traffic produces no "
            "additional dropped frames.");
    if (id == QLatin1String("clock-skew"))
        return QStringLiteral(
            "Check the timezone-independent system clock and time-sync service, "
            "correct the drift, then verify signed heartbeats and mirror leases "
            "are accepted without timestamp errors.");
    return QStringLiteral(
        "Inspect the node log around the report time, reproduce the affected "
        "operation, correct the root cause, and rerun the self-check to verify it clears.");
}

qint64 nowOr(qint64 nowMs)
{
    return nowMs > 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
}

void pruneOlderThan(QList<qint64> &times, qint64 cutoffMs)
{
    while (!times.isEmpty() && times.first() < cutoffMs)
        times.removeFirst();
}

} // namespace

QList<Finding> evaluate(const Inputs &in)
{
    QList<Finding> out;
    const QString window = formatWindow(in.windowMs);

    if (in.diskHistory.size() >= 2) {
        const DiskSample first = in.diskHistory.first();
        const DiskSample last = in.diskHistory.last();
        const qint64 spanMs = last.tsMs - first.tsMs;
        const qint64 lostBytes = first.freeBytes - last.freeBytes;
        if (spanMs >= kMinTrendSpanMs && lostBytes > 0 && last.freeBytes > 0) {
            const double perDay = double(lostBytes) * double(kDayMs) / double(spanMs);
            if (perDay >= double(kMinTrendBytesPerDay)) {
                const double days = double(last.freeBytes) / perDay;
                int severity = Ok;
                if (days < kDiskTrendCriticalDays)
                    severity = Critical;
                else if (days < kDiskTrendWarningDays)
                    severity = Warning;
                else if (days < kDiskTrendInfoDays)
                    severity = Info;
                if (severity != Ok) {
                    add(out, QStringLiteral("disk-trend"), severity,
                        QStringLiteral("Disk filling: %1/day lost, %2 free, full in %3")
                            .arg(SystemStats::formatBytes(qint64(perDay)),
                                 SystemStats::formatBytes(last.freeBytes),
                                 formatHorizon(days)));
                }
            }
        }
    }

    if (in.inodesTotal > 0 && in.inodesFree >= 0 && in.inodesFree <= in.inodesTotal) {
        const double usedPct =
            100.0 * double(in.inodesTotal - in.inodesFree) / double(in.inodesTotal);
        if (usedPct >= kInodeWarningPct) {
            add(out, QStringLiteral("disk-inodes"),
                usedPct >= kInodeCriticalPct ? Critical : Warning,
                QStringLiteral("Filesystem inodes %1% used (%2 of %3 free) — writes "
                               "fail before the disk does")
                    .arg(usedPct, 0, 'f', 0)
                    .arg(formatCount(in.inodesFree), formatCount(in.inodesTotal)));
        }
    }

    if (!in.dataDirWritable) {
        add(out, QStringLiteral("data-dir"), Critical,
            QStringLiteral("Data directory is not writable — serving and mirror "
                           "sync will fail (read-only mount or permissions)"));
    }

    if (in.openFileDescriptors >= 0 && in.fileDescriptorLimit > 0) {
        const double usedPct =
            100.0 * double(in.openFileDescriptors) / double(in.fileDescriptorLimit);
        if (usedPct >= kFdWarningPct) {
            add(out, QStringLiteral("file-descriptors"),
                usedPct >= kFdCriticalPct ? Critical : Warning,
                QStringLiteral("Open files %1 of %2 (%3%) — new connections will "
                               "start failing")
                    .arg(formatCount(in.openFileDescriptors),
                         formatCount(in.fileDescriptorLimit))
                    .arg(usedPct, 0, 'f', 0));
        }
    }

    if (in.zombieProcesses >= kZombieWarning) {
        add(out, QStringLiteral("zombie-processes"),
            in.zombieProcesses >= kZombieCritical ? Critical : Warning,
            QStringLiteral("%1 defunct child processes are not being reaped — a "
                           "git or agent helper is leaking")
                .arg(in.zombieProcesses));
    }

    if (in.logErrors > 0) {
        QString message = QStringLiteral("%1 error%2 in the log in the last %3")
                              .arg(in.logErrors)
                              .arg(in.logErrors == 1 ? "" : "s")
                              .arg(window);
        add(out, QStringLiteral("log-errors"),
            in.logErrors >= kLogErrorWarning ? Warning : Info, message);
        out.last().detail = in.logErrorSample.trimmed();
    }

    if (in.uiStalls >= kStallInfo) {
        add(out, QStringLiteral("ui-stalls"),
            in.uiStalls >= kStallWarning ? Warning : Info,
            QStringLiteral("%1 UI stalls recorded in the last %2 — the event loop "
                           "is being blocked")
                .arg(in.uiStalls)
                .arg(window));
    }

    if (in.relayDrops >= kRelayDropInfo) {
        add(out, QStringLiteral("relay-flap"),
            in.relayDrops >= kRelayDropWarning ? Warning : Info,
            QStringLiteral("Relay link dropped %1 times in the last %2")
                .arg(in.relayDrops)
                .arg(window));
    }

    if (in.backpressureDrops > 0) {
        add(out, QStringLiteral("relay-backpressure"), Warning,
            QStringLiteral("%1 frames dropped in the last %2 — the relay socket "
                           "is backing up")
                .arg(formatCount(in.backpressureDrops), window));
    }

    // A drifted clock breaks signed heartbeats and mirror leases long before
    // anyone thinks to check `date` on the box.
    if (in.clockSkewKnown && qAbs(in.clockSkewMs) >= kClockSkewWarningMs) {
        add(out, QStringLiteral("clock-skew"),
            qAbs(in.clockSkewMs) >= kClockSkewCriticalMs ? Critical : Warning,
            QStringLiteral("Clock is %1 %2 the rest of the mesh — signed requests "
                           "may be rejected")
                .arg(formatDuration(in.clockSkewMs),
                     in.clockSkewMs > 0 ? QStringLiteral("ahead of")
                                        : QStringLiteral("behind")));
    }

    std::sort(out.begin(), out.end(), [](const Finding &a, const Finding &b) {
        if (a.severity != b.severity)
            return a.severity > b.severity;
        return a.id < b.id;
    });
    return out;
}

int worstSeverity(const QList<Finding> &findings)
{
    int worst = Ok;
    for (const Finding &f : findings)
        worst = qMax(worst, f.severity);
    return worst;
}

QString severityName(int severity)
{
    switch (severity) {
    case Critical:
        return QStringLiteral("Critical");
    case Warning:
        return QStringLiteral("Warning");
    case Info:
        return QStringLiteral("Info");
    default:
        return QStringLiteral("OK");
    }
}

QString summaryLabel(const QList<Finding> &findings, bool reported)
{
    if (!reported)
        return QString::fromUtf8("\xE2\x80\x94"); // em dash: never reported
    if (findings.isEmpty())
        return QStringLiteral("OK");
    int critical = 0;
    int warnings = 0;
    int infos = 0;
    for (const Finding &f : findings) {
        if (f.severity >= Critical)
            ++critical;
        else if (f.severity == Warning)
            ++warnings;
        else
            ++infos;
    }
    QStringList parts;
    if (critical > 0)
        parts << QStringLiteral("%1 critical").arg(critical);
    if (warnings > 0)
        parts << QStringLiteral("%1 warning%2").arg(warnings).arg(warnings == 1 ? "" : "s");
    if (parts.isEmpty() && infos > 0)
        parts << QStringLiteral("%1 note%2").arg(infos).arg(infos == 1 ? "" : "s");
    return parts.join(QString::fromUtf8(" \xC2\xB7 "));
}

QString detailText(const QList<Finding> &findings)
{
    QStringList lines;
    for (const Finding &f : findings) {
        const QString message =
            f.detail.isEmpty() ? f.message
                               : f.message + QStringLiteral(": ") + f.detail;
        lines << QStringLiteral("%1 [%2]: %3\nNext: %4")
                     .arg(severityName(f.severity), f.id, message,
                          remediationFor(f.id));
    }
    return lines.join(QStringLiteral("\n\n"));
}

QJsonArray toJson(const QList<Finding> &findings)
{
    QJsonArray array;
    for (const Finding &f : findings) {
        if (array.size() >= kMaxWireFindings)
            break;
        if (f.id.isEmpty() || f.message.isEmpty())
            continue;
        array.append(QJsonObject{{"i", f.id.left(32)},
                                 {"s", qBound(int(Ok), f.severity, int(Critical))},
                                 {"m", f.message.left(kMaxMessageChars)}});
    }
    return array;
}

QList<Finding> fromJson(const QJsonArray &array)
{
    QList<Finding> findings;
    for (const QJsonValue &value : array) {
        if (findings.size() >= kMaxWireFindings)
            break;
        const QJsonObject object = value.toObject();
        Finding f;
        f.id = object.value(QStringLiteral("i")).toString().left(32);
        f.severity = qBound(int(Ok), object.value(QStringLiteral("s")).toInt(Info),
                            int(Critical));
        f.message = object.value(QStringLiteral("m")).toString().left(kMaxMessageChars);
        if (f.id.isEmpty() || f.message.isEmpty())
            continue;
        findings.append(f);
    }
    std::sort(findings.begin(), findings.end(),
              [](const Finding &a, const Finding &b) {
                  if (a.severity != b.severity)
                      return a.severity > b.severity;
                  return a.id < b.id;
              });
    return findings;
}


namespace {

void sampleInodes(const QString &path, qint64 *total, qint64 *free)
{
    *total = 0;
    *free = 0;
#if defined(Q_OS_UNIX)
    struct statvfs st;
    if (!path.isEmpty() && statvfs(path.toLocal8Bit().constData(), &st) == 0) {
        *total = qint64(st.f_files);
        *free = qint64(st.f_favail);
    }
#else
    Q_UNUSED(path);
#endif
}

void sampleFileDescriptors(int *open, int *limit)
{
    const int count = SystemStats::openFileCount();
    const int soft = SystemStats::openFileSoftLimit();
    *open = count > 0 ? count : -1;
    *limit = soft > 0 ? soft : -1;
}

int sampleZombies()
{
#if defined(Q_OS_LINUX)
    const qint64 self = qint64(getpid());
    int zombies = 0;
    const QStringList entries =
        QDir(QStringLiteral("/proc"))
            .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Unsorted);
    for (const QString &entry : entries) {
        bool numeric = false;
        entry.toLongLong(&numeric);
        if (!numeric)
            continue;
        QFile stat(QStringLiteral("/proc/%1/stat").arg(entry));
        if (!stat.open(QIODevice::ReadOnly))
            continue;
        const QByteArray line = stat.readLine(4096);
        const int close = line.lastIndexOf(')');
        if (close < 0)
            continue;
        const QList<QByteArray> fields = line.mid(close + 2).split(' ');
        if (fields.size() < 2)
            continue;
        if (fields.at(0) != "Z")
            continue;
        if (fields.at(1).toLongLong() == self)
            ++zombies;
    }
    return zombies;
#else
    return -1;
#endif
}

} // namespace

void Collector::setDataDir(const QString &dir)
{
    QMutexLocker locker(&m_mutex);
    const QString trimmed = dir.trimmed();
    if (trimmed == m_dataDir)
        return;
    m_dataDir = trimmed;
    m_diskHistory.clear(); // a different volume invalidates the trend
}

void Collector::setLogPaths(const QStringList &paths)
{
    QMutexLocker locker(&m_mutex);
    m_logPaths = paths;
    m_logPathsExplicit = true;
    m_logOffsets.clear();
}

void Collector::noteLogLine(const QString &line, qint64 nowMs)
{
    if (!lineLooksLikeError(line))
        return;
    noteErrorLine(line, nowMs);
}

void Collector::noteErrorLine(const QString &line, qint64 nowMs)
{
    if (line.contains(QLatin1String("Self-diagnostics"), Qt::CaseInsensitive))
        return;
    QMutexLocker locker(&m_mutex);
    const qint64 now = nowOr(nowMs);
    m_logErrorTimes.append(now);
    m_logErrorSample = line.trimmed().right(qMin(line.trimmed().size(), 80));
    pruneCounters(now);
}

void Collector::noteRelayDrop(qint64 nowMs)
{
    QMutexLocker locker(&m_mutex);
    const qint64 now = nowOr(nowMs);
    if (!m_relayDropTimes.isEmpty() &&
        now - m_relayDropTimes.last() < kRelayDropCoalesceMs)
        return;
    m_relayDropTimes.append(now);
    pruneCounters(now);
}

void Collector::noteBackpressureDrops(qint64 totalSoFar)
{
    if (totalSoFar < 0)
        return;
    QMutexLocker locker(&m_mutex);
    if (m_backpressureBaseline < 0 || totalSoFar < m_backpressureBaseline) {
        m_backpressureBaseline = totalSoFar;
        return;
    }
    m_backpressureDrops = totalSoFar - m_backpressureBaseline;
}

void Collector::notePeerTimestamp(qint64 peerMs, qint64 localMs)
{
    if (peerMs <= 0 || localMs <= 0)
        return;
    QMutexLocker locker(&m_mutex);
    const qint64 skew = localMs - peerMs;
    m_clockSkewMs = m_clockSkewKnown ? (m_clockSkewMs * 3 + skew) / 4 : skew;
    m_clockSkewKnown = true;
}

void Collector::pruneCounters(qint64 nowMs)
{
    const qint64 cutoff = nowMs - kWindowMs;
    pruneOlderThan(m_logErrorTimes, cutoff);
    pruneOlderThan(m_relayDropTimes, cutoff);
    pruneOlderThan(m_stallTimes, cutoff);
    if (m_logErrorTimes.isEmpty())
        m_logErrorSample.clear();
    const qint64 diskCutoff = nowMs - 2 * 60 * 60 * 1000;
    while (m_diskHistory.size() > 2 && m_diskHistory.first().tsMs < diskCutoff)
        m_diskHistory.removeFirst();
}

// Tails every diagnostics log from where the last scan stopped. Takes the
// offsets it starts from and hands back the ones it reached plus what it found,
// so the caller can do this file work without holding the collector's lock.
// code-health: allow(dir-walk-in-loop) only reached from Collector::run(), which ServerNode starts on a QThread::create worker
Collector::LogScan Collector::scanLogFiles(const QStringList &logPaths,
                                           bool explicitPaths,
                                           const QHash<QString, qint64> &offsets)
{
    LogScan scan;
    scan.offsets = offsets;
    QStringList paths = logPaths;
    if (!explicitPaths) {
        const QDir dir(forkmesh::paths::diagnosticsDir());
        for (const QString &name :
             dir.entryList({QStringLiteral("*.log")}, QDir::Files, QDir::Name))
            paths << dir.filePath(name);
    }
    int matches = 0;
    for (const QString &path : std::as_const(paths)) {
        QFile file(path);
        const qint64 size = QFileInfo(path).size();
        const bool known = scan.offsets.contains(path);
        qint64 offset = scan.offsets.value(path, 0);
        if (offset > size) // rotated out from under us
            offset = 0;
        scan.offsets.insert(path, size);
        if (!known || size <= offset)
            continue;
        if (size - offset > kMaxLogTailBytes)
            offset = size - kMaxLogTailBytes;
        if (!file.open(QIODevice::ReadOnly))
            continue;
        file.seek(offset);
        while (!file.atEnd() && matches < kMaxLogMatchesPerScan) {
            const QString line = QString::fromUtf8(file.readLine(4096)).trimmed();
            if (line.isEmpty())
                continue;
            if (line.contains(QLatin1String("UI stalled"))) {
                ++scan.stalls;
                ++matches;
                continue;
            }
            if (lineLooksLikeError(line)) {
                ++scan.errors;
                scan.errorSample = line.right(qMin(line.size(), 80));
                ++matches;
            }
        }
        file.close();
    }
    return scan;
}

bool Collector::runIsDue(qint64 nowMs) const
{
    QMutexLocker locker(&m_mutex);
    const qint64 now = nowOr(nowMs);
    return m_lastRunMs <= 0 || now - m_lastRunMs >= kRunIntervalMs;
}

// The host probing here — statvfs on the data directory, an inode count, a walk
// of /proc and a tail of every diagnostics log — is the slow part, and it runs
// with no lock held so a GUI-thread note*() or findings() never waits behind it
// (adhoc #2104). Only the snapshot at the top and the merge at the bottom take
// the mutex, and both are pure memory work.
QList<Finding> Collector::run(qint64 nowMs)
{
    QString dataDir;
    QStringList logPaths;
    bool explicitPaths = false;
    QHash<QString, qint64> offsets;
    qint64 now = 0;
    {
        QMutexLocker locker(&m_mutex);
        now = nowOr(nowMs);
        if (m_lastRunMs > 0 && now - m_lastRunMs < kRunIntervalMs)
            return m_findings;
        m_lastRunMs = now; // claims this pass; a second caller gets the memo
        dataDir = m_dataDir;
        logPaths = m_logPaths;
        explicitPaths = m_logPathsExplicit;
        offsets = m_logOffsets;
    }

    bool haveDiskSample = false;
    DiskSample sample;
    qint64 inodesTotal = 0;
    qint64 inodesFree = 0;
    bool dataDirWritable = true;
    if (!dataDir.isEmpty()) {
        const qint64 total = SystemStats::diskTotalBytes(dataDir);
        const qint64 free = SystemStats::diskFreeBytes(dataDir);
        if (total > 0) {
            sample.tsMs = now;
            sample.freeBytes = free;
            sample.totalBytes = total;
            haveDiskSample = true;
        }
        sampleInodes(dataDir, &inodesTotal, &inodesFree);
        const QFileInfo info(dataDir);
        dataDirWritable = !info.exists() || info.isWritable();
    }
    const LogScan scan = scanLogFiles(logPaths, explicitPaths, offsets);
    int openFileDescriptors = -1;
    int fileDescriptorLimit = -1;
    sampleFileDescriptors(&openFileDescriptors, &fileDescriptorLimit);
    const int zombies = sampleZombies();

    QMutexLocker locker(&m_mutex);
    if (haveDiskSample)
        m_diskHistory.append(sample);
    m_logOffsets = scan.offsets;
    for (int i = 0; i < scan.stalls; ++i)
        m_stallTimes.append(now);
    for (int i = 0; i < scan.errors; ++i)
        m_logErrorTimes.append(now);
    if (!scan.errorSample.isEmpty())
        m_logErrorSample = scan.errorSample;
    pruneCounters(now);

    Inputs in;
    in.nowMs = now;
    in.windowMs = kWindowMs;
    in.inodesTotal = inodesTotal;
    in.inodesFree = inodesFree;
    in.dataDirWritable = dataDirWritable;
    in.diskHistory = m_diskHistory;
    in.openFileDescriptors = openFileDescriptors;
    in.fileDescriptorLimit = fileDescriptorLimit;
    in.zombieProcesses = zombies;
    in.logErrors = int(m_logErrorTimes.size());
    in.logErrorSample = m_logErrorSample;
    in.uiStalls = int(m_stallTimes.size());
    in.relayDrops = int(m_relayDropTimes.size());
    in.backpressureDrops = m_backpressureDrops;
    in.clockSkewMs = m_clockSkewMs;
    in.clockSkewKnown = m_clockSkewKnown;

    m_findings = evaluate(in);
    return m_findings;
}

void Collector::reset()
{
    QMutexLocker locker(&m_mutex);
    m_diskHistory.clear();
    m_logErrorTimes.clear();
    m_relayDropTimes.clear();
    m_stallTimes.clear();
    m_logErrorSample.clear();
    m_logOffsets.clear();
    m_backpressureBaseline = -1;
    m_backpressureDrops = 0;
    m_clockSkewMs = 0;
    m_clockSkewKnown = false;
    m_findings.clear();
    m_lastRunMs = 0;
}

Collector &hostCollector()
{
    static Collector collector;
    return collector;
}

} // namespace NodeDiagnostics
