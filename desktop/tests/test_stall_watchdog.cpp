// The UI-stall watchdog's record must date a stall from when the GUI thread
// stopped answering, not from when the sampler thread got round to writing
// it down: on 2026-09-12 the sampler itself was held ~152 s inside its first
// capture (the whole process was paging) and a 153 s stall was logged as
// starting two seconds before it ended. Blocks the main thread on purpose
// and checks the record's timing and its machine-state lines.
#include "../src/StallWatchdog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

} // namespace

// Qt and glibc carry no frame pointers, so a chain walk from a stall inside
// them kept only the leaf: 407 of 449 records on 2026-09-22 named no app
// frame at all. The scan finds callers by the call in front of each return
// address instead.
void checkStackScan()
{
#if defined(__x86_64__)
    // A fake text segment: a call rel32 ending at 15, a call through a
    // register ending at 32, and a code address at 40 with no call before it.
    alignas(16) static unsigned char text[64] = {};
    text[10] = 0xE8;
    text[30] = 0xFF;
    text[31] = 0xD0; // call *%rax
    const auto base = reinterpret_cast<std::uintptr_t>(text);
    const stallwatch::detail::TextRange ranges[] = {{base, base + sizeof(text)}};
    std::uintptr_t stack[] = {0x1234, base + 15, base + 40, 0, base + 32,
                              base + 4096};
    const auto low = reinterpret_cast<std::uintptr_t>(stack);
    const auto high = low + sizeof(stack);
    void *trace[8] = {};
    const int count = stallwatch::detail::safeStackScanTrace(
        trace, 8, 0x1010, low, low, high, ranges, 1);
    check(count == 3 && reinterpret_cast<std::uintptr_t>(trace[0]) == 0x1010 &&
              reinterpret_cast<std::uintptr_t>(trace[1]) == base + 15 &&
              reinterpret_cast<std::uintptr_t>(trace[2]) == base + 32,
          "the stack scan keeps only addresses that follow a call");
    check(stallwatch::detail::safeStackScanTrace(trace, 2, 0x1010, low, low,
                                                 high, ranges, 1) == 2,
          "the stack scan stops at the caller's capacity");
    check(stallwatch::detail::safeStackScanTrace(trace, 8, 0x1010, high, low,
                                                 high, ranges, 1) == 1,
          "the stack scan never reads past the captured stack");
#endif
}

void checkInstrumentationFramesAreNotOrigins()
{
    const QString exe = QStringLiteral("/tmp/forkmesh-test");
    const QString sampled = QStringLiteral(
        "/lib/x86_64-linux-gnu/libQt6Widgets.so.6(+0x25dafc) [0x0]\n"
        "/tmp/forkmesh-test(_ZN8forkmesh2ui20QtEventDeliveryProbeD1Ev+0x6c) [0x1]\n"
        "/tmp/forkmesh-test(_ZN8forkmesh2ui20QtRuntimeDiagnostics12noteDeliveryEPKciN6QEvent4TypeEx+0x24) [0x2]\n"
        "/tmp/forkmesh-test(_ZN10MainWindow22renderRemoteBranchRowsERKNS_17BranchesPanelDataERiS3_S3_+0x11fd) [0x3]\n"
        "/tmp/forkmesh-test(_ZN10MainWindow19renderBranchesPanelERKNS_17BranchesPanelDataE+0x2a59) [0x4]\n");
    const QString origin = stallwatch::likelyOrigin(sampled, exe);
    check(!origin.contains(QStringLiteral("QtEventDeliveryProbe")) &&
              !origin.contains(QStringLiteral("noteDelivery")),
          "the event-delivery probe is never named as a stall origin");
    check(origin.startsWith(QStringLiteral("MainWindow::renderRemoteBranchRows")) &&
              origin.contains(QStringLiteral("MainWindow::renderBranchesPanel")),
          "the origin skips the probe to the application frames under it");
}

