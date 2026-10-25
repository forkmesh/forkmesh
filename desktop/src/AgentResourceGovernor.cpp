#include "AgentResourceGovernor.h"

#include "SystemPressure.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSet>
#include <QThread>

#include <cmath>
#include <limits>

namespace forkmesh::agentresources {

namespace {

constexpr int kTickMs = 5000;
constexpr int kThawOnExitTimeoutMs = 3000;
constexpr int kMaxEvents = 200;

Governor *g_instance = nullptr;

QByteArray readSmall(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QString megabytes(qint64 bytes)
{
    return QStringLiteral("%1 MB").arg(bytes / (1024 * 1024));
}

QString gigabytesText(qint64 bytes)
{
    return QStringLiteral("%1 GB").arg(double(bytes) / double(qint64(1) << 30), 0, 'f', 1);
}

qint64 readLimit(const QString &path)
{
    const QByteArray text = readSmall(path).trimmed();
    if (text.isEmpty())
        return -1;
    if (text == "max")
        return std::numeric_limits<qint64>::max();
    bool ok = false;
    const qint64 value = text.toLongLong(&ok);
    return ok ? value : -1;
}

} // namespace

bool freezable(const QString &unit)
{
    if (unit.startsWith(QLatin1String("forkmesh-agent-")) ||
        unit.startsWith(QLatin1String("forkmesh-action-")))
        return false;
    return unit.endsWith(QLatin1String(".service")) ||
           unit.endsWith(QLatin1String(".scope"));
}

QList<UnitSample> readSliceUnits(const QString &controlGroup)
{
    QList<UnitSample> units;
    if (controlGroup.isEmpty())
        return units;
    const QDir dir(controlGroup);
    const QStringList names =
        dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        if (!name.endsWith(QLatin1String(".service")) &&
            !name.endsWith(QLatin1String(".scope")))
            continue;
        const QString path = dir.filePath(name);
        UnitSample sample;
        sample.name = name;
        sample.memoryBytes =
            readSmall(path + QStringLiteral("/memory.current")).trimmed().toLongLong();
        const QList<QByteArray> events =
            readSmall(path + QStringLiteral("/cgroup.events")).split('\n');
        for (const QByteArray &line : events)
            if (line.trimmed() == "frozen 1")
                sample.frozen = true;
        sample.cpuUsageUsec =
            parseCpuUsageUsec(readSmall(path + QStringLiteral("/cpu.stat")));
        units.append(sample);
    }
    return units;
}

double slicePressure(const QString &controlGroup)
{
    if (controlGroup.isEmpty())
        return 0.0;
    double full = 0.0;
    parsePressureStallFile(readSmall(controlGroup + QStringLiteral("/memory.pressure")),
                           nullptr, &full, nullptr);
    return full;
}

QString nextFreezeCandidate(const GovernorPolicy &policy,
                            const QList<UnitSample> &units,
                            const GovernorState &state)
{
    int running = 0;
    const UnitSample *pick = nullptr;
    for (const UnitSample &unit : units) {
        if (!freezable(unit.name) || unit.frozen)
            continue;
        ++running;
        if (unit.memoryBytes < policy.minFreezeBytes)
            continue;
        const qint64 seen = state.firstSeenMs.value(unit.name);
        if (!pick || seen > state.firstSeenMs.value(pick->name) ||
            (seen == state.firstSeenMs.value(pick->name) &&
             unit.memoryBytes > pick->memoryBytes))
            pick = &unit;
    }
    return pick && running >= 2 ? pick->name : QString();
}

SliceCounters readSliceCounters(const QString &controlGroup)
{
    SliceCounters counters;
    if (controlGroup.isEmpty() || !QDir(controlGroup).exists())
        return counters;
    counters.valid = true;
    counters.currentBytes = readLimit(controlGroup + QStringLiteral("/memory.current"));
    counters.peakBytes = readLimit(controlGroup + QStringLiteral("/memory.peak"));
    counters.swapBytes =
        readLimit(controlGroup + QStringLiteral("/memory.swap.current"));
    counters.highBytes = readLimit(controlGroup + QStringLiteral("/memory.high"));
    counters.maxBytes = readLimit(controlGroup + QStringLiteral("/memory.max"));
    parsePressureStallFile(readSmall(controlGroup + QStringLiteral("/memory.pressure")),
                           &counters.someAvg10, &counters.fullAvg10,
                           &counters.fullAvg60);
    const QList<QByteArray> events =
        readSmall(controlGroup + QStringLiteral("/memory.events")).split('\n');
    for (const QByteArray &line : events) {
        const QList<QByteArray> parts = line.trimmed().split(' ');
        if (parts.size() != 2)
            continue;
        const qint64 value = parts.at(1).toLongLong();
        if (parts.at(0) == "high")
            counters.highEvents = value;
        else if (parts.at(0) == "max")
            counters.maxEvents = value;
        else if (parts.at(0) == "oom")
            counters.oomEvents = value;
        else if (parts.at(0) == "oom_kill")
            counters.oomKills = value;
    }
    counters.cpuMaxPercent =
        parseCpuMaxPercent(readSmall(controlGroup + QStringLiteral("/cpu.max")));
    const QByteArray cpuStat = readSmall(controlGroup + QStringLiteral("/cpu.stat"));
    counters.cpuUsageUsec = parseCpuUsageUsec(cpuStat);
    for (const QByteArray &line : cpuStat.split('\n')) {
        const QList<QByteArray> parts = line.trimmed().split(' ');
        if (parts.size() != 2)
            continue;
        if (parts.at(0) == "nr_throttled")
            counters.cpuThrottledPeriods = parts.at(1).toLongLong();
        else if (parts.at(0) == "throttled_usec")
            counters.cpuThrottledUsec = parts.at(1).toLongLong();
    }
    parsePressureStallFile(readSmall(controlGroup + QStringLiteral("/cpu.pressure")),
                           &counters.cpuSomeAvg10, &counters.cpuFullAvg10, nullptr);
    return counters;
}

bool parseProcStatCpu(const QByteArray &text, qint64 *busyTicks,
                      qint64 *totalTicks, int *cpus)
{
    bool found = false;
    int count = 0;
    for (const QByteArray &line : text.split('\n')) {
        if (!line.startsWith("cpu"))
            continue;
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.at(0) != "cpu") {
            ++count;
            continue;
        }
        if (fields.size() < 5)
            return false;
        qint64 total = 0;
        qint64 idle = 0;
        for (int i = 1; i < fields.size() && i <= 8; ++i) {
            const qint64 value = fields.at(i).toLongLong();
            total += value;
            if (i == 4 || i == 5)
                idle += value;
        }
        if (busyTicks)
            *busyTicks = total - idle;
        if (totalTicks)
            *totalTicks = total;
        found = true;
    }
    if (found && cpus)
        *cpus = count;
    return found;
}

