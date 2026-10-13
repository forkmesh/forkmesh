#include "SystemPressure.h"

#include "SystemStats.h"

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

namespace forkmesh {

namespace {

QByteArray readSmallFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    // /proc reports a zero size; readAll() still drains the whole file.
    return f.readAll();
}

// "avg10=45.35" out of a PSI line; false when the key is absent.
bool avgField(const QByteArray &line, const QByteArray &key, double *out)
{
    const int at = line.indexOf(key + '=');
    if (at < 0)
        return false;
    int end = at + key.size() + 1;
    while (end < line.size() && line.at(end) != ' ')
        ++end;
    bool ok = false;
    const double value =
        line.mid(at + key.size() + 1, end - at - key.size() - 1).toDouble(&ok);
    if (!ok)
        return false;
    *out = value;
    return true;
}

QString formatPercent(double value)
{
    return QString::number(value, 'f', 1) + QLatin1Char('%');
}

} // namespace

bool parsePressureStallFile(const QByteArray &text, double *someAvg10,
                            double *fullAvg10, double *fullAvg60)
{
    bool sawSome = false;
    const QList<QByteArray> lines = text.split('\n');
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        if (line.startsWith("some ")) {
            double value = 0.0;
            if (avgField(line, "avg10", &value)) {
                if (someAvg10)
                    *someAvg10 = value;
                sawSome = true;
            }
        } else if (line.startsWith("full ")) {
            double value = 0.0;
            if (fullAvg10 && avgField(line, "avg10", &value))
                *fullAvg10 = value;
            if (fullAvg60 && avgField(line, "avg60", &value))
                *fullAvg60 = value;
        }
    }
    return sawSome;
}

bool parseLoadAverage(const QByteArray &text, double *load1)
{
    const QList<QByteArray> fields = text.trimmed().split(' ');
    if (fields.isEmpty())
        return false;
    bool ok = false;
    const double value = fields.first().toDouble(&ok);
    if (!ok)
        return false;
    if (load1)
        *load1 = value;
    return true;
}

qint64 parseKilobyteField(const QByteArray &text, const QByteArray &key)
{
    const QList<QByteArray> lines = text.split('\n');
    for (const QByteArray &line : lines) {
        if (!line.startsWith(key + ':'))
            continue;
        const QList<QByteArray> parts = line.mid(key.size() + 1).trimmed().split(' ');
        if (parts.isEmpty())
            return -1;
        bool ok = false;
        const qint64 kb = parts.first().toLongLong(&ok);
        return ok ? kb * 1024 : -1;
    }
    return -1;
}

SystemPressureSnapshot sampleSystemPressure()
{
    SystemPressureSnapshot s;
    s.sampledAtMs = QDateTime::currentMSecsSinceEpoch();
    s.cpuCount = QThread::idealThreadCount();
#ifdef Q_OS_LINUX
    const QByteArray memory = readSmallFile(QStringLiteral("/proc/pressure/memory"));
    s.valid = parsePressureStallFile(memory, &s.memorySomeAvg10,
                                     &s.memoryFullAvg10, &s.memoryFullAvg60);
    parsePressureStallFile(readSmallFile(QStringLiteral("/proc/pressure/io")),
                           &s.ioSomeAvg10, &s.ioFullAvg10, nullptr);
    parsePressureStallFile(readSmallFile(QStringLiteral("/proc/pressure/cpu")),
                           &s.cpuSomeAvg10, nullptr, nullptr);
    parseLoadAverage(readSmallFile(QStringLiteral("/proc/loadavg")),
                     &s.loadAverage1);
    const QByteArray meminfo = readSmallFile(QStringLiteral("/proc/meminfo"));
    s.memoryTotalBytes = parseKilobyteField(meminfo, "MemTotal");
    s.memoryAvailableBytes = parseKilobyteField(meminfo, "MemAvailable");
    s.swapTotalBytes = parseKilobyteField(meminfo, "SwapTotal");
    s.swapFreeBytes = parseKilobyteField(meminfo, "SwapFree");
    const QByteArray status = readSmallFile(QStringLiteral("/proc/self/status"));
    s.selfResidentBytes = parseKilobyteField(status, "VmRSS");
    s.selfSwappedBytes = parseKilobyteField(status, "VmSwap");
#else
    s.memoryTotalBytes = SystemStats::totalMemoryBytes();
    s.memoryAvailableBytes = SystemStats::availableMemoryBytes();
    s.swapTotalBytes = SystemStats::totalSwapBytes();
    s.swapFreeBytes = SystemStats::freeSwapBytes();
    s.selfResidentBytes = SystemStats::residentBytes();
#endif
    return s;
}

