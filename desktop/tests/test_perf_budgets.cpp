// Performance budgets for the desktop client (issue #357): startup
// (MainWindow ctor -> first paint) and repo-detail tab switches were both
// found and fixed "by feel" (see the Commits-tab and lazy-list-tab work).
// This suite puts a machine-checked floor under both so the next regression
// fails CI instead of waiting to be noticed.
//
// Startup is measured TWICE, on purpose (adhoc #89):
//   1. in-process, MainWindow ctor -> first paint. Cheap, but structurally
//      blind to everything that happens before main() gets here.
//   2. end-to-end, by launching the built binary and reading the
//      "[startup +NNNNms] first main-window paint" line out of its own startup
//      trace. This is the number a user experiences.
// Only the first one existed until adhoc #89. The archived network logs of one
// developer box hold 83 real starts between 2026-08-10 and 2026-08-19: median
// 1901ms, p90 5274ms, slowest 14281ms, and *not one* of them under the 1000ms
// budget this suite reported as passing the whole time. Measuring the
// constructor of a window inside a process that had already paid for
// QApplication, the Qt plugin and font databases, single-instance acquisition,
// the temporary mirror sweep and settings parsing simply cannot see any of
// those costs.
// Each budget below states what it covers and what it does not.
//
// Reference end-to-end measurements taken with tools/measure_startup.py on
// 2026-08-19 (v0.7.24, offscreen QPA, warm page cache, dev box at load ~4.5,
// 7 runs per profile): empty profile min 1000ms / median 1018ms / max 1076ms;
// a 274 KB real ForkMesh.conf min 993ms / median 1066ms / max 1128ms. The same
// binary under a parallel build (load 16-27) measured 1.6-2.4s, so treat any
// single number without its machine load as meaningless.
//
// The end-to-end budget is deliberately left at the issue #357 target of 1000ms
// rather than recalibrated to what the app currently does, so it stays a target
// and not a rubber stamp; use FORKMESH_PERF_STARTUP_E2E_BUDGET_MS to recalibrate
// per-environment. tools/measure_startup.py is the same measurement outside
// ctest, with repeated runs, min/median/max, a populated-settings profile, and
// a --history mode that reads the startups this machine has already recorded.
//
// Deliberately its own binary, separate from forkmesh-window-tests: that
// suite is known-flaky in some sandboxed environments, and a perf gate must
// be skippable per-environment (FORKMESH_SKIP_PERF_TESTS=1) without losing
// window-tests' functional coverage. The end-to-end startup check has its own
// narrower switch (FORKMESH_SKIP_PERF_STARTUP_E2E=1) so a box that cannot
// launch the app does not have to give up every other budget here.

#include "../src/MainWindow.h"
#include "../src/MainWindowInternal.h"
#include "../src/ActionTelemetry.h"
#include "../src/BackgroundActivity.h"
#include "../src/LogTimelineChart.h"

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPair>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextEdit>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>

namespace {

int failures = 0;

class UserInputProbe final : public QWidget
{
public:
    bool mousePressSeen = false;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        mousePressSeen = true;
        QWidget::mousePressEvent(event);
    }
};

qint64 envBudgetMs(const char *name, qint64 fallback)
{
    const QByteArray raw = qgetenv(name);
    if (raw.isEmpty())
        return fallback;
    bool ok = false;
    const qint64 value = raw.toLongLong(&ok);
    return (ok && value > 0) ? value : fallback;
}

void checkBudget(const QString &what, qint64 elapsedMs, qint64 budgetMs)
{
    if (elapsedMs <= budgetMs) {
        qInfo("PASS: %s took %lldms (budget %lldms)", qPrintable(what),
              static_cast<long long>(elapsedMs), static_cast<long long>(budgetMs));
    } else {
        qCritical("FAIL: %s took %lldms, over the %lldms budget", qPrintable(what),
                  static_cast<long long>(elapsedMs), static_cast<long long>(budgetMs));
        ++failures;
    }
}

bool runGitChecked(const QString &repoDir, const QStringList &arguments)
{
    QProcess process;
    process.setWorkingDirectory(repoDir);
    process.start(QStringLiteral("git"), arguments);
    if (!process.waitForFinished(10000))
        return false;
    return process.exitCode() == 0;
}

bool initGitRepo(QTemporaryDir &repo)
{
    if (!repo.isValid())
        return false;
    return runGitChecked(repo.path(), {"init", "-q"}) &&
           runGitChecked(repo.path(), {"config", "user.email", "perf@forkmesh.local"}) &&
           runGitChecked(repo.path(), {"config", "user.name", "ForkMesh Perf"}) &&
           runGitChecked(repo.path(), {"checkout", "-q", "-b", "main"}) &&
           runGitChecked(repo.path(), {"commit", "-q", "--allow-empty", "-m", "initial"});
}

// Drives the tab switch exactly as testClickRepoDetailTab does for a real
// click, then pumps the event loop the way the app's own paint cycle would,
// and reports the wall-clock cost of that switch (including any first-open
// lazy build/load the tab does).
qint64 measureTabSwitch(MainWindow &window, int tabId)
{
    QElapsedTimer timer;
    timer.start();
    window.testClickRepoDetailTab(tabId);
    for (int i = 0; i < 10; ++i)
        QApplication::processEvents();
    return timer.elapsed();
}

// --- End-to-end startup measurement (adhoc #89) -------------------------
// The in-process startup budget above cannot see QApplication construction,
// Qt plugin/font-database init, single-instance acquisition or the temporary
// mirror sweep, because this process already paid for all of them before the
// first line of main() below ran. The only way to time what a user actually
// waits through is to launch the real binary and read its own startup trace
// back, which is what the rest of this section does.

// MainWindow logs this from its first paintEvent. The child harness reads both
// stderr and the application log because the message sink can change during
// startup; duplicate copies collapse to the same timestamp.
const char *const kEventLoopMarker = "first main-window paint";

struct StartupTraceLine
{
    qint64 ms = 0;
    QString message;
};

// main() runs *two* elapsed clocks and prints both behind the same
// "[startup +NNNNms]" prefix: the process-wide one from beginStartupTrace()
// and a short-lived one restarted just before the MainWindow constructor
// (which produces "MainWindow constructed" and "window shown; entering event
// loop"). Their numbers interleave in the output, so matching the prefix alone
// silently mixes the two. Only the process-wide clock is monotonic across the
// whole run, so a timestamp that goes backwards is dropped here. The secondary
// clock starts later and therefore always reports a smaller elapsed than the
// process clock does at the same instant, so it can never push the high-water
// mark past a real line -- the marker and the total are safe either way.
QList<StartupTraceLine> parseStartupTrace(const QStringList &rawLines)
{
    static const QRegularExpression logPrefix(
        QStringLiteral("^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\s+"));
    static const QRegularExpression logSuffix(
        QStringLiteral("\\s+\\[[^\\]\\s]+:\\d+\\]$"));
    static const QRegularExpression traceLine(
        QStringLiteral("\\[startup \\+\\s*(\\d+)ms\\]\\s?(.*)$"));

    QList<StartupTraceLine> timeline;
    QSet<QString> seen;
    qint64 highWater = -1;
    for (const QString &raw : rawLines) {
        QString line = raw;
        line.remove(logPrefix);
        line.remove(logSuffix);
        const QRegularExpressionMatch match = traceLine.match(line);
        if (!match.hasMatch())
            continue;
        const qint64 ms = match.captured(1).toLongLong();
        const QString message = match.captured(2).trimmed();
        const QString key = QString::number(ms) + QChar(0x1f) + message;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        if (ms < highWater)
            continue; // MainWindow-relative clock, not the process clock
        highWater = ms;
        timeline.append({ms, message});
    }
    return timeline;
}

