#include "../src/FleetVisualizer.h"
#include "../src/DesktopProcessView.h"
#include "../src/QueryInspector.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QLocale>
#include <QSet>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const QString &what)
{
    if (condition) {
        std::printf("ok   - %s\n", qPrintable(what));
        return;
    }
    ++failures;
    std::printf("FAIL - %s\n", qPrintable(what));
}

QString findToolTip(const forkmesh::FleetVisualizer &view, const QString &needle)
{
    for (int y = 0; y < view.height(); y += 6)
        for (int x = 0; x < view.width(); x += 6) {
            const QString text = view.toolTipAt(QPointF(x, y));
            if (text.contains(needle))
                return text;
        }
    return QString();
}

int distinctColours(const QImage &image)
{
    QSet<QRgb> colours;
    for (int y = 0; y < image.height(); y += 4)
        for (int x = 0; x < image.width(); x += 4)
            colours.insert(image.pixel(x, y));
    return colours.size();
}

forkmesh::corelogs::NodeVitals vitals()
{
    forkmesh::corelogs::NodeVitals v;
    v.valid = true;
    v.atMs = 1000;
    v.cpus = 1;
    v.cpuBusy = 42.0;
    v.load1 = 0.5;
    v.load5 = 0.4;
    v.load15 = 0.3;
    v.procsRunning = 2;
    v.procsTotal = 150;
    v.memTotal = 1000LL * 1024 * 1024;
    v.memAvailable = 300LL * 1024 * 1024;
    v.swapTotal = 512LL * 1024 * 1024;
    v.swapFree = 512LL * 1024 * 1024;
    v.diskTotal = 25LL * 1024 * 1024 * 1024;
    v.diskFree = 5LL * 1024 * 1024 * 1024;
    v.inodesTotal = 1000000;
    v.inodesFree = 900000;
    v.filesOpen = 2300;
    v.filesMax = 9223372;
    v.uptimeSeconds = 90000;
    return v;
}


template <typename View>
QString findTip(const View &view, const QString &needle)
{
    for (int y = 0; y < view.height(); y += 5)
        for (int x = 0; x < view.width(); x += 5) {
            const QString text = view.toolTipAt(QPointF(x, y));
            if (text.contains(needle))
                return text;
        }
    return QString();
}

template <typename View>
bool clickFirst(View &view, const QString &tip)
{
    for (int y = 0; y < view.height(); y += 3)
        for (int x = 0; x < view.width(); x += 3)
            if (view.toolTipAt(QPointF(x, y)) == tip)
                return view.clickAt(QPointF(x, y));
    return false;
}

forkmesh::corelogs::JournalEntry appLine(const QString &message)
{
    forkmesh::corelogs::JournalEntry entry;
    entry.valid = true;
    entry.message = message;
    return entry;
}

forkmesh::corelogs::StatementLine statementFrom(const QString &message)
{
    return forkmesh::corelogs::parseStatementLine(appLine(message),
                                                  QStringLiteral("app"));
}

