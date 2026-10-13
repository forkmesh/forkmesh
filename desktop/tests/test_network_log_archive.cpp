#include "NetworkLogArchive.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>

namespace netlog = forkmesh::netlog;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QString stamp(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// Writes `count` stored-format lines one second apart, starting at `startMs`.
static void writeLines(const QString &path, qint64 startMs, int count,
                       const QString &body = QStringLiteral("hello"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;
    QByteArray bytes;
    for (int index = 0; index < count; ++index) {
        bytes += stamp(startMs + qint64(index) * 1000).toUtf8();
        bytes += "  ";
        bytes += body.toUtf8();
        bytes += " #";
        bytes += QByteArray::number(index);
        bytes += '\n';
    }
    file.write(bytes);
    file.close();
}

static int lineCount(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return -1;
    return QString::fromUtf8(file.readAll())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts)
        .size();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid())
        return 1;
    const QString live = QDir(tmp.path()).filePath(QStringLiteral("network_log.txt"));
    const qint64 day = 24LL * 60 * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // --- rotation keeps the record instead of shortening it -----------------
    writeLines(live, now - 40 * day, 50);
    check(netlog::rotate(live), "a non-empty live file rotates");
    check(!QFileInfo::exists(live), "rotation moves the live file out of the way");
    QStringList segments = netlog::archiveSegments(live);
    check(segments.size() == 1, "rotation produced exactly one segment");
    check(lineCount(segments.value(0)) == 50, "the segment holds every line");

    qint64 firstMs = -1;
    qint64 lastMs = -1;
    check(netlog::segmentSpan(segments.value(0), &firstMs, &lastMs),
          "the segment name parses back into a span");
    // Second resolution: the stamps round-trip to the whole second.
    check(qAbs(firstMs - (now - 40 * day)) < 1000, "span starts at the first line");
    check(qAbs(lastMs - (now - 40 * day + 49000)) < 1000, "span ends at the last line");

    // An empty or missing live file is a no-op, not an empty segment.
    check(!netlog::rotate(live), "a missing live file does not rotate");
    check(netlog::archiveSegments(live).size() == 1, "no empty segment appeared");

    // A second rotation in the same second must not overwrite the first.
    writeLines(live, now - 40 * day, 50);
    check(netlog::rotate(live), "an identically stamped file still rotates");
    segments = netlog::archiveSegments(live);
    check(segments.size() == 2, "the collision got its own segment");
    check(netlog::segmentSpan(segments.value(1), &firstMs, &lastMs),
          "the tie-broken name still parses, so it can still age out");

    // --- the 30-day age-out --------------------------------------------------
    writeLines(live, now - 2 * day, 10);
    check(netlog::rotate(live), "a recent segment rotates");
    check(netlog::archiveSegments(live).size() == 3, "three segments on disk");
    const int removed =
        netlog::pruneArchive(live, now - netlog::kRetentionMs);
    check(removed == 2, "both 40-day-old segments aged out");
    segments = netlog::archiveSegments(live);
    check(segments.size() == 1, "the 2-day-old segment survived");
    check(netlog::segmentSpan(segments.value(0), &firstMs, nullptr) &&
              firstMs > now - netlog::kRetentionMs,
          "what survived is inside the retention window");

    // A segment straddling the cutoff is kept: it still holds fresh lines.
    writeLines(live, now - 31 * day, 5);
    writeLines(live, now - 29 * day, 5);
    check(netlog::rotate(live), "a straddling segment rotates");
    check(netlog::pruneArchive(live, now - netlog::kRetentionMs) == 0,
          "a segment ending inside the window is not dropped");
    check(netlog::archiveSegments(live).size() == 2, "both segments remain");

    // --- the byte backstop ---------------------------------------------------
    const qint64 tinyBudget = 200;
    check(netlog::pruneArchive(live, now - netlog::kRetentionMs, tinyBudget) > 0,
          "the byte budget evicts oldest-first when age alone is not enough");
    check(netlog::totalBytes(live) <= tinyBudget + 4096,
          "the archive was brought back near the budget");

    // --- reading back across a rotation --------------------------------------
    QTemporaryDir tailTmp;
    if (!tailTmp.isValid())
        return 1;
    const QString tailLive =
        QDir(tailTmp.path()).filePath(QStringLiteral("network_log.txt"));
    writeLines(tailLive, now - 3 * day, 30, QStringLiteral("older"));
    netlog::rotate(tailLive);
    writeLines(tailLive, now - 1 * day, 20, QStringLiteral("newer"));
    QStringList tail = netlog::tailLines(tailLive, 40);
    check(tail.size() == 40, "the tail is filled from the archive behind it");
    check(tail.constFirst().contains(QStringLiteral("older")),
          "the oldest line came out of the rotated segment");
    check(tail.constLast().contains(QStringLiteral("newer")),
          "the newest line came out of the live file");
    check(netlog::tailLines(tailLive, 10).size() == 10,
          "a tail smaller than the live file does not reach for the archive");
    check(netlog::tailLines(tailLive, 5000).size() == 50,
          "asking for more than exists returns everything");

    // --- the chart digest ----------------------------------------------------
    netlog::Digest digest;
    const qint64 bucket = now - (now % netlog::kDigestBucketMs);
    digest[bucket][QStringLiteral("CLOUD")] = 12;
    digest[bucket][QStringLiteral("ERROR")] = 3;
    digest[bucket - 40 * day][QStringLiteral("CLOUD")] = 7;
    check(netlog::saveDigest(tailLive, digest), "the digest writes");
    netlog::Digest loaded = netlog::loadDigest(tailLive);
    check(loaded == digest, "the digest round-trips");

    check(netlog::pruneDigest(loaded, now - netlog::kRetentionMs),
          "pruning drops the out-of-window bucket");
    check(loaded.size() == 1 && loaded.value(bucket).value(
                                    QStringLiteral("CLOUD")) == 12,
          "the in-window bucket survives intact");
    check(!netlog::pruneDigest(loaded, now - netlog::kRetentionMs),
          "a second prune is a no-op");

    // Rebuilding covers the live file *and* the archive, so a deleted digest
    // does not leave the 30-day chart blank.
    const netlog::Digest rebuilt = netlog::rebuildDigest(
        tailLive, [](const QString &line) {
            return line.contains(QStringLiteral("older"))
                       ? QStringLiteral("OLD")
                       : QStringLiteral("NEW");
        });
    int oldCount = 0;
    int newCount = 0;
    for (auto it = rebuilt.cbegin(); it != rebuilt.cend(); ++it) {
        oldCount += it->value(QStringLiteral("OLD"));
        newCount += it->value(QStringLiteral("NEW"));
    }
    check(oldCount == 30 && newCount == 20,
          "every retained line is counted exactly once");

    // Deleting one entry takes the newest exact match only, leaves partial
    // matches alone, and reaches back into a rotated segment when the live
    // file does not hold it.
    {
        QTemporaryDir removeDir;
        const QString live = removeDir.path() + QStringLiteral("/network_log.txt");
        const QString doomed = stamp(now) + QStringLiteral("  doomed");
        const QString multi =
            stamp(now) + QStringLiteral("  first half\nsecond half");
        QFile file(live);
        check(file.open(QIODevice::WriteOnly | QIODevice::Text),
              "remove fixture opens");
        file.write((doomed + QStringLiteral("\n") + doomed +
                    QStringLiteral(" extra\n") + multi + QStringLiteral("\n") +
                    doomed + QStringLiteral("\nkeep\n"))
                       .toUtf8());
        file.close();
        check(netlog::removeLine(live, doomed), "a stored line is removed");
        check(netlog::removeLine(live, multi), "a multi-line entry is removed");
        QFile after(live);
        check(after.open(QIODevice::ReadOnly | QIODevice::Text),
              "remove fixture reopens");
        const QString left = QString::fromUtf8(after.readAll());
        after.close();
        check(left == doomed + QStringLiteral("\n") + doomed +
                          QStringLiteral(" extra\nkeep\n"),
              "only the newest exact match and the multi-line entry went");
        check(!netlog::removeLine(live, stamp(now) + QStringLiteral("  absent")),
              "a missing line reports false");

        writeLines(live, now - 60 * 1000, 1, QStringLiteral("archived"));
        check(netlog::rotate(live), "remove fixture rotates");
        const QString archived = stamp(now - 60 * 1000) +
                                 QStringLiteral("  archived #0");
        check(netlog::removeLine(live, archived),
              "a line in a rotated segment is removed");
        check(!netlog::tailLines(live, 100).contains(archived),
              "the archived line is gone from the tail");
    }

    // Deleting many entries at once removes one occurrence per entry, across
    // the live file and the rotated segments, and counts only what it found.
    {
        QTemporaryDir batchDir;
        const QString live = batchDir.path() + QStringLiteral("/network_log.txt");
        writeLines(live, now - 60 * 1000, 3, QStringLiteral("old"));
        check(netlog::rotate(live), "batch fixture rotates");
        writeLines(live, now, 3, QStringLiteral("new"));
        const QString oldLine =
            stamp(now - 60 * 1000 + 1000) + QStringLiteral("  old #1");
        const QString newLine = stamp(now + 2000) + QStringLiteral("  new #2");
        const int batchRemoved = netlog::removeLines(
            live, {newLine, oldLine, stamp(now) + QStringLiteral("  absent"),
                   QString()});
        check(batchRemoved == 2, "a batch counts the lines it removed");
        const QStringList left = netlog::tailLines(live, 100);
        check(left.size() == 4 && !left.contains(newLine) && !left.contains(oldLine),
              "the batch removed a live line and an archived line only");
        check(netlog::removeLines(live, {}) == 0, "an empty batch removes nothing");
    }

    // Deleting a whole category walks every file: a segment it empties is
    // removed, a mixed one is rewritten, and the live file survives even when
    // nothing is left in it so appends carry on.
    {
        QTemporaryDir categoryDir;
        const QString live =
            categoryDir.path() + QStringLiteral("/network_log.txt");
        writeLines(live, now - 120 * 1000, 2, QStringLiteral("drop"));
        check(netlog::rotate(live), "category fixture rotates the drop-only file");
        writeLines(live, now - 60 * 1000, 2, QStringLiteral("drop"));
        writeLines(live, now - 50 * 1000, 2, QStringLiteral("keep"));
        check(netlog::rotate(live), "category fixture rotates the mixed file");
        writeLines(live, now, 3, QStringLiteral("drop"));
        check(netlog::archiveSegments(live).size() == 2,
              "category fixture has two segments");
        auto dropped = [](const QString &line) {
            return line.contains(QLatin1String("  drop #"));
        };
        check(netlog::removeMatchingLines(live, dropped) == 7,
              "every matching line is counted once");
        check(netlog::archiveSegments(live).size() == 1,
              "the segment left empty is removed");
        const QStringList left = netlog::tailLines(live, 100);
        check(left.size() == 2 && std::none_of(left.cbegin(), left.cend(), dropped),
              "only the other category is left");
        check(QFileInfo::exists(live) && lineCount(live) == 0,
              "the emptied live file stays in place");
        check(netlog::removeMatchingLines(live, dropped) == 0,
              "a second pass finds nothing");
        check(netlog::removeMatchingLines(live, {}) == 0,
              "no predicate removes nothing");
    }

    if (failures > 0) {
        std::cerr << failures << " network log archive test(s) failed\n";
        return 1;
    }
    std::cout << "network log archive tests passed\n";
    return 0;
}