QString startupLogPath(const QString &dataHome)
{
    const QString direct =
        dataHome + QStringLiteral("/ForkMesh/ForkMesh/network_log.txt");
    if (QFile::exists(direct))
        return direct;
    QDirIterator it(dataHome, {QStringLiteral("network_log.txt")}, QDir::Files,
                    QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}

QString readTextFile(const QString &path)
{
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

// Every environment variable the measured child gets. Deliberately built from
// nothing rather than inherited: acquireSingleInstance() locks
// $HOME/.forkmesh/forkmesh.lock and calls QLocalServer::removeServer(), and
// startup sweeps QDir::tempPath() for stale ForkMesh mirror directories -- so a
// child that saw the developer's real HOME or TMPDIR would hijack the
// single-instance socket of a running node and delete its temporary state.
// DISPLAY, WAYLAND_DISPLAY, DBUS_SESSION_BUS_ADDRESS and SSH_AUTH_SOCK are
// omitted on purpose: a throwaway instance must not reach the real session.
QProcessEnvironment isolatedChildEnvironment(const QString &scratch)
{
    QProcessEnvironment env; // empty, not systemEnvironment()
    const QByteArray path = qgetenv("PATH");
    env.insert(QStringLiteral("PATH"),
               path.isEmpty() ? QStringLiteral("/usr/local/bin:/usr/bin:/bin")
                              : QString::fromLocal8Bit(path));
    env.insert(QStringLiteral("HOME"), scratch + QStringLiteral("/home"));
    env.insert(QStringLiteral("XDG_DATA_HOME"), scratch + QStringLiteral("/data"));
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), scratch + QStringLiteral("/config"));
    env.insert(QStringLiteral("XDG_CACHE_HOME"), scratch + QStringLiteral("/cache"));
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), scratch + QStringLiteral("/run"));
    env.insert(QStringLiteral("TMPDIR"), scratch + QStringLiteral("/tmp"));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("GSETTINGS_BACKEND"), QStringLiteral("memory"));
    // A real desktop session has a UTF-8 locale; without one Qt spends part of
    // startup switching to C.UTF-8, which no real launch pays.
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    return env;
}

// Launch the real binary once in a throwaway profile and return the
// milliseconds from exec() to the event-loop marker, or -1 with *why set.
qint64 measureEndToEndStartupMs(const QString &binary, QString *why,
                                QList<StartupTraceLine> *timelineOut)
{
    QTemporaryDir scratchDir;
    if (!scratchDir.isValid()) {
        *why = QStringLiteral("could not create a throwaway profile directory");
        return -1;
    }
    const QString scratch = scratchDir.path();
    QDir root(scratch);
    for (const QString &name : {QStringLiteral("home"), QStringLiteral("data"),
                                QStringLiteral("config"), QStringLiteral("cache"),
                                QStringLiteral("run"), QStringLiteral("tmp")}) {
        if (!root.mkpath(name)) {
            *why = QStringLiteral("could not create %1 in the throwaway profile")
                       .arg(name);
            return -1;
        }
    }
    // Qt refuses an XDG_RUNTIME_DIR that other users can reach.
    QFile::setPermissions(scratch + QStringLiteral("/run"),
                          QFile::ReadOwner | QFile::WriteOwner |
                              QFile::ExeOwner);

    QProcess child;
    child.setProgram(binary);
    child.setWorkingDirectory(scratch + QStringLiteral("/home"));
    child.setProcessEnvironment(isolatedChildEnvironment(scratch));
    // One stream keeps the trace in emission order. It must also be drained
    // while the child runs: a full pipe would block the process being timed.
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start();
    if (!child.waitForStarted(15000)) {
        *why = QStringLiteral("%1 did not start (%2)")
                   .arg(binary, child.errorString());
        return -1;
    }

    QString console;
    const QString dataHome = scratch + QStringLiteral("/data");
    QString logPath;
    bool sawMarker = false;
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 90000) {
        console += QString::fromUtf8(child.readAllStandardOutput());
        if (logPath.isEmpty())
            logPath = startupLogPath(dataHome);
        // Either source is enough to stop waiting: the marker reaches the
        // console only while Qt's default message handler is still installed,
        // and the profile's network_log.txt only once the app log sink has
        // taken over. Waiting on one specific source would burn the whole
        // deadline the moment main() moves the sink swap.
        if (console.contains(QLatin1String(kEventLoopMarker)) ||
            (!logPath.isEmpty() &&
             readTextFile(logPath).contains(QLatin1String(kEventLoopMarker)))) {
            sawMarker = true;
            break;
        }
        if (child.state() == QProcess::NotRunning)
            break;
        // waitForReadyRead, not a plain sleep: this test never runs a Qt event
        // loop while the child is alive, so nothing else would drain the pipe,
        // and a full pipe buffer would block the very process being timed.
        child.waitForReadyRead(25);
    }
    // Let the queued log write land, while staying well inside the 1000ms delay
    // before the app's first deferred startup job runs.
    if (sawMarker) {
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 150) {
            const int remaining = static_cast<int>(150 - settle.elapsed());
            if (remaining <= 0 || !child.waitForReadyRead(remaining))
                break;
            console += QString::fromUtf8(child.readAllStandardOutput());
        }
    }

    // The instance must never outlive this test.
    if (child.state() != QProcess::NotRunning) {
        child.terminate(); // the app shuts down gracefully on SIGTERM
        if (!child.waitForFinished(10000)) {
            child.kill();
            child.waitForFinished(5000);
        }
    }
    console += QString::fromUtf8(child.readAllStandardOutput());

    QStringList rawLines = console.split(QLatin1Char('\n'));
    logPath = startupLogPath(dataHome);
    rawLines += readTextFile(logPath).split(QLatin1Char('\n'));
    const QList<StartupTraceLine> timeline = parseStartupTrace(rawLines);
    if (timelineOut)
        *timelineOut = timeline;
    qint64 markerMs = -1;
    for (const StartupTraceLine &line : timeline) {
        if (line.message.startsWith(QLatin1String(kEventLoopMarker)))
            markerMs = line.ms; // keep the last: the timeline is monotonic
    }
    if (markerMs >= 0)
        return markerMs;
    *why = QStringLiteral("no '%1' line in %2 trace line(s)%3")
               .arg(QLatin1String(kEventLoopMarker))
               .arg(timeline.size())
               .arg(sawMarker ? QString()
                              : QStringLiteral("; the marker never appeared"));
    return -1;
}