qint64 parseCpuUsageUsec(const QByteArray &cpuStat)
{
    for (const QByteArray &line : cpuStat.split('\n')) {
        const QList<QByteArray> parts = line.trimmed().split(' ');
        if (parts.size() == 2 && parts.at(0) == "usage_usec") {
            bool ok = false;
            const qint64 value = parts.at(1).toLongLong(&ok);
            return ok ? value : -1;
        }
    }
    return -1;
}

int parseCpuMaxPercent(const QByteArray &cpuMax)
{
    const QList<QByteArray> parts = cpuMax.trimmed().split(' ');
    if (parts.size() != 2 || parts.at(0) == "max")
        return -1;
    const qint64 quota = parts.at(0).toLongLong();
    const qint64 period = parts.at(1).toLongLong();
    if (quota <= 0 || period <= 0)
        return -1;
    return int((quota * 100 + period / 2) / period);
}

GovernorSnapshot governorSnapshot()
{
    if (g_instance)
        return g_instance->snapshot();
    GovernorSnapshot snapshot;
    snapshot.tickMs = kTickMs;
    snapshot.status = status();
    return snapshot;
}

GovernorDecision decide(const GovernorPolicy &policy,
                        const PressureInput &pressure,
                        const QList<UnitSample> &units, qint64 nowMs,
                        GovernorState &state)
{
    GovernorDecision decision;
    QSet<QString> present;
    QHash<QString, const UnitSample *> byName;
    for (const UnitSample &unit : units) {
        present.insert(unit.name);
        byName.insert(unit.name, &unit);
        if (!state.firstSeenMs.contains(unit.name))
            state.firstSeenMs.insert(unit.name, nowMs);
    }
    for (auto it = state.firstSeenMs.begin(); it != state.firstSeenMs.end();)
        it = present.contains(it.key()) ? std::next(it) : state.firstSeenMs.erase(it);
    for (auto it = state.frozenAtMs.begin(); it != state.frozenAtMs.end();) {
        const UnitSample *unit = byName.value(it.key());
        it = unit && unit->frozen ? std::next(it) : state.frozenAtMs.erase(it);
    }

    QString oldestFrozen;
    qint64 oldestFrozenAt = 0;
    for (auto it = state.frozenAtMs.cbegin(); it != state.frozenAtMs.cend(); ++it)
        if (oldestFrozen.isEmpty() || it.value() < oldestFrozenAt) {
            oldestFrozen = it.key();
            oldestFrozenAt = it.value();
        }

    if (!oldestFrozen.isEmpty() && nowMs - oldestFrozenAt >= policy.maxFrozenMs) {
        decision.thaw = oldestFrozen;
        decision.reason = QStringLiteral("frozen for %1 minutes")
                              .arg((nowMs - oldestFrozenAt) / 60'000);
        state.frozenAtMs.remove(oldestFrozen);
        qint64 earliest = nowMs;
        for (const qint64 seen : std::as_const(state.firstSeenMs))
            earliest = qMin(earliest, seen);
        state.firstSeenMs.insert(oldestFrozen, earliest - 1);
        state.lastThawMs = nowMs;
        return decision;
    }

    if (pressure.memoryFullAvg10 >= policy.freezeAbovePercent &&
        nowMs - state.lastFreezeMs >= policy.freezeCooldownMs) {
        const UnitSample *pick =
            byName.value(nextFreezeCandidate(policy, units, state));
        if (pick) {
            decision.freeze = pick->name;
            decision.reason =
                QStringLiteral("memory stall %1% (avg10), %2 held by the unit")
                    .arg(pressure.memoryFullAvg10, 0, 'f', 1)
                    .arg(megabytes(pick->memoryBytes));
            state.frozenAtMs.insert(pick->name, nowMs);
            state.lastFreezeMs = nowMs;
            return decision;
        }
    }

    if (!oldestFrozen.isEmpty() &&
        pressure.memoryFullAvg10 < policy.thawBelowPercent &&
        (pressure.availableBytes < 0 ||
         pressure.availableBytes >= policy.thawMinAvailableBytes) &&
        nowMs - state.lastThawMs >= policy.thawCooldownMs &&
        nowMs - state.lastFreezeMs >= policy.thawCooldownMs) {
        decision.thaw = oldestFrozen;
        decision.reason =
            QStringLiteral("memory stall down to %1% (avg10)")
                .arg(pressure.memoryFullAvg10, 0, 'f', 1);
        state.frozenAtMs.remove(oldestFrozen);
        state.lastThawMs = nowMs;
    }
    return decision;
}

CpuDecision decideCpu(const GovernorPolicy &policy, const Budget &budget,
                      const CpuInput &input, qint64 nowMs, GovernorState &state)
{
    CpuDecision decision;
    if (!input.valid || budget.sliceCpuQuota <= 0)
        return decision;
    if (state.cpuQuota <= 0)
        state.cpuQuota = budget.sliceCpuQuota;
    state.otherCores =
        state.otherCores < 0.0
            ? input.otherCores
            : qMax(input.otherCores, (state.otherCores + input.otherCores) / 2.0);
    const int wanted =
        qBound(budget.sliceCpuFloor,
               budget.sliceCpuQuota - int(std::lround(state.otherCores * 100.0)),
               budget.sliceCpuQuota);
    const int step = qMax(100, budget.cpuCount * 5);
    const int current = state.cpuQuota;
    const qint64 sinceChange = nowMs - state.lastCpuChangeMs;
    int next = current;
    if (wanted < current && sinceChange >= policy.cpuLowerCooldownMs &&
        (current - wanted >= step || wanted == budget.sliceCpuFloor))
        next = wanted;
    else if (wanted > current && sinceChange >= policy.cpuRaiseCooldownMs &&
             (wanted - current >= step || wanted == budget.sliceCpuQuota))
        next = qMin(wanted, current + qMax(step, budget.cpuCount * 100 / 8));
    if (next == current)
        return decision;
    state.cpuQuota = next;
    state.lastCpuChangeMs = nowMs;
    ++state.cpuChanges;
    decision.quota = next;
    decision.reason =
        QStringLiteral("other programs use %1, agents %2; agent CPU cap %3 "
                       "to %4")
            .arg(cpuCores(int(std::lround(input.otherCores * 100.0))),
                 cpuCores(int(std::lround(input.sliceCores * 100.0))),
                 cpuCores(current), cpuCores(next));
    return decision;
}

int retargetCpuQuota(const Budget &budget, const GovernorState &state)
{
    if (budget.sliceCpuQuota <= 0)
        return 0;
    if (state.otherCores < 0.0)
        return budget.sliceCpuQuota;
    return qBound(budget.sliceCpuFloor,
                  budget.sliceCpuQuota - int(std::lround(state.otherCores * 100.0)),
                  budget.sliceCpuQuota);
}

QStringList agentScopes(const QList<UnitSample> &units)
{
    QStringList scopes;
    for (const UnitSample &unit : units)
        if (unit.name.startsWith(QLatin1String("forkmesh-agent-")) &&
            unit.name.endsWith(QLatin1String(".scope")) &&
            !unit.name.startsWith(QLatin1String("forkmesh-agent-probe-")))
            scopes.append(unit.name);
    return scopes;
}

void applyBudget()
{
    if (g_instance)
        g_instance->applyBudget();
}

Governor::Governor(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &Governor::tick);
    if (!g_instance)
        g_instance = this;
}

