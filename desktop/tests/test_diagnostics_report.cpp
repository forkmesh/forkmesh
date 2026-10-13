#include "DiagnosticsReport.h"

#include <QJsonDocument>
#include <QString>

#include <iostream>

namespace diagreport = forkmesh::diagreport;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static diagreport::LogEntry entry(const char *badge, const QString &message)
{
    return {QString::fromLatin1(badge), message,
            QStringLiteral("2026-10-06 10:00:00  ") + message +
                QStringLiteral(" [desktop/src/X.cpp:12]")};
}

static diagreport::Snapshot sampleSnapshot()
{
    diagreport::Snapshot s;
    s.takenAtMs = 1'790'000'000'000;
    s.version = QStringLiteral("9.9.9");
    s.sourceDir = QStringLiteral("/src/forkmesh");
    s.uptimeMs = 30 * 60 * 1000;
    s.residentBytes = 3LL * 1024 * 1024 * 1024;
    s.loopLagP95Ms = 180;
    s.stallCount = 2;
    s.stalls = {QStringLiteral("STALL-1 old"), QStringLiteral("STALL-2 newest")};
    s.logLocations = QStringLiteral("App log: /logs/app.log");
    s.log = {
        entry("GIT", QStringLiteral("fetched origin")),
        entry("ERROR", QStringLiteral("Fetch failed for /home/a/repo after 31 ms")),
        entry("ERROR", QStringLiteral("Fetch failed for /home/b/other after 4 ms")),
        entry("ERROR", QStringLiteral("Relay 'n1' closed: code 1006")),
        entry("BGBLOCK", QStringLiteral("git status waited 812 ms on the GUI thread")),
    };
    s.categoryCounts = {{QStringLiteral("GIT"), 1}, {QStringLiteral("ERROR"), 3}};
    return s;
}

int main()
{
    check(diagreport::problemSignature(
              QStringLiteral("Fetch failed for /home/a/repo after 31 ms")) ==
              diagreport::problemSignature(
                  QStringLiteral("Fetch failed for ~/b/other after 4 ms")),
          "paths and numbers fold into one signature");
    check(diagreport::problemSignature(
              QStringLiteral("GET https://x.test/a?b=1 returned 502")) ==
              QStringLiteral("GET <url> returned N"),
          "a URL and a status code fold");
    check(diagreport::problemSignature(QStringLiteral("commit 4f2a9c1 'feat' rejected")) ==
              QStringLiteral("commit N <str> rejected"),
          "hashes and quoted text fold");
    check(!diagreport::isProblemBadge(QStringLiteral("GIT")) &&
              diagreport::isProblemBadge(QStringLiteral("BGBLOCK")),
          "only ERROR, STALL and BGBLOCK are problems");

    const diagreport::Snapshot first = sampleSnapshot();
    const QHash<QString, int> counts = diagreport::problemCounts(first.log);
    check(counts.size() == 3, "two fetch failures group, relay and BGBLOCK stand alone");

    const QString report = diagreport::build(first, diagreport::Baseline{});
    check(report.startsWith(QStringLiteral("# ForkMesh desktop diagnostic report #1")),
          "the first report is #1");
    check(report.contains(QStringLiteral("first report from this machine")),
          "the first report says there is nothing to compare");
    check(report.contains(QStringLiteral("## What to do with this report")),
          "the report carries the agent instructions");
    check(report.contains(QStringLiteral("ERROR x2: 2026-10-06 10:00:00  Fetch failed for /home/b/other")),
          "a group shows its count and newest example line");
    check(report.indexOf(QStringLiteral("ERROR x2")) <
              report.indexOf(QStringLiteral("Relay")),
          "the most frequent group is listed first");
    check(report.contains(QStringLiteral("[desktop/src/X.cpp:12]")),
          "the example keeps its source anchor");
    check(report.indexOf(QStringLiteral("STALL-2 newest")) <
              report.indexOf(QStringLiteral("STALL-1 old")),
          "stalls are newest first");
    check(!report.contains(QStringLiteral("NEW ")), "nothing is NEW without a baseline");

    const QStringList found = diagreport::findings(first);
    check(found.join(QLatin1Char('\n')).contains(QStringLiteral("Event-loop lag p95 is 180 ms")),
          "high loop lag is a finding");
    check(found.join(QLatin1Char('\n')).contains(QStringLiteral("3.0 GB resident")),
          "high resident memory is a finding");
    check(diagreport::findings(diagreport::Snapshot{}).isEmpty(),
          "an empty snapshot crosses no threshold");

    const diagreport::Baseline saved = diagreport::baselineFrom(first, 1);
    const diagreport::Baseline loaded = diagreport::baselineFromJson(
        QJsonDocument::fromJson(
            QJsonDocument(diagreport::toJson(saved)).toJson())
            .object());
    check(loaded.sequence == 1 && loaded.takenAtMs == first.takenAtMs &&
              loaded.problems == saved.problems &&
              loaded.residentBytes == first.residentBytes,
          "a baseline round-trips through JSON");

    diagreport::Snapshot second = sampleSnapshot();
    second.takenAtMs += 45 * 60 * 1000;
    second.stallCount = 0;
    second.stalls.clear();
    second.loopLagP95Ms = 20;
    second.log = {
        entry("ERROR", QStringLiteral("Fetch failed for /home/a/repo after 9 ms")),
        entry("ERROR", QStringLiteral("Token refresh returned 401")),
    };
    const QString next = diagreport::build(second, loaded);
    check(next.startsWith(QStringLiteral("# ForkMesh desktop diagnostic report #2")),
          "the next report is #2");
    check(next.contains(QStringLiteral("report #1 was copied 45 min earlier")),
          "the next report names the previous one");
    check(next.contains(QStringLiteral("NEW ERROR x1: 2026-10-06 10:00:00  Token refresh")),
          "a problem absent last time is NEW");
    check(!next.contains(QStringLiteral("NEW ERROR x1: 2026-10-06 10:00:00  Fetch")),
          "a recurring problem is not NEW");
    check(next.contains(QStringLiteral("## Not seen since the last report")) &&
              next.contains(QStringLiteral("Relay <str> closed")),
          "a problem that stopped is listed as not seen");
    check(next.contains(QStringLiteral("Problem groups: 3 -> 2 (1 new, 2 not seen)")),
          "the delta counts new and gone groups");
    check(next.contains(QStringLiteral("UI stalls this session: 2 -> 0 (-2)")),
          "the stall delta is shown");
    check(next.contains(QStringLiteral("Event-loop lag p95: 180 ms -> 20 ms (-160 ms)")),
          "the loop-lag delta is shown");

    diagreport::Snapshot huge = sampleSnapshot();
    huge.qtReport = QString(200000, QLatin1Char('x'));
    for (int i = 0; i < 5000; ++i)
        huge.log.append(entry("ERROR", QStringLiteral("unique failure kind %1").arg(QChar('a' + i % 26)).repeated(i % 7 + 1)));
    check(diagreport::build(huge, diagreport::Baseline{}).size() <=
              diagreport::kMaxReportChars + 200,
          "the report stays under its size cap");

    if (failures > 0) {
        std::cerr << failures << " diagnostics report test(s) failed\n";
        return 1;
    }
    std::cout << "diagnostics report tests passed\n";
    return 0;
}
