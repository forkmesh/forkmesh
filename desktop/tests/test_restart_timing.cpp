#include "RestartTiming.h"

#include <QCoreApplication>

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace forkmesh::restarttiming;

    const QList<qint64> noisy = {1000, 40'000, 60'000, 62'000, 64'000,
                                  3'000'000, 120'000, 130'000};
    const QList<qint64> history = normalizedHistory(noisy);
    check(history == QList<qint64>({60'000, 62'000, 64'000, 120'000, 130'000}),
          "history drops implausible samples and retains the newest five");
    check(estimatedDurationMs(history) == 64'000,
          "estimate uses the median of recent completed restarts");
    check(estimatedDurationMs({}) == kFallbackEstimateMs,
          "empty history gets a usable fallback estimate");

    check(medianMs({10'000, 30'000}) == 20'000,
          "an even sample count takes the midpoint of the middle pair");

    // Runs round-trip through settings text; the older bare totals still load.
    RestartRun run;
    run.finishedAtMs = 1'757'000'000'000;
    run.totalMs = 430'000;
    run.milestones = {{0, 0}, {20, 2'000}, {35, 9'000}, {60, 10'000}, {80, 400'000}};
    RestartRun parsed;
    check(parseRun(serializeRun(run), &parsed) && parsed.totalMs == run.totalMs &&
              parsed.finishedAtMs == run.finishedAtMs &&
              parsed.milestones == run.milestones,
          "a timed run with milestones round-trips through its stored text");
    check(parseRun(QStringLiteral("425277"), &parsed) && parsed.totalMs == 425'277 &&
              parsed.finishedAtMs == 0 && parsed.milestones.isEmpty(),
          "a bare total from the duration-only history still loads");
    check(!parseRun(QStringLiteral("1;500;"), &parsed) &&
              !parseRun(QStringLiteral("junk"), &parsed),
          "implausible or unreadable entries are dropped");
    QStringList many;
    for (int index = 0; index < kStoredRunLimit + 4; ++index)
        many.append(QString::number(60'000 + index));
    const QList<RestartRun> kept = parseRuns(many);
    check(kept.size() == kStoredRunLimit && kept.constLast().totalMs == 60'000 + kStoredRunLimit + 3,
          "stored runs keep the newest window for the Stats page");

    // Progress is not linear in time: the compile between 60% and 80% is most
    // of a rebuild. The estimate at each milestone is what recent restarts
    // really took from that milestone on, not a share of the total.
    QList<RestartRun> profiled;
    for (qint64 compile : {380'000, 400'000, 420'000}) {
        RestartRun timedRun;
        timedRun.totalMs = 10'000 + compile + 20'000;
        timedRun.milestones = {{0, 0}, {35, 5'000}, {60, 10'000},
                               {80, 10'000 + compile}, {100, 10'000 + compile + 5'000}};
        profiled.append(timedRun);
    }
    check(remainingAfterMilestoneMs(profiled, 0, 0) == 430'000,
          "the first estimate is the median total of recent restarts");
    check(remainingAfterMilestoneMs(profiled, 60, 12'000) == 420'000,
          "reaching 60% leaves the compile still ahead in the estimate");
    check(remainingAfterMilestoneMs(profiled, 80, 400'000) == 20'000,
          "after the compile only install and relaunch time remains");

    QList<RestartRun> totalsOnly = parseRuns({QStringLiteral("400000"),
                                              QStringLiteral("430000"),
                                              QStringLiteral("440000")});
    check(remainingAfterMilestoneMs(totalsOnly, 60, 30'000) == 400'000,
          "without milestone profiles the estimate counts down from the median total");
    check(remainingAfterMilestoneMs({}, 35, 20'000) == kFallbackEstimateMs - 20'000,
          "no history counts down from the fallback estimate");

    check(remainingNowMs(420'000, 12'000, 72'000) == 360'000,
          "the countdown keeps ticking on wall time between milestones");
    check(remainingNowMs(20'000, 400'000, 460'000) == 0,
          "an overrun restart shows no negative time");
    check(durationText(1'001) == QStringLiteral("2s"),
          "sub-minute text rounds up so the visible countdown never stalls at zero");
    check(durationText(61'001) == QStringLiteral("1m 02s"),
          "minute text stays compact and stable in the button row");

    // A long compile names its step count instead of sitting on "60%".
    const BuildProgress ninjaStep = lastBuildProgress(QStringLiteral(
        "[411/1210] Building CXX object CMakeFiles/forkmesh.dir/src/A.cpp.o\n"
        "[412/1210] Building CXX object CMakeFiles/forkmesh.dir/src/MainWindowAgents.cpp.o\n"
        "warning: unused variable\n"));
    check(ninjaStep.isValid() && ninjaStep.text() == QStringLiteral("compiling 412/1210"),
          "the newest Ninja progress line gives the step count");
    check(ninjaStep.target == QStringLiteral("MainWindowAgents.cpp"),
          "the step names the source file without its object suffix");
    const BuildProgress link = lastBuildProgress(QStringLiteral(
        "[1210/1210] Linking CXX executable forkmesh\r\n"));
    check(link.text() == QStringLiteral("linking 1210/1210") &&
              link.target == QStringLiteral("forkmesh"),
          "the link step reads as linking the binary");
    check(lastBuildProgress(QStringLiteral("[ 34%] Building CXX object x/B.cpp.o"))
                  .text() == QStringLiteral("compiling (34%)"),
          "Make's percent lines are read too");
    check(!lastBuildProgress(QStringLiteral("-- Configuring done\n")).isValid(),
          "output without a progress line leaves the activity alone");

    const QStringList compiled = compiledSources(QStringLiteral(
        "[0/2] Re-checking globbed directories...\n"
        "[1/3] Building CXX object CMakeFiles/forkmesh.dir/src/MainWindowGenie.cpp.o\n"
        "[2/3] Building CXX object CMakeFiles/forkmesh-package-crm.dir/home/f/marketplace/packages/crm/content/desktop/CrmFeature.cpp.o\r\n"
        "[ 50%] Building CXX object CMakeFiles/forkmesh.dir/src/MainWindowGenie.cpp.o\n"
        "[3/3] Linking CXX executable forkmesh\n"));
    check(compiled == QStringList({QStringLiteral("src/MainWindowGenie.cpp"),
                                   QStringLiteral("home/f/marketplace/packages/crm/content/desktop/CrmFeature.cpp")}),
          "each recompiled source is listed once, without its object suffix");
    check(compiledSources(QStringLiteral(
              "[1/2] Generating ForkMeshBuildCommit.h\n"
              "[2/2] Re-checking globbed directories...\n")).isEmpty(),
          "a build that compiled nothing lists no sources");
    check(rebuiltSummary({}) ==
              QStringLiteral("No source changes since the last build; restarting the same build."),
          "a no-op build says so");
    check(rebuiltSummary({QStringLiteral("src/A.cpp")}) ==
              QStringLiteral("Rebuilt 1 changed file: A.cpp"),
          "one rebuilt file is named");
    check(rebuiltSummary({QStringLiteral("src/A.cpp"), QStringLiteral("src/B.cpp"),
                          QStringLiteral("src/C.cpp"), QStringLiteral("src/D.cpp"),
                          QStringLiteral("src/E.cpp"), QStringLiteral("src/F.cpp"),
                          QStringLiteral("src/G.cpp")}) ==
              QStringLiteral("Rebuilt 7 changed files: A.cpp, B.cpp, C.cpp, D.cpp, E.cpp and 2 more"),
          "a long list names the first five and counts the rest");

    constexpr qint64 gib = 1024LL * 1024 * 1024;
    check(ccacheEnvironment(QStringLiteral("/mine"), QStringLiteral("/fast/ccache"),
                            QStringLiteral("/home/u/.cache"), 100 * gib)
              .isEmpty(),
          "a compile cache the desktop was started with is kept");
    const QMap<QString, QString> fast = ccacheEnvironment(
        QString(), QStringLiteral("/fast/ccache"), QStringLiteral("/home/u/.cache"), gib);
    check(fast.value(QStringLiteral("CCACHE_DIR")) == QStringLiteral("/fast/ccache") &&
              fast.value(QStringLiteral("CCACHE_MAXSIZE")) == QStringLiteral("40G"),
          "the restart shares the fast disk's compile cache with agent builds");
    const QMap<QString, QString> own = ccacheEnvironment(
        QString(), QString(), QStringLiteral("/home/u/.cache"), 26 * gib);
    check(own.value(QStringLiteral("CCACHE_DIR")) ==
                  QStringLiteral("/home/u/.cache/forkmesh/rebuild-ccache") &&
              own.value(QStringLiteral("CCACHE_MAXSIZE")) == QStringLiteral("6G"),
          "without a fast disk the restart keeps its own cache sized from free space");
    check(ccacheEnvironment(QString(), QString(), QStringLiteral("/c"), 500 * gib)
                  .value(QStringLiteral("CCACHE_MAXSIZE")) == QStringLiteral("20G"),
          "the restart's own cache has a ceiling");
    check(ccacheEnvironment(QString(), QString(), QStringLiteral("/c"), gib)
                  .value(QStringLiteral("CCACHE_MAXSIZE")) == QStringLiteral("2G"),
          "the restart's own cache has a floor");
    check(ccacheEnvironment(QString(), QString(), QString(), gib).isEmpty(),
          "no cache home leaves ccache's own default");

    const CcacheCounts counts = ccacheCounts(QStringLiteral(
        "# src/A.cpp\ncache_miss\ndirect_cache_miss\nlocal_storage_miss\n"
        "# src/B.cpp\ndirect_cache_hit\nlocal_storage_hit\n"
        "# src/C.cpp\npreprocessed_cache_hit\nlocal_storage_hit\n"
        "# src/D.cpp\ncould_not_use_precompiled_header\n"));
    check(counts.hits == 2 && counts.misses == 1,
          "the stats log counts each compile once as a hit or a miss");
    check(withCcacheSummary(QStringLiteral("Rebuilt 3 changed files: A.cpp, B.cpp, C.cpp"),
                            counts) ==
              QStringLiteral("Rebuilt 3 changed files: A.cpp, B.cpp, C.cpp"
                             " · compile cache: 2 of 3 hit"),
          "the summary says how many compiles came from the cache");
    check(withCcacheSummary(QStringLiteral("No source changes"), {}) ==
              QStringLiteral("No source changes"),
          "a build that compiled nothing adds no cache line");

    check(countdownText(60, QStringLiteral("compiling 412/1210"), 190'000, 30'000) ==
              QStringLiteral("60% · compiling 412/1210 · ~3m 10s left"),
          "the countdown says what the restart is doing and what is left");
    check(countdownText(60, QStringLiteral("compiling 900/1210"), 0, 422'000) ==
              QStringLiteral("60% · compiling 900/1210 · 7m 02s in, longer than usual…"),
          "an overrun restart shows how long it has really been going");
    check(countdownText(100, QStringLiteral("installing"), 0, 1'000) ==
              QStringLiteral("100% · relaunching…"),
          "the relaunch line stays short");

    return failures == 0 ? 0 : 1;
}
