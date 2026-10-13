#include "CrashLogScan.h"

#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QVersionNumber>

#include <algorithm>

namespace forkmesh::crashlog {

namespace {

// The crash footer is 26 '=' and the shutdown-signal footer is 36, so an exact
// compare is what keeps a stop request out of the crash path.
const QString kCrashFooter = QStringLiteral("==========================");
const QString kEpochPrefix = QStringLiteral("when (epoch):");
const QString kSignalPrefix = QStringLiteral("signal:");
const QString kBuildPrefix = QStringLiteral("build:");
const QString kContextHeading = QStringLiteral("context:");
const QString kBacktraceHeading = QStringLiteral("backtrace:");
// The compact one-liner the handler adds for the log file sits *inside* the
// block, between the context and the backtrace. It is not context.
const QString kSignalRecordPrefix = QStringLiteral("ForkMesh signal: ");

// The end of a file, without reading a multi-megabyte segment to get there.
QStringList tailLinesOfFile(const QString &path, qint64 maxBytes)
{
    QFile file(path);
    if (maxBytes <= 0 || !file.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = file.size();
    const bool partial = size > maxBytes;
    if (partial && !file.seek(size - maxBytes))
        return {};
    QStringList lines =
        QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    if (partial && !lines.isEmpty())
        lines.removeFirst(); // the seek landed mid-line
    return lines;
}

const QRegularExpression &appLogStamp()
{
    static const QRegularExpression stamp(QStringLiteral(
        "\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}  "));
    return stamp;
}

bool isAppLogLine(const QString &line)
{
    return appLogStamp()
        .match(line, 0, QRegularExpression::NormalMatch,
               QRegularExpression::AnchorAtOffsetMatchOption)
        .hasMatch();
}

QString withoutAppLogTail(const QString &line)
{
    const QRegularExpressionMatch match = appLogStamp().match(line, 1);
    return match.hasMatch() ? line.left(match.capturedStart()).trimmed()
                            : line;
}

enum class Field { None, Epoch, Signal, Build };

bool endsRecord(const QString &line)
{
    return line == kCrashFooter || line.contains(kCrashBanner) ||
           line.contains(QLatin1String("===== ForkMesh shutdown signal ====="));
}

} // namespace

QList<Record> parseCrashRecords(const QStringList &lines)
{
    QList<Record> records;
    QSet<qint64> seenEpochs;
    for (int index = 0; index < lines.size(); ++index) {
        if (!lines.at(index).contains(kCrashBanner))
            continue;

        Record record;
        bool inContext = false;
        bool inBacktrace = false;
        Field awaiting = Field::None;
        int contextLines = 0;
        int scanned = 0;
        int cursor = index + 1;
        auto assign = [&record](Field field, const QString &value) {
            if (field == Field::Epoch)
                record.whenEpochSeconds = value.toLongLong();
            else if (field == Field::Signal)
                record.signalName = value;
            else if (field == Field::Build)
                record.build = value;
        };
        for (; cursor < lines.size() && scanned < kMaxRecordLines;
             ++cursor, ++scanned) {
            const QString raw = lines.at(cursor).trimmed();
            if (endsRecord(raw))
                break;
            if (isAppLogLine(raw))
                continue;
            const QString line = withoutAppLogTail(raw);
            const bool inHeader = !inContext && !inBacktrace;
            Field field = Field::None;
            QString value;
            if (inHeader && line.startsWith(kEpochPrefix)) {
                field = Field::Epoch;
                value = line.mid(kEpochPrefix.size()).trimmed();
            } else if (inHeader && line.startsWith(kSignalPrefix)) {
                field = Field::Signal;
                value = line.mid(kSignalPrefix.size()).trimmed();
            } else if (inHeader && line.startsWith(kBuildPrefix)) {
                field = Field::Build;
                value = line.mid(kBuildPrefix.size()).trimmed();
            }
            if (field != Field::None) {
                assign(field, value);
                awaiting = value.isEmpty() ? field : Field::None;
                continue;
            }
            if (awaiting != Field::None && !line.isEmpty()) {
                assign(awaiting, line);
                awaiting = Field::None;
                continue;
            }
            if (line == kContextHeading) {
                inContext = true;
                continue;
            }
            if (line == kBacktraceHeading) {
                inContext = false;
                inBacktrace = true;
                continue;
            }
            if (line.isEmpty() || line.startsWith(kSignalRecordPrefix)) {
                inContext = false;
                continue;
            }
            if (inContext && contextLines < kMaxContextLines) {
                record.context +=
                    (record.context.isEmpty() ? QString()
                                              : QStringLiteral("\n")) +
                    line;
                ++contextLines;
                continue;
            }
            if (inBacktrace && record.frames.size() < kMaxFrames)
                record.frames.append(line);
        }
        // Resume after the block: a truncated record stops on the next
        // banner, which the outer loop must still see.
        index = cursor - 1;

        // A block with no epoch cannot be told apart from one already
        // repaired, and a partially written banner is not evidence of
        // anything. Both are dropped rather than dispatched blind.
        if (record.whenEpochSeconds <= 0 || record.signalName.isEmpty())
            continue;
        if (seenEpochs.contains(record.whenEpochSeconds))
            continue; // the same crash, written to stderr and to the log
        seenEpochs.insert(record.whenEpochSeconds);
        records.append(record);
    }
    return records;
}

QList<Record> parseCrashRecords(const QString &text)
{
    return parseCrashRecords(text.split(QLatin1Char('\n')));
}

QList<Record> scanRecentCrashes(const QStringList &liveTail,
                                const QStringList &segmentPaths,
                                int segmentsToScan, qint64 tailBytesPerSegment)
{
    QList<Record> records = parseCrashRecords(liveTail);
    int scanned = 0;
    for (auto it = segmentPaths.crbegin();
         it != segmentPaths.crend() && scanned < segmentsToScan;
         ++it, ++scanned) {
        records += parseCrashRecords(tailLinesOfFile(*it, tailBytesPerSegment));
    }
    std::sort(records.begin(), records.end(),
              [](const Record &a, const Record &b) {
                  return a.whenEpochSeconds < b.whenEpochSeconds;
              });
    // One record per crash, whichever file it turned up in.
    records.erase(std::unique(records.begin(), records.end(),
                              [](const Record &a, const Record &b) {
                                  return a.whenEpochSeconds ==
                                         b.whenEpochSeconds;
                              }),
                  records.end());
    return records;
}

QString summarize(const Record &record)
{
    const QString when =
        record.whenEpochSeconds > 0
            ? QDateTime::fromSecsSinceEpoch(record.whenEpochSeconds)
                  .toString(Qt::ISODate)
            : QStringLiteral("an unknown time");
    return QStringLiteral("%1 at %2").arg(record.signalName, when);
}

QString describe(const Record &record)
{
    QStringList block;
    block << QStringLiteral("Crash: %1").arg(summarize(record));
    if (!record.build.isEmpty())
        block << QStringLiteral("Build: %1").arg(record.build);
    if (!record.context.isEmpty())
        block << QStringLiteral("Crash context: %1").arg(record.context);
    if (!record.frames.isEmpty()) {
        block << QStringLiteral("Backtrace (innermost %1 frame(s)):")
                     .arg(record.frames.size());
        block << record.frames;
    }
    return block.join(QLatin1Char('\n'));
}

Record parseDescription(const QString &description)
{
    Record record;
    const QStringList lines = description.split(QLatin1Char('\n'));
    static const QRegularExpression header(
        QStringLiteral("^Crash: (.*) at (\\S+)$"));
    bool inFrames = false;
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("Crash: "))) {
            const QRegularExpressionMatch match = header.match(line);
            if (match.hasMatch()) {
                record.signalName = match.captured(1).trimmed();
                const QDateTime when =
                    QDateTime::fromString(match.captured(2), Qt::ISODate);
                if (when.isValid())
                    record.whenEpochSeconds = when.toSecsSinceEpoch();
            }
            continue;
        }
        if (line.startsWith(QLatin1String("Build: "))) {
            record.build = line.mid(7).trimmed();
            continue;
        }
        if (line.startsWith(QLatin1String("Backtrace ("))) {
            inFrames = true;
            continue;
        }
        if (inFrames && !line.isEmpty())
            record.frames.append(line);
    }
    return record;
}