// The slowest gaps between consecutive trace lines up to the marker: what a
// failing startup budget should print so the failure is diagnostic.
void reportSlowestStartupIntervals(const QList<StartupTraceLine> &timeline,
                                   int limit)
{
    int lastMarker = -1;
    for (int i = 0; i < timeline.size(); ++i) {
        if (timeline.at(i).message.startsWith(QLatin1String(kEventLoopMarker)))
            lastMarker = i;
    }
    QList<QPair<qint64, QString>> intervals;
    qint64 previous = 0;
    for (int i = 0; i < timeline.size(); ++i) {
        const StartupTraceLine &line = timeline.at(i);
        intervals.append({line.ms - previous,
                          QStringLiteral("+%1ms %2").arg(line.ms).arg(line.message)});
        previous = line.ms;
        if (i == lastMarker)
            break;
    }
    std::sort(intervals.begin(), intervals.end(),
              [](const QPair<qint64, QString> &a, const QPair<qint64, QString> &b) {
                  return a.first > b.first;
              });
    for (int i = 0; i < qMin(limit, intervals.size()); ++i) {
        qInfo("      %5lldms  %s", static_cast<long long>(intervals.at(i).first),
              qPrintable(intervals.at(i).second));
    }
}

} // namespace

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsSet("FORKMESH_SKIP_PERF_TESTS")) {
        qInfo("SKIP: performance budget tests disabled via "
              "FORKMESH_SKIP_PERF_TESTS (the gate is meant to be skippable "
              "per-environment; forkmesh-tests/forkmesh-window-tests still "
              "run functional coverage)");
        return 0;
    }

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        qCritical("FAIL: could not create temporary settings directory");
        return 1;
    }
    QTemporaryDir runtimeDir;
    if (!runtimeDir.isValid()) {
        qCritical("FAIL: could not create temporary runtime directory");
        return 1;
    }
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toUtf8());
    qputenv("GSETTINGS_BACKEND", QByteArrayLiteral("memory"));
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDir.path());

    // Same app-data isolation as forkmesh-window-tests: AppDataLocation reads
    // XDG_DATA_HOME at call time, so set it before QApplication (and thus
    // before MainWindow's initAgents()).
    QTemporaryDir dataDir;
    if (!dataDir.isValid()) {
        qCritical("FAIL: could not create temporary data directory");
        return 1;
    }
    qputenv("XDG_DATA_HOME", dataDir.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", dataDir.path().toUtf8());
    qputenv("XDG_CACHE_HOME", dataDir.path().toUtf8());
    // XDG already points every writable application path at this throwaway
    // directory. Qt's test mode would override it with ~/.qttest, which is not
    // writable in sandboxed builds and defeats the isolation above.
    QStandardPaths::setTestModeEnabled(false);

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName("ForkMeshTests");
    app.setApplicationName("PerfBudgets");

    const QString appDataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appDataPath.isEmpty()) {
        qCritical("FAIL: could not resolve temporary app data directory");
        return 1;
    }
    QDir appDataDir(appDataPath);
    if (appDataDir.exists() && !appDataDir.removeRecursively()) {
        qCritical("FAIL: could not clear temporary app data directory");
        return 1;
    }

    // --- Startup, number 1 of 2: MainWindow ctor -> first paint, in *this*
    // process, under the offscreen QPA plugin this suite (like
    // forkmesh-window-tests) runs on in CI.
    //
    // COVERS:         MainWindow's constructor and the first ten event-loop
    //                 turns after show(), against a brand-new empty profile in
    //                 a QTemporaryDir.
    // DOES NOT COVER: process exec, QApplication construction, Qt platform
    //                 plugin and font-database initialisation, single-instance
    //                 acquisition, the stale-mirror sweep of the temporary
    //                 directory, theme application, or the cost of parsing a
    //                 real (270 KB+) ForkMesh.conf -- this process paid all of
    //                 those before main() reached this line, and the empty
    //                 profile has no settings to read.
    // That is precisely why real 1.5-3.0s startups never tripped this gate
    // while it stayed green: it is a MainWindow-construction budget, not a
    // startup budget. The end-to-end number below is the one that would have
    // caught them. Target from issue #357: < 1000ms; overridable
    // per-environment for recalibration without a recompile.
    const qint64 startupBudgetMs =
        envBudgetMs("FORKMESH_PERF_STARTUP_BUDGET_MS", 1000);
    QElapsedTimer startupTimer;
    startupTimer.start();
    MainWindow window;
    window.show();
    for (int i = 0; i < 10; ++i)
        QApplication::processEvents();
    checkBudget(QStringLiteral("startup: MainWindow ctor -> first paint "
                               "(in-process, empty profile)"),
                startupTimer.elapsed(), startupBudgetMs);

    // --- Startup, number 2 of 2: the honest one (adhoc #89). Launch the built
    // binary as a child process and time exec() -> the first main-window paint
    // trace line emitted by the production deferred-startup path.
    //
    // COVERS:         everything a user waits through -- process exec and
    //                 dynamic linking of the real binary, QApplication
    //                 construction, Qt platform plugin and font-database
    //                 initialisation, single-instance acquisition, the
    //                 stale-mirror temporary sweep, settings load, theme,
    //                 MainWindow construction, first show and first paint.
    // DOES NOT COVER: a real windowing system (this runs offscreen, so no GL
    //                 context, compositor handshake or real font rendering); a
    //                 populated profile (the child gets a brand-new empty one,
    //                 which is the optimistic floor); a cold page cache; and
    //                 anything deferred until after the event loop starts.
    //                 For a populated-profile number, run
    //                 tools/measure_startup.py --profile realistic, which uses
    //                 the same marker and the same isolation recipe.
    //
    // The child is fully isolated (private HOME/XDG_*/TMPDIR, offscreen QPA)
    // because acquireSingleInstance() takes $HOME/.forkmesh/forkmesh.lock and
    // removes the "forkmesh-single-instance" socket, and startup deletes stale
    // ForkMesh mirror directories out of QDir::tempPath(). An under-isolated
    // launch here would hijack a node running on the same box.
    //
    // Skippable on its own (FORKMESH_SKIP_PERF_STARTUP_E2E=1) so a box that
    // cannot launch the app does not have to disable the whole perf suite and
    // lose every other budget with it.
    if (qEnvironmentVariableIsSet("FORKMESH_SKIP_PERF_STARTUP_E2E")) {
        qInfo("SKIP: end-to-end startup budget disabled via "
              "FORKMESH_SKIP_PERF_STARTUP_E2E");
    } else {
        QString startupBinary =
            qEnvironmentVariable("FORKMESH_PERF_STARTUP_BINARY");
        if (startupBinary.isEmpty()) {
            startupBinary = QCoreApplication::applicationDirPath() +
                            QStringLiteral("/forkmesh");
#ifdef Q_OS_WIN
            startupBinary += QStringLiteral(".exe");
#endif
#ifdef Q_OS_MACOS
            // The test binaries are plain executables (MACOSX_BUNDLE FALSE) but
            // the app target is a bundle.
            if (!QFileInfo(startupBinary).isFile()) {
                startupBinary = QCoreApplication::applicationDirPath() +
                                QStringLiteral("/forkmesh.app/Contents/MacOS/forkmesh");
            }
#endif
        }
        const QFileInfo binaryInfo(startupBinary);
        if (!binaryInfo.isFile() || !binaryInfo.isExecutable()) {
            // Loud, not silent: this suite is EXCLUDE_FROM_ALL, so it can be
            // built and run without the app target ever being built.
            qWarning("SKIP: no forkmesh binary at %s -- the end-to-end startup "
                     "budget did not run. Build the app first "
                     "(cmake --build <build> --target forkmesh) or point "
                     "FORKMESH_PERF_STARTUP_BINARY at it.",
                     qPrintable(startupBinary));
        } else {
            // Single cold launches are noisy on a shared box; take the best of
            // a few so the gate fails on a real regression, not on a moment of
            // scheduler contention.
            // envBudgetMs is just "positive qint64 from the environment,
            // else this fallback"; reused here for the attempt count.
            const qint64 e2eRuns =
                envBudgetMs("FORKMESH_PERF_STARTUP_E2E_RUNS", 2);
            qint64 best = -1;
            QList<StartupTraceLine> bestTimeline;
            QString why = QStringLiteral("no attempt completed");
            for (qint64 attempt = 0; attempt < e2eRuns; ++attempt) {
                QString attemptWhy;
                QList<StartupTraceLine> timeline;
                const qint64 measured =
                    measureEndToEndStartupMs(startupBinary, &attemptWhy, &timeline);
                if (measured < 0) {
                    why = attemptWhy;
                    continue;
                }
                if (best < 0 || measured < best) {
                    best = measured;
                    bestTimeline = timeline;
                }
            }
            if (best < 0) {
                qCritical("FAIL: could not measure end-to-end startup of %s (%s)",
                          qPrintable(startupBinary), qPrintable(why));
                ++failures;
            } else {
                const qint64 e2eBudgetMs =
                    envBudgetMs("FORKMESH_PERF_STARTUP_E2E_BUDGET_MS", 500);
                checkBudget(
                    QStringLiteral("startup: exec -> first window paint "
                                   "(real process, best of %1, empty profile)")
                        .arg(e2eRuns),
                    best, e2eBudgetMs);
                if (best > e2eBudgetMs) {
                    qInfo("    slowest startup intervals in the best run:");
                    reportSlowestStartupIntervals(bestTimeline, 8);
                    qInfo("    reproduce and compare profiles with: "
                          "tools/measure_startup.py --binary %s",
                          qPrintable(startupBinary));
                }
            }
        }
    }

    // --- Pathological rich-text diff: a single generated file used to bypass
    // per-file streaming and block QTextDocument layout for 2-12 seconds. The
    // renderer keeps the complete diff while streaming bounded fragments;
    // pathological documents use invisible scroll-driven windows so deferred
    // Qt layout can never grow to several megabytes.
    // Keep this header byte-for-byte shaped like diffFileHeaderHtml(). A stale
    // div-based fixture let the production table-header regression through:
    // every real file became one 1-6 MiB setHtml/insertHtml call.
    QString hugeDiffHtml = QStringLiteral(
        "<a name=\"file-0\"></a><div class='fileblock'>"
        "<table class='fileheader' width='100%' cellspacing='0' cellpadding='0'>"
        "<tr><td class='fpathcell'>large/generated.json</td>"
        "<td class='fctlcell' align='right'>0% read</td></tr></table>"
        "<table class='difftable' width='100%' cellspacing='0' cellpadding='0'>");
    const QString row =
        QStringLiteral("<tr><td class='ln'>123</td><td class='code add'>"
                       "+&quot;generated-key&quot;: &quot;generated-value&quot;"
                       "</td></tr>");
    hugeDiffHtml.reserve(4 * 1024 * 1024);
    while (hugeDiffHtml.size() < 4 * 1024 * 1024)
        hugeDiffHtml += row;
    hugeDiffHtml += QStringLiteral(
        "<tr><td class='ln'>999</td><td class='code add'>"
        "DIFF-TAIL-SENTINEL</td></tr></table></div>");
    QTextEdit hugeDiffView;
    hugeDiffView.setObjectName(QStringLiteral("perfHugeDiff"));
    const QList<qsizetype> hugeApplySizes =
        forkmesh::ui::diffPlannedUiApplySizesForTest(hugeDiffHtml);
    const qsizetype applyLimit =
        forkmesh::ui::diffMaxUiApplyCharsForTest();
    for (const qsizetype size : hugeApplySizes) {
        if (size > applyLimit) {
            qCritical("FAIL: planned diff GUI apply is %lld chars (limit %lld)",
                      static_cast<long long>(size),
                      static_cast<long long>(applyLimit));
            ++failures;
            break;
        }
    }
    const qint64 diffBudgetMs =
        envBudgetMs("FORKMESH_PERF_HUGE_DIFF_BUDGET_MS", 250);
    QElapsedTimer hugeDiffTimer;
    hugeDiffTimer.start();
    forkmesh::ui::renderDiffStreamed(
        &hugeDiffView, hugeDiffHtml,
        forkmesh::ui::diffStyleSheet());
    checkBudget(QStringLiteral("single-file 4 MiB diff first paint"),
                hugeDiffTimer.elapsed(), diffBudgetMs);
    // Oversized diffs are planned into bounded pages on a worker (that is
    // what keeps the first paint inside the budget above); give the plan a
    // moment to land before asserting on the pages it produced.
    const auto waitForDiffPages = [](QTextEdit *view) {
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 15000 &&
               forkmesh::ui::diffPageCountForTest(view) <= 0)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        for (int i = 0; i < 20; ++i)
            QApplication::processEvents();
    };
    waitForDiffPages(&hugeDiffView);
    const int hugePages = forkmesh::ui::diffPageCountForTest(&hugeDiffView);
    const QString firstPageText = hugeDiffView.toPlainText();
    if (hugePages <= 1 ||
        !firstPageText.contains(QStringLiteral("generated-key")) ||
        firstPageText.contains(QStringLiteral("responsive page"),
                               Qt::CaseInsensitive) ||
        firstPageText.contains(QStringLiteral("Previous")) ||
        firstPageText.contains(QStringLiteral("Next")) ||
        firstPageText.contains(QStringLiteral("DIFF-TAIL-SENTINEL"))) {
        qCritical("FAIL: oversized diff did not enter invisible bounded windowing");
        ++failures;
    }
    forkmesh::ui::showDiffPageForTest(&hugeDiffView, hugePages - 1);
    // A page streams in zero-delay batches; pump until the tail lands (or a
    // generous deadline passes) rather than a fixed number of turns.
    const auto waitForText = [](QTextEdit *view, const QString &needle) {
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 10000 && !view->toPlainText().contains(needle))
            QApplication::processEvents(QEventLoop::AllEvents, 50);
    };
    waitForText(&hugeDiffView, QStringLiteral("DIFF-TAIL-SENTINEL"));
    if (!hugeDiffView.toPlainText().contains(
            QStringLiteral("DIFF-TAIL-SENTINEL"))) {
        qCritical("FAIL: bounded diff windows did not retain the final content");
        ++failures;
    }

    // One minified source line can itself exceed the batch cap. It must be
    // split into safe visual continuation rows without losing its tail.
    QString hugeRowHtml = QStringLiteral(
        "<a name=\"file-0\"></a><div class='fileblock'>"
        "<table class='fileheader'><tr><td>minified.json</td></tr></table>"
        "<table class='difftable'><tr><td class='ln'>1</td>"
        "<td class='code add'>");
    hugeRowHtml += QString(1024 * 1024, QLatin1Char('x'));
    hugeRowHtml += QStringLiteral(
        "HUGE-ROW-TAIL</td></tr></table></div>");
    const QList<qsizetype> hugeRowApplySizes =
        forkmesh::ui::diffPlannedUiApplySizesForTest(hugeRowHtml);
    for (const qsizetype size : hugeRowApplySizes) {
        if (size > applyLimit) {
            qCritical("FAIL: huge-row GUI apply is %lld chars (limit %lld)",
                      static_cast<long long>(size),
                      static_cast<long long>(applyLimit));
            ++failures;
            break;
        }
    }
    QTextEdit hugeRowView;
    forkmesh::ui::renderDiffStreamed(
        &hugeRowView, hugeRowHtml, forkmesh::ui::diffStyleSheet());
    waitForDiffPages(&hugeRowView);
    const int hugeRowPages = forkmesh::ui::diffPageCountForTest(&hugeRowView);
    forkmesh::ui::showDiffPageForTest(&hugeRowView, hugeRowPages - 1);
    waitForText(&hugeRowView, QStringLiteral("HUGE-ROW-TAIL"));
    if (hugeRowPages <= 1 ||
        !hugeRowView.toPlainText().contains(QStringLiteral("HUGE-ROW-TAIL"))) {
        qCritical("FAIL: oversized single diff row was not safely retained");
        ++failures;
    }

    // The end cap supplies a viewport of trailing scroll range, so the final
    // line of the final file can reach the top edge of the diff viewport.
    QTextEdit endCapDiff;
    // The app-wide #diffView rule is what actually paints the page, so wear its
    // name: the cap colour is read back off the styled viewport.
    endCapDiff.setObjectName(QStringLiteral("diffView"));
    endCapDiff.resize(640, 320);
    endCapDiff.show();
    forkmesh::ui::renderDiffStreamed(
        &endCapDiff,
        QStringLiteral("<div class='fileblock'>first line<br>last source line</div>"),
        forkmesh::ui::diffStyleSheet());
    for (int i = 0; i < 4; ++i)
        QApplication::processEvents();
    QScrollBar *endCapBar = endCapDiff.verticalScrollBar();
    endCapBar->setValue(endCapBar->maximum());
    const QTextCursor lastSource =
        endCapDiff.document()->find(QStringLiteral("last source line"));
    if (endCapBar->maximum() <= 0 || lastSource.isNull() ||
        endCapDiff.cursorRect(lastSource).top() > endCapDiff.fontMetrics().height()) {
        qCritical("FAIL: diff end cap did not let the final source line reach the top");
        ++failures;
    }

    // …and it is the page colour with one line-tall black bar marking where the
    // diff ends (adhoc #1536). It used to be a whole viewport of solid black,
    // which on the light theme read as the page failing to render below the last
    // line. Read it from the top of the document, where the bar sits directly
    // under the final source line: pure black only, so the dark theme's #0d1117
    // page is never mistaken for the bar.
    endCapBar->setValue(0);
    QApplication::processEvents();
    const QImage endCapShot = endCapDiff.viewport()->grab().toImage();
    const int probeX = endCapShot.width() / 2; // clear of both short lines
    const auto isBar = [&endCapShot, probeX](int y) {
        const QColor pixel = endCapShot.pixelColor(probeX, y);
        return pixel.red() < 8 && pixel.green() < 8 && pixel.blue() < 8;
    };
    int barRows = 0;
    int barTop = -1;
    int barBottom = -1;
    for (int y = 0; y < endCapShot.height(); ++y) {
        if (!isBar(y))
            continue;
        ++barRows;
        if (barTop < 0)
            barTop = y;
        barBottom = y;
    }
    // One line tall and unbroken, page colour on both sides of it.
    const int lineHeight = endCapDiff.fontMetrics().lineSpacing();
    const QColor pageColour = endCapShot.pixelColor(endCapShot.width() - 6, 2);
    const QColor belowBar =
        barBottom >= 0 && barBottom + 4 < endCapShot.height()
            ? endCapShot.pixelColor(probeX, barBottom + 4)
            : QColor();
    if (barRows == 0 || barTop < 0 || barBottom - barTop + 1 != barRows ||
        barRows > 2 * lineHeight || belowBar != pageColour ||
        endCapShot.pixelColor(probeX, endCapShot.height() - 4) != pageColour) {
        qCritical("FAIL: the diff end cap is not one line-tall black bar over a "
                  "page-coloured run (%d black rows %d..%d, under it %s vs page %s)",
                  barRows, barTop, barBottom, qPrintable(belowBar.name()),
                  qPrintable(pageColour.name()));
        ++failures;
    }

    // --- Dense persisted network log: opening Logs previously parsed every
    // timestamp through QDateTime's locale/time-zone machinery and blocked the
    // UI for several seconds at the 20,000-line retention limit.
    QStringList denseLog;
    denseLog.reserve(20000);
    const QDateTime logNow = QDateTime::currentDateTime();
    for (int i = 20000; i > 0; --i) {
        denseLog.append(logNow.addSecs(-i).toString(
                            QStringLiteral("yyyy-MM-dd HH:mm:ss")) +
                        QStringLiteral("  net GET 200 mirror health"));
    }
    window.testLoadNetworkLog(denseLog);
    QElapsedTimer logOpenTimer;
    logOpenTimer.start();
    window.testShowLogSection();
    QApplication::processEvents();
    checkBudget(QStringLiteral("20,000-line log timeline first paint"),
                logOpenTimer.elapsed(),
                envBudgetMs("FORKMESH_PERF_LOG_OPEN_BUDGET_MS", 250));
    if (window.testLogTimelineVisibleCount() < denseLog.size()) {
        qCritical("FAIL: dense log timeline omitted retained entries");
        ++failures;
    }
    window.testShowHomeSection();

    // --- Streaming log feed. Every logged line lands in the timeline chart;
    // the chart used to fire its viewChanged callback (a visible-entry recount
    // and a frequency re-rank) synchronously per line. A burst must coalesce
    // into a handful of notifications.
    {
        LogTimelineChart chart;
        int notifications = 0;
        chart.viewChanged = [&notifications] { ++notifications; };
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        chart.setRange(nowMs - 60LL * 60 * 1000, nowMs);
        constexpr int kBurst = 2000;
        for (int i = 0; i < kBurst; ++i) {
            LogTimelineEntry entry;
            entry.timestampMs = nowMs - i * 10;
            entry.category = QStringLiteral("SESSION");
            chart.appendEntry(entry);
        }
        const int synchronous = notifications;
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 2000 && notifications == synchronous)
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (synchronous == 0 && notifications >= 1 && notifications <= 2 &&
            chart.visibleEntryCount() == kBurst) {
            qInfo("PASS: %d streamed log entries coalesced into %d chart "
                  "notification(s)", kBurst, notifications);
        } else {
            qCritical("FAIL: %d streamed log entries produced %d synchronous + "
                      "%d deferred chart notifications (visible %d)",
                      kBurst, synchronous, notifications - synchronous,
                      chart.visibleEntryCount());
            ++failures;
        }
    }

    // --- Off-screen Logs page: lines logged while another section is up must
    // not be laid out into the page's QTextDocument line by line; showing the
    // page catches up in one pass and ends on the newest line.
    if (QTextBrowser *logView = window.testNetworkLogView()) {
        const int blocksBefore = logView->document()->blockCount();
        for (int i = 0; i < 300; ++i)
            window.testLogSystem(QStringLiteral("Off-screen line %1").arg(i));
        const int blocksAfter = logView->document()->blockCount();
        window.testShowLogSection();
        QElapsedTimer logCatchUp;
        logCatchUp.start();
        QString shown;
        do {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            shown = logView->toPlainText();
        } while (!shown.contains(QStringLiteral("Off-screen line 299")) &&
                 logCatchUp.elapsed() < 2000);
        if (blocksAfter == blocksBefore &&
            shown.contains(QStringLiteral("Off-screen line 299"))) {
            qInfo("PASS: off-screen log lines defer their rich-text append and "
                  "land when the page is shown");
        } else {
            qCritical("FAIL: off-screen logging grew the hidden log document "
                      "(%d -> %d blocks) or the page did not catch up",
                      blocksBefore, blocksAfter);
            ++failures;
        }
        window.testShowHomeSection();
    } else {
        qCritical("FAIL: the Logs page view was not built");
        ++failures;
    }

    // --- Animated status glyphs are served from a frame cache. The fireball
    // and the rotated sync glyph are drawn per running row per 60 ms tick;
    // frames for the same (size, whole-degree angle) must share pixmap data.
    {
        const qint64 fireA =
            forkmesh::ui::agentRunningFireballPixmap(16, 33.2).cacheKey();
        const qint64 fireB =
            forkmesh::ui::agentRunningFireballPixmap(16, 33.4).cacheKey();
        const qint64 fireOther =
            forkmesh::ui::agentRunningFireballPixmap(16, 120.0).cacheKey();
        const qint64 spinA = forkmesh::ui::rotatedTintedOcticonPixmap(
                                 QStringLiteral("sync"), QColor("#58a6ff"), 14, 44.0)
                                 .cacheKey();
        const qint64 spinB = forkmesh::ui::rotatedTintedOcticonPixmap(
                                 QStringLiteral("sync"), QColor("#58a6ff"), 14, 44.3)
                                 .cacheKey();
        const qint64 lightA =
            forkmesh::ui::nodeStatusLightPixmap(QColor("#d29922"), 14, 36.0, true)
                .cacheKey();
        const qint64 lightB =
            forkmesh::ui::nodeStatusLightPixmap(QColor("#d29922"), 14, 36.0, true)
                .cacheKey();
        const QString markA = forkmesh::ui::octiconMarkup(QStringLiteral("check"), 13);
        const QString markB = forkmesh::ui::octiconMarkup(QStringLiteral("check"), 13);
        // The teammate badge on an agent row's avatar is redrawn on the same
        // per-tick path, and building it decodes artwork and rounds it off.
        const qint64 badgeA =
            forkmesh::ui::teammateAvatarPixmap(QStringLiteral("quality"),
                                               QStringLiteral("codex"),
                                               QStringLiteral("gpt-5.6-sol"), 9)
                .cacheKey();
        const qint64 badgeB =
            forkmesh::ui::teammateAvatarPixmap(QStringLiteral("quality"),
                                               QStringLiteral("codex"),
                                               QStringLiteral("gpt-5.6-sol"), 9)
                .cacheKey();
        const qint64 badgeOther =
            forkmesh::ui::teammateAvatarPixmap(QStringLiteral("cfo"),
                                               QStringLiteral("codex"),
                                               QStringLiteral("gpt-5.6-sol"), 9)
                .cacheKey();
        if (fireA == fireB && fireA != fireOther && spinA == spinB &&
            lightA == lightB && !markA.isEmpty() && markA == markB &&
            badgeA == badgeB && badgeA != badgeOther) {
            qInfo("PASS: animated glyph frames and octicon markup are cached");
        } else {
            qCritical("FAIL: animated glyph frames are re-rendered per call");
            ++failures;
        }
    }

    // --- Git reads keyed on the ref files instead of processes: the branch
    // tip resolves from loose and packed refs, the refs stamp moves for a
    // nested branch, a tip-keyed memo recomputes only when the tip moves, and
    // the worktree list memo refreshes when a worktree is added.
    {
        QTemporaryDir refsRepo;
        if (initGitRepo(refsRepo)) {
            const QString dir = refsRepo.path();
            const QString gitDir = dir + QStringLiteral("/.git");
            const auto revParse = [&dir](const QString &ref) {
                return QString::fromUtf8(
                           forkmesh::ui::runGitCaptureDirect(
                               dir, {QStringLiteral("rev-parse"), ref})
                               .output)
                    .trimmed();
            };
            bool ok = forkmesh::ui::refTipFromFiles(
                          gitDir, QStringLiteral("refs/heads/main")) ==
                      revParse(QStringLiteral("main"));
            const QString stampBefore = forkmesh::ui::gitRefsDirSignature(gitDir);
            int computed = 0;
            const auto memoized = [&] {
                return forkmesh::ui::mirrorTipMemoized<int>(
                    gitDir, QStringLiteral("main"), QStringLiteral("perf-test"),
                    [&computed] { return ++computed; });
            };
            memoized();
            memoized();
            ok = ok && computed == 1;
            ok = ok &&
                 runGitChecked(dir, {"checkout", "-q", "-b", "agent/nested/topic"}) &&
                 runGitChecked(dir, {"commit", "-q", "--allow-empty", "-m", "nested"});
            ok = ok && forkmesh::ui::refTipFromFiles(
                           gitDir, QStringLiteral("refs/heads/agent/nested/topic")) ==
                           revParse(QStringLiteral("HEAD"));
            ok = ok && forkmesh::ui::gitRefsDirSignature(gitDir) != stampBefore;
            ok = ok && runGitChecked(dir, {"checkout", "-q", "main"}) &&
                 runGitChecked(dir, {"commit", "-q", "--allow-empty", "-m", "moved"});
            memoized();
            ok = ok && computed == 2; // main's tip moved: recomputed once
            ok = ok && runGitChecked(dir, {"pack-refs", "--all"});
            ok = ok && forkmesh::ui::refTipFromFiles(
                           gitDir, QStringLiteral("refs/heads/main")) ==
                           revParse(QStringLiteral("main")); // packed now
            const auto forEachRefMatchesGit = [&dir, &gitDir] {
                const QByteArray fromGit =
                    forkmesh::ui::runGitCaptureDirect(
                        dir, {QStringLiteral("for-each-ref"),
                              QStringLiteral("--format=%(objectname) %(refname)"),
                              QStringLiteral("refs/heads/"),
                              QStringLiteral("refs/tags/")})
                        .output;
                QFile config(gitDir + QStringLiteral("/config"));
                const bool filesBackend =
                    !QFileInfo::exists(gitDir + QStringLiteral("/reftable")) &&
                    config.open(QIODevice::ReadOnly) &&
                    !config.readAll().toLower().contains("[extensions]");
                QByteArray fromFiles;
                const bool read = forkmesh::ui::forEachRefHeadsAndTagsFromFiles(
                    gitDir, &fromFiles);
                return filesBackend ? read && fromFiles == fromGit : !read;
            };
            ok = ok && runGitChecked(dir, {"tag", "-a", "v1", "-m", "annotated"}) &&
                 runGitChecked(dir, {"tag", "light"}) &&
                 runGitChecked(dir, {"branch", "agent-x.y"}) &&
                 runGitChecked(dir, {"pack-refs", "--all"}) &&
                 runGitChecked(dir, {"commit", "-q", "--allow-empty", "-m", "loose"}) &&
                 runGitChecked(dir, {"tag", "-a", "v2", "-m", "loose tag"}) &&
                 runGitChecked(dir, {"branch", "agent/loose/topic"});
            ok = ok && forEachRefMatchesGit();
            ok = ok && runGitChecked(dir, {"symbolic-ref", "refs/heads/alias",
                                           "refs/heads/main"});
            QByteArray symbolicOut;
            ok = ok && !forkmesh::ui::forEachRefHeadsAndTagsFromFiles(gitDir,
                                                                      &symbolicOut);
            ok = ok && runGitChecked(dir, {"symbolic-ref", "--delete",
                                           "refs/heads/alias"});
            ok = ok && forEachRefMatchesGit();
            QByteArray listBefore, listAfter;
            ok = ok && forkmesh::ui::cachedWorktreeListPorcelain(dir, &listBefore);
            const QString worktreePath = refsRepo.filePath(QStringLiteral("wt"));
            ok = ok && runGitChecked(dir, {"worktree", "add", "-q", "-b", "wt-branch",
                                           worktreePath, "main"});
            ok = ok && forkmesh::ui::cachedWorktreeListPorcelain(dir, &listAfter);
            ok = ok && !listBefore.contains(worktreePath.toUtf8()) &&
                 listAfter.contains(worktreePath.toUtf8());
            if (ok) {
                qInfo("PASS: ref tips, refs stamps, tip-keyed memos and the "
                      "worktree list memo follow the repository's files");
            } else {
                qCritical("FAIL: file-keyed git memos disagree with git "
                          "(computed=%d)", computed);
                ++failures;
            }
        } else {
            qCritical("FAIL: could not set up a temporary repo for the ref memo checks");
            ++failures;
        }
    }

    // Start the durable journal before the responsiveness probe so the test
    // can also prove that a worker-backed synchronous wait is surfaced as UI
    // blocking rather than disappearing into its worker's BGTASK record.
    const QString actionLog = dataDir.filePath(QStringLiteral("actions.jsonl"));
    forkmesh::ActionTelemetry::initialize(actionLog);

    if (!(forkmesh::ui::responsiveWaitEventFlags() &
          QEventLoop::ExcludeUserInputEvents)) {
        qInfo("PASS: GUI wait contract includes user and window-system input");
    } else {
        qCritical("FAIL: GUI wait contract excludes user and window-system input");
        ++failures;
    }

