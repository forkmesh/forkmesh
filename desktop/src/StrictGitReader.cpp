#include "StrictGitReader.h"

#include <QElapsedTimer>
#include <QProcess>

#include <climits>

namespace StrictGitReadInternal {

namespace {

constexpr qint64 kReadChunkBytes = 64 * 1024;
thread_local std::optional<Limits> limitsForTests;

Limits normalizedLimits(Limits limits)
{
    limits.maxStdoutBytes = qMax<qsizetype>(0, limits.maxStdoutBytes);
    limits.maxStderrBytes = qMax<qsizetype>(0, limits.maxStderrBytes);
    limits.maxTotalBytes = qMax<qsizetype>(0, limits.maxTotalBytes);
    limits.maxGitCommands = qMax(0, limits.maxGitCommands);
    limits.maxPullItems = qMax(0, limits.maxPullItems);
    limits.maxEventItems = qMax(0, limits.maxEventItems);
    limits.maxElapsedMs = qMax(0, limits.maxElapsedMs);
    return limits;
}

} // namespace

bool appendBounded(QByteArray *destination, const QByteArray &chunk,
                   qsizetype maxDestinationBytes,
                   qsizetype *remainingTotalBytes)
{
    if (!destination || !remainingTotalBytes || maxDestinationBytes < 0 ||
        *remainingTotalBytes < 0 ||
        destination->size() > maxDestinationBytes ||
        chunk.size() > maxDestinationBytes - destination->size() ||
        chunk.size() > *remainingTotalBytes) {
        return false;
    }
    destination->append(chunk);
    *remainingTotalBytes -= chunk.size();
    return true;
}

ScopedLimitsForTests::ScopedLimitsForTests(const Limits &limits)
    : m_previous(limitsForTests)
{
    limitsForTests = normalizedLimits(limits);
}

ScopedLimitsForTests::~ScopedLimitsForTests()
{
    limitsForTests = m_previous;
}

Reader::Reader()
    : m_limits(normalizedLimits(limitsForTests.value_or(Limits{}))),
      m_remainingBytes(m_limits.maxTotalBytes),
      m_remainingCommands(m_limits.maxGitCommands),
      m_remainingPullItems(m_limits.maxPullItems),
      m_remainingEventItems(m_limits.maxEventItems)
{
    m_elapsed.start();
}

bool Reader::consumePullItem()
{
    if (m_overflowed || m_remainingPullItems <= 0) {
        m_overflowed = true;
        return false;
    }
    --m_remainingPullItems;
    return true;
}

bool Reader::consumeEventItem()
{
    if (m_overflowed || m_remainingEventItems <= 0) {
        m_overflowed = true;
        return false;
    }
    --m_remainingEventItems;
    return true;
}

bool Reader::run(const QString &dir, const QStringList &args,
                 QByteArray *output, QString *error, int timeoutMs)
{
    return runInternal(dir, args, nullptr, output, error, timeoutMs);
}

bool Reader::runInput(const QString &dir, const QStringList &args,
                      const QByteArray &input, QByteArray *output,
                      QString *error, int timeoutMs)
{
    return runInternal(dir, args, &input, output, error, timeoutMs);
}

// code-health: allow(blocking-wait) sync git primitive for the strict loadFromMirror path; its only entry points (IssueStore/PullStore loadAllStrict*) have no call sites
bool Reader::runInternal(const QString &dir, const QStringList &args,
                         const QByteArray *input, QByteArray *output,
                         QString *error, int timeoutMs)
{
    if (output)
        output->clear();
    if (error)
        error->clear();
    if (m_overflowed) {
        if (error)
            *error = QStringLiteral("Strict Git read budget was exceeded.");
        return false;
    }
    if (dir.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Git source path is empty.");
        return false;
    }
    if (m_remainingCommands <= 0) {
        m_overflowed = true;
        if (error)
            *error = QStringLiteral("Strict Git command budget was exceeded.");
        return false;
    }
    const qint64 aggregateRemaining =
        qint64(m_limits.maxElapsedMs) - m_elapsed.elapsed();
    if (aggregateRemaining <= 0) {
        m_overflowed = true;
        if (error)
            *error = QStringLiteral("Strict Git read deadline was exceeded.");
        return false;
    }
    --m_remainingCommands;

    QProcess process;
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), dir} + args);
    if (input) {
        if (process.write(*input) < 0) {
            process.kill();
            if (error)
                *error = QStringLiteral("Could not write Git command input.");
            return false;
        }
        process.closeWriteChannel();
    }

    QByteArray standardOutput;
    QByteArray standardError;
    auto drainChannel = [&](QProcess::ProcessChannel channel,
                            QByteArray *destination, qsizetype maxBytes) {
        process.setReadChannel(channel);
        while (process.bytesAvailable() > 0) {
            const qsizetype remaining = maxBytes - destination->size();
            const qint64 requestSize = qMin<qint64>(
                kReadChunkBytes, qint64(remaining) + 1);
            const QByteArray chunk = process.read(requestSize);
            if (chunk.isEmpty())
                break;
            if (!appendBounded(destination, chunk, maxBytes,
                               &m_remainingBytes)) {
                return false;
            }
        }
        return true;
    };
    auto drainOutput = [&] {
        return drainChannel(QProcess::StandardOutput, &standardOutput,
                            m_limits.maxStdoutBytes) &&
               drainChannel(QProcess::StandardError, &standardError,
                            m_limits.maxStderrBytes);
    };

    QElapsedTimer commandTimer;
    commandTimer.start();
    const int commandDeadlineMs =
        qMin(timeoutMs, int(qMin<qint64>(aggregateRemaining, INT_MAX)));
    for (;;) {
        const qint64 commandRemaining =
            qint64(commandDeadlineMs) - commandTimer.elapsed();
        const qint64 totalRemaining =
            qint64(m_limits.maxElapsedMs) - m_elapsed.elapsed();
        if (commandRemaining <= 0 || totalRemaining <= 0) {
            process.kill();
            process.waitForFinished(1000);
            if (totalRemaining <= 0)
                m_overflowed = true;
            if (error) {
                *error = totalRemaining <= 0
                             ? QStringLiteral(
                                   "Strict Git read deadline was exceeded.")
                             : QStringLiteral("Git command timed out.");
            }
            return false;
        }
        const int waitMs = int(qMin<qint64>(
            40, qMin(commandRemaining, totalRemaining)));
        const bool finished = process.waitForFinished(qMax(1, waitMs));
        if (!drainOutput()) {
            m_overflowed = true;
            process.kill();
            process.waitForFinished(1000);
            if (error)
                *error = QStringLiteral("Strict Git read budget was exceeded.");
            return false;
        }
        if (finished || process.state() == QProcess::NotRunning)
            break;
    }

    if (output)
        *output = standardOutput;
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QString::fromUtf8(standardError).trimmed().left(300);
            if (error->isEmpty())
                *error = QStringLiteral("Git command failed.");
        }
        return false;
    }
    return true;
}

} // namespace StrictGitReadInternal
