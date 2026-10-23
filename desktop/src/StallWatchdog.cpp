#include "StallWatchdog.h"

#include "SystemPressure.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QTextStream>
#include <QTimer>

#include <chrono>
#include <cstdlib>
#include <cstring>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

#if defined(__linux__) && defined(__GLIBC__)
#include <algorithm>
#include <execinfo.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#define STALL_BACKTRACE 1
#endif

namespace {
QElapsedTimer &monoClock()
{
    static QElapsedTimer c;
    if (!c.isValid())
        c.start();
    return c;
}

#ifdef STALL_BACKTRACE
pthread_t g_mainThread;
constexpr int kBtCapacity = 96;
void *g_btBuf[kBtCapacity];
std::atomic<int> g_btCount{-1};
std::uintptr_t g_mainStackLow = 0;
std::uintptr_t g_mainStackHigh = 0;
// Executable mappings loaded when sampling started: the binary and every
// library it links. Those are never unmapped, so the handler may read the
// instruction bytes in front of a candidate return address. A library opened
// later is simply not in the list and its frames are skipped.
constexpr int kMaxTextRanges = 512;
stallwatch::detail::TextRange g_textRanges[kMaxTextRanges];
int g_textRangeCount = 0;

int collectTextRange(dl_phdr_info *info, size_t, void *)
{
    for (int i = 0; i < info->dlpi_phnum && g_textRangeCount < kMaxTextRanges;
         ++i) {
        const ElfW(Phdr) &header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || !(header.p_flags & PF_X) ||
            !(header.p_flags & PF_R) || header.p_memsz == 0)
            continue;
        const std::uintptr_t low = info->dlpi_addr + header.p_vaddr;
        g_textRanges[g_textRangeCount++] = {low, low + header.p_memsz};
    }
    return 0;
}

void collectTextRanges()
{
    g_textRangeCount = 0;
    dl_iterate_phdr(collectTextRange, nullptr);
    std::sort(g_textRanges, g_textRanges + g_textRangeCount,
              [](const stallwatch::detail::TextRange &a,
                 const stallwatch::detail::TextRange &b) {
                  return a.low < b.low;
              });
}

void btHandler(int, siginfo_t *, void *rawContext)
{
    std::uintptr_t instruction = 0;
    std::uintptr_t framePointer = 0;
    std::uintptr_t stackPointer = 0;
#if defined(__x86_64__)
    const auto *context = static_cast<const ucontext_t *>(rawContext);
    if (context) {
        instruction = static_cast<std::uintptr_t>(
            context->uc_mcontext.gregs[REG_RIP]);
        framePointer = static_cast<std::uintptr_t>(
            context->uc_mcontext.gregs[REG_RBP]);
        stackPointer = static_cast<std::uintptr_t>(
            context->uc_mcontext.gregs[REG_RSP]);
    }
#elif defined(__aarch64__)
    const auto *context = static_cast<const ucontext_t *>(rawContext);
    if (context) {
        instruction = static_cast<std::uintptr_t>(context->uc_mcontext.pc);
        framePointer =
            static_cast<std::uintptr_t>(context->uc_mcontext.regs[29]);
        stackPointer = static_cast<std::uintptr_t>(context->uc_mcontext.sp);
    }
#endif
    const int n =
        g_textRangeCount > 0 && stackPointer != 0
            ? stallwatch::detail::safeStackScanTrace(
                  g_btBuf, kBtCapacity, instruction, stackPointer,
                  g_mainStackLow, g_mainStackHigh, g_textRanges,
                  g_textRangeCount)
            : stallwatch::detail::safeFramePointerTrace(
                  g_btBuf, kBtCapacity, instruction, framePointer,
                  g_mainStackLow, g_mainStackHigh);
    g_btCount.store(n, std::memory_order_release);
}

QString captureBacktrace()
{
    g_btCount.store(-1, std::memory_order_release);
    if (pthread_kill(g_mainThread, SIGUSR2) != 0)
        return QString();
    for (int i = 0; i < 250 && g_btCount.load(std::memory_order_acquire) < 0; ++i)
        usleep(1000); // up to ~250ms for the handler to run
    const int n = g_btCount.load(std::memory_order_acquire);
    if (n <= 0)
        return QString();
    char **syms = backtrace_symbols(g_btBuf, n);
    if (!syms)
        return QString();
    QString out;
    for (int i = 0; i < n; ++i) {
        out += QString::fromUtf8(syms[i]);
        out += QLatin1Char('\n');
    }
    free(syms);
    return out;
}
#else
QString captureBacktrace() { return QString(); }
#endif

