#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

// Machine-wide starvation, read from the kernel's pressure stall information
// (/proc/pressure/*) plus the load average, memory totals and this process's
// own swap footprint. Two readers:
//
//  - the UI-stall watchdog stamps every record with a snapshot taken when the
//    stall was detected and another when the event loop resumed, so a freeze
//    that happened because the whole box was paging reads as such instead of
//    as a fault in whatever frame the sampler happened to catch;
//  - the periodic refresh timers ask systemUnderPressure() before starting
//    optional work, so under pressure the app stops adding its own directory
//    walks and git spawns to a machine that is already thrashing.
//
// Why: 2026-09-12 13:32-13:37 the GUI thread froze twice (153 s, then 95 s;
// the compositor offered "Force Quit") while two 2.6 GB test runs and two
// parallel C++ builds pushed the box into swap - PSI memory "full" avg10 was
// still 45 % minutes later, load 34 on 36 cores. The stall records blamed a
// `git rev-parse` wait, a refs directory walk and a footer repaint: all true,
// all beside the point, since each is microseconds on an idle machine.
namespace forkmesh {

struct SystemPressureSnapshot {
    bool valid = false;           // PSI is exposed (Linux 4.20+, CONFIG_PSI)
    double memorySomeAvg10 = 0.0; // % of the last 10 s some task waited on memory
    double memoryFullAvg10 = 0.0; // % of the last 10 s every task waited on memory
    double memoryFullAvg60 = 0.0;
    double ioSomeAvg10 = 0.0;
    double ioFullAvg10 = 0.0;
    double cpuSomeAvg10 = 0.0;
    double loadAverage1 = -1.0;
    int cpuCount = 0;
    qint64 memoryTotalBytes = -1;
    qint64 memoryAvailableBytes = -1;
    qint64 swapTotalBytes = -1;
    qint64 swapFreeBytes = -1;
    qint64 selfResidentBytes = -1; // this process, VmRSS
    qint64 selfSwappedBytes = -1;  // this process, VmSwap
    qint64 sampledAtMs = 0;        // wall clock, QDateTime::currentMSecsSinceEpoch()
};

// "full" means every runnable task on the machine was waiting on the resource
// for that share of the window - nothing was making progress. 10 % of the
// last ten seconds is already a second of machine-wide stall; a healthy
// desktop reads 0.00 for minutes at a time. IO pressure is set higher because
// a build farm next door keeps the disk busy without starving anyone.
constexpr double kMemoryPressureFullAvg10Percent = 10.0;
constexpr double kIoPressureFullAvg10Percent = 30.0;

// Pure parsers, shared with the tests. Each tolerates the file being absent or
// malformed by leaving its outputs untouched and returning false.
bool parsePressureStallFile(const QByteArray &text, double *someAvg10,
                            double *fullAvg10, double *fullAvg60);
bool parseLoadAverage(const QByteArray &text, double *load1);
// "<key>:  123456 kB" lines as in /proc/meminfo and /proc/self/status; bytes,
// or -1 when the key is missing.
qint64 parseKilobyteField(const QByteArray &text, const QByteArray &key);

// Fresh read of every source (a handful of small /proc files).
SystemPressureSnapshot sampleSystemPressure();
// The last sample if it is younger than maxAgeMs, else a fresh one. Safe from
// any thread; the periodic timers all fire within the same second, so they
// share one read.
SystemPressureSnapshot cachedSystemPressure(qint64 maxAgeMs = 1000);

bool systemUnderPressure(const SystemPressureSnapshot &snapshot);

// One line for logs and stall records, e.g.
// "memory PSI some 49.7% / full 45.4% (avg10), io full 0.5%, load 33.9 on 36
//  cpus, 14.0 GB of 31.2 GB available, swap 676 MB used of 24.0 GB; this
//  process 838 MB resident, 0 B swapped". Empty when PSI is unavailable and
// nothing else could be read either.
QString describeSystemPressure(const SystemPressureSnapshot &snapshot);

// True when a caller that just skipped `what` because of pressure should say
// so in the log: at most once per `what` every five minutes, so a timer that
// keeps deferring through a long build writes one line, not sixty.
bool announceDeferredWork(const QString &what, qint64 nowMs);

} // namespace forkmesh