QString buildVersion(const QString &build)
{
    static const QRegularExpression version(
        QStringLiteral("\\bv(\\d+(?:\\.\\d+)*)\\b"));
    const QRegularExpressionMatch match = version.match(build);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString signature(const Record &record)
{
    const QString signal =
        record.signalName.section(QLatin1Char(' '), 0, 0).trimmed();
    static const QRegularExpression named(
        QStringLiteral("\\(([^()+\\s]+)\\+0x[0-9a-fA-F]+\\)"));
    for (const QString &frame : record.frames) {
        if (frame.contains(QLatin1String("/libc.so")))
            continue;
        const QRegularExpressionMatch match = named.match(frame);
        if (match.hasMatch())
            return signal + QLatin1Char('|') + match.captured(1);
    }
    return signal;
}

QList<int> supersededByNewerBuild(const QStringList &descriptions,
                                  const QString &runningVersion)
{
    const QVersionNumber running = QVersionNumber::fromString(runningVersion);
    if (running.isNull())
        return {};
    QList<QVersionNumber> versions;
    QStringList signatures;
    QSet<QString> recurring;
    for (const QString &description : descriptions) {
        const Record record = parseDescription(description);
        versions.append(QVersionNumber::fromString(buildVersion(record.build)));
        signatures.append(signature(record));
        if (versions.last() >= running)
            recurring.insert(signatures.last());
    }
    QList<int> superseded;
    for (int index = 0; index < descriptions.size(); ++index) {
        if (!versions.at(index).isNull() && versions.at(index) < running &&
            !recurring.contains(signatures.at(index)))
            superseded.append(index);
    }
    return superseded;
}

} // namespace forkmesh::crashlog