QString firstFrame(const QString &bt)
{
    const int nl = bt.indexOf(QLatin1Char('\n'));
    return nl < 0 ? bt : bt.left(nl);
}

QString demangleFrameSymbol(const QString &symbol)
{
#if defined(__GNUG__)
    if (symbol.startsWith(QLatin1String("_Z"))) {
        int status = -1;
        const QByteArray encoded = symbol.toLatin1();
        char *decoded = abi::__cxa_demangle(encoded.constData(), nullptr,
                                            nullptr, &status);
        if (status == 0 && decoded) {
            const QString result = QString::fromUtf8(decoded);
            std::free(decoded);
            return result;
        }
        std::free(decoded);
    }
#endif
    return symbol;
}

QString frameSymbol(const QString &line)
{
    const int open = line.indexOf(QLatin1Char('('));
    if (open < 0)
        return QString();
    int end = line.indexOf(QLatin1String("+0x"), open + 1);
    if (end < 0)
        end = line.indexOf(QLatin1Char(')'), open + 1);
    if (end <= open + 1)
        return QString();
    return demangleFrameSymbol(line.mid(open + 1, end - open - 1));
}

bool isApplicationFrame(const QString &line, const QString &executablePath)
{
    if (!executablePath.isEmpty() &&
        line.startsWith(executablePath + QLatin1Char('('))) {
        return true;
    }
    // Older records use the build-relative spelling while installed binaries
    // may have been replaced since the record was captured.  These fallbacks
    // keep attribution useful when a saved report is pasted into a test or an
    // agent prompt.
    return line.contains(QLatin1String("/forkmesh(")) ||
           line.startsWith(QLatin1String("./build/forkmesh("));
}

bool isGlueFrame(const QString &symbol)
{
    if (symbol.isEmpty() || symbol == QLatin1String("main") ||
        symbol == QLatin1String("_start") ||
        symbol.startsWith(QLatin1String("std::")) ||
        symbol.startsWith(QLatin1String("QtPrivate::")) ||
        symbol.startsWith(QLatin1String("QArray")) ||
        symbol.startsWith(QLatin1String("QBasicAtomic")) ||
        symbol.startsWith(QLatin1String("QHash")) ||
        symbol.startsWith(QLatin1String("QHashPrivate")) ||
        symbol.startsWith(QLatin1String("QList")) ||
        symbol.startsWith(QLatin1String("QMap")) ||
        symbol.startsWith(QLatin1String("QSet")) ||
        symbol.startsWith(QLatin1String("QString")) ||
        symbol.startsWith(QLatin1String("QTypedArray")) ||
        symbol.startsWith(QLatin1String("QVector")) ||
        symbol.startsWith(QLatin1String("operator")) ||
        symbol.startsWith(QLatin1String("qHash(")) ||
        symbol.contains(QLatin1String("qHashEquals")) ||
        symbol.startsWith(QLatin1String("comparesEqual("))) {
        return true;
    }
    return symbol.contains(QLatin1String("ForkMeshApplication::notify")) ||
           symbol.contains(QLatin1String("QtEventDeliveryProbe")) ||
           symbol.contains(QLatin1String("QtRuntimeDiagnostics::noteDelivery")) ||
           symbol.contains(QLatin1String("QtRuntimeDiagnostics::recordEventDelivery")) ||
           symbol.contains(QLatin1String("::eventFilter(")) ||
           symbol.contains(QLatin1String("MainWindow::handleFramelessResizeEvent(")) ||
           symbol.contains(QLatin1String("MainWindow::resizeEdgesAtGlobalPos(")) ||
           symbol.contains(QLatin1String("MainWindow::updateFramelessResizeCursor(")) ||
           symbol.contains(QLatin1String("FunctorCall")) ||
           symbol.contains(QLatin1String("FunctorCallable")) ||
           symbol.contains(QLatin1String("deliverOffThreadResult")) ||
           symbol.contains(QLatin1String("runOffThread<")) ||
           symbol.contains(QLatin1String("runGitCaptureBackgrounded")) ||
           symbol.contains(QLatin1String("runGitCapture("));
}