void checkTimingHookAndPolishGuardAreNotOrigins()
{
    const QString exe = QStringLiteral("/tmp/forkmesh-test");
    const QString sampled = QStringLiteral(
        "/lib/x86_64-linux-gnu/libQt6Gui.so.6(_ZN4QCss13StyleSelector17styleRulesForNodeENS0_7NodePtrE+0x692) [0x0]\n"
        "/tmp/forkmesh-test(_ZN8forkmesh2ui20QtRuntimeDiagnostics15noteEventTimingEPKciN6QEvent4TypeEx+0x24) [0x1]\n"
        "/tmp/forkmesh-test(_ZN8forkmesh13hiddenunicode11guardWidgetEP7QWidget+0x22b) [0x2]\n"
        "/lib/x86_64-linux-gnu/libQt6Widgets.so.6(_ZNK7QWidget14ensurePolishedEv+0x72) [0x3]\n"
        "/tmp/forkmesh-test(_ZN10MainWindow22appendMarketGuideCardsEv+0x51) [0x4]\n");
    const QString origin = stallwatch::likelyOrigin(sampled, exe);
    check(!origin.contains(QStringLiteral("noteEventTiming")) &&
              !origin.contains(QStringLiteral("guardWidget")),
          "the event timing hook and the Polish input guard are never stall origins");
    check(origin.contains(QStringLiteral("MainWindow::appendMarketGuideCards")),
          "the origin names the application frame that created the widgets");
}

void checkEventFilterResidueIsNotOrigin()
{
    const QString exe = QStringLiteral("/tmp/forkmesh-test");
    const QString sampled = QStringLiteral(
        "/lib/x86_64-linux-gnu/libQt6Gui.so.6(+0x3bc944) [0x0]\n"
        "/tmp/forkmesh-test(_ZN10MainWindow26handleFramelessResizeEventEP7QObjectP6QEvent+0x29) [0x1]\n"
        "/tmp/forkmesh-test(_ZN10MainWindow11eventFilterEP7QObjectP6QEvent+0x36e) [0x2]\n"
        "/tmp/forkmesh-test(_ZN8forkmesh6sonify8UiSounds11eventFilterEP7QObjectP6QEvent+0x4d) [0x3]\n"
        "/tmp/forkmesh-test(_ZN8forkmesh2ui21appendDiffStreamChunkEP9QTextEditRK7QString+0xb4) [0x4]\n"
        "/tmp/forkmesh-test(_ZN10MainWindow20applyBranchDiffChunkEiRK7QStringii+0x4c) [0x5]\n");
    const QString origin = stallwatch::likelyOrigin(sampled, exe);
    check(!origin.contains(QStringLiteral("eventFilter")) &&
              !origin.contains(QStringLiteral("handleFramelessResizeEvent")),
          "app-wide event filter frames are never named as a stall origin");
    check(origin.startsWith(QStringLiteral("forkmesh::ui::appendDiffStreamChunk")) &&
              origin.contains(QStringLiteral("MainWindow::applyBranchDiffChunk")),
          "the origin skips filter residue to the frames that did the work");
}

