#pragma once

#include "AgentResources.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QThread;

namespace forkmesh::agentresources {

struct UnitSample {
    QString name;
    qint64 memoryBytes = 0;
    bool frozen = false;
    qint64 cpuUsageUsec = -1;
    double cpuCores = -1.0;
};

struct PressureInput {
    double memoryFullAvg10 = 0.0;
    qint64 availableBytes = -1;
};

struct CpuInput {
    bool valid = false;
    int machineCpus = 0;
    double machineBusyCores = 0.0;
    double sliceCores = 0.0;
    double otherCores = 0.0;
    double someAvg10 = 0.0;
};

struct GovernorPolicy {
    double freezeAbovePercent = 25.0;
    double thawBelowPercent = 5.0;
    qint64 thawMinAvailableBytes = qint64(3) << 30;
    qint64 minFreezeBytes = qint64(256) << 20;
    qint64 freezeCooldownMs = 20'000;
    qint64 thawCooldownMs = 15'000;
    qint64 maxFrozenMs = 15 * 60'000;
    qint64 cpuLowerCooldownMs = 5'000;
    qint64 cpuRaiseCooldownMs = 15'000;
};

struct GovernorState {
    QHash<QString, qint64> firstSeenMs;
    QHash<QString, qint64> frozenAtMs;
    qint64 lastFreezeMs = 0;
    qint64 lastThawMs = 0;
    int cpuQuota = 0;
    double otherCores = -1.0;
    qint64 lastCpuChangeMs = 0;
    int cpuChanges = 0;
};

struct GovernorDecision {
    QString freeze;
    QString thaw;
    QString reason;
};

struct CpuDecision {
    int quota = 0;
    QString reason;
};

struct GovernorEvent {
    qint64 atMs = 0;
    QString action;
    QString unit;
    QString reason;
    bool failed = false;
};

struct SliceCounters {
    bool valid = false;
    qint64 currentBytes = -1;
    qint64 peakBytes = -1;
    qint64 swapBytes = -1;
    qint64 highBytes = -1;
    qint64 maxBytes = -1;
    double someAvg10 = 0.0;
    double fullAvg10 = 0.0;
    double fullAvg60 = 0.0;
    qint64 highEvents = 0;
    qint64 maxEvents = 0;
    qint64 oomEvents = 0;
    qint64 oomKills = 0;
    int cpuMaxPercent = -1;
    qint64 cpuUsageUsec = -1;
    qint64 cpuThrottledPeriods = 0;
    qint64 cpuThrottledUsec = 0;
    double cpuSomeAvg10 = 0.0;
    double cpuFullAvg10 = 0.0;
};

struct GovernorSnapshot {
    bool running = false;
    int tickMs = 0;
    qint64 startedMs = 0;
    qint64 lastTickMs = 0;
    Status status;
    GovernorPolicy policy;
    PressureInput pressure;
    double machineFullAvg10 = 0.0;
    double sliceFullAvg10 = 0.0;
    CpuInput cpu;
    QList<UnitSample> units;
    GovernorState state;
    QList<GovernorEvent> events;
};

bool freezable(const QString &unit);
QList<UnitSample> readSliceUnits(const QString &controlGroup);
double slicePressure(const QString &controlGroup);
QString nextFreezeCandidate(const GovernorPolicy &policy,
                            const QList<UnitSample> &units,
                            const GovernorState &state);
SliceCounters readSliceCounters(const QString &controlGroup);
bool parseProcStatCpu(const QByteArray &text, qint64 *busyTicks,
                      qint64 *totalTicks, int *cpus);
qint64 parseCpuUsageUsec(const QByteArray &cpuStat);
int parseCpuMaxPercent(const QByteArray &cpuMax);
GovernorSnapshot governorSnapshot();
GovernorDecision decide(const GovernorPolicy &policy,
                        const PressureInput &pressure,
                        const QList<UnitSample> &units, qint64 nowMs,
                        GovernorState &state);
CpuDecision decideCpu(const GovernorPolicy &policy, const Budget &budget,
                      const CpuInput &input, qint64 nowMs, GovernorState &state);
int retargetCpuQuota(const Budget &budget, const GovernorState &state);
QStringList agentScopes(const QList<UnitSample> &units);
void applyBudget();

class Governor : public QObject {
public:
    explicit Governor(QObject *parent = nullptr);
    ~Governor() override;

    void start();
    void applyBudget();
    GovernorSnapshot snapshot() const;

private:
    void prepared();
    void tick();
    void runSystemctl(const QString &verb, const QString &unit,
                      const QStringList &extra = {});
    void thawAll();
    CpuInput sampleCpu(QList<UnitSample> &units);
    void restoreCpuQuota();
    void record(const QString &action, const QString &unit,
                const QString &reason, bool failed = false);

    QTimer m_timer;
    QPointer<QThread> m_prepareThread;
    GovernorPolicy m_policy;
    GovernorState m_state;
    Status m_status;
    bool m_running = false;
    qint64 m_startedMs = 0;
    qint64 m_lastTickMs = 0;
    PressureInput m_lastPressure;
    double m_lastMachineFull = 0.0;
    double m_lastSliceFull = 0.0;
    QList<UnitSample> m_lastUnits;
    CpuInput m_lastCpu;
    QElapsedTimer m_cpuClock;
    qint64 m_cpuSampleMs = -1;
    qint64 m_busyTicks = -1;
    qint64 m_totalTicks = -1;
    qint64 m_sliceUsec = -1;
    QHash<QString, qint64> m_unitUsec;
    QList<GovernorEvent> m_events;
};

} // namespace forkmesh::agentresources
