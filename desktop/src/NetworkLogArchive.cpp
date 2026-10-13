#include "NetworkLogArchive.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <utility>

namespace forkmesh::netlog {

namespace {

constexpr auto kStoredFormat = "yyyy-MM-dd HH:mm:ss";
constexpr auto kNameFormat = "yyyyMMddHHmmss";
constexpr auto kSegmentPrefix = "netlog-";
constexpr auto kSegmentSuffix = ".txt";

QString segmentStamp(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toString(
        QLatin1String(kNameFormat));
}

qint64 stampToMs(const QString &stamp)
{
    const QDateTime parsed =
        QDateTime::fromString(stamp, QLatin1String(kNameFormat));
    return parsed.isValid() ? parsed.toMSecsSinceEpoch() : -1;
}

// Reads a whole file as UTF-8 lines, empties skipped. The callers only ever
// point this at the live file or one rotated segment, both bounded by
// kRotateLines.
QStringList readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

} // namespace

QString archiveDir(const QString &currentPath)
{
    return QFileInfo(currentPath).absolutePath() +
           QStringLiteral("/network_log.d");
}

QString digestPath(const QString &currentPath)
{
    return archiveDir(currentPath) + QStringLiteral("/digest.tsv");
}

QStringList archiveSegments(const QString &currentPath)
{
    QDir dir(archiveDir(currentPath));
    if (!dir.exists())
        return {};
    // The name starts with the segment's first timestamp, so plain name order
    // is chronological order.
    const QStringList names =
        dir.entryList({QStringLiteral("%1*%2").arg(QLatin1String(kSegmentPrefix),
                                                   QLatin1String(kSegmentSuffix))},
                      QDir::Files, QDir::Name);
    QStringList paths;
    paths.reserve(names.size());
    for (const QString &name : names)
        paths << dir.absoluteFilePath(name);
    return paths;
}

bool segmentSpan(const QString &path, qint64 *firstMs, qint64 *lastMs)
{
    const QString name = QFileInfo(path).fileName();
    if (!name.startsWith(QLatin1String(kSegmentPrefix)) ||
        !name.endsWith(QLatin1String(kSegmentSuffix)))
        return false;
    const int prefix = QLatin1String(kSegmentPrefix).size();
    const int suffix = QLatin1String(kSegmentSuffix).size();
    const QString body = name.mid(prefix, name.size() - prefix - suffix);
    // "<first>-<last>", plus a "-<n>" tie-breaker when two rotations landed in
    // the same second.
    const QStringList parts = body.split(QLatin1Char('-'));
    if (parts.size() != 2 && parts.size() != 3)
        return false;
    const qint64 first = stampToMs(parts.at(0));
    const qint64 last = stampToMs(parts.at(1));
    if (first < 0 || last < 0)
        return false;
    if (firstMs)
        *firstMs = first;
    if (lastMs)
        *lastMs = last;
    return true;
}

qint64 storedLineTimestampMs(const QString &line)
{
    if (line.size() < 19)
        return -1;
    const QDateTime stamp =
        QDateTime::fromString(line.left(19), QLatin1String(kStoredFormat));
    return stamp.isValid() ? stamp.toMSecsSinceEpoch() : -1;
}

bool rotate(const QString &currentPath)
{
    QFileInfo info(currentPath);
    if (!info.exists() || info.size() <= 0)
        return false;
    const QStringList lines = readLines(currentPath);
    if (lines.isEmpty()) {
        QFile::remove(currentPath);
        return false;
    }
    // Name the segment for the span it actually covers so pruning by age never
    // has to open it. Lines with no parsable stamp (a crash-handler dump, say)
    // fall back to the file's own times.
    qint64 firstMs = -1;
    for (const QString &line : lines) {
        firstMs = storedLineTimestampMs(line);
        if (firstMs > 0)
            break;
    }
    qint64 lastMs = -1;
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        lastMs = storedLineTimestampMs(*it);
        if (lastMs > 0)
            break;
    }
    if (firstMs <= 0)
        firstMs = info.birthTime().isValid()
                      ? info.birthTime().toMSecsSinceEpoch()
                      : info.lastModified().toMSecsSinceEpoch();
    if (lastMs <= 0 || lastMs < firstMs)
        lastMs = qMax(firstMs, info.lastModified().toMSecsSinceEpoch());