void testQueryParsing()
{
    namespace corelogs = forkmesh::corelogs;
    const corelogs::StatementLine line = statementFrom(QStringLiteral(
        "D1 query: SELECT id FROM pulls WHERE repo = ? [1 arg] (for GET "
        "/api/repo/alice/demo/pulls/42, IP 203.0.113.9, user alice, session "
        "abcdefgh) (from pulls.py in list_pulls)"));
    check(line.valid && corelogs::statementArgCount(line.sql) == 1 &&
              corelogs::statementText(line.sql) ==
                  QLatin1String("SELECT id FROM pulls WHERE repo = ?"),
          "the bound-value count is split off the statement text");
    check(corelogs::statementKey(line.sql) == QLatin1String("6239d51c04"),
          "the desktop keys a statement exactly as the host does");
    const corelogs::CallerShape caller = corelogs::callerShape(line.caller);
    check(caller.kind == QLatin1String("request") &&
              caller.label == QLatin1String("GET /api/repo/alice/demo/pulls/:id") &&
              caller.ip == QLatin1String("203.0.113.9") &&
              caller.user == QLatin1String("alice") &&
              caller.session == QLatin1String("abcdefgh"),
          "a request caller names its route, address, user and session: " +
              caller.label);
    check(corelogs::callerShape(QStringLiteral("cron")).kind == QLatin1String("cron") &&
              corelogs::callerShape(QStringLiteral("queue mail")).label ==
                  QLatin1String("queue mail") &&
              corelogs::callerShape(QStringLiteral("alarm RelaySync")).kind ==
                  QLatin1String("alarm") &&
              corelogs::callerShape(QString()).kind == QLatin1String("none"),
          "background callers keep their own kinds");
    check(corelogs::callerRoute(QStringLiteral("/api/x/0123456789abcdef0123/y")) ==
              QLatin1String("/api/x/:id/y"),
          "long hex path segments fold into :id");

    const corelogs::StatementDone done = corelogs::parseStatementDone(
        appLine(QStringLiteral("D1-done 12.5 ms, 3 rows, 0 changed, local #6239d51c04")),
        QStringLiteral("app"));
    check(done.valid && qFuzzyCompare(done.ms, 12.5) && done.rows == 3 &&
              done.changes == 0 && done.served == QLatin1String("local") &&
              !done.failed && done.key == QLatin1String("6239d51c04"),
          "a D1-done line gives time, rows, changes, where it ran and the key");
    const corelogs::StatementDone failed = corelogs::parseStatementDone(
        appLine(QStringLiteral("D1-done 900.0 ms, 0 rows, 0 changed, failed #abcdef0123")),
        QStringLiteral("app"));
    check(failed.valid && failed.failed && failed.served.isEmpty(),
          "a failed statement is marked failed");
    check(!corelogs::parseStatementDone(
               appLine(QStringLiteral("D1-done 1.0 ms, 1 rows, 0 changed #6239d51c04")),
               QStringLiteral("rqlite"))
               .valid &&
              !statementFrom(QStringLiteral(
                                 "D1-done 1.0 ms, 1 rows, 0 changed #6239d51c04"))
                   .valid,
          "a timing line is never mistaken for a statement");
}

void testQueryInspector()
{
    namespace corelogs = forkmesh::corelogs;
    using forkmesh::QueryInspector;
    QueryInspector view;
    view.resize(1200, view.sizeHint().height());
    const qint64 now = 1'790'000'000'000;
    const corelogs::StatementLine select = statementFrom(QStringLiteral(
        "D1 query: SELECT id FROM pulls WHERE repo = ? [1 arg] (for GET "
        "/api/pulls, IP 203.0.113.9, user alice, session abcdefgh) (from pulls.py "
        "in list_pulls)"));
    const corelogs::StatementLine update = statementFrom(QStringLiteral(
        "D1 batch query: UPDATE users SET seen = ? WHERE id = ? [2 args] (for "
        "cron) (from account_users.py in touch_users)"));
    for (int i = 0; i < 5; ++i)
        view.noteStatement(QStringLiteral("n1"), select, now + i);
    view.noteStatement(QStringLiteral("n2"), update, now + 10);
    check(view.statementCount() == 6 && view.shapeCount() == 2 &&
              view.recentCount() == 6,
          "each statement is counted, grouped by its text and listed");
    check(view.routeCount(QStringLiteral("GET /api/pulls")) == 5 &&
              view.routeCount(QStringLiteral("cron")) == 1 &&
              view.userCount(QStringLiteral("alice")) == 5 &&
              view.kindCount(QStringLiteral("batch query")) == 1,
          "callers, users and statement kinds are tallied");
    check(view.sortColumn() == QueryInspector::SortColumn::Calls &&
              view.shapeOrder().value(0) == corelogs::statementKey(select.sql),
          "without timings the busiest statement leads");

    corelogs::StatementDone slow;
    slow.valid = true;
    slow.ms = 250.0;
    slow.changes = 7;
    slow.served = QStringLiteral("http");
    slow.key = corelogs::statementKey(update.sql);
    view.noteDone(QStringLiteral("n2"), slow, now + 11);
    corelogs::StatementDone quick;
    quick.valid = true;
    quick.ms = 0.5;
    quick.rows = 2;
    quick.served = QStringLiteral("local");
    quick.key = corelogs::statementKey(select.sql);
    view.noteDone(QStringLiteral("n1"), quick, now + 12);
    corelogs::StatementDone stray = quick;
    stray.key = QStringLiteral("ffffffffff");
    view.noteDone(QStringLiteral("n1"), stray, now + 13);
    check(view.timedCount() == 2 && view.unmatchedDoneCount() == 1,
          "timings join their statements; strays are only counted");
    check(view.recentMs(0) == 250.0 && view.recentMs(1) == 0.5 && view.recentMs(2) < 0.0,
          "a timing fills the newest matching row on the same node");
    check(view.sortColumn() == QueryInspector::SortColumn::TotalTime &&
              view.shapeOrder().value(0) == corelogs::statementKey(update.sql),
          "once timed, the statement that costs the most time leads");
    check(view.percentileMs(0.5) == 0.5 && view.percentileMs(1.0) == 250.0,
          "percentiles come from the timings");

    view.show();
    QApplication::processEvents();
    view.grab();
    const QString detail = findTip(view, QStringLiteral("UPDATE users SET seen"));
    check(detail.contains(QStringLiteral("From account_users.py · touch_users")) &&
              detail.contains(QLatin1String("250 ms total")) &&
              detail.contains(QLatin1String("7 changed")) &&
              detail.contains(QLatin1String("2 bound values")),
          "a statement row spells out code, time, rows and bound values: " + detail);
    const QString recent = findTip(view, QStringLiteral("For GET /api/pulls"));
    check(recent.contains(QLatin1String("Took 0.5 ms")) &&
              recent.contains(QLatin1String("served local")),
          "a latest-statement row names its caller and time: " + recent);
    check(!findTip(view, QStringLiteral("GET /api/pulls (request): 5")).isEmpty(),
          "the who-asked bars explain themselves");
    check(!findTip(view, QStringLiteral("100-500: 1 statements")).isEmpty(),
          "the latency histogram buckets each timing");
    check(clickFirst(view, QStringLiteral("Sort by calls")) &&
              view.sortColumn() == QueryInspector::SortColumn::Calls &&
              view.shapeOrder().value(0) == corelogs::statementKey(select.sql),
          "clicking a column header sorts by it");
    for (int i = 0; i < QueryInspector::kRecent + 5; ++i)
        view.noteStatement(QStringLiteral("n1"), select, now + 100 + i);
    check(view.recentCount() == QueryInspector::kRecent,
          "the latest-statement list is bounded");
}

