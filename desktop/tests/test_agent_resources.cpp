#include "../src/AgentResourceGovernor.h"
#include "../src/AgentResources.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <limits>

namespace ar = forkmesh::agentresources;

namespace {

int g_failures = 0;
constexpr qint64 kGiB = qint64(1) << 30;
constexpr qint64 kMiB = qint64(1) << 20;

void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

void testBudget()
{
    const qint64 total = qint64(32756204) * 1024;
    const ar::Budget b = ar::computeBudget(total, 36, 20);
    check(b.valid(), "a 32 GB box has a budget");
    check(b.reserveBytes == total / 5, "20% of memory is kept back");
    check(b.sliceHighBytes == total - b.reserveBytes,
          "the slice is throttled above everything but the reserve");
    check(b.sliceMaxBytes == total - b.reserveBytes / 2,
          "the slice hard limit is half-way into the reserve");
    check(b.sliceHighBytes < b.sliceMaxBytes && b.sliceMaxBytes < total,
          "throttle < hard limit < physical memory");
    check(b.agentHighBytes == b.sliceHighBytes * 3 / 5,
          "one agent is throttled at 60% of the slice");
    check(b.agentMaxBytes == b.sliceHighBytes * 4 / 5,
          "one agent is capped at 80% of the slice");
    check(b.jobsPerAgent == 4, "about 3 GB per build job");

    const ar::Budget small = ar::computeBudget(4 * kGiB, 4, 20);
    check(small.reserveBytes == 2 * kGiB, "at least 2 GB is kept back");
    check(small.jobsPerAgent == 2, "a small box still gets two jobs");

    const ar::Budget tiny = ar::computeBudget(2 * kGiB, 1, 20);
    check(tiny.reserveBytes == kGiB, "the reserve never exceeds half of memory");
    check(tiny.jobsPerAgent == 1, "one cpu means one job");

    check(ar::computeBudget(16 * kGiB, 8, 1).reserveBytes ==
              ar::computeBudget(16 * kGiB, 8, ar::kMinReservePercent).reserveBytes,
          "reserve percent is clamped from below");
    check(ar::computeBudget(16 * kGiB, 8, 99).reserveBytes == 8 * kGiB,
          "reserve percent is clamped from above");
    check(!ar::computeBudget(0, 8, 20).valid(), "unknown memory has no budget");
}

void testCpuBudget()
{
    const ar::Budget b = ar::computeBudget(32 * kGiB, 36, 20);
    check(b.cpuCount == 36, "cores are recorded");
    check(b.cpuReserve == 720, "20% of the cores are kept back");
    check(b.sliceCpuQuota == 2880, "agents get the other 28.8 cores");
    check(b.sliceCpuFloor == 900, "the cap never drops below a quarter");
    check(ar::cpuCores(b.sliceCpuQuota) == QStringLiteral("28.8 cores"),
          "cores are shown with one decimal");

    const ar::Budget four = ar::computeBudget(8 * kGiB, 4, 20);
    check(four.cpuReserve == 100, "at least one core is kept back");
    check(four.sliceCpuQuota == 300 && four.sliceCpuFloor == 100,
          "a four-core box caps agents at three");

    const ar::Budget one = ar::computeBudget(4 * kGiB, 1, 20);
    check(one.sliceCpuQuota == 50 && one.sliceCpuFloor == 50,
          "the reserve never exceeds half of the cores");

    check(ar::computeBudget(32 * kGiB, 36, 20, 50).sliceCpuQuota == 1800,
          "the cpu reserve has its own percent");
    check(ar::computeBudget(32 * kGiB, 36, 20, 99).sliceCpuQuota == 1800,
          "cpu reserve percent is clamped from above");
    check(ar::computeBudget(32 * kGiB, 36, 20, 0).sliceCpuQuota == 3420,
          "cpu reserve percent is clamped from below");
}

void testCpuParsers()
{
    const QByteArray stat =
        "cpu  100 10 50 800 40 0 5 0 0 0\n"
        "cpu0 50 5 25 400 20 0 2 0 0 0\n"
        "cpu1 50 5 25 400 20 0 3 0 0 0\n"
        "intr 12345\n";
    qint64 busy = 0;
    qint64 total = 0;
    int cpus = 0;
    check(ar::parseProcStatCpu(stat, &busy, &total, &cpus), "/proc/stat parses");
    check(total == 1005 && busy == 165, "idle and iowait are not busy");
    check(cpus == 2, "online cores are counted");
    check(!ar::parseProcStatCpu("intr 1\n", &busy, &total, &cpus),
          "no cpu line, no sample");

    check(ar::parseCpuUsageUsec("usage_usec 1234\nuser_usec 1000\n") == 1234,
          "cpu.stat usage");
    check(ar::parseCpuUsageUsec("") == -1, "missing cpu.stat");
    check(ar::parseCpuMaxPercent("288000 100000\n") == 288, "cpu.max quota");
    check(ar::parseCpuMaxPercent("max 100000\n") == -1, "no cpu limit");
}

void testCpuGovernor()
{
    const ar::GovernorPolicy policy;
    const ar::Budget budget = ar::computeBudget(32 * kGiB, 36, 20);
    ar::GovernorState state;
    qint64 now = 1'000'000;
    auto input = [](double others, double slice) {
        ar::CpuInput in;
        in.valid = true;
        in.machineCpus = 36;
        in.otherCores = others;
        in.sliceCores = slice;
        in.machineBusyCores = others + slice;
        return in;
    };

    ar::CpuDecision d = ar::decideCpu(policy, budget, ar::CpuInput{}, now, state);
    check(d.quota == 0, "no sample, no change");

    d = ar::decideCpu(policy, budget, input(0.5, 28.0), now, state);
    check(d.quota == 0 && state.cpuQuota == budget.sliceCpuQuota,
          "a quiet desktop leaves agents at the ceiling");

    now += 5000;
    d = ar::decideCpu(policy, budget, input(6.0, 28.0), now, state);
    check(d.quota > 0 && d.quota <= budget.sliceCpuQuota - 300,
          "a busy desktop lowers the agent cap at once");
    check(d.reason.contains(QStringLiteral("cores")), "reason names the cores");
    const int lowered = d.quota;

    now += 1000;
    d = ar::decideCpu(policy, budget, input(12.0, 20.0), now, state);
    check(d.quota == 0, "cap changes are spaced by the cooldown");

    now += policy.cpuLowerCooldownMs;
    d = ar::decideCpu(policy, budget, input(40.0, 2.0), now, state);
    check(d.quota == budget.sliceCpuFloor, "the cap stops at the floor");

    for (int i = 0; i < 2; ++i) {
        now += 5000;
        ar::decideCpu(policy, budget, input(0.0, 9.0), now, state);
    }
    check(state.cpuQuota == budget.sliceCpuFloor,
          "the cap is raised only after the raise cooldown");
    int last = state.cpuQuota;
    bool stepped = true;
    for (int i = 0; i < 20; ++i) {
        now += policy.cpuRaiseCooldownMs;
        ar::decideCpu(policy, budget, input(0.0, 9.0), now, state);
        if (state.cpuQuota - last > budget.cpuCount * 100 / 8)
            stepped = false;
        last = state.cpuQuota;
    }
    check(stepped, "the cap is raised in steps");
    check(state.cpuQuota == budget.sliceCpuQuota, "and gets back to the ceiling");
    check(lowered < budget.sliceCpuQuota, "lowered below the ceiling");

    ar::GovernorState small;
    small.cpuQuota = budget.sliceCpuQuota;
    d = ar::decideCpu(policy, budget, input(0.8, 20.0), now, small);
    check(d.quota == 0, "small changes are ignored");
}

void testArguments()
{
    const ar::Budget b = ar::computeBudget(32 * kGiB, 16, 20);
    const QStringList slice = ar::sliceProperties(b);
    check(slice.contains(QStringLiteral("MemoryHigh=%1").arg(b.sliceHighBytes)),
          "slice MemoryHigh");
    check(slice.contains(QStringLiteral("MemoryMax=%1").arg(b.sliceMaxBytes)),
          "slice MemoryMax");
    check(slice.contains(QStringLiteral("CPUWeight=50")),
          "agent work yields the cpu to the desktop");
    check(slice.contains(QStringLiteral("CPUQuota=%1%").arg(b.sliceCpuQuota)),
          "agent work is capped below the machine's cores");

    check(ar::scopeUnitName(QStringLiteral("Claude 788"), 4659, 3) ==
              QStringLiteral("forkmesh-agent-claude-788-4659-3.scope"),
          "unit name is sanitized");
    check(ar::scopeUnitName(QStringLiteral("  ***  "), 1, 1) ==
              QStringLiteral("forkmesh-agent-agent-1-1.scope"),
          "an empty label falls back to agent");

    const QStringList with = ar::scopeArguments(
        QStringLiteral("u.scope"), QStringLiteral("d"), b, true);
    check(with.contains(QStringLiteral("--scope")) &&
              with.contains(QStringLiteral("--user")),
          "a user scope");
    check(with.contains(QStringLiteral("--slice=forkmesh.slice")),
          "scope joins the agent slice");
    check(with.contains(QStringLiteral("--property=MemoryMax=%1")
                            .arg(b.agentMaxBytes)),
          "per-agent cap");
    check(with.contains(QStringLiteral("--property=OOMPolicy=continue")),
          "an OOM kill of one build leaves the agent running");
    check(!ar::scopeArguments(QStringLiteral("u.scope"), QStringLiteral("d"), b,
                              false)
               .contains(QStringLiteral("--property=OOMPolicy=continue")),
          "OOMPolicy is left out when systemd rejects it");
}

QString runShim(const QString &shim, const QStringList &arguments)
{
    QProcess process;
    process.start(QStringLiteral("/bin/sh"), QStringList{shim} + arguments);
    process.waitForFinished(5000);
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

void testShim()
{
#ifdef Q_OS_UNIX
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir");
    const QString fake = dir.filePath(QStringLiteral("real it's"));
    QFile fakeFile(fake);
    check(fakeFile.open(QIODevice::WriteOnly), "write fake systemd-run");
    fakeFile.write("#!/bin/sh\necho \"$*\"\n");
    fakeFile.close();
    fakeFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                            QFileDevice::ExeOwner);
    const QString shim = dir.filePath(QStringLiteral("systemd-run"));
    QFile shimFile(shim);
    check(shimFile.open(QIODevice::WriteOnly), "write shim");
    shimFile.write(ar::shimScript(fake).toUtf8());
    shimFile.close();

    check(runShim(shim, {QStringLiteral("--user"), QStringLiteral("--unit=b"),
                         QStringLiteral("ninja")}) ==
              QStringLiteral("--slice=forkmesh.slice --user --unit=b ninja"),
          "a user unit is put in the agent slice");
    check(runShim(shim, {QStringLiteral("--user"),
                         QStringLiteral("--slice=mine.slice"),
                         QStringLiteral("make")}) ==
              QStringLiteral("--user --slice=mine.slice make"),
          "an explicit slice is kept");
    check(runShim(shim, {QStringLiteral("--user"), QStringLiteral("-p"),
                         QStringLiteral("Slice=x.slice"), QStringLiteral("make")}) ==
              QStringLiteral("--user -p Slice=x.slice make"),
          "a Slice property is kept");
    check(runShim(shim, {QStringLiteral("--unit=sys"), QStringLiteral("make")}) ==
              QStringLiteral("--unit=sys make"),
          "system units are passed through");
#endif
}

void testEnvironment()
{
    ar::Budget b = ar::computeBudget(32 * kGiB, 16, 20);
    b.jobsPerAgent = 4;
    QProcessEnvironment env;
    env.insert(QStringLiteral("CARGO_BUILD_JOBS"), QStringLiteral("12"));
    env.insert(QStringLiteral("GRADLE_OPTS"),
               QStringLiteral("-Xmx2g -Dorg.gradle.workers.max=8"));
    ar::mergeEnvironment(env, ar::environmentDefaults(b));
    check(env.value(QStringLiteral("CMAKE_BUILD_PARALLEL_LEVEL")) ==
              QStringLiteral("4"),
          "cmake --build job cap");
    check(env.value(QStringLiteral("PYTEST_XDIST_AUTO_NUM_WORKERS")) ==
              QStringLiteral("4"),
          "pytest -n auto cap");
    check(env.value(QStringLiteral("CARGO_BUILD_JOBS")) == QStringLiteral("12"),
          "an explicit setting wins");
    check(env.value(QStringLiteral("GRADLE_OPTS")) ==
              QStringLiteral("-Xmx2g -Dorg.gradle.workers.max=8 "
                             "-Dorg.gradle.daemon.idletimeout=600000"),
          "GRADLE_OPTS gains only the missing properties");
    check(env.value(QStringLiteral("FORKMESH_AGENT_SLICE")) ==
              QStringLiteral("forkmesh.slice"),
          "agents can name the slice");
    check(!env.contains(QStringLiteral("MAKEFLAGS")),
          "plain make stays serial");
}

void testUnprepared()
{
    check(!ar::status().available, "nothing is contained before prepare()");
    const ar::Launch launch = ar::contain(QStringLiteral("bash"),
                                          {QStringLiteral("-lc"), QStringLiteral("x")},
                                          QStringLiteral("claude"));
    check(!launch.contained && launch.program == QStringLiteral("bash") &&
              launch.arguments.size() == 2,
          "contain() is a no-op when unavailable");
    check(ar::wrapShellCommand(QStringLiteral("exec claude")) ==
              QStringLiteral("exec claude"),
          "no PATH prefix without a shim");
    QProcessEnvironment env;
    ar::applyEnvironment(env);
    check(env.isEmpty(), "no build caps before prepare()");
    check(ar::describe(ar::status()).contains(QStringLiteral("off")),
          "status line says containment is off");
    QProcessEnvironment labelled;
    labelled.insert(QStringLiteral("FORKMESH_AGENT_GATE_SESSION"),
                    QStringLiteral("788:abc"));
    check(ar::launchLabel(QStringLiteral("claude"), labelled) ==
              QStringLiteral("claude 788"),
          "label carries the session number");
}

QList<ar::UnitSample> units(std::initializer_list<ar::UnitSample> list)
{
    return QList<ar::UnitSample>(list);
}

void testRetargetCpu()
{
    const ar::Budget budget = ar::computeBudget(32 * kGiB, 16, 20);
    ar::GovernorState state;
    check(ar::retargetCpuQuota(budget, state) == budget.sliceCpuQuota,
          "with nothing measured yet a new budget starts at its ceiling");
    state.otherCores = 2.0;
    check(ar::retargetCpuQuota(budget, state) == budget.sliceCpuQuota - 200,
          "a new budget leaves room for what other programs use now");
    state.otherCores = 15.0;
    check(ar::retargetCpuQuota(budget, state) == budget.sliceCpuFloor,
          "a new budget never drops below its floor");
    check(ar::retargetCpuQuota(ar::Budget{}, state) == 0, "no budget, no cap");

    const QStringList scopes = ar::agentScopes(
        units({{QStringLiteral("forkmesh-agent-claude-1-2.scope")},
               {QStringLiteral("forkmesh-agent-probe-1-1.scope")},
               {QStringLiteral("forkmesh-action-build-1.scope")},
               {QStringLiteral("run-u12.service")}}));
    check(scopes == QStringList{QStringLiteral("forkmesh-agent-claude-1-2.scope")},
          "only running agent scopes get the new per-agent limits");
}

void testReserveSettings()
{
    QTemporaryDir home;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, home.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.path());
    const ar::Status changed = ar::setReservePercents(35, 99);
    const QSettings saved;
    check(saved.value(ar::kReservePercentSetting).toInt() == 35 &&
              saved.value(ar::kCpuReservePercentSetting).toInt() == ar::kMaxReservePercent,
          "the sliders save both reserve percents, clamped");
    check(changed.budget.totalBytes == ar::budgetFor(35, 99).totalBytes &&
              changed.budget.sliceCpuQuota == ar::budgetFor(35, 99).sliceCpuQuota &&
              ar::status().budget.sliceHighBytes == ar::budgetFor(35, 99).sliceHighBytes,
          "new agents get the new budget at once");
    check(ar::currentBudget().reserveBytes == ar::budgetFor(35, 60).reserveBytes,
          "the saved percents are read back on the next start");
    ar::applyBudget();
}