Governor::~Governor()
{
    if (g_instance == this)
        g_instance = nullptr;
    m_timer.stop();
    if (m_prepareThread) {
        m_prepareThread->wait();
        delete m_prepareThread;
    }
    thawAll();
    restoreCpuQuota();
}

void Governor::start()
{
    if (m_prepareThread)
        return;
    m_prepareThread = QThread::create([] { prepare(); });
    connect(m_prepareThread, &QThread::finished, this, [this] {
        if (m_prepareThread)
            m_prepareThread->deleteLater();
        prepared();
    });
    m_prepareThread->start(QThread::LowPriority);
}

GovernorSnapshot Governor::snapshot() const
{
    GovernorSnapshot snapshot;
    snapshot.running = m_running;
    snapshot.tickMs = kTickMs;
    snapshot.startedMs = m_startedMs;
    snapshot.lastTickMs = m_lastTickMs;
    snapshot.status = m_running ? m_status : status();
    snapshot.policy = m_policy;
    snapshot.pressure = m_lastPressure;
    snapshot.machineFullAvg10 = m_lastMachineFull;
    snapshot.sliceFullAvg10 = m_lastSliceFull;
    snapshot.cpu = m_lastCpu;
    snapshot.units = m_lastUnits;
    snapshot.state = m_state;
    snapshot.events = m_events;
    return snapshot;
}

