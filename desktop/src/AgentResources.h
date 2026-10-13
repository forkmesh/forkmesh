#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace forkmesh::agentresources {

const QString kEnabledSetting = QStringLiteral("agents/memoryGovernor");
const QString kReservePercentSetting = QStringLiteral("agents/memoryReservePercent");
constexpr int kDefaultReservePercent = 20;
constexpr int kMinReservePercent = 5;
constexpr int kMaxReservePercent = 60;
const QString kCpuReservePercentSetting = QStringLiteral("agents/cpuReservePercent");
constexpr int kDefaultCpuReservePercent = 20;

QString sliceName();

struct Budget {
    qint64 totalBytes = 0;
    qint64 reserveBytes = 0;
    qint64 sliceHighBytes = 0;
    qint64 sliceMaxBytes = 0;
    qint64 agentHighBytes = 0;
    qint64 agentMaxBytes = 0;
    int jobsPerAgent = 1;
    int cpuCount = 0;
    int cpuReserve = 0;
    int sliceCpuQuota = 0;
    int sliceCpuFloor = 0;
    bool valid() const { return totalBytes > 0 && sliceHighBytes > 0; }
};

Budget computeBudget(qint64 totalBytes, int cpuCount, int reservePercent,
                     int cpuReservePercent = kDefaultCpuReservePercent);
QString cpuCores(int quotaPercent);
Budget currentBudget();

QStringList sliceProperties(const Budget &budget);
QString scopeUnitName(const QString &label, qint64 pid, int serial);
QStringList scopeArguments(const QString &unit, const QString &description,
                           const Budget &budget, bool oomPolicy);
QString shimScript(const QString &realSystemdRun);
QStringList environmentDefaults(const Budget &budget);
void mergeEnvironment(QProcessEnvironment &env, const QStringList &defaults);

struct Status {
    bool enabled = false;
    bool available = false;
    bool oomPolicy = false;
    QString reason;
    QString systemdRun;
    QString systemctl;
    QString controlGroup;
    QString shimDirectory;
    Budget budget;
};

bool enabled();
Status prepare();
Status status();
QString describe(const Status &status);

void applyEnvironment(QProcessEnvironment &env);
QString wrapShellCommand(const QString &command);
QString shimDirectory();

struct Launch {
    QString program;
    QStringList arguments;
    QString unit;
    bool contained = false;
};

Launch contain(const QString &program, const QStringList &arguments,
               const QString &label);
QString launchLabel(const QString &provider, const QProcessEnvironment &env);

} // namespace forkmesh::agentresources