QString qtFallbackOrigin(const QString &backtrace)
{
    if (backtrace.contains(QLatin1String("QTextDocumentLayout")) ||
        backtrace.contains(QLatin1String("QTextEngine")) ||
        backtrace.contains(QLatin1String("harfbuzz")) ||
        backtrace.contains(QLatin1String("libharfbuzz"))) {
        return QStringLiteral("Qt rich-text layout/text shaping");
    }
    if (backtrace.contains(QLatin1String("QWidgetRepaintManager")) ||
        backtrace.contains(QLatin1String("QRasterPaintEngine")) ||
        backtrace.contains(QLatin1String("paintEvent"))) {
        return QStringLiteral("Qt widget paint");
    }
    if (backtrace.contains(QLatin1String("QWaylandDisplay18blockingReadEvents")))
        return QStringLiteral("Wayland compositor event wait");
    if (backtrace.contains(QLatin1String("QRegularExpression")) ||
        backtrace.contains(QLatin1String("libpcre2"))) {
        return QStringLiteral("regular-expression scan on the UI thread");
    }
    return QStringLiteral("Qt event dispatch (application frame unresolved)");
}

QString waitAtSampleHead(const QString &backtrace,
                         const QString &executablePath)
{
    constexpr int kHeadFrames = 16;
    QString head;
    int seen = 0;
    for (const QString &line : backtrace.split(QLatin1Char('\n'))) {
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("--"))) {
            if (seen > 0)
                break;
            continue;
        }
        if (isApplicationFrame(line, executablePath) &&
            !isGlueFrame(frameSymbol(line)))
            break;
        if (++seen > kHeadFrames)
            break;
        head += line;
        head += QLatin1Char('\n');
    }
    if (head.contains(QLatin1String("swapBuffers")) ||
        head.contains(QLatin1String("QRhi8endFrame")) ||
        head.contains(QLatin1String("QRhi::endFrame"))) {
        return QStringLiteral(
            "Qt window flush: GPU buffer swap waiting on the compositor");
    }
    if (head.contains(QLatin1String("syncToDisk")) ||
        head.contains(QLatin1String("QSaveFile6commit")) ||
        head.contains(QLatin1String("QSaveFile::commit"))) {
        return backtrace.left(backtrace.indexOf(QLatin1String("\n--")))
                       .contains(QLatin1String("QSettings"))
                   ? QStringLiteral("QSettings write flushed to disk by fsync "
                                    "on the UI thread")
                   : QStringLiteral("file flushed to disk by fsync on the UI "
                                    "thread");
    }
    if (head.contains(QLatin1String("QCss13StyleSelector")) ||
        head.contains(QLatin1String("QCss::StyleSelector"))) {
        return QStringLiteral(
            "Qt style-sheet polish: widgets created or re-parented under the "
            "application style sheet");
    }
    return QString();
}

QMutex g_blockingMutex;
QString g_blockingCall;
} // namespace