void Governor::record(const QString &action, const QString &unit,
                      const QString &reason, bool failed)
{
    m_events.append({QDateTime::currentMSecsSinceEpoch(), action, unit, reason,
                     failed});
    if (m_events.size() > kMaxEvents)
        m_events.erase(m_events.begin(),
                       m_events.begin() + (m_events.size() - kMaxEvents));
}

void Governor::prepared()
{
    m_status = status();
    qInfo().noquote() << describe(m_status);
    record(QStringLiteral("start"), QString(), describe(m_status));
    if (!m_status.available || m_status.controlGroup.isEmpty())
        return;
    for (const UnitSample &unit : readSliceUnits(m_status.controlGroup))
        if (unit.frozen && freezable(unit.name)) {
            qInfo().noquote()
                << QStringLiteral("Agent memory: thawing %1, left frozen by an "
                                  "earlier run of the desktop.")
                       .arg(unit.name);
            record(QStringLiteral("thaw"), unit.name,
                   QStringLiteral("left frozen by an earlier run of the desktop"));
            runSystemctl(QStringLiteral("thaw"), unit.name);
        }
    m_state.cpuQuota = m_status.budget.sliceCpuQuota;
    m_cpuClock.start();
    m_running = true;
    m_startedMs = QDateTime::currentMSecsSinceEpoch();
    m_timer.start();
}

