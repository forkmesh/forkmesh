#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace SystemStats {

qint64 residentBytes();

qint64 totalMemoryBytes();

qint64 availableMemoryBytes();

qint64 totalSwapBytes();
qint64 freeSwapBytes();

qint64 diskTotalBytes(const QString &path);
qint64 diskFreeBytes(const QString &path);

double hostCpuPercent();

double cpuPercent();

QString formatBytes(qint64 bytes);

struct DescendantLoad {
    int count = 0;             // matching processes below the root
    qint64 residentBytes = 0;  // their combined RSS
};

struct DescendantProcess {
    qint64 pid = 0;
    qint64 parentPid = 0;
    QString command;
    // utime + stime in clock ticks. Monotonic per process, so a caller polling
    // the same tree can tell a background build that is still burning CPU from
    // an idle straggler that only outlived the run.
    qint64 cpuTicks = 0;
};

QList<DescendantProcess> descendantProcesses(qint64 rootPid);

// One walk of /proc: every process's parent, comm, state and CPU time. A
// caller asking about several roots at once reads the table once and asks it
// for each, instead of walking /proc per root.
struct ProcessTable {
    struct Entry {
        qint64 parentPid = 0;
        QString command;
        bool zombie = false;
        qint64 cpuTicks = 0;
    };
    QHash<qint64, Entry> entries;
    QHash<qint64, QList<qint64>> children;
};
ProcessTable processTable();
QList<DescendantProcess> descendantProcesses(qint64 rootPid, const ProcessTable &table);

DescendantLoad descendantsNamed(qint64 rootPid, const QString &comm);


int openFileCount();

int openFileSoftLimit();
int openFileHardLimit();

int threadCount();

int raiseOpenFileLimit();

enum class Termination { Requested, AlreadyExited, PermissionDenied, Failed, Unsupported };
Termination requestTermination(qint64 pid, QString *error = nullptr);

} // namespace SystemStats