namespace stallwatch {
namespace detail {
int safeFramePointerTrace(void **out, int capacity, std::uintptr_t instruction,
                          std::uintptr_t framePointer,
                          std::uintptr_t stackLow,
                          std::uintptr_t stackHigh) noexcept
{
    if (!out || capacity <= 0)
        return 0;

    int count = 0;
    if (instruction != 0)
        out[count++] = reinterpret_cast<void *>(instruction);

    constexpr std::uintptr_t kFrameWords = 2 * sizeof(std::uintptr_t);
    while (count < capacity && framePointer >= stackLow &&
           framePointer <= stackHigh &&
           stackHigh - framePointer >= kFrameWords &&
           framePointer % alignof(std::uintptr_t) == 0) {
        const auto *frame = reinterpret_cast<const std::uintptr_t *>(framePointer);
        const std::uintptr_t next = frame[0];
        const std::uintptr_t returnAddress = frame[1];
        if (returnAddress == 0)
            break;
        out[count++] = reinterpret_cast<void *>(returnAddress);
        if (next <= framePointer)
            break;
        framePointer = next;
    }
    return count;
}

namespace {
const TextRange *textRangeFor(std::uintptr_t address, const TextRange *ranges,
                              int rangeCount) noexcept
{
    int low = 0;
    int high = rangeCount - 1;
    while (low <= high) {
        const int mid = low + (high - low) / 2;
        if (address < ranges[mid].low)
            high = mid - 1;
        else if (address >= ranges[mid].high)
            low = mid + 1;
        else
            return &ranges[mid];
    }
    return nullptr;
}

// A return address sits just past the call that pushed it. Checking those
// bytes throws out most of the code pointers a stack also holds (function
// pointers, vtables' targets, stale jump targets).
bool followsCall(std::uintptr_t address, const TextRange &range) noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    if (address - range.low < 7)
        return false;
    const auto *code = reinterpret_cast<const unsigned char *>(address);
    if (code[-5] == 0xE8) // call rel32
        return true;
    // call r/m: FF /2, two to seven bytes with its ModRM, SIB and displacement.
    for (int length = 2; length <= 7; ++length) {
        if (code[-length] == 0xFF && ((code[-length + 1] >> 3) & 7) == 2)
            return true;
    }
    return false;
#elif defined(__aarch64__)
    if (address % 4 != 0 || address - range.low < 4)
        return false;
    const std::uint32_t call =
        *reinterpret_cast<const std::uint32_t *>(address - 4);
    return (call & 0xFC000000u) == 0x94000000u     // bl
           || (call & 0xFFFFFC1Fu) == 0xD63F0000u; // blr
#else
    (void)address;
    (void)range;
    return true;
#endif
}
} // namespace

int safeStackScanTrace(void **out, int capacity, std::uintptr_t instruction,
                       std::uintptr_t stackPointer, std::uintptr_t stackLow,
                       std::uintptr_t stackHigh, const TextRange *ranges,
                       int rangeCount) noexcept
{
    if (!out || capacity <= 0)
        return 0;

    int count = 0;
    if (instruction != 0)
        out[count++] = reinterpret_cast<void *>(instruction);
    if (!ranges || rangeCount <= 0 || stackPointer < stackLow ||
        stackPointer >= stackHigh)
        return count;

    // Deep enough for any GUI-thread stack seen in the stall log, and bounded
    // so a sample stays well under a millisecond.
    constexpr std::uintptr_t kMaxScanBytes = 512 * 1024;
    constexpr std::uintptr_t kWord = sizeof(std::uintptr_t);
    const std::uintptr_t end = stackHigh - stackPointer > kMaxScanBytes
                                   ? stackPointer + kMaxScanBytes
                                   : stackHigh;
    std::uintptr_t slot = (stackPointer + kWord - 1) & ~(kWord - 1);
    for (; count < capacity && slot <= end - kWord; slot += kWord) {
        const std::uintptr_t word =
            *reinterpret_cast<const std::uintptr_t *>(slot);
        const TextRange *range = textRangeFor(word, ranges, rangeCount);
        if (range && followsCall(word, *range))
            out[count++] = reinterpret_cast<void *>(word);
    }
    return count;
}
} // namespace detail

void noteBlockingCall(const QString &what)
{
    QMutexLocker lock(&g_blockingMutex);
    g_blockingCall = what;
}
QString blockingCall()
{
    QMutexLocker lock(&g_blockingMutex);
    return g_blockingCall;
}

QString likelyOrigin(const QString &backtrace, const QString &executablePath)
{
    const QString wait = waitAtSampleHead(backtrace, executablePath);
    QStringList frames;
    for (const QString &line : backtrace.split(QLatin1Char('\n'))) {
        if (!isApplicationFrame(line, executablePath))
            continue;
        const QString symbol = frameSymbol(line);
        if (isGlueFrame(symbol))
            continue;
        if (!frames.contains(symbol))
            frames.append(symbol.left(220));
        if (frames.size() == 2)
            break;
    }
    if (!wait.isEmpty())
        return frames.isEmpty()
                   ? wait
                   : QStringLiteral("%1, under %2")
                         .arg(wait, frames.join(QStringLiteral(" -> ")));
    if (!frames.isEmpty())
        return frames.join(QStringLiteral(" -> "));
    return qtFallbackOrigin(backtrace);
}