void Governor::applyBudget()
{
    m_status.budget = status().budget;
    if (!m_running || m_status.systemctl.isEmpty() || !m_status.budget.valid())
        return;
    const Budget &budget = m_status.budget;
    QStringList properties{QStringLiteral("--runtime"),
                           QStringLiteral("MemoryHigh=%1").arg(budget.sliceHighBytes),
                           QStringLiteral("MemoryMax=%1").arg(budget.sliceMaxBytes)};
    const int quota = retargetCpuQuota(budget, m_state);
    if (quota > 0) {
        m_state.cpuQuota = quota;
        m_state.lastCpuChangeMs = QDateTime::currentMSecsSinceEpoch();
        ++m_state.cpuChanges;
        properties << QStringLiteral("CPUQuota=%1%").arg(quota);
    }
    const QString reason =
        QStringLiteral("limits changed: agents throttled above %1 (hard limit %2), "
                       "CPU ceiling %3, cap now %4")
            .arg(gigabytesText(budget.sliceHighBytes), gigabytesText(budget.sliceMaxBytes),
                 cpuCores(budget.sliceCpuQuota),
                 cpuCores(quota > 0 ? quota : budget.sliceCpuQuota));
    qInfo().noquote() << QStringLiteral("Agent resources: %1.").arg(reason);
    record(QStringLiteral("limits"), sliceName(), reason);
    runSystemctl(QStringLiteral("set-property"), sliceName(), properties);
    for (const QString &scope : agentScopes(m_lastUnits))
        runSystemctl(QStringLiteral("set-property"), scope,
                     {QStringLiteral("--runtime"),
                      QStringLiteral("MemoryHigh=%1").arg(budget.agentHighBytes),
                      QStringLiteral("MemoryMax=%1").arg(budget.agentMaxBytes)});
}

void Governor::tick()
{
    const SystemPressureSnapshot snapshot = cachedSystemPressure(1000);
    PressureInput input;
    m_lastMachineFull = snapshot.valid ? snapshot.memoryFullAvg10 : 0.0;
    m_lastSliceFull = slicePressure(m_status.controlGroup);
    input.memoryFullAvg10 = qMax(m_lastMachineFull, m_lastSliceFull);
    input.availableBytes = snapshot.memoryAvailableBytes;
    m_lastPressure = input;
    m_lastUnits = readSliceUnits(m_status.controlGroup);
    m_lastCpu = sampleCpu(m_lastUnits);
    m_lastCpu.someAvg10 = snapshot.valid ? snapshot.cpuSomeAvg10 : 0.0;
    m_lastTickMs = QDateTime::currentMSecsSinceEpoch();
    const GovernorDecision decision =
        decide(m_policy, input, m_lastUnits, m_lastTickMs, m_state);
    if (!decision.freeze.isEmpty()) {
        qInfo().noquote()
            << QStringLiteral("Agent memory: froze %1 so the other agent work "
                              "can finish (%2). It resumes when the stall "
                              "clears.")
                   .arg(decision.freeze, decision.reason);
        record(QStringLiteral("freeze"), decision.freeze, decision.reason);
        runSystemctl(QStringLiteral("freeze"), decision.freeze);
    }
    if (!decision.thaw.isEmpty()) {
        qInfo().noquote() << QStringLiteral("Agent memory: resumed %1 (%2).")
                                 .arg(decision.thaw, decision.reason);
        record(QStringLiteral("thaw"), decision.thaw, decision.reason);
        runSystemctl(QStringLiteral("thaw"), decision.thaw);
    }
    const CpuDecision cpu = decideCpu(m_policy, m_status.budget, m_lastCpu,
                                      m_lastTickMs, m_state);
    if (cpu.quota > 0) {
        qInfo().noquote() << QStringLiteral("Agent CPU: %1.").arg(cpu.reason);
        record(QStringLiteral("cpu cap"), sliceName(), cpu.reason);
        runSystemctl(QStringLiteral("set-property"), sliceName(),
                     {QStringLiteral("--runtime"),
                      QStringLiteral("CPUQuota=%1%").arg(cpu.quota)});
    }
}