SystemPressureSnapshot cachedSystemPressure(qint64 maxAgeMs)
{
    static QMutex mutex;
    static SystemPressureSnapshot last;
    QMutexLocker lock(&mutex);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (last.sampledAtMs > 0 && now - last.sampledAtMs < maxAgeMs)
        return last;
    last = sampleSystemPressure();
    return last;
}

bool systemUnderPressure(const SystemPressureSnapshot &snapshot)
{
    if (!snapshot.valid)
        return false; // no PSI: nothing to go on, never hold work back
    return snapshot.memoryFullAvg10 >= kMemoryPressureFullAvg10Percent ||
           snapshot.ioFullAvg10 >= kIoPressureFullAvg10Percent;
}

QString describeSystemPressure(const SystemPressureSnapshot &s)
{
    QStringList parts;
    if (s.valid) {
        parts << QStringLiteral("memory PSI some %1 / full %2 (avg10), io full %3")
                     .arg(formatPercent(s.memorySomeAvg10),
                          formatPercent(s.memoryFullAvg10),
                          formatPercent(s.ioFullAvg10));
    }
    if (s.loadAverage1 >= 0.0) {
        QString load = QStringLiteral("load %1").arg(
            QString::number(s.loadAverage1, 'f', 1));
        if (s.cpuCount > 0)
            load += QStringLiteral(" on %1 cpus").arg(s.cpuCount);
        parts << load;
    }
    if (s.memoryAvailableBytes >= 0 && s.memoryTotalBytes > 0) {
        parts << QStringLiteral("%1 of %2 available")
                     .arg(SystemStats::formatBytes(s.memoryAvailableBytes),
                          SystemStats::formatBytes(s.memoryTotalBytes));
    }
    if (s.swapTotalBytes > 0 && s.swapFreeBytes >= 0) {
        parts << QStringLiteral("swap %1 used of %2")
                     .arg(SystemStats::formatBytes(s.swapTotalBytes -
                                                   s.swapFreeBytes),
                          SystemStats::formatBytes(s.swapTotalBytes));
    }
    QString line = parts.join(QStringLiteral(", "));
    if (s.selfResidentBytes >= 0) {
        QString self = QStringLiteral("this process %1 resident")
                           .arg(SystemStats::formatBytes(s.selfResidentBytes));
        if (s.selfSwappedBytes >= 0)
            self += QStringLiteral(", %1 swapped")
                        .arg(SystemStats::formatBytes(s.selfSwappedBytes));
        line += (line.isEmpty() ? QString() : QStringLiteral("; ")) + self;
    }
    return line;
}

bool announceDeferredWork(const QString &what, qint64 nowMs)
{
    constexpr qint64 kAnnounceIntervalMs = 5 * 60 * 1000;
    static QMutex mutex;
    static QHash<QString, qint64> lastAnnouncedMs;
    QMutexLocker lock(&mutex);
    const qint64 last = lastAnnouncedMs.value(what, 0);
    if (last > 0 && nowMs - last < kAnnounceIntervalMs)
        return false;
    lastAnnouncedMs.insert(what, nowMs);
    return true;
}

} // namespace forkmesh
