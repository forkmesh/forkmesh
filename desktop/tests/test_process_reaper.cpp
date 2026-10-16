#include "../src/ProcessReaper.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace pr = forkmesh::processreaper;

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

const QString kSelfGroup = QStringLiteral(
    "/user.slice/user-1001.slice/user@1001.service/app.slice/forkmesh.service");
const QString kSlice = QStringLiteral(
    "/user.slice/user-1001.slice/user@1001.service/forkmesh.slice/");
const QByteArray kMarker("0123456789abcdef0123456789abcdef");

struct FakeProcess {
    qint64 pid;
    qint64 parent;
    qint64 group;
    qint64 uid;
    QString cgroup;
    QByteArray marker;
    char state = 'S';
};

QList<FakeProcess> fakeProcesses()
{
    const QByteArray other("ffffffffffffffffffffffffffffffff");
    return {
        {1, 0, 1, 0, QStringLiteral("/init.scope"), {}},
        {90, 1, 90, 1001, QStringLiteral("/user.slice/user-1001.slice/session-2.scope"), {}},
        {100, 90, 100, 1001, kSelfGroup, kMarker},
        {101, 100, 100, 1001, kSelfGroup, kMarker},
        {102, 101, 102, 1001, kSelfGroup, kMarker},
        {103, 1, 103, 1001, kSelfGroup, kMarker},
        {104, 1, 104, 1001, kSelfGroup, other},
        {105, 1, 105, 0, kSelfGroup, kMarker},
        {106, 1, 106, 1001,
         QStringLiteral("/user.slice/user-1001.slice/user@1001.service/app.slice/"
                        "fmnext-add-core-20261001-101500.service"),
         kMarker},
        {107, 1, 107, 1001, kSlice + QStringLiteral("forkmesh-agent-claude-1-100-1.scope"), {}},
        {108, 1, 108, 1001, kSlice + QStringLiteral("forkmesh-agent-claude-1-100-2.scope"), {}},
        {109, 1, 109, 1001, kSlice + QStringLiteral("long-build.service"), kMarker},
        {110, 100, 110, 1001, kSelfGroup, kMarker, 'Z'},
        {111, 1, 111, 1001, kSlice + QStringLiteral("fmnext-roll.scope"), kMarker},
        {112, 1, 103, 1001, kSelfGroup, {}},
        {113, 100, 113, 1001, kSelfGroup, {}},
        {114, 1, 100, 1001, kSelfGroup, {}},
        {115, 1, 115, 1001, kSelfGroup, {}},
        {116, 1, 116, 1001,
         kSlice + QStringLiteral("forkmesh-action-100-7-0.scope/inner"), {}},
    };
}