    const QString dir = archiveDir(currentPath);
    QDir().mkpath(dir);
    const QString stem = QStringLiteral("%1/%2%3-%4")
                             .arg(dir, QLatin1String(kSegmentPrefix),
                                  segmentStamp(firstMs), segmentStamp(lastMs));
    QString target = stem + QLatin1String(kSegmentSuffix);
    // Two rotations inside the same second would collide. The tie-breaker stays
    // inside the "<first>-<last>-<n>" shape segmentSpan() parses, so an
    // overflow segment still ages out on schedule.
    for (int attempt = 1; QFileInfo::exists(target) && attempt < 100; ++attempt)
        target = QStringLiteral("%1-%2%3")
                     .arg(stem)
                     .arg(attempt)
                     .arg(QLatin1String(kSegmentSuffix));
    if (QFileInfo::exists(target))
        return false;
    return QFile::rename(currentPath, target);
}

int pruneArchive(const QString &currentPath, qint64 cutoffMs, qint64 maxBytes)
{
    const QStringList segments = archiveSegments(currentPath);
    int removed = 0;
    QStringList kept;
    qint64 keptBytes = 0;
    for (const QString &path : segments) {
        qint64 firstMs = -1;
        qint64 lastMs = -1;
        if (segmentSpan(path, &firstMs, &lastMs) && lastMs < cutoffMs) {
            if (QFile::remove(path))
                ++removed;
            continue;
        }
        kept << path;
        keptBytes += QFileInfo(path).size();
    }
    // Byte backstop: kept is oldest-first, so drop from the front.
    for (int index = 0; index < kept.size() && keptBytes > maxBytes; ++index) {
        const qint64 size = QFileInfo(kept.at(index)).size();
        if (!QFile::remove(kept.at(index)))
            continue;
        keptBytes -= size;
        ++removed;
    }
    return removed;
}

void clearArchive(const QString &currentPath)
{
    const QStringList segments = archiveSegments(currentPath);
    for (const QString &path : segments)
        QFile::remove(path);
    QFile::remove(digestPath(currentPath));
}

namespace {

// Cut the newest occurrence of `needle` that fills whole lines out of `bytes`.
bool cutNewestLine(QByteArray &bytes, const QByteArray &needle)
{
    qsizetype from = bytes.size();
    while (from >= 0) {
        const qsizetype at = bytes.lastIndexOf(needle, from);
        if (at < 0)
            return false;
        const qsizetype end = at + needle.size();
        if ((at == 0 || bytes.at(at - 1) == '\n') &&
            (end == bytes.size() || bytes.at(end) == '\n')) {
            bytes.remove(at, end < bytes.size() ? needle.size() + 1
                                                : needle.size());
            return true;
        }
        from = at - 1;
    }
    return false;
}

// Cuts every needle this file holds and rewrites it once. The needles that
// were found leave `needles`; the rest carry on to the next (older) file.
void removeLinesFromFile(const QString &path, QList<QByteArray> &needles)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QByteArray bytes = file.readAll();
    file.close();
    QList<QByteArray> missing;
    for (const QByteArray &needle : std::as_const(needles)) {
        if (!cutNewestLine(bytes, needle))
            missing.append(needle);
    }
    if (missing.size() == needles.size())
        return;
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    out.write(bytes);
    if (out.commit())
        needles = std::move(missing);
}

} // namespace

bool removeLine(const QString &currentPath, const QString &line)
{
    return removeLines(currentPath, QStringList{line}) == 1;
}

int removeLines(const QString &currentPath, const QStringList &lines)
{
    QList<QByteArray> needles;
    for (const QString &line : lines) {
        if (!line.isEmpty())
            needles.append(line.toUtf8());
    }
    const int wanted = int(needles.size());
    if (needles.isEmpty())
        return 0;
    removeLinesFromFile(currentPath, needles);
    const QStringList segments =
        needles.isEmpty() ? QStringList() : archiveSegments(currentPath);
    for (auto it = segments.crbegin(); it != segments.crend() && !needles.isEmpty();
         ++it)
        removeLinesFromFile(*it, needles);
    return wanted - int(needles.size());
}