void checkSystemWaitsAreNamed()
{
    const QString exe = QStringLiteral("/tmp/forkmesh-test");
    const QString swap = QStringLiteral(
        "/lib/x86_64-linux-gnu/libc.so.6(poll+0x16) [0x0]\n"
        "/lib/x86_64-linux-gnu/libwayland-client.so.0(wl_display_dispatch_queue+0xfc) [0x1]\n"
        "/lib/x86_64-linux-gnu/libQt6Gui.so.6(_ZN14QOpenGLContext11swapBuffersEP8QSurface+0x79) [0x2]\n"
        "/tmp/forkmesh-test(_ZN10MainWindow11eventFilterEP7QObjectP6QEvent+0x36e) [0x3]\n"
        "/tmp/forkmesh-test(_ZN17GenieSpiderWidget4tickEv+0x4c) [0x4]\n");
    const QString swapOrigin = stallwatch::likelyOrigin(swap, exe);
    check(swapOrigin.startsWith(QStringLiteral("Qt window flush")) &&
              swapOrigin.contains(QStringLiteral("GenieSpiderWidget::tick")),
          "a compositor buffer swap is named before the app frames under it");
    const QString settings = QStringLiteral(
        "/lib/x86_64-linux-gnu/libc.so.6(fdatasync+0x1a) [0x0]\n"
        "/lib/x86_64-linux-gnu/libQt6Core.so.6(_ZN13QFSFileEngine10syncToDiskEv+0x84) [0x1]\n"
        "/lib/x86_64-linux-gnu/libQt6Core.so.6(_ZN9QSaveFile6commitEv+0xd5) [0x2]\n"
        "/tmp/forkmesh-test(_ZN10MainWindow26handleFramelessResizeEventEP7QObjectP6QEvent+0x29) [0x3]\n"
        "/lib/x86_64-linux-gnu/libQt6Core.so.6(_ZN9QSettings5eventEP6QEvent+0x2e) [0x4]\n");
    check(stallwatch::likelyOrigin(settings, exe)
              .startsWith(QStringLiteral("QSettings write flushed to disk")),
          "a QSettings fsync is named even past filter residue");
    const QString plain = QStringLiteral(
        "/lib/x86_64-linux-gnu/libQt6Gui.so.6(+0x3bc944) [0x0]\n"
        "/tmp/forkmesh-test(_ZN10MainWindow20applyBranchDiffChunkEiRK7QStringii+0x4c) [0x1]\n");
    check(stallwatch::likelyOrigin(plain, exe)
              .startsWith(QStringLiteral("MainWindow::applyBranchDiffChunk")),
          "an ordinary sample keeps its application frame as the origin");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    checkStackScan();
    checkInstrumentationFramesAreNotOrigins();
    checkTimingHookAndPolishGuardAreNotOrigins();
    checkEventFilterResidueIsNotOrigin();
    checkSystemWaitsAreNamed();
    QTemporaryDir scratch;
    check(scratch.isValid(), "temporary directory");
    const QString logPath = QDir(scratch.path()).filePath(QStringLiteral("stalls.log"));

    StallWatchdog watchdog;
    // The heartbeat ticks every 200 ms, so the threshold has to clear a
    // whole idle gap or every quiet turn counts; the app itself runs 500 ms.
    // The block below is looked up by size, since process start-up on a
    // loaded machine can trip the threshold once on its own.
    qint64 reportedPeakMs = 0;
    QString reportedId;
    QString reportedReport;
    qint64 reportedStartedAtMs = 0;
    int longReports = 0;
    constexpr int kBlockMs = 1500;
    QObject::connect(&watchdog, &StallWatchdog::stalled, &app,
                     [&](qint64 peakMs, const QString &stallId, const QString &,
                         const QString &, const QString &report,
                         qint64 startedAtMs) {
                         if (peakMs < kBlockMs - 300)
                             return;
                         ++longReports;
                         reportedPeakMs = peakMs;
                         reportedId = stallId;
                         reportedReport = report;
                         reportedStartedAtMs = startedAtMs;
                     });
    watchdog.start(/*stallThresholdMs=*/400, logPath,
                   QStringLiteral("ForkMesh test build"));

    qint64 blockedFromMs = 0;
    qint64 blockedUntilMs = 0;
    QTimer::singleShot(400, &app, [&] {
        const BlockingCallScope scope(QStringLiteral("test: sleeping on the GUI thread"));
        blockedFromMs = QDateTime::currentMSecsSinceEpoch();
        std::this_thread::sleep_for(std::chrono::milliseconds(kBlockMs));
        blockedUntilMs = QDateTime::currentMSecsSinceEpoch();
    });
    // The sampler polls every 100 ms and the report is queued to this thread;
    // leave ample room for a loaded machine.
    QTimer::singleShot(400 + kBlockMs + 1500, &app, &QCoreApplication::quit);
    app.exec();
    watchdog.stop();

    check(longReports == 1, "exactly one stall the size of the block reported");
    check(reportedPeakMs >= kBlockMs - 100, "peak covers the whole block");
    check(reportedId.startsWith(QStringLiteral("STALL-")), "stall id");
    check(reportedReport.contains(
              QStringLiteral("blocking call: test: sleeping on the GUI thread")),
          "blocking-call breadcrumb reached the record");
    check(qAbs(reportedStartedAtMs - blockedFromMs) <= 500,
          "the signal carries the wall clock at which the block began");

    QFile log(logPath);
    check(log.open(QIODevice::ReadOnly | QIODevice::Text), "stall log written");
    const QString wholeLog = QString::fromUtf8(log.readAll());
    const QString begin =
        QStringLiteral("===== ") + reportedId + QStringLiteral(" BEGIN =====");
    const QString end =
        QStringLiteral("===== ") + reportedId + QStringLiteral(" END =====");
    check(wholeLog.contains(begin) && wholeLog.contains(end),
          "record has BEGIN and END markers");
    // Only the block's own record: start-up may have left a short one.
    const int from = wholeLog.indexOf(begin);
    const int to = wholeLog.indexOf(end, qMax(from, 0));
    const QString text =
        from >= 0 && to > from ? wholeLog.mid(from, to - from) : wholeLog;

    // The estimated start must sit where the block began, and the report
    // time where it ended - not both at the end.
    const QRegularExpression timing(QStringLiteral(
        "timing: estimated start (\\S+) \\| watchdog report (\\S+)"));
    const QRegularExpressionMatch match = timing.match(text);
    check(match.hasMatch(), "timing line present");
    if (match.hasMatch()) {
        const qint64 startMs =
            QDateTime::fromString(match.captured(1), Qt::ISODateWithMs)
                .toMSecsSinceEpoch();
        const qint64 reportMs =
            QDateTime::fromString(match.captured(2), Qt::ISODateWithMs)
                .toMSecsSinceEpoch();
        const qint64 span = reportMs - startMs;
        // The sampler polls every 100 ms and the last heartbeat before the
        // block may be up to 200 ms old, so allow half a second of slack.
        check(qAbs(span - reportedPeakMs) <= 500,
              "wall-clock span agrees with the monotonic peak");
        check(qAbs(startMs - blockedFromMs) <= 500,
              "estimated start is when the GUI thread stopped answering");
        check(reportMs >= blockedUntilMs - 50 && reportMs <= blockedUntilMs + 700,
              "watchdog report is when it resumed");
        if (qAbs(span - reportedPeakMs) > 500 || qAbs(startMs - blockedFromMs) > 500 ||
            reportMs < blockedUntilMs - 50 || reportMs > blockedUntilMs + 700) {
            std::fprintf(stderr, "start %lld (blocked from %lld), report %lld "
                                 "(blocked until %lld), peak %lld\n",
                         (long long)startMs, (long long)blockedFromMs,
                         (long long)reportMs, (long long)blockedUntilMs,
                         (long long)reportedPeakMs);
        }
    }
    check(!text.contains(QStringLiteral("note: monotonic peak")),
          "no clock disagreement is noted for an honest block");
#if defined(__linux__) && defined(__GLIBC__) && \
    (defined(__x86_64__) || defined(__aarch64__))
    // The block sleeps inside glibc; the sample has to climb out of it,
    // through Qt's timer dispatch, back to this executable's own frames.
    const QString exeName =
        QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    check(reportedReport.contains(exeName + QLatin1Char('(')),
          "the sample reaches the application frames above a libc leaf");
#endif
#ifdef Q_OS_LINUX
    // Machine state rides along on every record (PSI is standard on the
    // kernels ForkMesh ships for; the lines are simply absent elsewhere).
    if (QFile::exists(QStringLiteral("/proc/pressure/memory"))) {
        check(text.contains(QStringLiteral("system at detection: memory PSI")),
              "record carries the machine state at detection");
        check(text.contains(QStringLiteral("system at resume: memory PSI")),
              "record carries the machine state at resume");
    }
    check(text.contains(QStringLiteral("this process ")) &&
              text.contains(QStringLiteral(" resident")),
          "record carries this process's footprint");
#endif
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n---\n%s\n", g_failures,
                     wholeLog.toUtf8().constData());
        return 1;
    }
    std::printf("stall watchdog tests passed\n");
    return 0;
}