pr::Policy fakePolicy()
{
    pr::Policy policy;
    policy.instance = kMarker;
    policy.ownedUnits = {QStringLiteral("forkmesh-agent-claude-1-100-1.scope"),
                         QStringLiteral("forkmesh-action-100-7-0.scope"),
                         QStringLiteral("fmnext-roll.scope")};
    policy.protectedUnitPrefixes = pr::defaultProtectedUnitPrefixes();
    return policy;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

bool writeFakeProc(const QString &root, const QList<FakeProcess> &processes)
{
    bool ok = QDir().mkpath(root + QStringLiteral("/self")) &&
              writeFile(root + QStringLiteral("/uptime"), "1.0 1.0\n");
    for (const FakeProcess &p : processes) {
        const QString dir = root + QLatin1Char('/') + QString::number(p.pid);
        ok = ok && QDir().mkpath(dir);
        const QByteArray stat = QByteArray::number(p.pid) + " (odd) name) " +
                                QByteArray(1, p.state) + ' ' +
                                QByteArray::number(p.parent) + ' ' +
                                QByteArray::number(p.group) +
                                " 1 0 -1 4194304 0 0 0 0\n";
        ok = ok && writeFile(dir + QStringLiteral("/stat"), stat);
        const QByteArray uid = QByteArray::number(p.uid);
        ok = ok && writeFile(dir + QStringLiteral("/status"),
                             "Name:\todd\nState:\tS\nUid:\t" + uid + '\t' + uid +
                                 '\t' + uid + '\t' + uid + "\nGid:\t1\n");
        ok = ok && writeFile(dir + QStringLiteral("/cgroup"),
                             "0::" + p.cgroup.toUtf8() + '\n');
        QByteArray procEnvironment("HOME=/home/x");
        procEnvironment.append('\0');
        if (!p.marker.isEmpty()) {
            procEnvironment.append(pr::kInstanceVariable);
            procEnvironment.append('=');
            procEnvironment.append(p.marker);
            procEnvironment.append('\0');
        }
        procEnvironment.append("PATH=/usr/bin");
        procEnvironment.append('\0');
        ok = ok && writeFile(dir + QStringLiteral("/environ"), procEnvironment);
    }
    return ok;
}

void testUnitNames()
{
    check(pr::unitOfControlGroup(kSelfGroup) == QStringLiteral("forkmesh.service"),
          "the desktop's unit is the deepest .service component");
    check(pr::unitOfControlGroup(kSlice + QStringLiteral("forkmesh-action-1-2-3.scope/inner")) ==
              QStringLiteral("forkmesh-action-1-2-3.scope"),
          "a nested cgroup belongs to the scope above it");
    check(pr::unitOfControlGroup(QStringLiteral("/")).isEmpty(),
          "the root cgroup has no unit");
    check(pr::isOwnedUnitName(QStringLiteral("forkmesh-agent-claude-12-100-3.scope")),
          "agent scopes are reapable names");
    check(pr::isOwnedUnitName(QStringLiteral("forkmesh-action-100-7-0.scope")),
          "action scopes are reapable names");
    check(!pr::isOwnedUnitName(QStringLiteral("forkmesh-node-deploy.service")),
          "services are never reapable names");
    check(!pr::isOwnedUnitName(QStringLiteral("fmnext-add-core-1.scope")),
          "fleet units are never reapable names");
    const QStringList prefixes = pr::defaultProtectedUnitPrefixes();
    for (const char *unit :
         {"fmnext-add-core-20261001.service", "fmnext-remove-core-x.service",
          "fm-roll-1.service", "forkmesh-node-deploy.service",
          "forkmesh-source-publish.service", "forkmesh-mirror-renew.service",
          "forkmesh-release-host.service", "forkmesh-backup.service",
          "forkmesh-dns-health.service", "forkmesh-headless-forkmesh.service",
          "forkmesh-tray-forkmesh.scope"})
        check(pr::isProtectedUnit(QString::fromLatin1(unit), prefixes), unit);
    check(!pr::isProtectedUnit(QStringLiteral("forkmesh-agent-a-1-1.scope"), prefixes),
          "agent scopes are not protected");
}

void testParsers()
{
    pr::ProcessEntry entry;
    check(pr::parseProcStat("4242 (a) b (c)) R 17 4242 4242 0 -1", &entry),
          "a comm with parentheses and spaces parses");
    check(entry.pid == 4242 && entry.parentPid == 17 && entry.groupId == 4242 &&
              !entry.zombie,
          "pid, parent and group come from the fields after the last paren");
    check(pr::parseProcStat("9 (z) Z 1 9 9", &entry) && entry.zombie,
          "a zombie is recognised");
    check(!pr::parseProcStat("garbage", &entry), "garbage is rejected");

    check(pr::parseProcControlGroup("0::/a/b.scope\n") == QStringLiteral("/a/b.scope"),
          "cgroup v2 path");
    check(pr::parseProcControlGroup("12:cpu:/x\n1:name=systemd:/legacy/u.service\n") ==
              QStringLiteral("/legacy/u.service"),
          "a hybrid hierarchy falls back to the systemd line");

    QByteArray procEnvironment("A=1");
    procEnvironment.append('\0');
    procEnvironment.append("XFORKMESH_DESKTOP_INSTANCE=no");
    procEnvironment.append('\0');
    procEnvironment.append("FORKMESH_DESKTOP_INSTANCE=abc");
    procEnvironment.append('\0');
    check(pr::instanceFromEnviron(procEnvironment) == "abc",
          "the marker is read from its own NUL-separated entry only");
    check(pr::instanceFromEnviron("A=1").isEmpty(), "no marker, no instance");
    check(pr::uidFromProcStatus("Name:\tx\nUid:\t1001\t1001\t1001\t1001\n") == 1001,
          "real uid from status");

    const pr::ProcessTable ps = pr::parsePsTable(
        "  100     1   100  501 Ss\n  101   100   100  501 S+\n  102   100   102  501 Z\nbad line\n",
        100, 501);
    check(ps.processes.size() == 3, "ps rows parse");
    check(ps.processes.value(2).zombie, "ps zombie state");
    check(pr::selectTargets(ps, pr::Policy{}) == QList<qint64>{101},
          "without /proc a child is found by ancestry only");
}

void testSelectionFromFakeProc()
{
    QTemporaryDir temp;
    check(temp.isValid(), "temp dir");
    const QString root = temp.path() + QStringLiteral("/proc");
    check(writeFakeProc(root, fakeProcesses()), "fake /proc written");
    const pr::ProcessTable table = pr::readProcTable(root, 100);
    check(table.selfUid == 1001, "self uid read from the fake tree");
    check(table.selfControlGroup == kSelfGroup, "self cgroup read from the fake tree");
    check(table.processes.size() == fakeProcesses().size(),
          "every numeric entry is read, the others are skipped");

    const QList<qint64> targets = pr::selectTargets(table, fakePolicy());
    const QList<qint64> expected{101, 102, 103, 107, 112, 113, 116};
    if (targets != expected) {
        QStringList got;
        for (qint64 pid : targets)
            got << QString::number(pid);
        std::fprintf(stderr, "targets: %s\n", qPrintable(got.join(QLatin1Char(' '))));
    }
    check(targets == expected,
          "marked children, escaped marked processes, their groups, unmarked "
          "children and registered scopes are the targets");
    check(!targets.contains(1) && !targets.contains(90) && !targets.contains(100),
          "init, the launching shell and the desktop itself are never targets");
    check(!targets.contains(104), "another instance's processes are left alone");
    check(!targets.contains(105), "another user's processes are left alone");
    check(!targets.contains(106), "a fleet add-core unit is never touched");
    check(!targets.contains(108), "an unregistered agent scope is left alone");
    check(!targets.contains(109), "work put in its own unit outlives the desktop");
    check(!targets.contains(110), "zombies are not signalled");
    check(!targets.contains(111), "a registered fleet scope is still protected");
    check(!targets.contains(114) && !targets.contains(115),
          "the desktop's own process group and unrelated same-cgroup processes "
          "are left alone");

    const QStringList units = pr::liveOwnedUnits(table, fakePolicy());
    check(units == QStringList{QStringLiteral("forkmesh-action-100-7-0.scope"),
                               QStringLiteral("forkmesh-agent-claude-1-100-1.scope")},
          "only registered, live, unprotected scopes are stopped");

    pr::ProcessTable foreign = table;
    foreign.selfUid = -1;
    check(pr::selectTargets(foreign, fakePolicy()).isEmpty(),
          "an unknown own uid selects nothing");
    check(pr::selectTargets(table, pr::Policy{}).size() == 3,
          "without a marker or registered units only descendants and their "
          "groups are found");
}

struct FakeSystem {
    QHash<qint64, pr::ProcessEntry> live;
    QSet<qint64> ignoresTerm;
    QSet<qint64> ignoresKill;
    QHash<qint64, QList<pr::Signal>> sent;
    QList<QPair<QStringList, pr::Signal>> unitCalls;
    qint64 now = 0;
    qint64 selfPid = 100;
    QList<pr::ProcessEntry> spawnOnFirstSleep;

    pr::Hooks hooks()
    {
        pr::Hooks h;
        h.snapshot = [this] {
            pr::ProcessTable table;
            table.selfPid = selfPid;
            table.selfUid = 1001;
            table.selfControlGroup = kSelfGroup;
            table.processes = live.values();
            return table;
        };
        h.send = [this](qint64 pid, pr::Signal signal) {
            sent[pid].append(signal);
            if (signal == pr::Signal::Terminate && !ignoresTerm.contains(pid))
                live.remove(pid);
            if (signal == pr::Signal::Kill && !ignoresKill.contains(pid))
                live.remove(pid);
        };
        h.stopUnits = [this](const QStringList &units, pr::Signal signal) {
            unitCalls.append({units, signal});
        };
        h.nowMs = [this] { return now; };
        h.sleepMs = [this](int ms) {
            now += ms;
            for (const pr::ProcessEntry &entry : std::as_const(spawnOnFirstSleep))
                live.insert(entry.pid, entry);
            spawnOnFirstSleep.clear();
        };
        return h;
    }

    void add(qint64 pid, qint64 parent, const QString &cgroup, const QByteArray &marker)
    {
        pr::ProcessEntry entry;
        entry.pid = pid;
        entry.parentPid = parent;
        entry.groupId = pid;
        entry.uid = 1001;
        entry.controlGroup = cgroup;
        entry.instance = marker;
        live.insert(pid, entry);
    }
};

void testEscalation()
{
    FakeSystem fake;
    fake.add(100, 1, kSelfGroup, kMarker);
    fake.add(201, 100, kSelfGroup, kMarker);
    fake.add(202, 1, kSelfGroup, kMarker);
    fake.add(203, 1, kSlice + QStringLiteral("forkmesh-agent-claude-1-100-1.scope"), {});
    fake.add(204, 1,
             QStringLiteral("/user.slice/user-1001.slice/user@1001.service/app.slice/"
                            "fmnext-add-core-x.service"),
             kMarker);
    fake.ignoresTerm = {201, 203, 204};
    pr::ProcessEntry late;
    late.pid = 205;
    late.parentPid = 201;
    late.groupId = 205;
    late.uid = 1001;
    late.controlGroup = kSelfGroup;
    fake.spawnOnFirstSleep = {late};

    const pr::Report report = pr::reap(fakePolicy(), fake.hooks(), 3000, 5000);
    check(fake.sent.value(202) == QList<pr::Signal>{pr::Signal::Terminate},
          "a process that honours SIGTERM gets nothing else");
    check(fake.sent.value(201) ==
              (QList<pr::Signal>{pr::Signal::Terminate, pr::Signal::Kill}),
          "a process that ignores SIGTERM is killed after the grace period");
    check(fake.sent.value(203) ==
              (QList<pr::Signal>{pr::Signal::Terminate, pr::Signal::Kill}),
          "a registered scope's process escalates too");
    check(fake.sent.value(205).value(0) == pr::Signal::Terminate,
          "a process forked during the teardown is caught on the next round");
    check(!fake.sent.contains(204), "a protected fleet unit is never signalled");
    check(!fake.sent.contains(100), "the desktop never signals itself");
    check(fake.unitCalls.size() == 2 &&
              fake.unitCalls.value(0).second == pr::Signal::Terminate &&
              fake.unitCalls.value(1).second == pr::Signal::Kill &&
              fake.unitCalls.value(0).first ==
                  QStringList{QStringLiteral("forkmesh-agent-claude-1-100-1.scope")},
          "registered scopes are stopped, then killed once after the grace period");
    check(!report.timedOut && report.survivors.isEmpty(), "everything is gone");
    check(fake.now >= 3000 && fake.now <= 3200,
          "SIGKILL follows the grace period without waiting for the deadline");
    check(report.killed.contains(201) && report.killed.contains(203),
          "the report names the killed processes");
    check(pr::describe(report).contains(QStringLiteral("SIGKILL")),
          "the summary mentions the escalation");

    FakeSystem stuck;
    stuck.add(100, 1, kSelfGroup, kMarker);
    stuck.add(301, 100, kSelfGroup, kMarker);
    stuck.ignoresTerm = {301};
    stuck.ignoresKill = {301};
    const pr::Report hung = pr::reap(fakePolicy(), stuck.hooks(), 3000, 5000);
    check(hung.timedOut && hung.survivors == QList<qint64>{301},
          "an unkillable process is reported, not waited on forever");
    check(stuck.now <= 5000, "the reap never runs past its deadline");
    check(stuck.sent.value(301) ==
              (QList<pr::Signal>{pr::Signal::Terminate, pr::Signal::Kill}),
          "SIGKILL is sent once, not every round");

    FakeSystem empty;
    empty.add(100, 1, kSelfGroup, kMarker);
    const pr::Report nothing = pr::reap(fakePolicy(), empty.hooks(), 3000, 5000);
    check(nothing.terminated.isEmpty() && empty.now == 0 && empty.unitCalls.isEmpty(),
          "with nothing left the quit does not wait at all");
    check(pr::describe(nothing).contains(QStringLiteral("no leftover")),
          "an empty reap says so");
}

void testRegistry()
{
    pr::registerOwnedUnit(QStringLiteral("forkmesh-agent-x-1-1.scope"));
    pr::registerOwnedUnit(QString());
    check(pr::ownedUnits() == QSet<QString>{QStringLiteral("forkmesh-agent-x-1-1.scope")},
          "registered units are remembered, empty names are ignored");
    check(pr::currentPolicy().ownedUnits.contains(QStringLiteral("forkmesh-agent-x-1-1.scope")),
          "the quit policy carries the registered units");
}

void testMarkerReachesChildren()
{
#if defined(Q_OS_UNIX)
    const QByteArray marker = pr::initializeInstance();
    check(marker.size() == 32, "the marker is 128 random bits in hex");
    check(pr::initializeInstance() == marker, "initializing twice keeps the marker");
    check(qgetenv(pr::kInstanceVariable) == marker, "the marker is in the environment");
    check(pr::currentPolicy().instance == marker, "the quit policy uses the marker");

    const QString script = QStringLiteral("printf %s \"$FORKMESH_DESKTOP_INSTANCE\"");
    QProcess child;
    child.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script});
    check(child.waitForFinished(10000), "plain child ran");
    check(child.readAllStandardOutput() == marker,
          "a plain QProcess child inherits the marker");

    QProcess custom;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("EXTRA"), QStringLiteral("1"));
    custom.setProcessEnvironment(env);
    custom.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script});
    check(custom.waitForFinished(10000), "custom-environment child ran");
    check(custom.readAllStandardOutput() == marker,
          "a child given a copy of the system environment carries the marker");

    QProcess released;
    released.setProcessEnvironment(pr::releasedEnvironment());
    released.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script});
    check(released.waitForFinished(10000), "released child ran");
    check(released.readAllStandardOutput().isEmpty(),
          "a released child does not carry the marker");
    check(!pr::releasedEnvironment(env).contains(QString::fromLatin1(pr::kInstanceVariable)),
          "releasing strips the marker from a given environment");