QString eventLoopContext(const QString &backtrace)
{
    if (backtrace.contains(QLatin1String("runGitCaptureBackgrounded"))) {
        return QStringLiteral(
            "nested Qt event loop while the GUI awaited a worker Git read; "
            "queued callbacks could run re-entrantly");
    }
    if ((backtrace.contains(QLatin1String("waitForGit")) ||
         backtrace.contains(QLatin1String("pumpKeepAlive"))) &&
        backtrace.contains(QLatin1String("processEvents"))) {
        return QStringLiteral(
            "nested Qt event pump during a synchronous Git wait; queued "
            "callbacks could run re-entrantly");
    }
    const int execFrames =
        backtrace.count(QLatin1String("_ZN10QEventLoop4exec")) +
        backtrace.count(QLatin1String("QEventLoop::exec"));
    if (execFrames > 1)
        return QStringLiteral("nested Qt event loop (%1 exec frames)")
            .arg(execFrames);
    return QStringLiteral("top-level Qt event dispatch");
}
} // namespace stallwatch

BlockingCallScope::BlockingCallScope(const QString &what)
    : m_prev(stallwatch::blockingCall())
{
    stallwatch::noteBlockingCall(what);
}
BlockingCallScope::~BlockingCallScope() { stallwatch::noteBlockingCall(m_prev); }

StallWatchdog::StallWatchdog(QObject *parent) : QObject(parent)
{
    monoClock();
}

StallWatchdog::~StallWatchdog()
{
    stop();
}

void StallWatchdog::stop()
{
    m_running.store(false);
    if (m_beatTimer)
        m_beatTimer->stop();
    if (m_worker.joinable())
        m_worker.join();
}

void StallWatchdog::start(int stallThresholdMs, const QString &logPath,
                          const QString &buildInfo)
{
    if (m_running.load())
        return;
    m_thresholdMs = stallThresholdMs;
    m_logPath = logPath;
    m_buildInfo = buildInfo;
    m_pid = QCoreApplication::applicationPid();
    m_exePath = QCoreApplication::applicationFilePath();
    m_lastBeatMs.store(monoClock().elapsed());

#ifdef STALL_BACKTRACE
    g_mainThread = pthread_self();
    // Resolve the stack bounds before arming SIGUSR2. The handler itself may
    // interrupt malloc/free, so it must only read already-known memory and
    // publish raw addresses; backtrace_symbols() remains on the worker thread.
    pthread_attr_t attributes;
    if (pthread_getattr_np(g_mainThread, &attributes) == 0) {
        void *stackBase = nullptr;
        size_t stackSize = 0;
        if (pthread_attr_getstack(&attributes, &stackBase, &stackSize) == 0) {
            g_mainStackLow = reinterpret_cast<std::uintptr_t>(stackBase);
            g_mainStackHigh = g_mainStackLow + stackSize;
        }
        pthread_attr_destroy(&attributes);
    }
    collectTextRanges();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = btHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_SIGINFO; // let interrupted syscalls resume
    sigaction(SIGUSR2, &sa, nullptr);
#endif

    m_beatTimer = new QTimer(this);
    m_beatTimer->setInterval(200);
    connect(m_beatTimer, &QTimer::timeout, this, [this] { beat(); });
    m_beatTimer->start();

    m_running.store(true);
    m_worker = std::thread([this] { watchLoop(); });
}

void StallWatchdog::beat()
{
    m_lastBeatMs.store(monoClock().elapsed(), std::memory_order_relaxed);
}

