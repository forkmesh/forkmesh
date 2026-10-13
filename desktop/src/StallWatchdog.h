#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <thread>

class QTimer;

namespace stallwatch {
void noteBlockingCall(const QString &what); // pass an empty string to clear
QString blockingCall();
// Turn a raw backtrace into the short, stable attribution shown in the live
// log.  Kept public so diagnostics tests can exercise the same parser used by
// the watchdog without having to manufacture a real GUI freeze.
QString likelyOrigin(const QString &backtrace,
                     const QString &executablePath = QString());
QString eventLoopContext(const QString &backtrace);

namespace detail {
// Walk a conventional frame-pointer chain without allocating or taking locks.
// The SIGUSR2 sampler calls this from its signal handler, so keep this helper
// limited to validated pointer reads and writes into caller-owned storage.
int safeFramePointerTrace(void **out, int capacity, std::uintptr_t instruction,
                          std::uintptr_t framePointer,
                          std::uintptr_t stackLow,
                          std::uintptr_t stackHigh) noexcept;

// One executable mapping, [low, high). The watchdog collects these once when it
// starts, sorted by low, so the signal handler only ever reads them.
struct TextRange {
    std::uintptr_t low = 0;
    std::uintptr_t high = 0;
};

// Scan the interrupted stack from its stack pointer upwards for words that are
// return addresses: they point into a text range just past a call instruction.
// Qt, glibc and the other distribution libraries are built without frame
// pointers, so the chain above breaks at the first such frame and most samples
// kept only their leaf - never the app code that called in. The scan needs no
// chain. It may keep a stale return address left in a live frame's locals;
// every read stays inside [stackPointer, stackHigh) and the given ranges.
int safeStackScanTrace(void **out, int capacity, std::uintptr_t instruction,
                       std::uintptr_t stackPointer, std::uintptr_t stackLow,
                       std::uintptr_t stackHigh, const TextRange *ranges,
                       int rangeCount) noexcept;
} // namespace detail
} // namespace stallwatch

// RAII breadcrumb: names the blocking operation for the scope's lifetime and
// restores the previous breadcrumb on exit, so nested/re-entrant calls (an event-
// loop pump running another read mid-wait) still report the right culprit. Cheap
// enough to wrap every synchronous main-thread git read.
class BlockingCallScope
{
public:
    explicit BlockingCallScope(const QString &what);
    ~BlockingCallScope();
    BlockingCallScope(const BlockingCallScope &) = delete;
    BlockingCallScope &operator=(const BlockingCallScope &) = delete;

private:
    QString m_prev;
};

class StallWatchdog : public QObject
{
    Q_OBJECT
public:
    explicit StallWatchdog(QObject *parent = nullptr);
    ~StallWatchdog() override;

    void start(int stallThresholdMs = 1500, const QString &logPath = QString(),
               const QString &buildInfo = QString());
    // MainWindow teardown can legitimately stop the event-loop heartbeat while
    // Qt frees its widget tree. Stop stack sampling before that destruction
    // starts: the SIGUSR2 unwinder must never interrupt allocator teardown.
    void stop();
    bool isRunning() const { return m_running.load(); }

signals:
    void stalled(qint64 peakMs, const QString &stallId,
                 const QString &likelyOrigin, const QString &blockingCall,
                 const QString &report, qint64 startedAtMs);

private:
    void beat();      // runs on the main thread (heartbeat timer)
    void watchLoop(); // runs on the background thread

    std::atomic<qint64> m_lastBeatMs{0};
    std::atomic<bool> m_running{false};
    int m_thresholdMs = 1500;
    QString m_logPath;
    QString m_buildInfo;  // version + source dir, recorded with each stall
    QString m_exePath;    // running binary, for the addr2line symbolize hint
    qint64 m_pid = 0;
    quint64 m_reportSequence = 0;
    QTimer *m_beatTimer = nullptr;
    std::thread m_worker;
};