void testPolicy()
{
    check(!ar::freezable(QStringLiteral("forkmesh-agent-claude-1-1-1.scope")),
          "agent CLIs are never frozen");
    check(!ar::freezable(QStringLiteral("forkmesh-action-1-2-0.scope")),
          "Actions steps are never frozen");
    check(ar::freezable(QStringLiteral("fm784-build.service")),
          "agent-started builds can be frozen");
    check(!ar::freezable(QStringLiteral("nested.slice")), "slices are skipped");

    const ar::GovernorPolicy policy;
    ar::GovernorState state;
    const ar::PressureInput calm{0.0, 10 * kGiB};
    const ar::PressureInput stall{45.0, kGiB};
    qint64 now = 1'000'000;

    ar::GovernorDecision d = ar::decide(
        policy, calm,
        units({{QStringLiteral("a.service"), 2 * kGiB, false}}), now, state);
    check(d.freeze.isEmpty() && d.thaw.isEmpty(), "calm: nothing happens");

    now += 5000;
    const auto two = units({{QStringLiteral("a.service"), 2 * kGiB, false},
                            {QStringLiteral("b.service"), 3 * kGiB, false}});
    d = ar::decide(policy, stall,
                   units({{QStringLiteral("a.service"), 2 * kGiB, false}}), now,
                   state);
    check(d.freeze.isEmpty(), "the only build is never frozen");

    now += 5000;
    d = ar::decide(policy, stall, two, now, state);
    check(d.freeze == QStringLiteral("b.service"), "the newest build is frozen");
    check(d.reason.contains(QStringLiteral("45.0%")), "reason names the stall");

    const auto oneFrozen = units({{QStringLiteral("a.service"), 2 * kGiB, false},
                                  {QStringLiteral("b.service"), 3 * kGiB, true},
                                  {QStringLiteral("c.service"), kGiB, false}});
    now += 5000;
    d = ar::decide(policy, stall, oneFrozen, now, state);
    check(d.freeze.isEmpty(), "freezes are spaced by the cooldown");

    now += policy.freezeCooldownMs;
    d = ar::decide(policy, stall, oneFrozen, now, state);
    check(d.freeze == QStringLiteral("c.service"),
          "a second build is frozen after the cooldown");

    const auto bothFrozen = units({{QStringLiteral("a.service"), 2 * kGiB, false},
                                   {QStringLiteral("b.service"), 3 * kGiB, true},
                                   {QStringLiteral("c.service"), kGiB, true}});
    now += policy.freezeCooldownMs;
    d = ar::decide(policy, stall, bothFrozen, now, state);
    check(d.freeze.isEmpty(), "the last running build keeps going");

    now += 1000;
    d = ar::decide(policy, ar::PressureInput{2.0, kGiB}, bothFrozen, now, state);
    check(d.thaw.isEmpty(), "no thaw while free memory is short");

    now += 1000;
    d = ar::decide(policy, calm, bothFrozen, now, state);
    check(d.thaw == QStringLiteral("b.service"),
          "the first frozen build resumes first");

    now += 1000;
    const auto cStill = units({{QStringLiteral("a.service"), 2 * kGiB, false},
                               {QStringLiteral("b.service"), 3 * kGiB, false},
                               {QStringLiteral("c.service"), kGiB, true}});
    d = ar::decide(policy, calm, cStill, now, state);
    check(d.thaw.isEmpty(), "thaws are spaced by the cooldown");
    now += policy.thawCooldownMs;
    d = ar::decide(policy, calm, cStill, now, state);
    check(d.thaw == QStringLiteral("c.service"), "then the next one resumes");

    ar::GovernorState stuck;
    const auto pair = units({{QStringLiteral("x.service"), 2 * kGiB, false},
                             {QStringLiteral("y.service"), 2 * kGiB, false}});
    qint64 t = 0;
    ar::decide(policy, stall, pair, t, stuck);
    t += policy.freezeCooldownMs;
    d = ar::decide(policy, stall, pair, t, stuck);
    check(!d.freeze.isEmpty(), "under a stall one of the pair is frozen");
    const QString frozen = d.freeze;
    const auto pairFrozen =
        units({{QStringLiteral("x.service"), 2 * kGiB, frozen == QStringLiteral("x.service")},
               {QStringLiteral("y.service"), 2 * kGiB, frozen == QStringLiteral("y.service")}});
    t += policy.maxFrozenMs;
    d = ar::decide(policy, stall, pairFrozen, t, stuck);
    check(d.thaw == frozen, "a unit is never frozen longer than the limit");
    t += policy.freezeCooldownMs;
    d = ar::decide(policy, stall, pair, t, stuck);
    check(!d.freeze.isEmpty() && d.freeze != frozen,
          "the other unit takes its turn after a forced thaw");

    ar::GovernorState small;
    const auto tinyUnits = units({{QStringLiteral("x.service"), 10 * kMiB, false},
                                  {QStringLiteral("y.service"), 20 * kMiB, false}});
    d = ar::decide(policy, stall, tinyUnits, policy.freezeCooldownMs, small);
    check(d.freeze.isEmpty(), "small units are not worth freezing");

    ar::GovernorState external;
    ar::decide(policy, stall, pair, policy.freezeCooldownMs, external);
    check(external.frozenAtMs.size() == 1, "freeze is recorded");
    ar::decide(policy, calm, pair, policy.freezeCooldownMs + 1000, external);
    check(external.frozenAtMs.isEmpty(),
          "a unit thawed by someone else is forgotten");
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(data);
}

