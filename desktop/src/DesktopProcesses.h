#pragma once

#include "CoreNodeLogs.h"

#include <QByteArray>
#include <QChar>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace forkmesh {
namespace desktopprocs {

struct Process {
    qint64 pid = 0;
    qint64 parentPid = 0;
    QString name;
    QString command;
    QChar state;
    uint uid = 0;
    QString user;
    qint64 cpuTicks = 0;
    double cpuPercent = -1.0;
    qint64 rssBytes = 0;
    int threads = 0;
    qint64 startTicks = 0;
    qint64 ageSeconds = -1;
    bool forkmesh = false;
    bool self = false;
};

struct Core {
    int cpu = -1;
    int coreId = -1;
    int package = 0;
    QString kind;
    double busy = -1.0;
    qint64 curKhz = -1;
    qint64 maxKhz = -1;
    double tempC = -1.0;
    QString tempPath;
};

struct SwapDevice {
    QString type;
    qint64 sizeBytes = 0;
    qint64 usedBytes = 0;
};

struct Machine {
    bool valid = false;
    QString cpuModel;
    int physicalCores = 0;
    qint64 cacheL3Bytes = -1;
    QList<Core> cores;
    QString governor;
    QStringList governors;
    QString driver;
    QString driverStatus;
    QString energyPreference;
    qint64 baseKhz = -1;
    qint64 minKhz = -1;
    qint64 maxKhz = -1;
    int turbo = -1;
    QString smt;
    double packageTempC = -1.0;
    QString packageTempPath;
    double pressureCpu = -1.0;
    double pressureMemory = -1.0;
    double pressureIo = -1.0;
    QString hugePages;
    QString hugePagesDefrag;
    int swappiness = -1;
    int overcommit = -1;
    int dirtyRatio = -1;
    int dirtyBackgroundRatio = -1;
    int cachePressure = -1;
    QString zswap;
    QList<SwapDevice> swaps;
    QString kernel;
    int numaNodes = 0;
};

struct CpuTicks {
    qint64 busy = 0;
    qint64 total = 0;
};

struct Snapshot {
    bool valid = false;
    QString reason;
    qint64 atMs = 0;
    QList<Process> processes;
    int running = 0;
    int threads = 0;
    corelogs::NodeVitals vitals;
    int forkmeshCount = 0;
    double forkmeshCpu = 0.0;
    qint64 forkmeshRss = 0;
    Machine machine;
};

Process parseStatLine(const QByteArray &line, qint64 pageBytes);
QString stateWord(QChar state);
QString descriptionFor(const Process &process);
QString detailTextFor(const Process &process);
void markTree(QList<Process> &processes, qint64 rootPid);
QList<int> parseCpuList(const QString &text);
QHash<int, CpuTicks> parsePerCpuTicks(const QByteArray &procStat);
QString selectedChoice(const QString &text);
double pressureSome(const QByteArray &text);
QString shortCpuModel(const QString &model);
QList<SwapDevice> parseSwaps(const QByteArray &text);
Machine readMachine(const QString &root);
void refreshMachine(Machine &machine, const QString &root);
void applyCpuTicks(Machine &machine, const QHash<int, CpuTicks> &previous,
                   const QHash<int, CpuTicks> &current);

class Sampler
{
public:
    Snapshot sample(qint64 nowMs);
    QString commandLine(qint64 pid);
    Process readOne(qint64 pid);

private:
    QString userName(uint uid);

    QHash<qint64, qint64> m_lastTicks;
    QHash<qint64, qint64> m_lastStart;
    QHash<uint, QString> m_users;
    QHash<qint64, QString> m_commands;
    QElapsedTimer m_clock;
    qint64 m_lastElapsedMs = -1;
    double m_lastBusy = -1.0;
    double m_lastTotal = -1.0;
    Machine m_machine;
    QHash<int, CpuTicks> m_lastCpuTicks;
};

} // namespace desktopprocs
} // namespace forkmesh
