#include "LocalControlHosts.h"
#include "LocalControlStats.h"

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QTime>
#include <QTimeZone>

#include <iostream>

using namespace forkmesh::localcontrol::stats;
namespace hostctl = forkmesh::localcontrol::hosts;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static qint64 at(const QDate &day, int hour, int minute = 0)
{
    return QDateTime(day, QTime(hour, minute)).toMSecsSinceEpoch();
}

static void testPrefixAndSlugs()
{
    check(familyPrefix() == QLatin1String("stats."), "prefix");
    QString owner;
    QString name;
    check(parseRepoSlug(QStringLiteral("alice/app"), &owner, &name), "slug parses");
    check(owner == QLatin1String("alice") && name == QLatin1String("app"), "slug parts");
    check(!parseRepoSlug(QStringLiteral("alice"), nullptr, nullptr), "no slash");
    check(!parseRepoSlug(QStringLiteral("a/b/c"), nullptr, nullptr), "too deep");
    check(!parseRepoSlug(QStringLiteral("../etc"), nullptr, nullptr), "dot dot owner");
    check(!parseRepoSlug(QStringLiteral("a/.."), nullptr, nullptr), "dot dot name");
    check(!parseRepoSlug(QStringLiteral("/home/a"), nullptr, nullptr), "absolute path");
    check(!parseRepoSlug(QStringLiteral("a b/c"), nullptr, nullptr), "spaces");
}

static void testTimeline()
{
    check(severityForBadge(QStringLiteral("ERROR")) == QLatin1String("error"), "error badge");
    check(severityForBadge(QStringLiteral("crash")) == QLatin1String("error"), "crash badge");
    check(severityForBadge(QStringLiteral("STALL")) == QLatin1String("warning"), "stall badge");
    check(severityForBadge(QStringLiteral("BGBLOCK")) == QLatin1String("warning"), "bgblock");
    check(severityForBadge(QStringLiteral("GIT")) == QLatin1String("info"), "git is info");
    check(clampDays(QJsonValue(400)) == 30, "days capped");
    check(clampDays(QJsonValue(0)) == 1, "days floored");
    check(clampDays(QJsonValue()) == 30, "days default");
    check(clampDays(QJsonValue(QStringLiteral("7"))) == 30, "string days default");

    const QDate today(2026, 9, 24);
    const qint64 now = at(today, 15);
    QMap<qint64, QHash<QString, int>> buckets;
    buckets[at(today.addDays(-40), 10)] = {{QStringLiteral("ERROR"), 99}};
    buckets[at(today.addDays(-2), 9)] = {{QStringLiteral("ERROR"), 2},
                                         {QStringLiteral("GIT"), 5}};
    buckets[at(today.addDays(-2), 23, 45)] = {{QStringLiteral("STALL"), 1}};
    buckets[at(today, 0)] = {{QStringLiteral("INFO"), 3}, {QStringLiteral("CRASH"), 1}};
    buckets[at(today.addDays(1), 1)] = {{QStringLiteral("ERROR"), 50}};

    const QJsonArray days = timeline(buckets, now, 30).value(QStringLiteral("days")).toArray();
    check(days.size() == 30, "thirty days, empty ones included");
    check(qint64(days.first().toObject().value(QStringLiteral("dayTs")).toDouble()) ==
              at(today.addDays(-29), 0),
          "oldest first");
    const QJsonObject last = days.last().toObject();
    check(qint64(last.value(QStringLiteral("dayTs")).toDouble()) == at(today, 0), "today last");
    check(last.value(QStringLiteral("errors")).toInt() == 1, "crash counted as error");
    check(last.value(QStringLiteral("info")).toInt() == 3, "info today");
    const QJsonObject twoAgo = days.at(27).toObject();
    check(twoAgo.value(QStringLiteral("errors")).toInt() == 2, "errors two days ago");
    check(twoAgo.value(QStringLiteral("warnings")).toInt() == 1, "late bucket same day");
    check(twoAgo.value(QStringLiteral("info")).toInt() == 5, "info two days ago");
    int total = 0;
    for (const QJsonValue &value : days)
        total += value.toObject().value(QStringLiteral("errors")).toInt();
    check(total == 3, "outside the window ignored");
    check(timeline(buckets, now, 3).value(QStringLiteral("days")).toArray().size() == 3,
          "shorter window");
    check(timeline({}, now, 5).value(QStringLiteral("days")).toArray().size() == 5,
          "empty digest still answers");
}

