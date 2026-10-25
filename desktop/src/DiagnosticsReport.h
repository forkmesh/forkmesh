#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace forkmesh::diagreport {

inline constexpr int kMaxReportChars = 48000;
inline constexpr int kMaxErrorGroups = 25;
inline constexpr int kMaxRecentLines = 60;
inline constexpr int kMaxLineChars = 300;
inline constexpr int kMaxStalls = 3;
inline constexpr int kMaxStallChars = 2500;
inline constexpr int kMaxQtReportChars = 6000;

struct LogEntry {
    QString badge;
    QString message;
    QString line;
};

struct Snapshot {
    qint64 takenAtMs = 0;
    QString version;
    QString sourceDir;
    QString platform;
    QString qtVersion;
    qint64 uptimeMs = -1;
    bool offline = false;
    double cpuPercent = -1.0;
    qint64 residentBytes = -1;
    qint64 hostMemoryTotal = -1;
    qint64 hostMemoryAvailable = -1;
    qint64 swapTotal = -1;
    qint64 swapFree = -1;
    qint64 diskTotal = -1;
    qint64 diskFree = -1;
    int openFiles = -1;
    int openFileLimit = -1;
    int threads = -1;
    qint64 networkRequests = 0;
    qint64 logLinesThisSession = 0;
    qint64 loopLagP95Ms = -1;
    qint64 loopLagPeakMs = -1;
    int widgets = -1;
    int timers = -1;
    int runningAgents = -1;
    QHash<QString, int> categoryCounts;
    QList<LogEntry> log;
    int stallCount = 0;
    QStringList stalls;
    QStringList crashes;
    QStringList securityIncidents;
    QString healthFindings;
    QString qtReport;
    QString logLocations;
};

struct Baseline {
    int sequence = 0;
    qint64 takenAtMs = 0;
    int stallCount = 0;
    int crashes = 0;
    qint64 residentBytes = -1;
    qint64 loopLagP95Ms = -1;
    QHash<QString, int> problems;
};

QString problemSignature(const QString &message);
bool isProblemBadge(const QString &badge);
QHash<QString, int> problemCounts(const QList<LogEntry> &log);
Baseline baselineFrom(const Snapshot &snapshot, int sequence);
QJsonObject toJson(const Baseline &baseline);
Baseline baselineFromJson(const QJsonObject &object);
QStringList findings(const Snapshot &snapshot);

struct AlertInputs {
    int crashes = 0;
    int stalls = 0;
    qint64 loopLagP95Ms = -1;
    double cpuPercent = -1.0;
    qint64 residentBytes = -1;
    qint64 hostMemoryTotal = -1;
    qint64 hostMemoryAvailable = -1;
    qint64 diskTotal = -1;
    qint64 diskFree = -1;
    int openFiles = -1;
    int openFileLimit = -1;
    int securityIncidents = 0;
    QString securitySample;
};

struct Alert {
    QStringList performance;
    QStringList security;
    QStringList ui;
    bool detected() const
    {
        return !performance.isEmpty() || !security.isEmpty() || !ui.isEmpty();
    }
    QString summary() const;
};

bool isSecurityIncident(const QString &message);
Alert alert(const AlertInputs &inputs);
QString build(const Snapshot &snapshot, const Baseline &previous);

} // namespace forkmesh::diagreport