void testSliceReader()
{
    QTemporaryDir dir;
    const QDir root(dir.path());
    root.mkpath(QStringLiteral("fm1-build.service"));
    root.mkpath(QStringLiteral("forkmesh-agent-claude-1-1-1.scope"));
    root.mkpath(QStringLiteral("inner.slice"));
    writeFile(root.filePath(QStringLiteral("fm1-build.service/memory.current")),
              "1073741824\n");
    writeFile(root.filePath(QStringLiteral("fm1-build.service/cgroup.events")),
              "populated 1\nfrozen 1\n");
    writeFile(root.filePath(
                  QStringLiteral("forkmesh-agent-claude-1-1-1.scope/memory.current")),
              "524288000\n");
    writeFile(root.filePath(
                  QStringLiteral("forkmesh-agent-claude-1-1-1.scope/cgroup.events")),
              "populated 1\nfrozen 0\n");
    writeFile(root.filePath(QStringLiteral("memory.pressure")),
              "some avg10=30.00 avg60=1.00 avg300=0.00 total=1\n"
              "full avg10=22.50 avg60=1.00 avg300=0.00 total=1\n");
    const QList<ar::UnitSample> read = ar::readSliceUnits(dir.path());
    check(read.size() == 2, "services and scopes are read, slices skipped");
    bool sawBuild = false;
    for (const ar::UnitSample &unit : read)
        if (unit.name == QStringLiteral("fm1-build.service")) {
            sawBuild = true;
            check(unit.memoryBytes == kGiB, "memory.current is read");
            check(unit.frozen, "frozen state is read");
        } else {
            check(!unit.frozen, "a running scope is not frozen");
        }
    check(sawBuild, "build unit is listed");
    check(ar::slicePressure(dir.path()) > 22.4 && ar::slicePressure(dir.path()) < 22.6,
          "slice memory pressure is read");
    check(ar::readSliceUnits(QString()).isEmpty(), "no slice, no units");
}