#ifdef Q_OS_UNIX
    // A GUI-thread Git helper does run the process on a worker, but its caller
    // still waits in a nested event loop for the result. That loop used
    // ExcludeUserInputEvents, so the footer showed BGTASK while mouse presses,
    // native window moves and other interaction sat queued until Git exited.
    // A deliberately slow Git alias exercises that path. The named event-loop
    // contract is asserted directly because synthetic posted mouse events are
    // not classified as platform input by every Qt backend.
    {
        UserInputProbe probe;
        probe.resize(40, 40);
        probe.show();
        QTimer::singleShot(25, &probe, [&probe] {
            QCoreApplication::postEvent(
                &probe,
                new QMouseEvent(QEvent::MouseButtonPress, QPointF(4, 4),
                                QPointF(5000, 5000), Qt::LeftButton,
                                Qt::LeftButton, Qt::NoModifier));
        });
        const forkmesh::ui::GitCaptureResult delayed =
            forkmesh::ui::runGitCaptureBackgrounded(
                QDir::tempPath(),
                {QStringLiteral("-c"),
                 QStringLiteral("alias.forkmesh-responsive=!sleep 0.35"),
                 QStringLiteral("forkmesh-responsive")});
        if (delayed.ok && probe.mousePressSeen) {
            qInfo("PASS: synchronous worker-Git wait keeps dispatching user input");
        } else {
            qCritical("FAIL: synchronous worker-Git wait starved user input");
            ++failures;
        }
    }
