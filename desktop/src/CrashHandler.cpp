#include "ForkMeshVersion.h"
#include "AppPaths.h"
#include "CrashHandler.h"
#include "SourceDir.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <cstdlib>
#include <atomic>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#define FORKMESH_CRASH_HANDLER 1
#endif

namespace forkmesh {

#ifdef FORKMESH_CRASH_HANDLER
namespace {

char g_buildInfo[512] = {0};
char g_crashContext[kCrashContextCapacityBytes + 1] = {0};

char g_altStack[64 * 1024];

int g_mainLogFd = -1;

std::atomic_flag g_handling = ATOMIC_FLAG_INIT;

std::atomic_int g_surviveTerminationSignals{0};
std::atomic_bool g_terminationSignalReceived{false};

int g_terminationPipe[2] = {-1, -1};
std::atomic_bool g_gracefulShutdownArmed{false};
std::atomic_flag g_gracefulShutdownRequested = ATOMIC_FLAG_INIT;

constexpr unsigned int kGracefulShutdownDeadlineSeconds = 15;

int safeUtoa(unsigned long v, char *buf)
{
    char tmp[24];
    int n = 0;
    if (v == 0)
        tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = char('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < n; ++i)
        buf[i] = tmp[n - 1 - i];
    return n;
}

int safeItoa(long v, char *buf)
{
    if (v < 0) {
        buf[0] = '-';
        const unsigned long magnitude =
            (unsigned long)(-(v + 1)) + 1UL;
        return 1 + safeUtoa(magnitude, buf + 1);
    }
    return safeUtoa((unsigned long)v, buf);
}

void safeWriteFd(int fd, const char *s)
{
    if (fd < 0 || !s || !*s)
        return;
    const size_t len = ::strlen(s);
    ssize_t r = ::write(fd, s, len);
    (void)r;
}

void safeWrite(const char *s)
{
    safeWriteFd(2, s);
    safeWriteFd(g_mainLogFd, s);
}

class SignalSafeRecord
{
public:
    void add(const char *s)
    {
        if (!s)
            return;
        while (*s && m_length < sizeof(m_text))
            m_text[m_length++] = *s++;
    }

    void addNumber(unsigned long v)
    {
        char digits[24];
        const int n = safeUtoa(v, digits);
        addBytes(digits, n);
    }

    void addSigned(long v)
    {
        char digits[24];
        const int n = safeItoa(v, digits);
        addBytes(digits, n);
    }

    void writeTo(int fd) const
    {
        if (fd < 0 || m_length == 0)
            return;
        size_t written = 0;
        while (written < m_length) {
            const ssize_t r = ::write(fd, m_text + written, m_length - written);
            if (r <= 0)
                return;
            written += size_t(r);
        }
    }

    void writeToBoth() const
    {
        writeTo(2);
        writeTo(g_mainLogFd);
    }

    void clear() { m_length = 0; }

private:
    void addBytes(const char *bytes, int n)
    {
        for (int i = 0; i < n && m_length < sizeof(m_text); ++i)
            m_text[m_length++] = bytes[i];
    }

    char m_text[sizeof(g_buildInfo) + sizeof(g_crashContext) + 1024];
    size_t m_length = 0;
};

SignalSafeRecord g_record;

void addSignalInfo(SignalSafeRecord &record, const siginfo_t *info)
{
    if (!info)
        return;
    record.add("signal code: ");
    record.addSigned((long)info->si_code);
    record.add("\n");
    record.add("sender pid: ");
    record.addNumber((unsigned long)info->si_pid);
    record.add("\n");
    record.add("sender uid: ");
    record.addNumber((unsigned long)info->si_uid);
    record.add("\n");
}

void addCrashContext(SignalSafeRecord &record)
{
    if (!g_crashContext[0])
        return;
    record.add("context:\n");
    record.add(g_crashContext);
    record.add("\n");
}

void safeBacktrace(void *const *frames, int n)
{
    backtrace_symbols_fd(frames, n, 2);
    if (g_mainLogFd >= 0)
        backtrace_symbols_fd(frames, n, g_mainLogFd);
}

const char *signalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGABRT: return "SIGABRT (abort / assertion / unhandled exception)";
    case SIGBUS:  return "SIGBUS (bus error)";
    case SIGFPE:  return "SIGFPE (floating-point / integer error)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    case SIGTERM: return "SIGTERM (terminated)";
    case SIGINT:  return "SIGINT (interrupt)";
    case SIGHUP:  return "SIGHUP (hangup)";
    case SIGQUIT: return "SIGQUIT (quit)";
#ifdef SIGTRAP
    case SIGTRAP: return "SIGTRAP (trace/breakpoint trap)";
#endif
#ifdef SIGSYS
    case SIGSYS:  return "SIGSYS (bad system call)";
#endif
#ifdef SIGXCPU
    case SIGXCPU: return "SIGXCPU (CPU time limit exceeded)";
#endif
#ifdef SIGXFSZ
    case SIGXFSZ: return "SIGXFSZ (file size limit exceeded)";
#endif
    default:      return "signal";
    }
}

bool isTerminationSignal(int sig)
{
    switch (sig) {
    case SIGTERM:
    case SIGINT:
    case SIGHUP:
    case SIGQUIT:
        return true;
    default:
        return false;
    }
}

void safeWriteMainLogSignalRecord(int sig, unsigned long when,
                                  const char *disposition)
{
    if (g_mainLogFd < 0)
        return;

    g_record.clear();
    g_record.add("ForkMesh signal: ");
    g_record.add(signalName(sig));
    g_record.add(" at epoch ");
    g_record.addNumber(when);
    g_record.add(disposition);
    g_record.add("\n");
    g_record.writeTo(g_mainLogFd);
}

bool requestGracefulShutdown()
{
    if (!g_gracefulShutdownArmed.load(std::memory_order_acquire))
        return false;
    if (g_gracefulShutdownRequested.test_and_set())
        return false;
    const char byte = 1;
    const ssize_t written = ::write(g_terminationPipe[1], &byte, 1);
    if (written != 1) {
        return false;
    }
    ::alarm(kGracefulShutdownDeadlineSeconds);
    return true;
}

void crashHandler(int sig, siginfo_t *info, void *)
{
    if (g_handling.test_and_set()) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    const bool terminationSignal = isTerminationSignal(sig);
    if (terminationSignal)
        g_terminationSignalReceived.store(true, std::memory_order_release);
    const char *const banner = terminationSignal
                                   ? "\n===== ForkMesh shutdown signal =====\n"
                                   : "\n===== ForkMesh crash =====\n";
    const char *const footer = terminationSignal
                                   ? "====================================\n"
                                   : "==========================\n";
    const unsigned long when = (unsigned long)::time(nullptr);
    g_record.clear();
    g_record.add(banner);
    g_record.add("when (epoch): ");
    g_record.addNumber(when);
    g_record.add("\nsignal: ");
    g_record.add(signalName(sig));
    g_record.add("\nbuild: ");
    g_record.add(g_buildInfo);
    g_record.add("\n");
    addSignalInfo(g_record, info);
    addCrashContext(g_record);
    g_record.writeToBoth();

    const bool surviveTermination =
        terminationSignal &&
        g_surviveTerminationSignals.load(std::memory_order_relaxed) > 0;
    const bool gracefulShutdown =
        terminationSignal && !surviveTermination && requestGracefulShutdown();
    safeWriteMainLogSignalRecord(
        sig, when,
        surviveTermination
            ? "; action workflow survived and will run to its end"
        : gracefulShutdown ? "; shutting down cleanly"
                           : "; terminating; full record logged above");

    if (surviveTermination) {
        safeWrite("termination signal survived: an action workflow is still "
                  "running, so ForkMesh keeps going until the run ends\n");
        safeWrite(footer);
        g_handling.clear(std::memory_order_release);
        return;
    }

    if (gracefulShutdown) {
        safeWrite("stop requested: closing the window and quitting normally so "
                  "settings, chat history and the log are saved; sending it "
                  "again exits immediately\n");
        safeWrite(footer);
        g_handling.clear(std::memory_order_release);
        return;
    }

    if (terminationSignal) {
        safeWrite(footer);
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    safeWrite("\nbacktrace:\n");
    void *frames[64];
    int n = backtrace(frames, 64);
    safeBacktrace(frames, n);
    safeWrite("==========================\n");

    signal(sig, SIG_DFL);
    raise(sig);
}

void terminateHandler()
{
    const char *what = nullptr;
    if (std::current_exception()) {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception &e) {
            what = e.what();
        } catch (...) {
            what = "(non-std exception)";
        }
    }
    g_record.clear();
    g_record.add("\n===== ForkMesh unhandled exception =====\n");
    g_record.add("what: ");
    g_record.add(what ? what : "(unknown / no active exception)");
    g_record.add("\n");
    addCrashContext(g_record);
    g_record.writeToBoth();
    std::abort(); // -> SIGABRT -> crashHandler() logs the backtrace
}

void installSignalHandlers()
{
    stack_t ss;
    ss.ss_sp = g_altStack;
    ss.ss_size = sizeof(g_altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTERM, SIGINT,
                    SIGHUP, SIGQUIT
#ifdef SIGTRAP
                    , SIGTRAP
#endif
#ifdef SIGSYS
                    , SIGSYS
#endif
#ifdef SIGXCPU
                    , SIGXCPU
#endif
#ifdef SIGXFSZ
                    , SIGXFSZ
#endif
         })
        sigaction(sig, &sa, nullptr);
}

} // namespace
#endif // FORKMESH_CRASH_HANDLER