CpuInput Governor::sampleCpu(QList<UnitSample> &units)
{
    CpuInput input;
    qint64 busy = -1;
    qint64 total = -1;
    int cpus = 0;
    const bool machine = parseProcStatCpu(readSmall(QStringLiteral("/proc/stat")),
                                          &busy, &total, &cpus);
    const qint64 sliceUsec = m_status.controlGroup.isEmpty()
                                 ? -1
                                 : parseCpuUsageUsec(readSmall(
                                       m_status.controlGroup + QStringLiteral("/cpu.stat")));
    const qint64 nowMs = m_cpuClock.isValid() ? m_cpuClock.elapsed() : -1;
    const qint64 elapsedMs = m_cpuSampleMs >= 0 ? nowMs - m_cpuSampleMs : 0;
    QHash<QString, qint64> unitUsec;
    for (UnitSample &unit : units) {
        if (unit.cpuUsageUsec < 0)
            continue;
        unitUsec.insert(unit.name, unit.cpuUsageUsec);
        const qint64 before = m_unitUsec.value(unit.name, -1);
        if (elapsedMs > 0 && before >= 0 && unit.cpuUsageUsec >= before)
            unit.cpuCores = double(unit.cpuUsageUsec - before) / (elapsedMs * 1000.0);
    }
    if (machine && sliceUsec >= 0 && elapsedMs > 0 && m_totalTicks >= 0 &&
        total > m_totalTicks && m_sliceUsec >= 0 && sliceUsec >= m_sliceUsec &&
        cpus > 0) {
        input.valid = true;
        input.machineCpus = cpus;
        input.machineBusyCores = double(busy - m_busyTicks) /
                                 double(total - m_totalTicks) * cpus;
        input.sliceCores = double(sliceUsec - m_sliceUsec) / (elapsedMs * 1000.0);
        input.otherCores = qMax(0.0, input.machineBusyCores - input.sliceCores);
    }
    m_unitUsec = unitUsec;
    m_cpuSampleMs = nowMs;
    m_busyTicks = machine ? busy : -1;
    m_totalTicks = machine ? total : -1;
    m_sliceUsec = sliceUsec;
    return input;
}

void Governor::restoreCpuQuota()
{
    const int ceiling = m_status.budget.sliceCpuQuota;
    if (m_status.systemctl.isEmpty() || ceiling <= 0 || m_state.cpuQuota <= 0 ||
        m_state.cpuQuota == ceiling)
        return;
    QProcess process;
    process.start(m_status.systemctl,
                  {QStringLiteral("--user"), QStringLiteral("set-property"),
                   QStringLiteral("--runtime"), sliceName(),
                   QStringLiteral("CPUQuota=%1%").arg(ceiling)});
    if (!process.waitForFinished(kThawOnExitTimeoutMs))
        process.kill();
    m_state.cpuQuota = ceiling;
}

void Governor::runSystemctl(const QString &verb, const QString &unit,
                            const QStringList &extra)
{
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this,
            [this, process, verb, unit](int code, QProcess::ExitStatus status) {
                if (status != QProcess::NormalExit || code != 0) {
                    const QString error =
                        QString::fromUtf8(process->readAllStandardError())
                            .trimmed();
                    qWarning().noquote()
                        << QStringLiteral("Agent memory: systemctl %1 %2 "
                                          "failed: %3")
                               .arg(verb, unit, error);
                    record(verb, unit,
                           QStringLiteral("systemctl failed: %1").arg(error),
                           true);
                }
                process->deleteLater();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, verb, unit](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    record(verb, unit,
                           QStringLiteral("systemctl did not start: %1")
                               .arg(process->errorString()),
                           true);
                    process->deleteLater();
                }
            });
    process->start(m_status.systemctl,
                   QStringList{QStringLiteral("--user"), verb, unit} + extra);
}

void Governor::thawAll()
{
    if (m_status.systemctl.isEmpty())
        return;
    for (auto it = m_state.frozenAtMs.cbegin(); it != m_state.frozenAtMs.cend();
         ++it) {
        QProcess process;
        process.start(m_status.systemctl,
                      {QStringLiteral("--user"), QStringLiteral("thaw"),
                       it.key()});
        if (!process.waitForFinished(kThawOnExitTimeoutMs))
            process.kill();
    }
    m_state.frozenAtMs.clear();
}

} // namespace forkmesh::agentresources