static void testQuality()
{
    check(relativeFindingPath(QStringLiteral("src/a.cpp"), QStringLiteral("/r")) ==
              QLatin1String("src/a.cpp"),
          "relative kept");
    check(relativeFindingPath(QStringLiteral("/r/src/a.cpp"), QStringLiteral("/r")) ==
              QLatin1String("src/a.cpp"),
          "absolute under root made relative");
    check(relativeFindingPath(QStringLiteral("/etc/passwd"), QStringLiteral("/r")) ==
              QLatin1String("passwd"),
          "absolute elsewhere reduced to a name");
    check(relativeFindingPath(QStringLiteral("../../x"), QStringLiteral("/r")) ==
              QLatin1String("x"),
          "escape reduced");
    check(relativeFindingPath(QString(), QStringLiteral("/r")) == repositoryFileLabel(),
          "repository wide");
    check(gradeFor(95) == QLatin1String("A") && gradeFor(59) == QLatin1String("F"), "grades");

    QList<QualityFinding> findings{
        {QStringLiteral("a.cpp"), 2, QStringLiteral("TODO markers")},
        {QStringLiteral("a.cpp"), 4, QStringLiteral("Hard-coded\ncredential")},
        {QStringLiteral("b.cpp"), 1, QStringLiteral("Large file")},
        {QString(), 3, QStringLiteral("No tests")},
        {QStringLiteral("c.cpp"), 0, QStringLiteral("Pass")},
    };
    const QJsonObject report = qualityReport(findings, {0, 2}, 1234);
    check(report.value(QStringLiteral("computedAt")).toDouble() == 1234, "computed at");
    const int expected = 100 - (4 + 4 + 25 + 1 + 12);
    check(report.value(QStringLiteral("score")).toInt() == expected, "score");
    check(report.value(QStringLiteral("grade")).toString() == gradeFor(expected), "grade");
    const QJsonArray files = report.value(QStringLiteral("files")).toArray();
    check(files.size() == 3, "pass findings dropped");
    const QJsonObject worst = files.first().toObject();
    check(worst.value(QStringLiteral("path")).toString() == QLatin1String("a.cpp"), "worst first");
    check(worst.value(QStringLiteral("score")).toInt() == 100 - 35 - 8, "file score");
    const QJsonArray notes = worst.value(QStringLiteral("findings")).toArray();
    check(notes.size() == 2, "two notes");
    check(notes.first().toString() == QLatin1String("Critical: Hard-coded credential"),
          "most severe note first, flattened");

    QList<QualityFinding> many;
    for (int i = 0; i < 300; ++i)
        many.append({QStringLiteral("f%1.cpp").arg(i), 1, QStringLiteral("x")});
    for (int i = 0; i < 20; ++i)
        many.append({QStringLiteral("f0.cpp"), 1, QStringLiteral("y")});
    const QJsonObject big = qualityReport(many, {}, 0);
    check(big.value(QStringLiteral("files")).toArray().size() == kMaxQualityFiles, "files capped");
    check(big.value(QStringLiteral("files")).toArray().first().toObject()
                  .value(QStringLiteral("findings")).toArray().size() == kMaxFindingsPerFile,
          "notes capped");
    check(big.value(QStringLiteral("score")).toInt() == 0, "score floored");
}

static void testRepoStats()
{
    RepoFacts facts;
    facts.commits = 42;
    facts.files = 10;
    facts.lines = 1000;
    facts.bytes = 2048;
    facts.day = QStringLiteral("2026-09-24");
    QJsonObject out = repoStats(facts);
    check(out.value(QStringLiteral("commits")).toInt() == 42, "commits");
    check(out.value(QStringLiteral("lines")).toInt() == 1000, "lines");
    check(!out.value(QStringLiteral("ratchet")).toObject().value(QStringLiteral("enabled")).toBool(),
          "ratchet off");
    check(out.value(QStringLiteral("ratchet")).toObject().value(QStringLiteral("note")).toString()
              .isEmpty(),
          "no note when off");
    facts.ratchetEnabled = true;
    facts.ceilingBytes = 3 * 1024 * 1024;
    facts.ceilingDay = QStringLiteral("2026-09-20");
    out = repoStats(facts);
    const QJsonObject ratchet = out.value(QStringLiteral("ratchet")).toObject();
    check(ratchet.value(QStringLiteral("enabled")).toBool(), "ratchet on");
    check(ratchet.value(QStringLiteral("note")).toString() ==
              QLatin1String("Commits may not grow the repository past 3.0 MB (set 2026-09-20)."),
          "ratchet note");
    check(ratchetSummary(QStringLiteral("a/b"), true).contains(QStringLiteral("a/b")),
          "summary names the repo");
    check(ratchetAction(true) != ratchetAction(false), "actions differ");
}