#if defined(Q_OS_LINUX)
    QProcess waiting;
    waiting.start(QStringLiteral("/bin/sh"),
                  {QStringLiteral("-c"), QStringLiteral("read line")});
    check(waiting.waitForStarted(10000), "waiting child started");
    const qint64 childPid = waiting.processId();
    const pr::ProcessTable table =
        pr::readProcTable(QStringLiteral("/proc"), QCoreApplication::applicationPid());
    check(table.selfUid == qint64(::getuid()), "the real /proc gives our uid");
    bool found = false;
    for (const pr::ProcessEntry &entry : table.processes) {
        if (entry.pid == childPid) {
            found = true;
            check(entry.instance == marker, "/proc shows the marker on a live child");
            check(entry.parentPid == QCoreApplication::applicationPid(),
                  "/proc shows the child's parent");
        }
    }
    check(found, "the live child is in the /proc table");
    pr::Policy policy;
    policy.instance = marker;
    check(pr::selectTargets(table, policy).contains(childPid),
          "the live child would be reaped");
    waiting.write("done\n");
    waiting.closeWriteChannel();
    check(waiting.waitForFinished(10000), "waiting child exited on its own");
#endif
#endif
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testUnitNames();
    testParsers();
    testSelectionFromFakeProc();
    testEscalation();
    testRegistry();
    testMarkerReachesChildren();
    if (g_failures == 0)
        std::printf("process reaper tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