void testDesktopProcesses()
{
    namespace procs = forkmesh::desktopprocs;
    const procs::Process parsed = procs::parseStatLine(
        "4242 (Web Content (x)) S 100 4242 4242 0 -1 4194560 100 0 0 0 250 50 0 0 "
        "20 0 12 0 9000 123456789 2048 18446744073709551615",
        4096);
    check(parsed.pid == 4242 && parsed.name == QLatin1String("Web Content (x)") &&
              parsed.state == QLatin1Char('S') && parsed.parentPid == 100 &&
              parsed.cpuTicks == 300 && parsed.threads == 12 &&
              parsed.startTicks == 9000 && parsed.rssBytes == 2048LL * 4096,
          "a /proc stat line gives name, parent, CPU ticks, threads and memory");
    check(procs::parseStatLine("garbage", 4096).pid == 0, "a broken line is no process");

    QList<procs::Process> tree;
    for (const auto &pair : QList<QPair<qint64, qint64>>{
             {1, 0}, {10, 1}, {11, 10}, {12, 11}, {20, 1}}) {
        procs::Process process;
        process.pid = pair.first;
        process.parentPid = pair.second;
        tree.append(process);
    }
    procs::markTree(tree, 10);
    check(tree.at(1).self && tree.at(1).forkmesh && tree.at(2).forkmesh &&
              tree.at(3).forkmesh && !tree.at(0).forkmesh && !tree.at(4).forkmesh,
          "the app and everything below it are ForkMesh's");

#if defined(Q_OS_LINUX)
    procs::Sampler sampler;
    sampler.sample(1000);
    const procs::Snapshot live = sampler.sample(2000);
    bool sawSelf = false;
    for (const procs::Process &process : live.processes)
        sawSelf |= process.self && process.pid == QCoreApplication::applicationPid();
    check(live.valid && live.vitals.valid && live.vitals.memTotal > 0 &&
              live.processes.size() > 1 && sawSelf && live.forkmeshCount >= 1,
          "sampling this machine finds its processes and this test among them");
    check(!sampler.commandLine(QCoreApplication::applicationPid()).isEmpty(),
          "a process's command line can be read");
#endif

    procs::Snapshot fake;
    fake.valid = true;
    fake.vitals.valid = true;
    fake.vitals.cpus = 4;
    fake.vitals.cpuBusy = 37.0;
    fake.vitals.memTotal = 8LL << 30;
    fake.vitals.memAvailable = 1LL << 30;
    fake.vitals.swapTotal = 0;
    fake.vitals.swapFree = 0;
    fake.vitals.diskTotal = 100LL << 30;
    fake.vitals.diskFree = 40LL << 30;
    fake.vitals.filesOpen = 9000;
    fake.vitals.filesMax = 100000;
    fake.vitals.load1 = 1.5;
    fake.vitals.load5 = 1.2;
    fake.vitals.load15 = 1.0;
    auto add = [&fake](qint64 pid, const QString &name, double cpu, qint64 rss,
                       int threads, bool forkmesh) {
        procs::Process process;
        process.pid = pid;
        process.parentPid = 1;
        process.name = name;
        process.state = QLatin1Char('S');
        process.user = QStringLiteral("f");
        process.cpuPercent = cpu;
        process.rssBytes = rss;
        process.threads = threads;
        process.forkmesh = forkmesh;
        process.self = pid == 500;
        process.command = QStringLiteral("/usr/bin/") + name + QStringLiteral(" --flag");
        fake.processes.append(process);
    };
    add(500, QStringLiteral("forkmesh"), 12.0, 900LL << 20, 40, true);
    add(501, QStringLiteral("claude"), 3.0, 400LL << 20, 12, true);
    add(600, QStringLiteral("chrome"), 55.0, 2LL << 30, 80, false);
    add(700, QStringLiteral("sshd"), 0.0, 8LL << 20, 1, false);
    fake.forkmeshCount = 2;
    fake.forkmeshCpu = 15.0;
    fake.forkmeshRss = 1300LL << 20;

    forkmesh::DesktopProcessView view;
    view.sampleSource = [&fake](qint64) { return fake; };
    view.resize(1200, view.sizeHint().height());
    view.show();
    QApplication::processEvents();
    check(view.sampling(), "the process view samples while it is shown");
    view.grab();
    check(view.shownProcesses().value(0).name == QLatin1String("chrome"),
          "the busiest process leads by default");
    const QString row = findTip(view, QStringLiteral("chrome (pid 600"));
    check(row.contains(QLatin1String("/usr/bin/chrome --flag")) &&
              row.contains(QLatin1String("80 threads")),
          "a process row spells out its command line and threads: " + row);
    QString inspected;
    view.onProcessClicked = [&inspected](const procs::Process &process) {
        inspected = process.name;
    };
    check(clickFirst(view, row) && inspected == QLatin1String("chrome"),
          "clicking a process opens its inspection action");
    const QString self = findTip(view, QStringLiteral("forkmesh (pid 500"));
    check(self.contains(QLatin1String("This ForkMesh desktop")),
          "the app's own row says it is this desktop: " + self);
    check(!findTip(view, QStringLiteral("% of CPU in use")).isEmpty() &&
              !findTip(view, QStringLiteral("of resident memory")).isEmpty(),
          "CPU and memory share strips explain each slice");
    view.setSortColumn(forkmesh::DesktopProcessView::SortColumn::Memory);
    check(view.shownProcesses().value(0).name == QLatin1String("chrome") &&
              view.shownProcesses().value(1).name == QLatin1String("forkmesh"),
          "sorting by memory reorders the list");
    view.setForkMeshOnly(true);
    check(view.shownProcesses().size() == 2,
          "ForkMesh only keeps the app and the processes it started");
    view.hide();
    check(!view.sampling(), "hiding the tab stops sampling");
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    using forkmesh::FleetVisualizer;

    QSet<QString> colours;
    for (const QString &group : FleetVisualizer::verbOrder())
        colours.insert(FleetVisualizer::verbColour(group, false).name() +
                       FleetVisualizer::verbColour(group, true).name());
    check(colours.size() == 5, "each statement kind has its own colour");
    check(FleetVisualizer::verbGroup(QStringLiteral("PRAGMA")) ==
                  QLatin1String("OTHER") &&
              FleetVisualizer::verbGroup(QStringLiteral("DELETE")) ==
                  QLatin1String("DELETE"),
          "schema and transaction verbs fold into OTHER");

    FleetVisualizer view;
    view.resize(1000, 900);
    const QImage empty = view.grab().toImage();
    check(!empty.isNull() && view.toolTipAt(QPointF(500, 450)).isEmpty(),
          "the empty visualizer paints its quiet state");

    view.noteStatement(QStringLiteral("n1"),
                       QStringLiteral("SELECT id FROM pulls WHERE repo = ?"),
                       QStringLiteral("pulls.py"), 1'000'000);
    check(view.statementCount() == 1 && view.particleCount() == 0 &&
              view.tableCount() == 1,
          "a statement seen while hidden is counted without a flight");

    QList<forkmesh::corelogs::NodeRow> rows;
    for (const char *name : {"n1", "n2", "r1"}) {
        forkmesh::corelogs::NodeRow row;
        row.node = QString::fromLatin1(name);
        row.connected = true;
        row.state = QStringLiteral("following");
        rows.append(row);
    }
    view.nodeSource = [&rows] { return rows; };
    view.show();
    QApplication::processEvents();
    check(view.nodeCount() == 3, "showing the tab lists every followed node");

    const qint64 now = 2'000'000;
    for (int i = 0; i < 30; ++i) {
        view.noteStatement(QStringLiteral("n2"),
                           QStringLiteral("INSERT INTO sessions VALUES (?)"),
                           QStringLiteral("sessions.py"), now + i);
        view.noteStatement(QStringLiteral("n1"),
                           QStringLiteral("UPDATE users SET seen = ?"),
                           QStringLiteral("account_users.py"), now + i);
    }
    view.noteStatement(QStringLiteral("r1"),
                       QStringLiteral("DELETE FROM locks WHERE k = ?"),
                       QString(), now);
    view.noteStatement(QStringLiteral("n1"), QStringLiteral("PRAGMA foreign_keys"),
                       QString(), now);
    check(view.statementCount() == 63 && view.tableCount() == 5,
          "statements land on their tables");
    check(view.verbCount(QStringLiteral("INSERT")) == 30 &&
              view.verbCount(QStringLiteral("UPDATE")) == 30 &&
              view.verbCount(QStringLiteral("DELETE")) == 1 &&
              view.verbCount(QStringLiteral("OTHER")) == 1,
          "the legend counts each statement kind");
    check(view.particleCount() == 62, "each visible statement takes flight");

    view.noteVitals(QStringLiteral("n1"), vitals(), now);
    forkmesh::corelogs::NodeVitals full = vitals();
    full.memAvailable = 20LL * 1024 * 1024;
    view.noteVitals(QStringLiteral("n2"), full, now);
    view.advance(0.3);
    const QImage busy = view.grab().toImage();
    check(distinctColours(busy) > distinctColours(empty) + 20,
          "flights, bubbles and node cards are drawn");

    const QString sessions = findToolTip(view, QStringLiteral("sessions\n"));
    check(sessions.contains(QLatin1String("30 statements")) &&
              sessions.contains(QLatin1String("INSERT 30")),
          "a table bubble explains its traffic and mix: " + sessions);
    const QString memory = findToolTip(view, QStringLiteral("n1 (core)\nMemory"));
    check(memory.contains(QLatin1String("70% used (ok)")) &&
              memory.contains(QLatin1String("Open file handles: 2,300")) &&
              memory.contains(QLatin1String("CPU busy: 42.0%")),
          "a node card spells out memory, files and CPU: " + memory);
    const QString critical = findToolTip(view, QStringLiteral("n2 (core)\nMemory"));
    check(critical.contains(QLatin1String("98% used (critical)")),
          "a nearly full node is named critical, not only coloured: " + critical);
    const QString waiting = findToolTip(view, QStringLiteral("r1: waiting"));
    check(!waiting.isEmpty(), "a node without vitals says it is waiting");
    check(!findToolTip(view, QStringLiteral("Statements per second")).isEmpty(),
          "the rate sparkline explains itself");

    view.advance(10.0);
    check(view.particleCount() == 0, "every flight lands");

    rows.removeLast();
    view.hide();
    check(view.particleCount() == 0, "hiding the tab drops flights");
    view.show();
    QApplication::processEvents();
    check(view.nodeCount() == 3,
          "a node that left the fleet keeps its card while it has history");

    for (int i = 0; i < 40; ++i)
        view.noteStatement(QStringLiteral("n1"),
                           QStringLiteral("SELECT * FROM table_%1").arg(i),
                           QString(), now);
    view.grab();
    check(!findToolTip(view, QStringLiteral("more tables")).isEmpty(),
          "past fifteen tables the rest fold into one bubble");

    testQueryParsing();
    testQueryInspector();
    testDesktopProcesses();

    std::printf("%s\n", failures == 0 ? "fleet visualizer tests passed"
                                      : "fleet visualizer tests FAILED");
    return failures == 0 ? 0 : 1;
}
