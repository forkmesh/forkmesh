#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Crash records the crash handler left behind in the app log.
//
// CrashHandler writes its block straight to the log file descriptor from the
// dying process, so nothing in the running app ever observes it: the only way
// a crash becomes work is for the next launch to read it back out of the log.
// Reading it lives here, apart from MainWindow, so "is this a crash" stays
// covered by a test that does not need the widget stack.
namespace forkmesh::crashlog {

inline const QString kCrashBanner =
    QStringLiteral("===== ForkMesh crash =====");

// Backtrace frames and context lines kept per record. The whole record is a
// prompt for a repair agent, not an archive: the log itself has the rest.
inline constexpr int kMaxFrames = 12;
inline constexpr int kMaxContextLines = 8;
// A truncated block (the process died mid-write) must not swallow the rest of
// the log looking for a footer that was never written.
inline constexpr int kMaxRecordLines = 200;
// Rotated segments to look into, and how much of the end of each to read. The
// handler writes through the descriptor it opened at startup, so a log that
// rotated mid-session leaves the block at the end of a segment instead of in
// the live file - always at the end, because the process died right after.
inline constexpr int kMaxSegmentsScanned = 3;
inline constexpr qint64 kSegmentTailBytes = 256 * 1024;

// One "===== ForkMesh crash =====" block.
struct Record {
    qint64 whenEpochSeconds = 0;
    QString signalName;  // "SIGSEGV (invalid memory access)"
    QString build;
    QString context;     // setCrashContext() text captured at the fault
    QStringList frames;  // backtrace, innermost first

    qint64 whenMs() const { return whenEpochSeconds * 1000; }
};

// Crash blocks in `lines`, oldest first, at most one per epoch: the handler
// writes each block to stderr *and* to the log fd, so a node that points both
// at the same file stores it twice. A stop request keeps its own
// "shutdown signal" heading and is never a crash, however it was delivered.
QList<Record> parseCrashRecords(const QStringList &lines);
QList<Record> parseCrashRecords(const QString &text);

// Every crash still visible from the app log: `liveTail` - the buffer the
// window already holds, which covers the live file and the segments behind it
// - plus the end of the newest rotated segments, for the crash a mid-session
// rotation left further back than that buffer reaches.
QList<Record> scanRecentCrashes(const QStringList &liveTail,
                                const QStringList &segmentPaths,
                                int segmentsToScan = kMaxSegmentsScanned,
                                qint64 tailBytesPerSegment = kSegmentTailBytes);

// "SIGSEGV (invalid memory access) at 2026-08-20T10:00:00" - one line, for a
// log entry or a trigger string.
QString summarize(const Record &record);

// Signal, time, context and frames as the evidence block of an assignment.
QString describe(const Record &record);

Record parseDescription(const QString &description);

QString buildVersion(const QString &build);

QString signature(const Record &record);

QList<int> supersededByNewerBuild(const QStringList &descriptions,
                                  const QString &runningVersion);

} // namespace forkmesh::crashlog