void testSliceCounters()
{
    QTemporaryDir dir;
    const QDir root(dir.path());
    writeFile(root.filePath(QStringLiteral("memory.current")), "2147483648\n");
    writeFile(root.filePath(QStringLiteral("memory.high")), "26575110144\n");
    writeFile(root.filePath(QStringLiteral("memory.max")), "max\n");
    writeFile(root.filePath(QStringLiteral("memory.pressure")),
              "some avg10=12.00 avg60=3.00 avg300=0.00 total=1\n"
              "full avg10=8.50 avg60=2.25 avg300=0.00 total=1\n");
    writeFile(root.filePath(QStringLiteral("memory.events")),
              "low 0\nhigh 41\nmax 3\noom 2\noom_kill 1\n");
    const ar::SliceCounters counters = ar::readSliceCounters(dir.path());
    check(counters.valid, "an existing slice is read");
    check(counters.currentBytes == 2 * kGiB, "slice memory.current is read");
    check(counters.highBytes == 26575110144LL, "slice memory.high is read");
    check(counters.maxBytes == std::numeric_limits<qint64>::max(),
          "an unlimited memory.max reads as no limit");
    check(counters.peakBytes == -1, "a missing file reads as unknown");
    check(counters.fullAvg10 > 8.4 && counters.fullAvg10 < 8.6 &&
              counters.someAvg10 > 11.9 && counters.fullAvg60 > 2.2,
          "slice pressure is read");
    check(counters.highEvents == 41 && counters.maxEvents == 3 &&
              counters.oomEvents == 2 && counters.oomKills == 1,
          "memory.events counters are read");
    check(!ar::readSliceCounters(QString()).valid, "no slice, no counters");
}

