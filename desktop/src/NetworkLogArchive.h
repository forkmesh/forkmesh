#ifndef FORKMESH_NETWORK_LOG_ARCHIVE_H
#define FORKMESH_NETWORK_LOG_ARCHIVE_H

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// On-disk retention for the app log.
//
// The live file (network_log.txt) is append-only. Once it grows past
// kRotateLines it is *rotated* into a sibling directory rather than rewritten,
// so closing the app can never shorten the record. Retention is by age (30
// days) with a byte budget as a backstop for nodes that log fast enough to fill
// a disk.
//
// A 30-day span of raw lines is far too much to hold in memory, so the chart is
// fed from a digest instead: fixed-width buckets of (bucket start, category,
// count) that are written as lines are logged and pruned on the same 30-day
// clock. The digest is a few hundred KB at worst.
namespace forkmesh::netlog {

constexpr int kRetentionDays = 30;
constexpr qint64 kRetentionMs = qint64(kRetentionDays) * 24 * 60 * 60 * 1000;

// Lines in the live file before it is rotated. Twice the in-memory buffer, so a
// rotation costs one rename every few hours even on a very chatty node.
constexpr int kRotateLines = 40000;

// Backstop for the rotated segments. Age is the real rule; this only bites on a
// node logging hard enough that 30 days would not fit anywhere sensible.
constexpr qint64 kMaxArchiveBytes = 512LL * 1024 * 1024;

// Digest resolution. At the widest view (30 days across ~180 columns) a column
// spans four hours, so 15 minutes is finer than anything the chart can draw.
constexpr qint64 kDigestBucketMs = 15LL * 60 * 1000;

// bucket start (ms since epoch) -> category badge -> line count
using Digest = QMap<qint64, QHash<QString, int>>;

// <dir of currentPath>/network_log.d
QString archiveDir(const QString &currentPath);
QString digestPath(const QString &currentPath);

// Rotated segments, oldest first. Absolute paths.
QStringList archiveSegments(const QString &currentPath);

// The span a rotated segment covers, read back out of its file name. Returns
// false when the name is not one of ours.
bool segmentSpan(const QString &path, qint64 *firstMs, qint64 *lastMs);

// Timestamp of a stored log line ("yyyy-MM-dd HH:mm:ss  message"), or -1.
qint64 storedLineTimestampMs(const QString &line);

// Move the live file into the archive, named for the span it covers. A missing
// or empty file is a no-op. Returns true when a file was rotated.
bool rotate(const QString &currentPath);

// Drop segments that ended before `cutoffMs`, then oldest-first until the
// archive fits `maxBytes`. Returns how many files were removed.
int pruneArchive(const QString &currentPath, qint64 cutoffMs,
                 qint64 maxBytes = kMaxArchiveBytes);

// Delete every rotated segment and the digest (the Clear button).
void clearArchive(const QString &currentPath);

// Delete one stored line (the Delete action on a search result). The newest
// exact occurrence goes: the live file is checked first, then the rotated
// segments newest first. A line logged with embedded newlines matches the run
// of file lines it was written as. Returns true when a line was removed.
bool removeLine(const QString &currentPath, const QString &line);

// Delete many stored lines at once (Delete all on a search result group). Each
// entry removes one occurrence, exactly as removeLine() would, but every file
// is rewritten at most once. Returns how many lines were removed.
int removeLines(const QString &currentPath, const QStringList &lines);

// Delete every stored line `drop` accepts, from the live file and from every
// rotated segment ("delete all of this category"). Unlike removeLines() this
// is not occurrence-counted: the whole record is walked and rewritten, and a
// segment left with nothing is removed. Returns how many lines went.
int removeMatchingLines(const QString &currentPath,
                        const std::function<bool(const QString &)> &drop);

// Bytes held by the live file plus every rotated segment.
qint64 totalBytes(const QString &currentPath);

// The newest `maxLines` stored lines, oldest first, walking back from the live
// file into the rotated segments so a restart does not visibly shorten the log.
QStringList tailLines(const QString &currentPath, int maxLines);

Digest loadDigest(const QString &currentPath);
bool saveDigest(const QString &currentPath, const Digest &digest);

// How much of the archive a bootstrap rebuild is allowed to read. The rebuild
// happens on the GUI thread at startup, so it is newest-first and bounded;
// history past the budget stays unrepresented in the chart until it ages out.
constexpr qint64 kRebuildScanBytes = 64LL * 1024 * 1024;

// Count retained lines, newest file first, into a fresh digest until
// kRebuildScanBytes is spent. Only used to bootstrap (or repair) the digest.
Digest rebuildDigest(const QString &currentPath,
                     const std::function<QString(const QString &)> &badgeFor);

// Drop buckets that start before `cutoffMs`. Returns true when anything went.
bool pruneDigest(Digest &digest, qint64 cutoffMs);

} // namespace forkmesh::netlog

#endif
