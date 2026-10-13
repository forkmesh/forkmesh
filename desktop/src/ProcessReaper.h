#pragma once

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::processreaper {

inline constexpr char kInstanceVariable[] = "FORKMESH_DESKTOP_INSTANCE";
inline constexpr int kDefaultGraceMs = 3000;
inline constexpr int kDefaultDeadlineMs = 5000;

inline QProcessEnvironment releasedEnvironment(
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment())
{
    environment.remove(QString::fromLatin1(kInstanceVariable));
    return environment;
}

inline bool startReleased(const QString &program, const QStringList &arguments,
                          const QString &workingDirectory = QString(),
                          qint64 *pid = nullptr)
{
    QProcess launcher;
    launcher.setProgram(program);
    launcher.setArguments(arguments);
    if (!workingDirectory.isEmpty())
        launcher.setWorkingDirectory(workingDirectory);
    launcher.setProcessEnvironment(releasedEnvironment());
    return launcher.startDetached(pid);
}

inline QMutex &ownedUnitMutex()
{
    static QMutex mutex;
    return mutex;
}

inline QSet<QString> &ownedUnitStorage()
{
    static QSet<QString> units;
    return units;
}

inline void registerOwnedUnit(const QString &unit)
{
    if (unit.isEmpty())
        return;
    QMutexLocker lock(&ownedUnitMutex());
    ownedUnitStorage().insert(unit);
}

inline QSet<QString> ownedUnits()
{
    QMutexLocker lock(&ownedUnitMutex());
    return ownedUnitStorage();
}

struct ProcessEntry {
    qint64 pid = 0;
    qint64 parentPid = 0;
    qint64 groupId = 0;
    qint64 uid = -1;
    bool zombie = false;
    QString controlGroup;
    QByteArray instance;
};

struct ProcessTable {
    qint64 selfPid = 0;
    qint64 selfUid = -1;
    QString selfControlGroup;
    QList<ProcessEntry> processes;
};

struct Policy {
    QByteArray instance;
    QSet<QString> ownedUnits;
    QStringList protectedUnitPrefixes;
};

enum class Signal { Terminate, Kill };

struct Hooks {
    std::function<ProcessTable()> snapshot;
    std::function<void(qint64 pid, Signal signal)> send;
    std::function<void(const QStringList &units, Signal signal)> stopUnits;
    std::function<qint64()> nowMs;
    std::function<void(int ms)> sleepMs;
};

struct Report {
    QList<qint64> terminated;
    QList<qint64> killed;
    QList<qint64> survivors;
    QStringList stoppedUnits;
    int rounds = 0;
    bool timedOut = false;
};

QByteArray initializeInstance();
QByteArray instance();

QStringList defaultProtectedUnitPrefixes();
Policy currentPolicy();

QString unitOfControlGroup(const QString &controlGroup);
bool isOwnedUnitName(const QString &unit);
bool isProtectedUnit(const QString &unit, const QStringList &prefixes);
QList<qint64> selectTargets(const ProcessTable &table, const Policy &policy);
QStringList liveOwnedUnits(const ProcessTable &table, const Policy &policy);

bool parseProcStat(const QByteArray &stat, ProcessEntry *entry);
QString parseProcControlGroup(const QByteArray &cgroup);
QByteArray instanceFromEnviron(const QByteArray &environment);
qint64 uidFromProcStatus(const QByteArray &status);
ProcessTable readProcTable(const QString &procRoot, qint64 selfPid);
ProcessTable parsePsTable(const QByteArray &psOutput, qint64 selfPid,
                          qint64 selfUid);

Report reap(const Policy &policy, const Hooks &hooks,
            int graceMs = kDefaultGraceMs,
            int deadlineMs = kDefaultDeadlineMs);
Report reapOnQuit(int graceMs = kDefaultGraceMs,
                  int deadlineMs = kDefaultDeadlineMs);
QString describe(const Report &report);

} // namespace forkmesh::processreaper