#endif

    // --- Repo-detail tab switches, including each tab's first (lazy-build)
    // open -- the Commits tab and the "and more" list tabs (Worktrees,
    // Releases, Mirror nodes) were exactly the ones found slow by feel.
    // Target from issue #357: < 150ms; overridable per-environment.
    const qint64 tabBudgetMs = envBudgetMs("FORKMESH_PERF_TABSWITCH_BUDGET_MS", 150);

    QTemporaryDir repoDir;
    if (initGitRepo(repoDir)) {
        const int idx =
            window.testAddLocalRepository("perf", "budgetrepo", repoDir.path());
        QElapsedTimer repoOpenTimer;
        repoOpenTimer.start();
        window.testOpenRepository(idx);
        QApplication::processEvents();
        checkBudget(QStringLiteral("repository open first paint"),
                    repoOpenTimer.elapsed(),
                    envBudgetMs("FORKMESH_PERF_REPO_OPEN_BUDGET_MS", 250));

        struct TabCase {
            int id;
            const char *label;
        };
        const QList<TabCase> cases = {
            {1, "Commits"},
            {2, "Issues"},
            {window.testWorktreesTabIndex(), "Worktrees"},
            {window.testReleasesTabIndex(), "Releases"},
        };
        for (const TabCase &tc : cases) {
            const qint64 elapsed = measureTabSwitch(window, tc.id);
            checkBudget(
                QStringLiteral("tab switch: %1").arg(QString::fromLatin1(tc.label)),
                elapsed, tabBudgetMs);
        }
    } else {
        qCritical("FAIL: could not set up a temporary repo for tab-switch timing");
        ++failures;
    }

    // --- Agent merge-state sweep: git processes must not scale with the number
    // of agent sessions.
    //
    // Field evidence (~/.forkmesh/diagnostics/actions.jsonl, 13,424 recorded
    // git starts): four agent-panel helpers accounted for 47% of every git
    // process the app spawned, and 44% of all git commands were exact
    // duplicates within 10s. The worst single refreshAgentMergeState() sweep
    // issued 167 `merge-base --is-ancestor` calls for 13 distinct (commit,
    // base) pairs -- 132 of them byte-identical -- because dozens of agent
    // branches sit on the same commit. The sweep resolves one base tip up
    // front, so every repeated pair is a pure duplicate, and the branch tip of
    // every candidate is already available from one bulk for-each-ref.
    //
    // This is deliberately a *count* budget rather than a wall-clock one:
    // process spawns are what the fix removes, and unlike the timings above a
    // count does not move with machine load.
    {
        QTemporaryDir agentRepo;
        constexpr int kSessions = 24;
        bool setupOk = initGitRepo(agentRepo);
        QString sharedTip;
        if (setupOk) {
            sharedTip = QString::fromUtf8(forkmesh::ui::runGitCaptureDirect(
                                              agentRepo.path(),
                                              {QStringLiteral("rev-parse"),
                                               QStringLiteral("HEAD")})
                                              .output)
                            .trimmed();
            setupOk = !sharedTip.isEmpty();
        }
        // Every session's branch parks on main's tip. That is the shape the
        // stall log shows in the field (a fleet of worktree branches at one
        // commit) and the one where both is-ancestor probes per candidate run.
        for (int i = 0; setupOk && i < kSessions; ++i)
            setupOk = runGitChecked(agentRepo.path(),
                                    {"branch",
                                     QStringLiteral("agent/perf-%1").arg(i),
                                     sharedTip});
        if (setupOk) {
            const int agentIdx = window.testAddLocalRepository(
                QStringLiteral("perf"), QStringLiteral("agentsweep"),
                agentRepo.path());
            window.testOpenRepository(agentIdx);
            QApplication::processEvents();
            for (int i = 0; i < kSessions; ++i) {
                AgentSession session;
                session.id = 900000 + i;
                session.owner = QStringLiteral("perf");
                session.name = QStringLiteral("agentsweep");
                session.provider = QStringLiteral("claude-code");
                session.prompt = QStringLiteral("merge-sweep fixture");
                session.status = AgentStatus::Success;
                session.branchName = QStringLiteral("agent/perf-%1").arg(i);
                session.baseBranch = QStringLiteral("main");
                session.mergeCandidateHead = sharedTip;
                window.testAddAgentSession(session);
            }
            // waitForGit is the one chokepoint every git subprocess passes, and
            // it opens a "git" ticket on whichever thread it runs on -- so the
            // activity bus counts spawns without new instrumentation.
            std::atomic<int> ancestorProbes{0};
            std::atomic<int> branchTipProbes{0};
            std::atomic<int> gitStarts{0};
            forkmesh::BackgroundActivity::setListener(
                [&](const forkmesh::BackgroundTicket &ticket, bool started) {
                    if (!started || ticket.kind != QLatin1String("git"))
                        return;
                    ++gitStarts;
                    if (ticket.detail.contains(
                            QLatin1String("merge-base --is-ancestor")))
                        ++ancestorProbes;
                    else if (ticket.detail.contains(QLatin1String("rev-parse")) &&
                             ticket.detail.contains(QLatin1String("refs/heads/")))
                        ++branchTipProbes;
                });
            QElapsedTimer sweepTimer;
            sweepTimer.start();
            window.testRefreshAgentMergeState();
            // The sweep owns a worker thread: wait for it to retire rather than
            // guessing an interval.
            while (window.testAgentMergeStateRefreshing() &&
                   sweepTimer.elapsed() < 30000) {
                QApplication::processEvents();
                QThread::msleep(2);
            }
            const qint64 sweepMs = sweepTimer.elapsed();
            forkmesh::BackgroundActivity::setListener(nullptr);
            const int ancestor = ancestorProbes.load();
            const int tips = branchTipProbes.load();
            const int spawns = gitStarts.load();
            qInfo("agent merge sweep over %d sessions on one shared tip: "
                  "%d is-ancestor, %d branch-tip, %d git spawns total, %lldms",
                  kSessions, ancestor, tips, spawns,
                  static_cast<long long>(sweepMs));
            // One base tip and one distinct commit across every session, so the
            // sweep needs a couple of probes -- never one per session. The
            // budgets are overridable for recalibration, not for papering over
            // a regression.
            const qint64 ancestorBudget =
                envBudgetMs("FORKMESH_PERF_AGENT_SWEEP_ANCESTOR_PROBES", 4);
            const qint64 tipBudget =
                envBudgetMs("FORKMESH_PERF_AGENT_SWEEP_TIP_PROBES", 2);
            // The total also has a ceiling, so adding a *different* per-session
            // git read to this loop fails here even though the two counters
            // above would not notice it.
            const qint64 spawnBudget =
                envBudgetMs("FORKMESH_PERF_AGENT_SWEEP_GIT_SPAWNS", 8);
            if (ancestor <= ancestorBudget && tips <= tipBudget &&
                spawns <= spawnBudget) {
                qInfo("PASS: agent merge sweep git probes stay constant across "
                      "%d sessions (%d is-ancestor <= %lld, %d branch-tip <= "
                      "%lld, %d spawns <= %lld)",
                      kSessions, ancestor,
                      static_cast<long long>(ancestorBudget), tips,
                      static_cast<long long>(tipBudget), spawns,
                      static_cast<long long>(spawnBudget));
            } else {
                qCritical("FAIL: agent merge sweep spawned %d is-ancestor "
                          "(budget %lld), %d branch-tip (budget %lld) and %d "
                          "git processes total (budget %lld) for %d sessions -- "
                          "per-session git work is not being collapsed",
                          ancestor, static_cast<long long>(ancestorBudget), tips,
                          static_cast<long long>(tipBudget), spawns,
                          static_cast<long long>(spawnBudget), kSessions);
                ++failures;
            }
        } else {
            qCritical("FAIL: could not set up a temporary repo for the agent "
                      "merge sweep probe count");
            ++failures;
        }
    }

    // The action journal must contain a matching start/finish pair and must have
    // flushed before shutdown returns.
    const quint64 probe = forkmesh::BackgroundActivity::begin(
        QStringLiteral("test"), QStringLiteral("perf telemetry probe"),
        forkmesh::ActionTelemetry::Execution::Worker);
    forkmesh::BackgroundActivity::end(probe, QStringLiteral("succeeded"));
    forkmesh::ActionTelemetry::shutdown();
    QFile actionFile(actionLog);
    const QByteArray actionBytes =
        actionFile.open(QIODevice::ReadOnly) ? actionFile.readAll() : QByteArray();
    if (actionBytes.count("\"detail\":\"perf telemetry probe\"") == 2 &&
        actionBytes.contains("\"event\":\"started\"") &&
        actionBytes.contains("\"event\":\"finished\"") &&
        actionBytes.contains("\"outcome\":\"succeeded\"")) {
        qInfo("PASS: asynchronous action journal records start, finish, duration, "
              "execution context and outcome");
    } else {
        qCritical("FAIL: asynchronous action journal did not flush a complete "
                  "start/finish pair");
        ++failures;
    }
#ifdef Q_OS_UNIX
    bool uiWaitRecorded = false;
    for (const QByteArray &line : actionBytes.split('\n')) {
        if (line.contains("\"kind\":\"git wait\"") &&
            line.contains("\"execution\":\"ui-blocking\"")) {
            uiWaitRecorded = true;
            break;
        }
    }
    if (uiWaitRecorded) {
        qInfo("PASS: synchronous worker wait is recorded as UI-blocking telemetry");
    } else {
        qCritical("FAIL: synchronous worker wait was absent from UI-blocking telemetry");
        ++failures;
    }
#endif

    return failures == 0 ? 0 : 1;
}