static void testRatchetConfirmations()
{
    RatchetConfirmations store;
    const qint64 now = 1000000;
    const auto issued = store.issue(QStringLiteral("K"), QStringLiteral("a/b"), true, now);
    check(!issued.token.isEmpty(), "token");
    check(issued.expiresAtMs == now + hostctl::ConfirmationStore::kLifetimeMs, "absolute expiry");
    QString repo;
    bool enabled = false;
    check(store.consume(issued.token, QStringLiteral("other"), now, &repo, &enabled) ==
              hostctl::ConfirmationStore::Result::Mismatch,
          "another device cannot spend it");
    check(repo.isEmpty(), "nothing leaked on mismatch");
    check(store.consume(issued.token, QStringLiteral("K"), now, &repo, &enabled) ==
              hostctl::ConfirmationStore::Result::Missing,
          "a spent token is gone");

    const auto second = store.issue(QStringLiteral("K"), QStringLiteral("a/b"), false, now);
    check(store.consume(second.token, QStringLiteral("K"), now + 10, &repo, &enabled) ==
              hostctl::ConfirmationStore::Result::Ok,
          "the right device spends it");
    check(repo == QLatin1String("a/b") && !enabled, "intent returned");
    check(store.consume(second.token, QStringLiteral("K"), now + 10, &repo, &enabled) ==
              hostctl::ConfirmationStore::Result::Missing,
          "single use");

    const auto late = store.issue(QStringLiteral("K"), QStringLiteral("a/b"), true, now);
    check(store.consume(late.token, QStringLiteral("K"),
                        now + hostctl::ConfirmationStore::kLifetimeMs + 1, &repo, &enabled) ==
              hostctl::ConfirmationStore::Result::Expired,
          "expired");
    check(store.consume(QString(), QStringLiteral("K"), now, &repo, &enabled) ==
              hostctl::ConfirmationStore::Result::Missing,
          "empty token");
    for (int i = 0; i < 100; ++i)
        store.issue(QStringLiteral("K"), QStringLiteral("a/b"), true, now);
    check(store.outstanding() <= hostctl::ConfirmationStore::kMaxOutstanding, "bounded");
}

static void testSentry()
{
    QJsonArray raw;
    raw.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("123")},
                           {QStringLiteral("title"), QStringLiteral("TypeError:\n boom")},
                           {QStringLiteral("project"), QStringLiteral("web")},
                           {QStringLiteral("level"), QStringLiteral("ERROR")},
                           {QStringLiteral("count"), 7},
                           {QStringLiteral("lastSeen"), 1700000000000.0}});
    raw.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("bad id with spaces")}});
    raw.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("9")},
                           {QStringLiteral("level"), QStringLiteral("weird")},
                           {QStringLiteral("count"), -4}});
    const QJsonArray out = sentryIssues(raw);
    check(out.size() == 2, "bad ids dropped");
    const QJsonObject first = out.first().toObject();
    check(first.value(QStringLiteral("title")).toString() == QLatin1String("TypeError: boom"),
          "title flattened");
    check(first.value(QStringLiteral("level")).toString() == QLatin1String("error"), "level");
    check(first.value(QStringLiteral("count")).toInt() == 7, "count");
    check(first.value(QStringLiteral("lastSeen")).toDouble() == 1700000000000.0, "last seen ms");
    const QJsonObject second = out.at(1).toObject();
    check(second.value(QStringLiteral("level")).toString() == QLatin1String("error"),
          "unknown level");
    check(second.value(QStringLiteral("count")).toInt() == 0, "negative count");
    check(second.value(QStringLiteral("title")).toString() == QLatin1String("9"),
          "untitled falls back to the id");
    QJsonArray lots;
    for (int i = 0; i < 300; ++i)
        lots.append(QJsonObject{{QStringLiteral("id"), QString::number(i)}});
    check(sentryIssues(lots).size() == kMaxSentryIssues, "capped");
}

