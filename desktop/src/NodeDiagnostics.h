#pragma once

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QtGlobal>

// Node self-diagnostics: the health checks a node runs on *itself* and pushes to
// the mesh in its heartbeats, so every node list can show what is going wrong on
// a machine nobody is sitting in front of.
// Deliberately NOT the CPU / RAM / disk gauges — those already have their own
// columns and say nothing about trends. These are the slow-burn failures a point
// sample hides: a filesystem that is filling a few GB a day, inodes running out
// before the bytes do, file descriptors leaking, defunct children piling up, a
// relay link that keeps flapping, errors accumulating in the log, a clock that
// has drifted far enough for signed frames to be rejected.
// The rule engine (evaluate) is pure: it takes an Inputs snapshot and returns
// findings, so every threshold is testable without a host to break. Collector
// does the messy host sampling (Linux-first, best-effort — an unavailable metric
// is left at its "unknown" sentinel and simply produces no finding).
namespace NodeDiagnostics {

enum Severity {
    Ok = 0,
    Info = 1,      // worth knowing, nothing is failing yet
    Warning = 2,   // will bite if left alone
    Critical = 3,  // failing now or within hours
};

struct Finding {
    QString id;               // stable check id, e.g. "disk-trend"
    int severity = Info;      // Severity
    QString message;          // one-line human summary, already formatted
    QString detail;
};

struct DiskSample {
    qint64 tsMs = 0;
    qint64 freeBytes = 0;
    qint64 totalBytes = 0;
};

struct Inputs {
    qint64 nowMs = 0;
    QList<DiskSample> diskHistory;
    qint64 inodesTotal = 0;         // 0 == unknown (not Linux/Unix, or stat failed)
    qint64 inodesFree = 0;
    bool dataDirWritable = true;
    int openFileDescriptors = -1;   // -1 == unknown
    int fileDescriptorLimit = -1;
    int zombieProcesses = -1;       // defunct children of this process
    int logErrors = 0;              // error lines seen in the last windowMs
    QString logErrorSample;         // newest of those lines (already trimmed)
    int uiStalls = 0;               // stall reports appended in the last windowMs
    int relayDrops = 0;             // link losses in the last windowMs
    qint64 backpressureDrops = 0;   // frames dropped because the socket backed up
    qint64 clockSkewMs = 0;         // local clock minus the mesh's (peer frames)
    bool clockSkewKnown = false;
    qint64 windowMs = 0;            // period the counters above cover
};

constexpr qint64 kWindowMs = 30 * 60 * 1000;
constexpr qint64 kMinTrendSpanMs = 10 * 60 * 1000;
constexpr int kMaxWireFindings = 6;
constexpr int kMaxMessageChars = 140;

QList<Finding> evaluate(const Inputs &in);

int worstSeverity(const QList<Finding> &findings);
QString severityName(int severity);

QString summaryLabel(const QList<Finding> &findings, bool reported);

QString detailText(const QList<Finding> &findings);

QJsonArray toJson(const QList<Finding> &findings);
QList<Finding> fromJson(const QJsonArray &array);

class Collector
{
public:
    void setDataDir(const QString &dir);
    void setLogPaths(const QStringList &paths);

    void noteLogLine(const QString &line, qint64 nowMs = 0);
    void noteErrorLine(const QString &line, qint64 nowMs = 0);
    void noteRelayDrop(qint64 nowMs = 0);
    void noteBackpressureDrops(qint64 totalSoFar);
    void notePeerTimestamp(qint64 peerMs, qint64 localMs);

    QList<Finding> run(qint64 nowMs = 0);
    QList<Finding> findings() const
    {
        QMutexLocker locker(&m_mutex);
        return m_findings;
    }
    qint64 lastRunMs() const
    {
        QMutexLocker locker(&m_mutex);
        return m_lastRunMs;
    }
    // True when run() would do real work rather than hand back the memo, so a
    // caller can skip dispatching a worker that would only return the cache.
    bool runIsDue(qint64 nowMs = 0) const;

    void reset();

    static constexpr qint64 kRunIntervalMs = 60 * 1000;
    static constexpr qint64 kRelayDropCoalesceMs = 15 * 1000;

private:
    void pruneCounters(qint64 nowMs);
    // What one tail of the diagnostics logs found, and the offsets it reached.
    struct LogScan {
        QHash<QString, qint64> offsets;
        int stalls = 0;
        int errors = 0;
        QString errorSample;
    };
    static LogScan scanLogFiles(const QStringList &logPaths, bool explicitPaths,
                                const QHash<QString, qint64> &offsets);

    // run() opens files, stats filesystems and walks /proc — seconds of it on a
    // loaded box (adhoc #2104), so the node samples it on a worker while the
    // note*() calls keep arriving from the GUI thread. Every public method
    // takes this; the private helpers above are only reached with it held.
    mutable QMutex m_mutex;
    QString m_dataDir;
    QStringList m_logPaths;
    bool m_logPathsExplicit = false;
    QList<DiskSample> m_diskHistory;
    QList<qint64> m_logErrorTimes;   // rolling event times, oldest first
    QList<qint64> m_relayDropTimes;
    QList<qint64> m_stallTimes;
    QString m_logErrorSample;
    qint64 m_backpressureBaseline = -1;
    qint64 m_backpressureDrops = 0;
    qint64 m_clockSkewMs = 0;
    bool m_clockSkewKnown = false;
    QHash<QString, qint64> m_logOffsets; // path -> bytes already read
    QList<Finding> m_findings;
    qint64 m_lastRunMs = 0;
};

Collector &hostCollector();

} // namespace NodeDiagnostics