void StallWatchdog::watchLoop()
{
    bool inStall = false;
    qint64 peak = 0;
    qint64 lastSampleMs = 0;
    int sampleCount = 0;
    QString bt;       // first sample (also handed to the stalled() signal)
    QString extra;    // later samples taken while a long stall keeps dragging on
    QString lastTop;  // deepest frame of the last sample, to skip duplicate spots
    QString culprit;  // breadcrumb of the operation blocking when the stall began
    qint64 estimatedStartedAtMs = 0;
    qint64 firstSampleAgeMs = 0;
    // The longest this sampler thread itself lost inside one capture. On a
    // healthy machine a capture is a few milliseconds; seconds here mean every
    // thread of the process was starved (paging), not only the GUI thread.
    qint64 watchdogHeldMs = 0;
    forkmesh::SystemPressureSnapshot pressureAtDetection;
    constexpr int kMaxSamples = 6;
    constexpr qint64 kResampleMs = 1500;
    while (m_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // stop() joins this thread before QObject/widget destruction proceeds.
        // Do not take one final SIGUSR2 sample merely because stop arrived
        // during the polling sleep.
        if (!m_running.load())
            break;
        const qint64 now = monoClock().elapsed();
        const qint64 age = now - m_lastBeatMs.load(std::memory_order_relaxed);
        if (age > m_thresholdMs) {
            if (!inStall) {
                inStall = true;
                peak = age;
                // Wall clock first, before anything below can block. On
                // 2026-09-12 this thread sat ~152 s inside its first capture
                // while the whole process paged, and a start estimated after
                // that put a 153 s stall's beginning two seconds before its
                // end (STALL-20260912T133439106-p367098-0015).
                estimatedStartedAtMs =
                    QDateTime::currentMSecsSinceEpoch() - age;
                firstSampleAgeMs = age;
                bt = captureBacktrace(); // sample the stack at first detection
                culprit = stallwatch::blockingCall();
                pressureAtDetection = forkmesh::sampleSystemPressure();
                watchdogHeldMs = monoClock().elapsed() - now;
                lastTop = firstFrame(bt);
                lastSampleMs = now;
                sampleCount = bt.isEmpty() ? 0 : 1;
            } else {
                peak = qMax(peak, age);
                if (culprit.isEmpty()) // set slightly after the stall began
                    culprit = stallwatch::blockingCall();
                if (sampleCount < kMaxSamples && now - lastSampleMs >= kResampleMs) {
                    lastSampleMs = now;
                    const QString s = captureBacktrace();
                    watchdogHeldMs =
                        qMax(watchdogHeldMs, monoClock().elapsed() - now);
                    const QString top = firstFrame(s);
                    if (!s.isEmpty() && top != lastTop) {
                        ++sampleCount;
                        extra += QStringLiteral(
                                     "-- sample %1: still blocked ~%2 ms --\n")
                                     .arg(sampleCount)
                                     .arg(age);
                        extra += s;
                        lastTop = top;
                    }
                }
            }
        } else if (inStall) {
            inStall = false; // the event loop resumed
            const QDateTime resumedAt = QDateTime::currentDateTime();
            const forkmesh::SystemPressureSnapshot pressureAtResume =
                forkmesh::sampleSystemPressure();
            const QDateTime startedAt = QDateTime::fromMSecsSinceEpoch(
                estimatedStartedAtMs > 0
                    ? estimatedStartedAtMs
                    : resumedAt.toMSecsSinceEpoch() - peak);
            const qint64 wallBlockedMs =
                resumedAt.toMSecsSinceEpoch() - startedAt.toMSecsSinceEpoch();
            const quint64 sequence = ++m_reportSequence;
            const QString stallId =
                QStringLiteral("STALL-%1-p%2-%3")
                    .arg(startedAt.toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz")))
                    .arg(m_pid)
                    .arg(sequence, 4, 10, QLatin1Char('0'));
            const QString origin = stallwatch::likelyOrigin(bt + extra, m_exePath);
            const QString loopContext = stallwatch::eventLoopContext(bt + extra);
            QString header =
                QStringLiteral("stall-id: %1\n"
                               "likely origin: %2\n"
                               "event-loop context: %3\n"
                               "timing: estimated start %4 | watchdog report %5\n"
                               "context: %6 | pid %7 | main thread blocked ~%8 ms "
                               "(threshold %9 ms) | %10 stack sample(s)\n")
                    .arg(stallId, origin, loopContext,
                         startedAt.toString(Qt::ISODateWithMs),
                         resumedAt.toString(Qt::ISODateWithMs),
                         m_buildInfo.isEmpty()
                             ? QStringLiteral("ForkMesh (build unknown)")
                             : m_buildInfo)
                    .arg(m_pid)
                    .arg(peak)
                    .arg(m_thresholdMs)
                    .arg(sampleCount);
            if (!culprit.isEmpty())
                header += QStringLiteral("blocking call: %1\n").arg(culprit);
            // What the rest of the machine was doing. A freeze on a box that is
            // paging is a different bug from the same frames on an idle one:
            // the first is fixed by taking load off the machine (or by the
            // app deferring optional work, see SystemPressure.h), the second
            // by moving the sampled call off the GUI thread.
            const QString systemAtDetection =
                forkmesh::describeSystemPressure(pressureAtDetection);
            const QString systemAtResume =
                forkmesh::describeSystemPressure(pressureAtResume);
            if (!systemAtDetection.isEmpty()) {
                header += QStringLiteral("system at detection: %1\n")
                              .arg(systemAtDetection);
            }
            if (!systemAtResume.isEmpty())
                header += QStringLiteral("system at resume: %1\n").arg(systemAtResume);
            if (watchdogHeldMs > 1000) {
                header += QStringLiteral(
                              "watchdog: the sampler thread was itself held ~%1 ms "
                              "inside one capture - every thread of this process "
                              "was starved, not only the GUI thread\n")
                              .arg(watchdogHeldMs);
            }
            const bool starved =
                forkmesh::systemUnderPressure(pressureAtDetection) ||
                forkmesh::systemUnderPressure(pressureAtResume) ||
                watchdogHeldMs > 1000;
            if (starved) {
                header += QStringLiteral(
                    "verdict: machine-wide memory/io starvation. The frames "
                    "below show where the GUI thread happened to wait on paging, "
                    "not code that is slow on an idle machine; reduce the load "
                    "on this box (parallel builds, test runs) before treating "
                    "this as an application fault.\n");
            } else if (qAbs(wallBlockedMs - peak) > 2000) {
                header += QStringLiteral(
                              "note: monotonic peak %1 ms disagrees with the "
                              "wall-clock span %2 ms\n")
                              .arg(peak)
                              .arg(wallBlockedMs);
            }
            if (!m_logPath.isEmpty()) {
                const QString actions =
                    QFileInfo(m_logPath).dir().filePath(QStringLiteral("actions.jsonl"));
                header += QStringLiteral(
                              "correlate actions: %1 (match pid %2 and the timing window)\n")
                              .arg(actions)
                              .arg(m_pid);
            }
            if (!m_exePath.isEmpty()) {
                header += QStringLiteral(
                              "symbolize unresolved app frames: copy the +0xOFFSET "
                              "value (not the ASLR address) into: addr2line -fCpe %1 "
                              "0xOFFSET\n")
                              .arg(m_exePath);
            }
            QString samples;
            if (bt.isEmpty()) {
                samples = QStringLiteral(
                    "-- sample unavailable: the main thread did not answer the "
                    "backtrace signal --\n");
            } else {
                samples = QStringLiteral("-- sample 1: first observed blocked ~%1 ms --\n")
                              .arg(firstSampleAgeMs);
                samples += bt;
            }
            const QString full = header + samples + extra;
            if (!m_logPath.isEmpty()) {
                QDir().mkpath(QFileInfo(m_logPath).absolutePath());
                QFile f(m_logPath);
                if (f.open(QIODevice::Append | QIODevice::Text)) {
                    QTextStream ts(&f);
                    ts << "===== " << stallId << " BEGIN =====\n";
                    ts << resumedAt.toString(Qt::ISODateWithMs)
                       << "  UI stalled ~" << peak << " ms\n";
                    ts << full;
                    ts << "===== " << stallId << " END =====\n";
                    // Keep the legacy separator so older support tooling can
                    // still split a mixed old/new log while BEGIN/END provides
                    // the exact searchable boundary for new records.
                    ts << "----\n\n";
                }
            }
            // Queued to the main thread for the live summary.  The same marker
            // is already durable above, so the one-line log can point straight
            // at this exact record rather than only at a file containing many.
            emit stalled(peak, stallId, origin, culprit, full,
                         startedAt.toMSecsSinceEpoch());
            peak = 0;
            sampleCount = 0;
            bt.clear();
            extra.clear();
            lastTop.clear();
            culprit.clear();
            estimatedStartedAtMs = 0;
            firstSampleAgeMs = 0;
            watchdogHeldMs = 0;
            pressureAtDetection = forkmesh::SystemPressureSnapshot();
        }
    }
}