void testFreezeCandidate()
{
    const ar::GovernorPolicy policy;
    ar::GovernorState state;
    state.firstSeenMs.insert(QStringLiteral("old.service"), 100);
    state.firstSeenMs.insert(QStringLiteral("new.service"), 200);
    state.firstSeenMs.insert(QStringLiteral("tiny.service"), 300);
    const auto running = units({{QStringLiteral("old.service"), 2 * kGiB, false},
                                {QStringLiteral("new.service"), kGiB, false},
                                {QStringLiteral("tiny.service"), 10 * kMiB, false}});
    check(ar::nextFreezeCandidate(policy, running, state) ==
              QStringLiteral("new.service"),
          "the newest unit big enough is the candidate");
    const auto lone = units({{QStringLiteral("old.service"), 2 * kGiB, false},
                             {QStringLiteral("new.service"), kGiB, true}});
    check(ar::nextFreezeCandidate(policy, lone, state).isEmpty(),
          "no candidate when only one unit is still running");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("forkmesh-agent-resources-tests"));
    testBudget();
    testCpuBudget();
    testCpuParsers();
    testCpuGovernor();
    testArguments();
    testShim();
    testEnvironment();
    testUnprepared();
    testPolicy();
    testSliceReader();
    testSliceCounters();
    testFreezeCandidate();
    testRetargetCpu();
    testReserveSettings();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("agent resources tests passed\n");
    return 0;
}
