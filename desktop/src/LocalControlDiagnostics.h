#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// The "diag." local control family (WS16; docs/native/DESKTOP_PARITY.md
// section 19): the desktop's logs, health and stall records. This is the policy half, kept free of widgets so the
// tests can drive it; the requests are answered in
// MainWindowLocalControlDiagnostics.cpp. Gate 2 creates it empty; WS16 owns it.
namespace forkmesh::localcontrol::diag {

// The method prefix this family answers.
QString familyPrefix();

QString severityOf(const QString &line);
qint64 timestampOf(const QString &line);

struct LogPage {
    QJsonArray lines;
    qint64 start = 0;
    qint64 end = 0;
    bool more = false;
    int scanned = 0;
};
inline constexpr int kMaxLogLines = 400;
inline constexpr qint64 kMaxLogScanBytes = 512 * 1024;
LogPage logPage(const QString &path, qint64 before, int maxLines,
                const QString &query, const QString &severity);

QJsonObject stallRow(int index, const QString &entry);
QJsonObject stallDetail(int index, const QString &entry, int maxChars);

struct HealthInput {
    bool pressureValid = false;
    double memoryFullAvg10 = 0;
    double memorySomeAvg10 = 0;
    double ioFullAvg10 = 0;
    double cpuSomeAvg10 = 0;
    double loadAverage1 = -1;
    int cpuCount = 0;
    double cpuPercent = -1;
    qint64 memoryTotal = -1;
    qint64 memoryAvailable = -1;
    qint64 swapTotal = -1;
    qint64 swapFree = -1;
    qint64 selfResident = -1;
    qint64 diskTotal = -1;
    qint64 diskFree = -1;
    int agentsRunning = 0;
    int agentsQueued = 0;
    int agentsTotal = 0;
    int stalls = 0;
    int openFiles = -1;
    int openFileLimit = -1;
    int threads = -1;
    bool powerKnown = false;
    bool onBattery = false;
    int batteryPercent = -1;
};
QString healthLevel(const HealthInput &input);
QJsonObject healthSummary(const HealthInput &input);

struct VitalsSample {
    double memory = -1;
    double cpu = -1;
    double disk = -1;
    double swap = -1;
};
inline constexpr int kVitalsSeconds = 30;
double usedPercent(qint64 total, qint64 free);
QJsonObject vitalsSampleJson(const VitalsSample &sample);
QJsonObject vitalsHistoryJson(const QList<VitalsSample> &samples);
void appendVitals(QList<VitalsSample> &samples, const VitalsSample &sample);

struct ProcessRow {
    qint64 pid = 0;
    qint64 ppid = 0;
    QString user;
    qint64 rssKb = 0;
    double percent = 0;
    QString name;
    QString command;
};
QList<ProcessRow> parseProcessTable(const QByteArray &psOutput, int maxRows,
                                    QHash<qint64, qint64> *parents);
QString commandSummary(const QString &command, int maxChars = 160);

inline constexpr int kMaxProcessDetailChars = 65536;
qint64 processPid(const QJsonObject &params);
QString processSubject(qint64 pid, const QString &identity);
bool processStoppable(qint64 pid, qint64 selfPid);
bool sudoPasswordUsable(const QString &password);
QString processDetailText(const QString &text);

QStringList mutatingActions();
QString subjectFor(const QString &action, const QJsonObject &params,
                   QString *error = nullptr);
QString actionSummary(const QString &action, const QString &subject,
                      const QString &label);

inline constexpr int kFreeDiskSpaceTimeoutMs = 20 * 60 * 1000;
inline constexpr int kFreeDiskSpaceMaxLines = 200;
inline constexpr int kFreeDiskSpaceMaxLineChars = 300;
QStringList freeDiskSpaceSteps();
QString freeDiskSpaceStepLabel(const QString &step);
QString freeDiskSpaceScript();
QStringList freeDiskSpaceArguments(const QString &checkout, const QString &step);
QJsonObject freeDiskSpaceReply(const QString &step, const QByteArray &output, qint64 ms,
                               QString *error = nullptr);

struct BackupSnapshotFacts {
    qint64 takenAtMs = 0;
    qint64 bytes = 0;
};
struct BackupStatusInput {
    bool enabled = false;
    int keep = 0;
    bool running = false;
    QString location;
    QList<BackupSnapshotFacts> snapshots;
    QString failure;
    qint64 failureAtMs = 0;
    qint64 intervalMs = 0;
    qint64 nowMs = 0;
};
inline constexpr int kMaxBackupSnapshots = 240;
QString backupLocationLabel(const QString &root, const QString &dataRoot);
QString backupFailureCode(const QString &failure);
QJsonObject backupStatus(const BackupStatusInput &input);

inline constexpr qint64 kBuildMacStaleLockSeconds = 4 * 60 * 60;
QString buildMacProbeScript();
QJsonObject buildMacStatus(bool reachable, const QByteArray &probeOutput, qint64 checkedAtMs);

} // namespace forkmesh::localcontrol::diag