int removeMatchingLines(const QString &currentPath,
                        const std::function<bool(const QString &)> &drop)
{
    if (!drop)
        return 0;
    int removed = 0;
    QStringList files{currentPath};
    files += archiveSegments(currentPath);
    for (const QString &path : std::as_const(files)) {
        const QStringList lines = readLines(path);
        if (lines.isEmpty())
            continue;
        QStringList kept;
        kept.reserve(lines.size());
        for (const QString &line : lines) {
            if (!drop(line))
                kept.append(line);
        }
        const int cut = int(lines.size() - kept.size());
        if (cut == 0)
            continue;
        // A rotated segment with nothing left is just a file; the live file
        // stays in place (truncated) because appends keep landing in it.
        if (kept.isEmpty() && path != currentPath) {
            if (QFile::remove(path))
                removed += cut;
            continue;
        }
        QByteArray bytes;
        if (!kept.isEmpty()) {
            bytes = kept.join(QLatin1Char('\n')).toUtf8();
            bytes += '\n';
        }
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
            continue;
        out.write(bytes);
        if (out.commit())
            removed += cut;
    }
    return removed;
}

qint64 totalBytes(const QString &currentPath)
{
    qint64 bytes = QFileInfo(currentPath).size();
    if (bytes < 0)
        bytes = 0;
    const QStringList segments = archiveSegments(currentPath);
    for (const QString &path : segments)
        bytes += qMax<qint64>(0, QFileInfo(path).size());
    return bytes;
}

QStringList tailLines(const QString &currentPath, int maxLines)
{
    if (maxLines <= 0)
        return {};
    QStringList lines = readLines(currentPath);
    if (lines.size() >= maxLines)
        return lines.mid(lines.size() - maxLines);
    // Walk back through the rotated segments until the buffer is full, so a
    // restart right after a rotation still opens on a full log.
    QStringList segments = archiveSegments(currentPath);
    for (auto it = segments.crbegin();
         it != segments.crend() && lines.size() < maxLines; ++it) {
        QStringList older = readLines(*it);
        const int wanted = maxLines - lines.size();
        if (older.size() > wanted)
            older = older.mid(older.size() - wanted);
        lines = older + lines;
    }
    return lines;
}

Digest loadDigest(const QString &currentPath)
{
    Digest digest;
    QFile file(digestPath(currentPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return digest;
    const QStringList rows = QString::fromUtf8(file.readAll())
                                 .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &row : rows) {
        const QStringList fields = row.split(QLatin1Char('\t'));
        if (fields.size() != 3)
            continue;
        bool startOk = false;
        bool countOk = false;
        const qint64 start = fields.at(0).toLongLong(&startOk);
        const int count = fields.at(2).toInt(&countOk);
        if (!startOk || !countOk || count <= 0)
            continue;
        digest[start][fields.at(1)] += count;
    }
    return digest;
}

bool saveDigest(const QString &currentPath, const Digest &digest)
{
    const QString path = digestPath(currentPath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QByteArray bytes;
    for (auto bucket = digest.cbegin(); bucket != digest.cend(); ++bucket) {
        for (auto category = bucket->cbegin(); category != bucket->cend();
             ++category) {
            if (category.value() <= 0)
                continue;
            bytes += QByteArray::number(bucket.key());
            bytes += '\t';
            bytes += category.key().toUtf8();
            bytes += '\t';
            bytes += QByteArray::number(category.value());
            bytes += '\n';
        }
    }
    // The digest is rewritten whole; a torn write would lose the history the
    // chart draws from, so it goes through QSaveFile.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(bytes);
    return file.commit();
}

Digest rebuildDigest(const QString &currentPath,
                     const std::function<QString(const QString &)> &badgeFor)
{
    Digest digest;
    // Newest first, and stop once the scan budget is spent: this runs on the
    // GUI thread at startup, and re-reading a full archive would be a
    // multi-second freeze. Only the bootstrap path gets here, and the result is
    // written straight back out, so the cost is paid once.
    QStringList files = archiveSegments(currentPath);
    std::reverse(files.begin(), files.end());
    files.prepend(currentPath);
    qint64 budget = kRebuildScanBytes;
    for (const QString &path : std::as_const(files)) {
        if (budget <= 0)
            break;
        budget -= qMax<qint64>(0, QFileInfo(path).size());
        const QStringList lines = readLines(path);
        for (const QString &line : lines) {
            const qint64 timestampMs = storedLineTimestampMs(line);
            if (timestampMs < 0)
                continue;
            const qint64 bucket =
                timestampMs - (timestampMs % kDigestBucketMs);
            ++digest[bucket][badgeFor(line)];
        }
    }
    return digest;
}

bool pruneDigest(Digest &digest, qint64 cutoffMs)
{
    const auto first = digest.begin();
    const auto last = digest.lowerBound(cutoffMs);
    if (first == last)
        return false;
    digest.erase(first, last);
    return true;
}

} // namespace forkmesh::netlog