static void testCodemagic()
{
    const qint64 now = QDateTime(QDate(2026, 10, 1), QTime(15, 30), QTimeZone::utc())
                           .toMSecsSinceEpoch();
    check(nextUtcMonthStartMs(now) ==
              QDateTime(QDate(2026, 11, 1), QTime(0, 0), QTimeZone::utc()).toMSecsSinceEpoch(),
          "codemagic resets on the first of next month");
    const qint64 december = QDateTime(QDate(2026, 12, 31), QTime(23, 59), QTimeZone::utc())
                                .toMSecsSinceEpoch();
    check(nextUtcMonthStartMs(december) ==
              QDateTime(QDate(2027, 1, 1), QTime(0, 0), QTimeZone::utc()).toMSecsSinceEpoch(),
          "codemagic reset rolls the year");

    const QJsonObject buildTime{{QStringLiteral("mac_mini_m2_free"), 3815},
                                {QStringLiteral("mac_mini_m2_paid"), 120},
                                {QStringLiteral("linux_x2_personal"), 0}};
    const QJsonObject billing{
        {QStringLiteral("usage"),
         QJsonObject{{QStringLiteral("currentPeriod"),
                      QJsonObject{{QStringLiteral("buildTime"), buildTime}}},
                     {QStringLiteral("previousPeriod"),
                      QJsonObject{{QStringLiteral("buildTime"),
                                   QJsonObject{{QStringLiteral("mac_mini_m2_free"), 31787}}}}},
                     {QStringLiteral("freeLimit"),
                      QJsonObject{{QStringLiteral("buildTime"), 30000}}}}}};
    const QJsonObject usage =
        codemagicUsage(QJsonObject{{QStringLiteral("user"),
                                    QJsonObject{{QStringLiteral("billing"), billing}}}},
                       now);
    check(usage.value(QStringLiteral("limitSeconds")).toDouble() == 30000, "codemagic limit");
    check(usage.value(QStringLiteral("usedSeconds")).toDouble() == 3815, "codemagic free used");
    check(usage.value(QStringLiteral("remainingSeconds")).toDouble() == 30000 - 3815,
          "codemagic remaining");
    check(usage.value(QStringLiteral("previousSeconds")).toDouble() == 31787,
          "codemagic previous period");
    check(usage.value(QStringLiteral("paidSeconds")).toDouble() == 120, "codemagic paid");
    check(usage.value(QStringLiteral("resetsAt")).toDouble() == double(nextUtcMonthStartMs(now)),
          "codemagic reset stamp");

    const QJsonObject preferred{
        {QStringLiteral("billing"), billing},
        {QStringLiteral("buildTimes"),
         QJsonObject{{QStringLiteral("monthlyFreeBuildTimeLimit"), 60000},
                     {QStringLiteral("currentPeriod"),
                      QJsonObject{{QStringLiteral("free"), 70000}}}}}};
    const QJsonObject over = codemagicUsage(QJsonObject{{QStringLiteral("user"), preferred}}, now);
    check(over.value(QStringLiteral("limitSeconds")).toDouble() == 60000,
          "codemagic buildTimes limit wins");
    check(over.value(QStringLiteral("remainingSeconds")).toDouble() == 0,
          "codemagic remaining never negative");
    check(codemagicUsage(QJsonObject{}, now).isEmpty(), "codemagic without a limit is empty");
    check(codemagicError(401) == QLatin1String("codemagic_unauthorized"), "codemagic 401");
    check(codemagicError(0) == QLatin1String("codemagic_unreachable"), "codemagic offline");
    check(codemagicError(200) == QLatin1String("bad_answer"), "codemagic bad body");
}

static void testAppStoreStatus()
{
    QString error;
    const QByteArray snapshot =
        "{\n  \"format\": \"forkmesh-app-store-status-v1\",\n  \"generatedAt\": 5,\n"
        "  \"versions\": []\n}\n";
    const QJsonObject reply = appStoreStatusReply(snapshot, 1234, &error);
    check(error.isEmpty(), "app store snapshot parses");
    check(reply.value(QStringLiteral("published")).toBool(), "app store reply is published");
    check(reply.value(QStringLiteral("source")).toString() == QLatin1String("desktop"),
          "app store reply names the desktop");
    check(reply.value(QStringLiteral("publishedAt")).toDouble() == 1234, "app store stamp");
    check(reply.value(QStringLiteral("snapshot")).toObject().value(QStringLiteral("generatedAt"))
                  .toInt() == 5,
          "app store snapshot carried");
    check(appStoreStatusReply("not json", 1, &error).isEmpty() &&
              error == QLatin1String("app_store_report_unreadable"),
          "app store garbage refused");
    check(appStoreStatusReply("{\"format\": \"other\"}", 1, &error).isEmpty(),
          "app store other format refused");
    check(appStoreReportError("Traceback\nERROR: missing App Store Connect credential ASC_KEY_ID.\n") ==
              QLatin1String("missing App Store Connect credential ASC_KEY_ID."),
          "app store error is the last line without ERROR:");
    check(appStoreReportError("") == QLatin1String("The App Store report did not finish."),
          "app store silent failure");
    check(appStoreReportVariables().contains(QStringLiteral("ASC_VENDOR_NUMBER")),
          "app store vendor number passed");
}

int main()
{
    testPrefixAndSlugs();
    testTimeline();
    testQuality();
    testRepoStats();
    testRatchetConfirmations();
    testSentry();
    testCodemagic();
    testAppStoreStatus();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local control stats: ok\n";
    return 0;
}
