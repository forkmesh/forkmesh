#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::systemdtimers {

struct CommandResult {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    QByteArray output;
    QString error;

    bool ok() const { return started && !timedOut && exitCode == 0; }
};

using Runner = std::function<CommandResult(const QStringList &arguments)>;

Runner systemctlRunner();
bool systemctlAvailable();

struct Timer {
    QString unit;
    QString service;
    QString description;
    QString fileState;
    QString activeState;
    QString sourcePath;
    qint64 nextUsec = 0;
    qint64 lastUsec = 0;
    QString serviceActiveState;
    QString serviceResult;
    QString command;
    QStringList dropIns;

    bool installed() const { return !fileState.isEmpty(); }
    bool enabled() const;
};

struct Snapshot {
    QList<Timer> timers;
    QString error;
};

bool isManagedUnit(const QString &unit);
QString serviceFor(const QString &timerUnit);

struct ListedTimer {
    QString unit;
    QString activates;
    qint64 nextUsec = 0;
    qint64 lastUsec = 0;
};

QList<ListedTimer> parseListTimers(const QByteArray &json);
QHash<QString, QString> parseUnitFiles(const QByteArray &json);
QList<QHash<QString, QString>> parseShow(const QByteArray &text);
QString commandLine(const QString &execStart);

Snapshot collect(const Runner &runner, const QString &unitSourceDirectory);

QList<QStringList> enableCommands(const Timer &timer);
QList<QStringList> disableCommands(const Timer &timer);
QStringList runNowArguments(const Timer &timer);

QString relativeTime(qint64 usec, qint64 nowUsec);

} // namespace forkmesh::systemdtimers
