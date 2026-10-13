#pragma once

#include "CoreNodeLogs.h"

#include <QByteArray>
#include <QChar>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QString>
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
};

Process parseStatLine(const QByteArray &line, qint64 pageBytes);
QString stateWord(QChar state);
void markTree(QList<Process> &processes, qint64 rootPid);

class Sampler
{
public:
    Snapshot sample(qint64 nowMs);
    QString commandLine(qint64 pid);

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
};

} // namespace desktopprocs
} // namespace forkmesh