namespace {

QString compactDiagnosticForMainLog(QString text)
{
    text.replace(QChar(0x2014), QLatin1Char('-'));
    text.replace(QChar(0x2026), QStringLiteral("..."));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QStringLiteral(" | "));
    text = text.simplified();
    constexpr qsizetype kMaxMainLogDiagnosticChars = 6000;
    if (text.size() > kMaxMainLogDiagnosticChars) {
        text = QStringLiteral("...(diagnostic truncated; showing tail)... ") +
               text.right(kMaxMainLogDiagnosticChars);
    }
    return text;
}

void appendDiagnosticToMainLog(const QString &kind, const QString &context,
                               const QString &details)
{
    const QString dir =
        forkmesh::paths::dataRoot();
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    QFile f(dir + QStringLiteral("/network_log.txt"));
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    const QString line =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) +
        QStringLiteral("  %1: %2").arg(kind, compactDiagnosticForMainLog(context));
    f.write(line.toUtf8());
    if (!details.trimmed().isEmpty()) {
        f.write(" | ");
        f.write(compactDiagnosticForMainLog(details).toUtf8());
    }
    f.write("\n");
}

} // namespace

void installCrashHandler(const QString &crashLogPath,
                         const QString &mainLogPath)
{
#ifdef FORKMESH_CRASH_HANDLER
    const QByteArray build =
        QByteArrayLiteral("ForkMesh v" FORKMESH_VERSION " (src ") +
        QByteArray(forkmesh::sourceDirUtf8()) + QByteArrayLiteral(")");
    qstrncpy(g_buildInfo, build.constData(), sizeof(g_buildInfo));

    Q_UNUSED(crashLogPath);
    if (!mainLogPath.isEmpty()) {
        QDir().mkpath(QFileInfo(mainLogPath).absolutePath());
        const QByteArray path = QFile::encodeName(mainLogPath);
        g_mainLogFd = ::open(path.constData(),
                             O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    }

    void *warm[4];
    backtrace(warm, 4);

    installSignalHandlers();
    std::set_terminate(terminateHandler);
#else
    Q_UNUSED(crashLogPath); // kept for API compatibility; never used
    Q_UNUSED(mainLogPath);
#endif
}

void enableGracefulTerminationShutdown(QObject *context,
                                       std::function<void()> onTerminate)
{
#ifdef FORKMESH_CRASH_HANDLER
    if (!context || !onTerminate)
        return;
    if (g_gracefulShutdownArmed.load(std::memory_order_acquire))
        return; // already armed; one owner of the stop request is enough

    if (::pipe(g_terminationPipe) != 0) {
        g_terminationPipe[0] = g_terminationPipe[1] = -1;
        return; // no pipe: termination keeps its default, immediate behavior
    }
    for (int fd : {g_terminationPipe[0], g_terminationPipe[1]}) {
        ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }

    auto *notifier = new QSocketNotifier(g_terminationPipe[0],
                                         QSocketNotifier::Read, context);
    QObject::connect(notifier, &QSocketNotifier::activated, context,
                     [notifier, callback = std::move(onTerminate)]() {
                         notifier->setEnabled(false);
                         char drain[16];
                         while (::read(g_terminationPipe[0], drain,
                                       sizeof(drain)) > 0)
                             ;
                         callback();
                     });
    g_gracefulShutdownArmed.store(true, std::memory_order_release);
#else
    Q_UNUSED(context);
    Q_UNUSED(onTerminate);
#endif
}

void setCrashContext(const QString &context)
{
#ifdef FORKMESH_CRASH_HANDLER
    const QByteArray utf8 = context.toUtf8();
    const int n = qMin<int>(utf8.size(), int(sizeof(g_crashContext)) - 1);
    if (n > 0)
        memcpy(g_crashContext, utf8.constData() + utf8.size() - n, size_t(n));
    g_crashContext[n] = '\0';
#else
    Q_UNUSED(context);
#endif
}

void setTerminationSignalSurvivalEnabled(bool enabled)
{
#ifdef FORKMESH_CRASH_HANDLER
    if (enabled) {
        g_surviveTerminationSignals.fetch_add(1, std::memory_order_relaxed);
    } else {
        int prev = g_surviveTerminationSignals.load(std::memory_order_relaxed);
        while (prev > 0 &&
               !g_surviveTerminationSignals.compare_exchange_weak(
                   prev, prev - 1, std::memory_order_relaxed))
            ;
    }
#else
    Q_UNUSED(enabled);
#endif
}

bool terminationSignalReceived()
{
#ifdef FORKMESH_CRASH_HANDLER
    return g_terminationSignalReceived.load(std::memory_order_acquire);
#else
    return false;
#endif
}

void logDiagnosticEvent(const QString &context, const QString &details)
{
    const QString block =
        QStringLiteral("\n===== ForkMesh diagnostic =====\n")
        + QStringLiteral("when (epoch): ")
        + QString::number(QDateTime::currentSecsSinceEpoch()) + QLatin1Char('\n')
        + QStringLiteral("context: ") + context + QLatin1Char('\n')
        + details + QLatin1Char('\n')
        + QStringLiteral("===============================\n");

    qCritical().noquote() << block;
    appendDiagnosticToMainLog(QStringLiteral("ForkMesh diagnostic"), context,
                              details);
}

void logCaughtFault(const QString &context, const QString &what)
{
    const QString block =
        QStringLiteral("\n===== ForkMesh caught fault =====\n")
        + QStringLiteral("when (epoch): ")
        + QString::number(QDateTime::currentSecsSinceEpoch()) + QLatin1Char('\n')
        + QStringLiteral("context: ") + context + QLatin1Char('\n')
        + QStringLiteral("what: ") + what + QLatin1Char('\n')
        + QStringLiteral("=================================\n");

    qCritical().noquote() << block;
    appendDiagnosticToMainLog(QStringLiteral("ForkMesh caught fault"), context,
                              what);
}

} // namespace forkmesh
